# wlrdp Phase 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the raw-bitmap SurfaceBits pipeline with H.264 encoding via FFmpeg and the RDPGFX graphics channel, with adaptive framerate and a fallback path for clients that don't support H.264.

**Architecture:** The encoder is rewritten to use FFmpeg's libavcodec for H.264 encoding. It tries hardware encoders first (VA-API `h264_vaapi`, then NVENC `h264_nvenc`) and falls back to software `libx264`. Encoded NALUs are sent via FreeRDP's RDPGFX server-side channel using AVC420 surface commands. For clients that don't negotiate GFX+H.264, the existing NSCodec/SurfaceBits path is retained as a fallback. Frame pacing is made adaptive: the capture loop tracks encode+send time and adjusts the interval to avoid queueing.

**Tech Stack:** C11, Meson, FFmpeg (`libavcodec`, `libavutil`, `libswscale`), FreeRDP 3.x RDPGFX server API, VA-API (optional runtime), NVENC (optional runtime)

**Spec:** `docs/superpowers/specs/2026-04-16-wlrdp-design.md` — Phase 3 section

---

## File Map

| File | Responsibility |
|------|---------------|
| `meson.build` | Add FFmpeg dependencies (libavcodec, libavutil, libswscale) |
| `meson_options.txt` | Enable `enable-h264` option |
| `.devcontainer/Dockerfile` | Add FFmpeg dev packages, VA-API drivers |
| `src/session/encoder.h` | Rewrite: encoder interface supporting H.264 and NSCodec modes |
| `src/session/encoder.c` | Rewrite: FFmpeg H.264 encoding with hw accel probe + NSCodec fallback |
| `src/session/rdp_peer.h` | Add RDPGFX context and GFX frame-send function |
| `src/session/rdp_peer.c` | Add RDPGFX channel setup, AVC420 surface commands, fallback logic |
| `src/session/main.c` | Adaptive frame pacing, pass codec info through pipeline |

---

### Task 1: Build System — FFmpeg Dependencies

**Files:**
- Modify: `.devcontainer/Dockerfile`
- Modify: `meson.build`
- Modify: `meson_options.txt`

- [ ] **Step 1: Add FFmpeg packages to Dockerfile**

Add to the `dnf install` list in `.devcontainer/Dockerfile`, after the existing packages:

```
    ffmpeg-free-devel \
    libva-devel \
    mesa-va-drivers \
```

- [ ] **Step 2: Enable the h264 build option in meson_options.txt**

Change the default value from `'disabled'` to `'auto'`:

```meson
option('enable-h264', type: 'feature', value: 'auto',
    description: 'H.264 encoding via FFmpeg (Phase 3)')
```

- [ ] **Step 3: Add FFmpeg dependencies to meson.build**

Add after the existing `pam_dep` line in `meson.build`:

```meson
h264_opt = get_option('enable-h264')
libavcodec_dep = dependency('libavcodec', required: h264_opt)
libavutil_dep = dependency('libavutil', required: h264_opt)
libswscale_dep = dependency('libswscale', required: h264_opt)
have_h264 = libavcodec_dep.found() and libavutil_dep.found() and libswscale_dep.found()
if have_h264
    add_project_arguments('-DWLRDP_HAVE_H264=1', language: 'c')
endif
```

- [ ] **Step 4: Update src/session/meson.build to link FFmpeg**

Replace the current `executable` block:

```meson
session_sources = files(
    'main.c',
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
    'rdp_peer.c',
)

session_deps = [
    freerdp_dep,
    freerdp_server_dep,
    winpr_dep,
    wayland_client_dep,
    xkbcommon_dep,
    protocols_dep,
    common_dep,
]

if have_h264
    session_deps += [libavcodec_dep, libavutil_dep, libswscale_dep]
endif

executable('wlrdp-session',
    session_sources,
    dependencies: session_deps,
    install: true,
)
```

- [ ] **Step 5: Verify the build still compiles**

```bash
meson setup build --wipe && meson compile -C build
```

Expected: Both binaries compile. If FFmpeg is not installed, `have_h264` is false and `WLRDP_HAVE_H264` is not defined — existing code compiles unchanged.

- [ ] **Step 6: Commit**

```bash
git add .devcontainer/Dockerfile meson.build meson_options.txt src/session/meson.build
git commit -m "build: add FFmpeg dependencies for H.264 encoding (Phase 3)"
```

---

### Task 2: Rewrite Encoder — H.264 via FFmpeg with NSCodec Fallback

**Files:**
- Modify: `src/session/encoder.h`
- Modify: `src/session/encoder.c`

The encoder becomes a dual-mode component. When H.264 is available and the client supports it, it uses FFmpeg. Otherwise it falls back to NSCodec. The public API stays the same: `encoder_init`, `encoder_encode`, `encoder_destroy` — but `encoder_init` now takes a mode parameter, and the output includes codec metadata.

- [ ] **Step 1: Rewrite src/session/encoder.h**

Replace the entire file:

```c
#ifndef WLRDP_ENCODER_H
#define WLRDP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

enum wlrdp_encoder_mode {
    WLRDP_ENCODER_NSC,     /* NSCodec fallback (Phase 1 behavior) */
    WLRDP_ENCODER_H264,    /* H.264 via FFmpeg */
};

struct wlrdp_encoder {
    enum wlrdp_encoder_mode mode;
    uint32_t width;
    uint32_t height;

    /* NSCodec state (used when mode == WLRDP_ENCODER_NSC) */
    void *nsc_ctx;
    void *nsc_stream;

    /* H.264 state (used when mode == WLRDP_ENCODER_H264) */
    void *av_codec_ctx;    /* AVCodecContext* */
    void *av_frame;        /* AVFrame* (NV12 or YUV420P) */
    void *av_packet;       /* AVPacket* */
    void *sws_ctx;         /* SwsContext* for BGRX->YUV conversion */

    /* Output — valid after encoder_encode until next call or destroy */
    uint8_t *out_buf;
    uint32_t out_len;
    bool is_keyframe;
};

/*
 * Initialize the encoder. Tries H.264 if mode is WLRDP_ENCODER_H264
 * and WLRDP_HAVE_H264 is defined; falls back to NSC on failure.
 * Returns true on success. After init, check enc->mode for actual mode.
 */
bool encoder_init(struct wlrdp_encoder *enc, enum wlrdp_encoder_mode mode,
                  uint32_t width, uint32_t height);

/*
 * Encode a frame of XRGB8888 pixels.
 * After calling, enc->out_buf and enc->out_len contain the encoded data.
 * enc->is_keyframe indicates whether this is an IDR frame (H.264 only).
 */
bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride);

/*
 * Force the next frame to be a keyframe (H.264 only, no-op for NSC).
 */
void encoder_request_keyframe(struct wlrdp_encoder *enc);

void encoder_destroy(struct wlrdp_encoder *enc);

#endif /* WLRDP_ENCODER_H */
```

- [ ] **Step 2: Rewrite src/session/encoder.c**

Replace the entire file:

```c
#include "encoder.h"
#include "common.h"

#include <freerdp/codec/nsc.h>
#include <freerdp/codec/color.h>
#include <winpr/stream.h>

#ifdef WLRDP_HAVE_H264
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#endif

/* --- NSCodec helpers --- */

static bool nsc_init(struct wlrdp_encoder *enc)
{
    NSC_CONTEXT *nsc = nsc_context_new();
    if (!nsc) {
        WLRDP_LOG_ERROR("failed to create NSCodec context");
        return false;
    }

    nsc_context_set_parameters(nsc, NSC_COLOR_LOSS_LEVEL, 0);
    nsc_context_set_parameters(nsc, NSC_ALLOW_SUBSAMPLING, 1);
    nsc_context_set_parameters(nsc, NSC_DYNAMIC_COLOR_FIDELITY, 1);

    if (!nsc_context_set_parameters(nsc, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRX32)) {
        WLRDP_LOG_ERROR("failed to set NSCodec pixel format");
        nsc_context_free(nsc);
        return false;
    }

    wStream *s = Stream_New(NULL, enc->width * enc->height * 4);
    if (!s) {
        WLRDP_LOG_ERROR("failed to create encoder stream");
        nsc_context_free(nsc);
        return false;
    }

    enc->nsc_ctx = nsc;
    enc->nsc_stream = s;
    enc->mode = WLRDP_ENCODER_NSC;
    WLRDP_LOG_INFO("encoder initialized (%ux%u, NSCodec)", enc->width, enc->height);
    return true;
}

static bool nsc_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                       uint32_t stride)
{
    NSC_CONTEXT *nsc = enc->nsc_ctx;
    wStream *s = enc->nsc_stream;

    Stream_SetPosition(s, 0);

    if (!nsc_compose_message(nsc, s, pixels, enc->width, enc->height, stride)) {
        WLRDP_LOG_ERROR("nsc_compose_message failed");
        return false;
    }

    enc->out_buf = Stream_Buffer(s);
    enc->out_len = (uint32_t)Stream_GetPosition(s);
    enc->is_keyframe = true; /* NSCodec frames are always complete */
    return true;
}

static void nsc_cleanup(struct wlrdp_encoder *enc)
{
    if (enc->nsc_stream) {
        Stream_Free(enc->nsc_stream, TRUE);
        enc->nsc_stream = NULL;
    }
    if (enc->nsc_ctx) {
        nsc_context_free(enc->nsc_ctx);
        enc->nsc_ctx = NULL;
    }
}

/* --- H.264 helpers --- */

#ifdef WLRDP_HAVE_H264

static bool h264_try_encoder(struct wlrdp_encoder *enc, const char *name)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
        WLRDP_LOG_INFO("H.264 encoder '%s' not found", name);
        return false;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    ctx->width = enc->width;
    ctx->height = enc->height;
    ctx->time_base = (AVRational){1, 30};
    ctx->framerate = (AVRational){30, 1};
    ctx->gop_size = 60;        /* keyframe every 2 seconds at 30fps */
    ctx->max_b_frames = 0;     /* RDP doesn't benefit from B-frames */
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->bit_rate = enc->width * enc->height * 2; /* ~2 bits/pixel */
    ctx->thread_count = 1;     /* single-threaded for low latency */

    /* Low-latency tuning */
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(ctx->priv_data, "profile", "baseline", 0);

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        WLRDP_LOG_INFO("H.264 encoder '%s' failed to open", name);
        avcodec_free_context(&ctx);
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        avcodec_free_context(&ctx);
        return false;
    }
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }

    struct SwsContext *sws = sws_getContext(
        enc->width, enc->height, AV_PIX_FMT_BGRA,
        enc->width, enc->height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }

    enc->av_codec_ctx = ctx;
    enc->av_frame = frame;
    enc->av_packet = pkt;
    enc->sws_ctx = sws;
    enc->mode = WLRDP_ENCODER_H264;

    WLRDP_LOG_INFO("encoder initialized (%ux%u, H.264 via %s)",
                    enc->width, enc->height, name);
    return true;
}

static bool h264_init(struct wlrdp_encoder *enc)
{
    /* Try hardware encoders first, then software */
    static const char *encoders[] = {
        "h264_vaapi",
        "h264_nvenc",
        "libx264",
        NULL,
    };

    for (int i = 0; encoders[i]; i++) {
        if (h264_try_encoder(enc, encoders[i])) {
            return true;
        }
    }

    WLRDP_LOG_WARN("no H.264 encoder available, falling back to NSCodec");
    return false;
}

static bool h264_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                        uint32_t stride)
{
    AVCodecContext *ctx = enc->av_codec_ctx;
    AVFrame *frame = enc->av_frame;
    AVPacket *pkt = enc->av_packet;
    struct SwsContext *sws = enc->sws_ctx;

    /* Convert BGRX -> YUV420P */
    const uint8_t *src_data[1] = { pixels };
    int src_linesize[1] = { (int)stride };

    if (av_frame_make_writable(frame) < 0) {
        WLRDP_LOG_ERROR("av_frame_make_writable failed");
        return false;
    }

    sws_scale(sws, src_data, src_linesize, 0, enc->height,
              frame->data, frame->linesize);

    frame->pts = ctx->frame_num;

    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        WLRDP_LOG_ERROR("avcodec_send_frame failed: %d", ret);
        return false;
    }

    ret = avcodec_receive_packet(ctx, pkt);
    if (ret == AVERROR(EAGAIN)) {
        /* Encoder needs more frames — shouldn't happen with zerolatency */
        enc->out_len = 0;
        return true;
    }
    if (ret < 0) {
        WLRDP_LOG_ERROR("avcodec_receive_packet failed: %d", ret);
        return false;
    }

    enc->out_buf = pkt->data;
    enc->out_len = pkt->size;
    enc->is_keyframe = !!(pkt->flags & AV_PKT_FLAG_KEY);

    return true;
}

static void h264_cleanup(struct wlrdp_encoder *enc)
{
    if (enc->sws_ctx) {
        sws_freeContext(enc->sws_ctx);
        enc->sws_ctx = NULL;
    }
    if (enc->av_packet) {
        av_packet_free((AVPacket **)&enc->av_packet);
    }
    if (enc->av_frame) {
        av_frame_free((AVFrame **)&enc->av_frame);
    }
    if (enc->av_codec_ctx) {
        avcodec_free_context((AVCodecContext **)&enc->av_codec_ctx);
    }
}

#endif /* WLRDP_HAVE_H264 */

/* --- Public API --- */

bool encoder_init(struct wlrdp_encoder *enc, enum wlrdp_encoder_mode mode,
                  uint32_t width, uint32_t height)
{
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;

#ifdef WLRDP_HAVE_H264
    if (mode == WLRDP_ENCODER_H264) {
        if (h264_init(enc)) {
            return true;
        }
        /* Fall through to NSCodec */
    }
#else
    if (mode == WLRDP_ENCODER_H264) {
        WLRDP_LOG_WARN("H.264 not compiled in, falling back to NSCodec");
    }
#endif

    return nsc_init(enc);
}

bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride)
{
#ifdef WLRDP_HAVE_H264
    if (enc->mode == WLRDP_ENCODER_H264) {
        return h264_encode(enc, pixels, stride);
    }
#endif
    return nsc_encode(enc, pixels, stride);
}

void encoder_request_keyframe(struct wlrdp_encoder *enc)
{
#ifdef WLRDP_HAVE_H264
    if (enc->mode == WLRDP_ENCODER_H264 && enc->av_frame) {
        ((AVFrame *)enc->av_frame)->pict_type = AV_PICTURE_TYPE_I;
    }
#else
    (void)enc;
#endif
}

void encoder_destroy(struct wlrdp_encoder *enc)
{
#ifdef WLRDP_HAVE_H264
    h264_cleanup(enc);
#endif
    nsc_cleanup(enc);
    memset(enc, 0, sizeof(*enc));
}
```

- [ ] **Step 3: Verify it compiles with H.264 disabled**

```bash
meson setup build --wipe -Denable-h264=disabled && meson compile -C build
```

Expected: Compiles. NSCodec path is unchanged.

- [ ] **Step 4: Verify it compiles with H.264 enabled**

```bash
meson setup build --wipe -Denable-h264=enabled && meson compile -C build
```

Expected: Compiles if FFmpeg dev packages are installed. If not installed, this will fail — that's expected and correct for the `enabled` option.

- [ ] **Step 5: Commit**

```bash
git add src/session/encoder.h src/session/encoder.c
git commit -m "feat: add H.264 encoding via FFmpeg with NSCodec fallback"
```

---

### Task 3: RDPGFX Channel — AVC420 Frame Delivery

**Files:**
- Modify: `src/session/rdp_peer.h`
- Modify: `src/session/rdp_peer.c`

Add FreeRDP's RDPGFX server-side channel. When the client negotiates GFX+AVC420, frames go through the graphics pipeline. Otherwise, fall back to SurfaceBits with NSCodec.

- [ ] **Step 1: Update src/session/rdp_peer.h**

Replace the entire file:

```c
#ifndef WLRDP_RDP_PEER_H
#define WLRDP_RDP_PEER_H

#include <stdbool.h>
#include <stdint.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

struct wlrdp_input;

enum wlrdp_send_mode {
    WLRDP_SEND_SURFACE_BITS,  /* Phase 1/2 fallback: SurfaceBits + NSCodec */
    WLRDP_SEND_GFX_AVC420,    /* Phase 3: RDPGFX + H.264 */
};

struct wlrdp_peer_context {
    rdpContext base;

    struct wlrdp_input *input;
    uint32_t width;
    uint32_t height;
    bool activated;

    /* RDPGFX state */
    void *gfx_context;       /* RdpgfxServerContext* */
    uint16_t gfx_surface_id;
    bool gfx_opened;
    uint32_t gfx_frame_id;

    enum wlrdp_send_mode send_mode;
};

bool rdp_peer_init(freerdp_peer *client, const char *cert_file,
                   const char *key_file, struct wlrdp_input *input);

/*
 * Send a frame to the RDP client.
 * For GFX mode: data is H.264 NAL units, len is byte count.
 * For SurfaceBits mode: data is raw BGRX pixels, len is byte count.
 * is_keyframe is only meaningful for GFX mode.
 */
bool rdp_peer_send_frame(freerdp_peer *client,
                         uint8_t *data, uint32_t len,
                         uint32_t width, uint32_t height,
                         bool is_keyframe);

/*
 * Query whether the client supports GFX+AVC420.
 * Only valid after the peer is activated.
 */
bool rdp_peer_supports_gfx_h264(freerdp_peer *client);

bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input);

#endif /* WLRDP_RDP_PEER_H */
```

- [ ] **Step 2: Rewrite src/session/rdp_peer.c**

Replace the entire file:

```c
#include "rdp_peer.h"
#include "input.h"
#include "common.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/server/rdpgfx.h>
#include <linux/input-event-codes.h>

/* --- GFX channel callbacks --- */

static UINT gfx_caps_advertise(RdpgfxServerContext *context,
                                const RDPGFX_CAPS_ADVERTISE_PDU *pdu)
{
    freerdp_peer *client = context->rdpcontext->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    /* Accept the first capability set that supports AVC420 */
    bool found_avc420 = false;
    uint32_t chosen = 0;

    for (uint32_t i = 0; i < pdu->capsSetCount; i++) {
        if (pdu->capsSets[i].version >= RDPGFX_CAPVERSION_10) {
            /* RDPGFX 10.x+ supports AVC420 */
            chosen = i;
            found_avc420 = true;
            break;
        }
        if (pdu->capsSets[i].version >= RDPGFX_CAPVERSION_81) {
            chosen = i;
        }
    }

    RDPGFX_CAPS_CONFIRM_PDU confirm = {
        .capsSet = &pdu->capsSets[chosen],
    };

    if (found_avc420) {
        ctx->send_mode = WLRDP_SEND_GFX_AVC420;
        WLRDP_LOG_INFO("GFX: client supports AVC420 (caps version 0x%08x)",
                        pdu->capsSets[chosen].version);
    } else {
        ctx->send_mode = WLRDP_SEND_SURFACE_BITS;
        WLRDP_LOG_INFO("GFX: client does not support AVC420, "
                        "falling back to SurfaceBits");
    }

    return context->CapsConfirm(context, &confirm);
}

static bool gfx_init(struct wlrdp_peer_context *ctx)
{
    RdpgfxServerContext *gfx = rdpgfx_server_context_new(
        (HANDLE)ctx->base.peer);
    if (!gfx) {
        WLRDP_LOG_WARN("failed to create RDPGFX server context");
        return false;
    }

    gfx->rdpcontext = &ctx->base;
    gfx->CapsAdvertise = gfx_caps_advertise;

    ctx->gfx_context = gfx;
    ctx->gfx_surface_id = 0;
    ctx->gfx_frame_id = 0;
    ctx->gfx_opened = false;

    if (gfx->Open(gfx) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("RDPGFX channel Open failed");
        rdpgfx_server_context_free(gfx);
        ctx->gfx_context = NULL;
        return false;
    }

    ctx->gfx_opened = true;
    WLRDP_LOG_INFO("RDPGFX channel opened");
    return true;
}

static bool gfx_create_surface(struct wlrdp_peer_context *ctx,
                                uint32_t width, uint32_t height)
{
    RdpgfxServerContext *gfx = ctx->gfx_context;

    /* Reset output with the new surface dimensions */
    RDPGFX_RESET_GRAPHICS_PDU reset = {
        .width = width,
        .height = height,
        .monitorCount = 1,
        .monitorDefArray = &(MONITOR_DEF){
            .left = 0, .top = 0,
            .right = width - 1, .bottom = height - 1,
            .flags = MONITOR_PRIMARY,
        },
    };

    if (gfx->ResetGraphics(gfx, &reset) != CHANNEL_RC_OK) {
        WLRDP_LOG_ERROR("GFX ResetGraphics failed");
        return false;
    }

    RDPGFX_CREATE_SURFACE_PDU create = {
        .surfaceId = 1,
        .width = width,
        .height = height,
        .pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888,
    };

    if (gfx->CreateSurface(gfx, &create) != CHANNEL_RC_OK) {
        WLRDP_LOG_ERROR("GFX CreateSurface failed");
        return false;
    }

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = {
        .surfaceId = 1,
        .outputOriginX = 0,
        .outputOriginY = 0,
    };

    if (gfx->MapSurfaceToOutput(gfx, &map) != CHANNEL_RC_OK) {
        WLRDP_LOG_ERROR("GFX MapSurfaceToOutput failed");
        return false;
    }

    ctx->gfx_surface_id = 1;
    WLRDP_LOG_INFO("GFX surface created (%ux%u)", width, height);
    return true;
}

static void gfx_cleanup(struct wlrdp_peer_context *ctx)
{
    if (ctx->gfx_context) {
        RdpgfxServerContext *gfx = ctx->gfx_context;
        if (ctx->gfx_surface_id) {
            RDPGFX_DELETE_SURFACE_PDU del = { .surfaceId = ctx->gfx_surface_id };
            gfx->DeleteSurface(gfx, &del);
        }
        if (ctx->gfx_opened) {
            gfx->Close(gfx);
        }
        rdpgfx_server_context_free(gfx);
        ctx->gfx_context = NULL;
    }
}

/* --- RDP input callbacks (unchanged from Phase 2) --- */

static BOOL on_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code)
{
    freerdp_peer *client = input->context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->input) return TRUE;

    uint32_t evdev_key;
    if (flags & KBD_FLAGS_EXTENDED) {
        switch (code) {
        case 0x1C: evdev_key = KEY_KPENTER; break;
        case 0x1D: evdev_key = KEY_RIGHTCTRL; break;
        case 0x35: evdev_key = KEY_KPSLASH; break;
        case 0x38: evdev_key = KEY_RIGHTALT; break;
        case 0x47: evdev_key = KEY_HOME; break;
        case 0x48: evdev_key = KEY_UP; break;
        case 0x49: evdev_key = KEY_PAGEUP; break;
        case 0x4B: evdev_key = KEY_LEFT; break;
        case 0x4D: evdev_key = KEY_RIGHT; break;
        case 0x4F: evdev_key = KEY_END; break;
        case 0x50: evdev_key = KEY_DOWN; break;
        case 0x51: evdev_key = KEY_PAGEDOWN; break;
        case 0x52: evdev_key = KEY_INSERT; break;
        case 0x53: evdev_key = KEY_DELETE; break;
        case 0x5B: evdev_key = KEY_LEFTMETA; break;
        case 0x5C: evdev_key = KEY_RIGHTMETA; break;
        case 0x5D: evdev_key = KEY_COMPOSE; break;
        default:   evdev_key = code; break;
        }
    } else {
        evdev_key = code;
    }

    bool pressed = !(flags & KBD_FLAGS_RELEASE);
    input_keyboard_key(ctx->input, evdev_key, pressed);

    return TRUE;
}

static BOOL on_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    freerdp_peer *client = input->context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->input) return TRUE;

    if (flags & PTR_FLAGS_MOVE) {
        input_pointer_motion(ctx->input, x, y);
    }

    if (flags & PTR_FLAGS_BUTTON1) {
        input_pointer_button(ctx->input, BTN_LEFT, !!(flags & PTR_FLAGS_DOWN));
    }
    if (flags & PTR_FLAGS_BUTTON2) {
        input_pointer_button(ctx->input, BTN_RIGHT, !!(flags & PTR_FLAGS_DOWN));
    }
    if (flags & PTR_FLAGS_BUTTON3) {
        input_pointer_button(ctx->input, BTN_MIDDLE, !!(flags & PTR_FLAGS_DOWN));
    }

    if (flags & PTR_FLAGS_WHEEL) {
        int16_t value = (int16_t)(flags & 0x01FF);
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) {
            value = -value;
        }
        input_pointer_axis(ctx->input, value / 120);
    }

    return TRUE;
}

static BOOL on_extended_mouse_event(rdpInput *input, UINT16 flags,
                                    UINT16 x, UINT16 y)
{
    freerdp_peer *client = input->context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    (void)flags;
    if (!ctx->input) return TRUE;

    input_pointer_motion(ctx->input, x, y);
    return TRUE;
}

/* --- Peer lifecycle callbacks --- */

static BOOL on_post_connect(freerdp_peer *client)
{
    rdpSettings *settings = client->context->settings;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    WLRDP_LOG_INFO("RDP client connected: %ux%u %ubpp",
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
                    freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth));

    WLRDP_LOG_INFO("negotiated: NSCodec=%d SurfaceCommands=%d GfxH264=%d",
        freerdp_settings_get_bool(settings, FreeRDP_NSCodec),
        freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled),
        freerdp_settings_get_bool(settings, FreeRDP_GfxH264));

    /* Try to open GFX channel */
    if (freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline)) {
        gfx_init(ctx);
    }

    return TRUE;
}

static BOOL on_activate(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    ctx->activated = true;

    /* If GFX is open but surface not yet created, create it now */
    if (ctx->gfx_context && !ctx->gfx_surface_id) {
        rdpSettings *settings = client->context->settings;
        uint32_t w = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
        uint32_t h = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
        if (!gfx_create_surface(ctx, w, h)) {
            /* Fall back to SurfaceBits */
            ctx->send_mode = WLRDP_SEND_SURFACE_BITS;
            gfx_cleanup(ctx);
        }
    }

    WLRDP_LOG_INFO("RDP peer activated (send_mode=%s)",
                    ctx->send_mode == WLRDP_SEND_GFX_AVC420
                        ? "GFX_AVC420" : "SurfaceBits");
    return TRUE;
}

/* --- Public API --- */

bool rdp_peer_init(freerdp_peer *client, const char *cert_file,
                   const char *key_file, struct wlrdp_input *input)
{
    rdpSettings *settings = client->context->settings;

    rdpCertificate *cert = freerdp_certificate_new_from_file(cert_file);
    if (!cert) {
        WLRDP_LOG_ERROR("failed to load certificate from '%s'", cert_file);
        return false;
    }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
        WLRDP_LOG_ERROR("failed to set server certificate");
        return false;
    }

    rdpPrivateKey *key = freerdp_key_new_from_file(key_file);
    if (!key) {
        WLRDP_LOG_ERROR("failed to load private key from '%s'", key_file);
        return false;
    }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1)) {
        WLRDP_LOG_ERROR("failed to set server private key");
        return false;
    }

    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);

    freerdp_settings_set_bool(settings, FreeRDP_SurfaceCommandsEnabled, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    /* Advertise GFX pipeline support */
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);

    client->PostConnect = on_post_connect;
    client->Activate = on_activate;
    client->context->input->KeyboardEvent = on_keyboard_event;
    client->context->input->MouseEvent = on_mouse_event;
    client->context->input->ExtendedMouseEvent = on_extended_mouse_event;

    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    ctx->input = input;
    ctx->send_mode = WLRDP_SEND_SURFACE_BITS; /* default until GFX negotiated */

    return true;
}

bool rdp_peer_supports_gfx_h264(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    return ctx->send_mode == WLRDP_SEND_GFX_AVC420;
}

static bool send_gfx_avc420(struct wlrdp_peer_context *ctx,
                             uint8_t *data, uint32_t len,
                             uint32_t width, uint32_t height,
                             bool is_keyframe)
{
    RdpgfxServerContext *gfx = ctx->gfx_context;

    RDPGFX_AVC420_BITMAP_STREAM avc420 = {
        .data = data,
        .length = len,
        .metaData = {
            .regionRects = &(RECTANGLE_16){
                .left = 0, .top = 0,
                .right = (UINT16)width, .bottom = (UINT16)height,
            },
            .numRegionRects = 1,
            .quantQualityVals = &(RDPGFX_H264_QUANT_QUALITY){
                .qp = 22,
                .qualityVal = 100,
                .qpVal = 22,
            },
        },
    };

    ctx->gfx_frame_id++;

    RDPGFX_START_FRAME_PDU start = { .frameId = ctx->gfx_frame_id };
    if (gfx->StartFrame(gfx, &start) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("GFX StartFrame failed");
        return false;
    }

    RDPGFX_SURFACE_COMMAND cmd = {
        .surfaceId = ctx->gfx_surface_id,
        .codecId = RDPGFX_CODECID_AVC420,
        .format = GFX_PIXEL_FORMAT_XRGB_8888,
        .left = 0,
        .top = 0,
        .right = width,
        .bottom = height,
        .length = sizeof(avc420),
        .data = (BYTE *)&avc420,
    };

    if (gfx->SurfaceCommand(gfx, &cmd) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("GFX SurfaceCommand failed");
        return false;
    }

    RDPGFX_END_FRAME_PDU end = { .frameId = ctx->gfx_frame_id };
    if (gfx->EndFrame(gfx, &end) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("GFX EndFrame failed");
        return false;
    }

    return true;
}

static bool send_surface_bits(freerdp_peer *client,
                               uint8_t *data, uint32_t len,
                               uint32_t width, uint32_t height)
{
    rdpUpdate *update = client->context->update;

    SURFACE_BITS_COMMAND cmd = { 0 };
    cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
    cmd.destLeft = 0;
    cmd.destTop = 0;
    cmd.destRight = width;
    cmd.destBottom = height;
    cmd.bmp.bpp = 32;
    cmd.bmp.width = width;
    cmd.bmp.height = height;
    cmd.bmp.codecID = 0; /* raw bitmap */
    cmd.bmp.bitmapDataLength = len;
    cmd.bmp.bitmapData = data;

    BOOL ret = update->SurfaceBits(update->context, &cmd);
    if (!ret) {
        WLRDP_LOG_WARN("SurfaceBits failed");
    }

    return ret;
}

bool rdp_peer_send_frame(freerdp_peer *client,
                         uint8_t *data, uint32_t len,
                         uint32_t width, uint32_t height,
                         bool is_keyframe)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (ctx->send_mode == WLRDP_SEND_GFX_AVC420 && ctx->gfx_context) {
        return send_gfx_avc420(ctx, data, len, width, height, is_keyframe);
    }

    return send_surface_bits(client, data, len, width, height);
}

bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input)
{
    client->sockfd = peer_fd;
    return rdp_peer_init(client, cert_file, key_file, input);
}
```

- [ ] **Step 3: Verify it compiles**

```bash
meson compile -C build
```

Expected: Compiles. The RDPGFX headers come from `freerdp-server3`. If any struct/function names differ in the installed FreeRDP version, fix them in the next step.

- [ ] **Step 4: Fix any FreeRDP RDPGFX API issues**

Check for API differences:

```bash
grep -r 'rdpgfx_server_context_new\|RdpgfxServerContext\|RDPGFX_AVC420' /usr/include/freerdp3/ /usr/include/freerdp-server3/
```

Adjust struct names, function signatures, and include paths as needed for the installed FreeRDP 3.x version. Common differences:
- `rdpgfx.h` may be at `freerdp/channels/rdpgfx.h` or `freerdp/gdi/gfx.h`
- `rdpgfx_server_context_new` may take different parameters
- `RDPGFX_SURFACE_COMMAND` struct fields may differ

- [ ] **Step 5: Commit**

```bash
git add src/session/rdp_peer.h src/session/rdp_peer.c
git commit -m "feat: add RDPGFX channel with AVC420 surface commands"
```

---

### Task 4: Update Main Event Loop — Adaptive Framerate and Codec Selection

**Files:**
- Modify: `src/session/main.c`

The main loop needs to:
1. Choose encoder mode based on client capabilities (GFX+H.264 vs NSCodec)
2. Pass the right data to `rdp_peer_send_frame` based on mode
3. Adapt frame pacing based on encode time

- [ ] **Step 1: Rewrite src/session/main.c**

Replace the entire file:

```c
#include "common.h"
#include "ipc.h"
#include "compositor.h"
#include "capture.h"
#include "input.h"
#include "encoder.h"
#include "rdp_peer.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>

#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define FRAME_INTERVAL_MIN_MS  16  /* ~60 fps ceiling */
#define FRAME_INTERVAL_MAX_MS  100 /* 10 fps floor */
#define FRAME_INTERVAL_DEFAULT_MS 33 /* ~30 fps starting point */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct wlrdp_server {
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;
    int ipc_fd;

    struct wlrdp_compositor comp;
    struct wlrdp_capture capture;
    struct wlrdp_input input;
    struct wlrdp_encoder encoder;
    freerdp_listener *listener;
    freerdp_peer *client;

    int epoll_fd;
    bool client_active;
    bool encoder_initialized;
    uint64_t last_frame_ms;
    uint32_t frame_interval_ms;
};

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGCHLD) {
        g_child_exited = 1;
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void init_encoder_for_client(struct wlrdp_server *srv)
{
    if (srv->encoder_initialized) {
        encoder_destroy(&srv->encoder);
        srv->encoder_initialized = false;
    }

    enum wlrdp_encoder_mode mode = WLRDP_ENCODER_NSC;

    if (srv->client && rdp_peer_supports_gfx_h264(srv->client)) {
        mode = WLRDP_ENCODER_H264;
    }

    if (!encoder_init(&srv->encoder, mode, srv->width, srv->height)) {
        WLRDP_LOG_ERROR("failed to initialize encoder");
        return;
    }

    srv->encoder_initialized = true;

    WLRDP_LOG_INFO("encoder mode: %s",
                    srv->encoder.mode == WLRDP_ENCODER_H264
                        ? "H.264" : "NSCodec");
}

static void on_frame_ready(void *data, uint8_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t stride)
{
    struct wlrdp_server *srv = data;

    if (!srv->client_active || !srv->client || !srv->encoder_initialized)
        return;

    uint64_t now = now_ms();
    if (now - srv->last_frame_ms < srv->frame_interval_ms) {
        capture_request_frame(&srv->capture);
        return;
    }

    uint64_t encode_start = now;

    /* For SurfaceBits mode, flip the image (screencopy is top-down,
     * SurfaceBits with codecID=0 expects bottom-up). */
    if (srv->encoder.mode == WLRDP_ENCODER_NSC) {
        uint32_t row_bytes = width * 4;
        for (uint32_t top = 0, bot = height - 1; top < bot; top++, bot--) {
            uint8_t *a = pixels + top * stride;
            uint8_t *b = pixels + bot * stride;
            for (uint32_t i = 0; i < row_bytes; i++) {
                uint8_t tmp = a[i];
                a[i] = b[i];
                b[i] = tmp;
            }
        }
    }

    if (!encoder_encode(&srv->encoder, pixels, stride)) {
        capture_request_frame(&srv->capture);
        return;
    }

    if (srv->encoder.out_len == 0) {
        /* Encoder buffering (shouldn't happen with zerolatency) */
        capture_request_frame(&srv->capture);
        return;
    }

    rdp_peer_send_frame(srv->client, srv->encoder.out_buf,
                        srv->encoder.out_len, width, height,
                        srv->encoder.is_keyframe);

    /* Adaptive framerate: target interval = 2x encode+send time,
     * clamped to [16ms, 100ms] */
    uint64_t encode_time = now_ms() - encode_start;
    uint32_t target = (uint32_t)(encode_time * 2);
    if (target < FRAME_INTERVAL_MIN_MS) target = FRAME_INTERVAL_MIN_MS;
    if (target > FRAME_INTERVAL_MAX_MS) target = FRAME_INTERVAL_MAX_MS;

    /* Smooth the interval to avoid jitter */
    srv->frame_interval_ms = (srv->frame_interval_ms * 3 + target) / 4;
    srv->last_frame_ms = now_ms();

    capture_request_frame(&srv->capture);
}

static void disconnect_client(struct wlrdp_server *srv)
{
    if (srv->client) {
        freerdp_peer_context_free(srv->client);
        freerdp_peer_free(srv->client);
        srv->client = NULL;
        srv->client_active = false;
        WLRDP_LOG_INFO("client disconnected");
    }

    if (srv->encoder_initialized) {
        encoder_destroy(&srv->encoder);
        srv->encoder_initialized = false;
    }

    if (srv->ipc_fd >= 0) {
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_DISCONNECTED;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv->ipc_fd, &msg, -1);
    }
}

static bool accept_ipc_client(struct wlrdp_server *srv)
{
    struct wlrdp_ipc_msg msg;
    int peer_fd = -1;

    if (!ipc_recv_msg(srv->ipc_fd, &msg, &peer_fd)) {
        WLRDP_LOG_ERROR("IPC recv failed — daemon gone?");
        g_running = 0;
        return false;
    }

    if (msg.type == WLRDP_MSG_SHUTDOWN) {
        WLRDP_LOG_INFO("received shutdown from daemon");
        g_running = 0;
        return false;
    }

    if (msg.type != WLRDP_MSG_NEW_CLIENT || peer_fd < 0) {
        WLRDP_LOG_WARN("unexpected IPC message type %u", msg.type);
        if (peer_fd >= 0) close(peer_fd);
        return false;
    }

    disconnect_client(srv);

    WLRDP_LOG_INFO("received new client fd %d from daemon", peer_fd);

    freerdp_peer *client = freerdp_peer_new(peer_fd);
    if (!client) {
        WLRDP_LOG_ERROR("freerdp_peer_new failed");
        close(peer_fd);
        return false;
    }

    client->ContextSize = sizeof(struct wlrdp_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return false;
    }

    if (!rdp_peer_init(client, srv->cert_file, srv->key_file, &srv->input)) {
        WLRDP_LOG_ERROR("rdp_peer_init failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return false;
    }

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return false;
    }

    srv->client = client;
    WLRDP_LOG_INFO("IPC client initialized");
    return true;
}

static BOOL on_peer_accepted(freerdp_listener *listener,
                             freerdp_peer *client)
{
    struct wlrdp_server *srv = listener->info;

    if (srv->client) {
        WLRDP_LOG_WARN("rejecting second client (single-session mode)");
        freerdp_peer_free(client);
        return TRUE;
    }

    client->ContextSize = sizeof(struct wlrdp_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return FALSE;
    }

    if (!rdp_peer_init(client, srv->cert_file, srv->key_file, &srv->input)) {
        WLRDP_LOG_ERROR("rdp_peer_init failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    srv->client = client;
    WLRDP_LOG_INFO("RDP client accepted");
    return TRUE;
}

static bool epoll_add_fd(int epoll_fd, int fd)
{
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static void generate_self_signed_cert(const char *cert_file,
                                      const char *key_file)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 "
             "-keyout '%s' -out '%s' "
             "-days 365 -nodes -subj '/CN=wlrdp' 2>/dev/null",
             key_file, cert_file);

    WLRDP_LOG_INFO("generating self-signed TLS certificate...");
    if (system(cmd) != 0) {
        WLRDP_LOG_ERROR("failed to generate certificate");
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT          RDP listen port (standalone mode, default: 3389)\n"
        "  --ipc-fd FD          IPC socket fd (daemon mode)\n"
        "  --width WIDTH        Display width (default: 800)\n"
        "  --height HEIGHT      Display height (default: 600)\n"
        "  --desktop-cmd CMD    Command to run inside cage (default: foot)\n"
        "  --cert FILE          TLS certificate file\n"
        "  --key FILE           TLS private key file\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    struct wlrdp_server srv = {
        .port = WLRDP_DEFAULT_PORT,
        .width = WLRDP_DEFAULT_WIDTH,
        .height = WLRDP_DEFAULT_HEIGHT,
        .desktop_cmd = "foot",
        .cert_file = NULL,
        .key_file = NULL,
        .ipc_fd = -1,
        .frame_interval_ms = FRAME_INTERVAL_DEFAULT_MS,
    };

    static struct option long_opts[] = {
        { "port",        required_argument, NULL, 'p' },
        { "ipc-fd",      required_argument, NULL, 'i' },
        { "width",       required_argument, NULL, 'W' },
        { "height",      required_argument, NULL, 'H' },
        { "desktop-cmd", required_argument, NULL, 'd' },
        { "cert",        required_argument, NULL, 'c' },
        { "key",         required_argument, NULL, 'k' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:i:W:H:d:c:k:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': srv.port = atoi(optarg); break;
        case 'i': srv.ipc_fd = atoi(optarg); break;
        case 'W': srv.width = atoi(optarg); break;
        case 'H': srv.height = atoi(optarg); break;
        case 'd': srv.desktop_cmd = optarg; break;
        case 'c': srv.cert_file = optarg; break;
        case 'k': srv.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    bool standalone = (srv.ipc_fd < 0);

    if (standalone) {
        WLRDP_LOG_INFO("running in standalone mode (no daemon)");
    } else {
        WLRDP_LOG_INFO("running in IPC mode (fd=%d)", srv.ipc_fd);
    }

    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (standalone && (!srv.cert_file || !srv.key_file)) {
        generate_self_signed_cert(auto_cert, auto_key);
        if (!srv.cert_file) srv.cert_file = auto_cert;
        if (!srv.key_file) srv.key_file = auto_key;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    WLRDP_LOG_INFO("launching compositor: cage -- %s (%dx%d)",
                    srv.desktop_cmd, srv.width, srv.height);
    if (!compositor_launch(&srv.comp, srv.desktop_cmd,
                           srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to launch compositor");
        return 1;
    }

    if (!capture_init(&srv.capture, srv.comp.display_name,
                      on_frame_ready, &srv)) {
        WLRDP_LOG_ERROR("failed to initialize capture");
        compositor_destroy(&srv.comp);
        return 1;
    }

    if (!input_init(&srv.input, srv.comp.display_name,
                    srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize input");
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    /* Encoder is initialized per-client after capability negotiation */

    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    int capture_fd = capture_get_fd(&srv.capture);
    epoll_add_fd(srv.epoll_fd, capture_fd);

    if (standalone) {
        srv.listener = freerdp_listener_new();
        if (!srv.listener) {
            WLRDP_LOG_ERROR("freerdp_listener_new failed");
            goto cleanup;
        }

        srv.listener->info = &srv;
        srv.listener->PeerAccepted = on_peer_accepted;

        if (!srv.listener->Open(srv.listener, "0.0.0.0", srv.port)) {
            WLRDP_LOG_ERROR("failed to listen on port %d", srv.port);
            goto cleanup;
        }

        WLRDP_LOG_INFO("listening on port %d", srv.port);

        HANDLE events[32];
        DWORD count = srv.listener->GetEventHandles(srv.listener, events, 32);
        for (DWORD i = 0; i < count; i++) {
            int fd = GetEventFileDescriptor(events[i]);
            if (fd >= 0) epoll_add_fd(srv.epoll_fd, fd);
        }
    } else {
        epoll_add_fd(srv.epoll_fd, srv.ipc_fd);

        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_READY;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv.ipc_fd, &msg, -1);
    }

    WLRDP_LOG_INFO("entering main event loop");

    while (g_running) {
        if (g_child_exited) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid == srv.comp.pid) {
                WLRDP_LOG_INFO("cage exited, shutting down");
                break;
            }
            g_child_exited = 0;
        }

        wl_display_flush(srv.capture.display);
        input_flush(&srv.input);

        struct epoll_event events[16];
        int nfds = epoll_wait(srv.epoll_fd, events, 16, 16);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            WLRDP_LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == capture_fd) {
                if (capture_dispatch(&srv.capture) < 0) {
                    WLRDP_LOG_ERROR("Wayland dispatch error");
                    g_running = 0;
                }
                continue;
            }

            if (!standalone && fd == srv.ipc_fd) {
                accept_ipc_client(&srv);
                continue;
            }

            if (standalone && srv.listener) {
                if (!srv.listener->CheckFileDescriptor(srv.listener)) {
                    WLRDP_LOG_ERROR("listener check failed");
                    g_running = 0;
                }
            }
        }

        if (srv.client) {
            struct wlrdp_peer_context *ctx =
                (struct wlrdp_peer_context *)srv.client->context;

            if (!srv.client->CheckFileDescriptor(srv.client)) {
                disconnect_client(&srv);
                continue;
            }

            if (ctx->activated && !srv.client_active) {
                srv.client_active = true;
                srv.frame_interval_ms = FRAME_INTERVAL_DEFAULT_MS;

                /* Now that capabilities are negotiated, init encoder */
                init_encoder_for_client(&srv);

                WLRDP_LOG_INFO("starting frame capture");
                capture_request_frame(&srv.capture);
            }
        }
    }

cleanup:
    WLRDP_LOG_INFO("shutting down");

    if (srv.client) {
        freerdp_peer_context_free(srv.client);
        freerdp_peer_free(srv.client);
    }
    if (srv.listener) {
        srv.listener->Close(srv.listener);
        freerdp_listener_free(srv.listener);
    }
    if (srv.ipc_fd >= 0) {
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_TERMINATED;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv.ipc_fd, &msg, -1);
        close(srv.ipc_fd);
    }
    if (srv.epoll_fd >= 0) {
        close(srv.epoll_fd);
    }
    if (srv.encoder_initialized) {
        encoder_destroy(&srv.encoder);
    }
    input_destroy(&srv.input);
    capture_destroy(&srv.capture);
    compositor_destroy(&srv.comp);

    WLRDP_LOG_INFO("bye");
    return 0;
}
```

- [ ] **Step 2: Verify it compiles**

```bash
meson compile -C build
```

Expected: Clean build with both H.264 and NSCodec paths.

- [ ] **Step 3: Commit**

```bash
git add src/session/main.c
git commit -m "feat: adaptive framerate and per-client encoder selection"
```

---

### Task 5: Build Verification and API Fixes

This task handles any compilation errors from the RDPGFX or FFmpeg API integration. FreeRDP's RDPGFX server API varies between 3.x patch versions, and FFmpeg's API has deprecation cycles.

**Files:**
- Modify: any files with compilation errors

- [ ] **Step 1: Attempt a clean build**

```bash
meson setup build --wipe -Denable-h264=auto && meson compile -C build 2>&1
```

If this succeeds with zero errors, skip to Step 4.

- [ ] **Step 2: Fix RDPGFX API issues**

Common issues to check:

```bash
# Check RDPGFX server API location and signatures
grep -r 'rdpgfx_server_context_new' /usr/include/freerdp3/ /usr/include/freerdp-server3/ /usr/include/winpr3/
grep -r 'RDPGFX_AVC420_BITMAP_STREAM' /usr/include/freerdp3/
grep -r 'RdpgfxServerContext' /usr/include/freerdp3/ /usr/include/freerdp-server3/
grep -r 'SupportGraphicsPipeline\|GfxH264' /usr/include/freerdp3/
```

Fix include paths and struct field names in `rdp_peer.c` as needed.

- [ ] **Step 3: Fix FFmpeg API issues**

Common issues to check:

```bash
# Check if AVCodecContext.frame_num exists (added in FFmpeg 6.1, replaces frame_number)
grep -r 'frame_num\|frame_number' /usr/include/libavcodec/
# Check AV_PIX_FMT_BGRA availability
grep -r 'AV_PIX_FMT_BGRA' /usr/include/libavutil/
```

If `frame_num` doesn't exist, use `frame_number` instead. If `AV_PIX_FMT_BGRA` is named differently, adjust the `sws_getContext` call.

- [ ] **Step 4: Rebuild and verify zero errors**

```bash
meson compile -C build
```

Expected: Clean build.

- [ ] **Step 5: Commit fixes (if any)**

```bash
git add -u
git commit -m "fix: adjust for FreeRDP RDPGFX and FFmpeg API compatibility"
```

Skip if no fixes were needed.

---

### Task 6: Integration Test

Test the complete H.264 pipeline end-to-end.

- [ ] **Step 1: Test NSCodec fallback (no FFmpeg or H.264-disabled build)**

```bash
meson setup build --wipe -Denable-h264=disabled && meson compile -C build
./build/src/session/wlrdp-session --port 3389 --desktop-cmd foot --width 800 --height 600
```

From another terminal:
```bash
xfreerdp /v:localhost:3389 /cert:ignore /w:800 /h:600
```

Expected: Same behavior as Phase 2 — foot terminal visible, keyboard and mouse work. Logs show "encoder mode: NSCodec".

- [ ] **Step 2: Test H.264 path**

```bash
meson setup build --wipe -Denable-h264=auto && meson compile -C build
./build/src/session/wlrdp-session --port 3389 --desktop-cmd foot --width 800 --height 600
```

From another terminal:
```bash
xfreerdp /v:localhost:3389 /cert:ignore /w:800 /h:600 /gfx:AVC420
```

Expected: Logs show "encoder mode: H.264" and "send_mode=GFX_AVC420". Display should be visible with H.264 compression. Visual quality should be good at the default bitrate.

- [ ] **Step 3: Test adaptive framerate**

While connected via H.264, watch the logs for frame interval adjustments. Move windows around or type rapidly to generate screen changes.

Expected: Frame interval adapts — shorter when encode is fast, longer when encode is slow. No visible stutter or frame drops.

- [ ] **Step 4: Test client without GFX support**

```bash
xfreerdp /v:localhost:3389 /cert:ignore /w:800 /h:600 -gfx
```

Expected: Falls back to SurfaceBits/NSCodec automatically. Logs show "send_mode=SurfaceBits".

- [ ] **Step 5: Test disconnect and reconnect**

Disconnect the client, then reconnect. Expected: Encoder is re-initialized for the new client's capabilities. Session persists.

- [ ] **Step 6: Commit any final adjustments**

```bash
git add -u
git commit -m "fix: integration test adjustments for Phase 3"
```

Skip if no changes needed.
