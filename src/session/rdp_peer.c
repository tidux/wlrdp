#include "rdp_peer.h"
#include "input.h"
#include "clipboard.h"
#include "common.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/color.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/codec/audio.h>
#include <freerdp/pointer.h>
#include <winpr/wtsapi.h>
#include <linux/input-event-codes.h>

/* Forward declarations */
static void gfx_cleanup(struct wlrdp_peer_context *ctx);
static bool gfx_create_surface(struct wlrdp_peer_context *ctx,
                                uint32_t width, uint32_t height);

/* --- GFX channel callbacks --- */

static UINT gfx_caps_advertise(RdpgfxServerContext *context,
                                const RDPGFX_CAPS_ADVERTISE_PDU *pdu)
{
    freerdp_peer *client = context->rdpcontext->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    /* Scan capability sets for H.264 support.
     * Prefer AVC444v2 > AVC444 > AVC420 > no-AVC fallback.
     * AVC444 requires caps version >= 10 with AVC not disabled. */
    int best_rank = -1;
    uint32_t chosen = 0;

    for (uint32_t i = 0; i < pdu->capsSetCount; i++) {
        const RDPGFX_CAPSET *cap = &pdu->capsSets[i];
        int rank = 0;  /* 0 = no AVC, 1 = AVC420, 2 = AVC444, 3 = AVC444v2 */

        if (cap->version >= RDPGFX_CAPVERSION_10) {
            if (!(cap->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED)) {
                rank = 2;  /* AVC420 + AVC444 available at v10+ */
                if (cap->version >= RDPGFX_CAPVERSION_107) {
                    rank = 3;  /* AVC444v2 at 10.7+ */
                }
            }
        } else if (cap->version >= RDPGFX_CAPVERSION_81 &&
                   (cap->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED)) {
            rank = 1;  /* AVC420 only */
        }

        if (rank > best_rank) {
            best_rank = rank;
            chosen = i;
        }
    }

    /* If no caps at all, pick the first one */
    if (best_rank < 0 && pdu->capsSetCount > 0) {
        chosen = 0;
    }

    RDPGFX_CAPS_CONFIRM_PDU confirm = {
        .capsSet = &pdu->capsSets[chosen],
    };

    switch (best_rank) {
    case 3:
        ctx->send_mode = WLRDP_SEND_GFX_AVC444V2;
        WLRDP_LOG_INFO("GFX: client supports AVC444v2 (caps version 0x%08x)",
                        pdu->capsSets[chosen].version);
        break;
    case 2:
        ctx->send_mode = WLRDP_SEND_GFX_AVC444;
        WLRDP_LOG_INFO("GFX: client supports AVC444 (caps version 0x%08x)",
                        pdu->capsSets[chosen].version);
        break;
    case 1:
        ctx->send_mode = WLRDP_SEND_GFX_AVC420;
        WLRDP_LOG_INFO("GFX: client supports AVC420 (caps version 0x%08x)",
                        pdu->capsSets[chosen].version);
        break;
    default:
        ctx->send_mode = WLRDP_SEND_SURFACE_BITS;
        WLRDP_LOG_INFO("GFX: client does not support AVC, "
                        "falling back to SurfaceBits");
        break;
    }

    UINT rc = context->CapsConfirm(context, &confirm);
    if (rc != CHANNEL_RC_OK)
        return rc;

    /* Caps negotiation complete — now create the GFX surface.
     * Use compositor dimensions (ctx->width/height), not the client's
     * negotiated DesktopWidth/Height which may differ. */
    if (ctx->activated && !ctx->gfx_surface_id) {
        uint32_t w = ctx->width;
        uint32_t h = ctx->height;
        if (gfx_create_surface(ctx, w, h)) {
            ctx->gfx_ready = true;
            WLRDP_LOG_INFO("GFX pipeline ready after caps negotiation");
        } else {
            ctx->send_mode = WLRDP_SEND_SURFACE_BITS;
            gfx_cleanup(ctx);
        }
    }

    return CHANNEL_RC_OK;
}

/*
 * Phase 1: Create the VCM and open the DRDYNVC transport.
 * Called from on_post_connect. The GFX channel is NOT opened yet —
 * that happens in rdp_peer_check_vcm once DRDYNVC is ready.
 */
static bool vcm_init(struct wlrdp_peer_context *ctx)
{
    /* Register FreeRDP's WTS API so WTSOpenServerA can create a VCM
     * from a peer context (without this, it tries FreeRDS IPC). */
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());

    HANDLE vcm = WTSOpenServerA((LPSTR)&ctx->base);
    if (!vcm || vcm == INVALID_HANDLE_VALUE) {
        WLRDP_LOG_WARN("failed to open WTS virtual channel manager");
        return false;
    }

    ctx->gfx_vcm = vcm;

    /* Initialize DRDYNVC transport — this starts the async handshake */
    if (!WTSVirtualChannelManagerOpen(vcm)) {
        WLRDP_LOG_WARN("WTSVirtualChannelManagerOpen failed");
        WTSCloseServer(vcm);
        ctx->gfx_vcm = NULL;
        return false;
    }

    WLRDP_LOG_INFO("VCM opened, waiting for DRDYNVC handshake");
    return true;
}

/*
 * Phase 2: Open the GFX channel once DRDYNVC is ready.
 */
static bool gfx_open(struct wlrdp_peer_context *ctx)
{
    HANDLE vcm = ctx->gfx_vcm;

    RdpgfxServerContext *gfx = rdpgfx_server_context_new(vcm);
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

    if (!gfx->Open(gfx)) {
        WLRDP_LOG_WARN("RDPGFX channel Open failed");
        rdpgfx_server_context_free(gfx);
        ctx->gfx_context = NULL;
        return false;
    }

    ctx->gfx_opened = true;
    WLRDP_LOG_INFO("RDPGFX channel opened");
    return true;
}

bool gfx_create_surface(struct wlrdp_peer_context *ctx,
                                uint32_t width, uint32_t height)
{
    RdpgfxServerContext *gfx = ctx->gfx_context;

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

    UINT rc = gfx->ResetGraphics(gfx, &reset);
    if (rc != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("GFX ResetGraphics failed (rc=%u), continuing anyway", rc);
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

    if (ctx->rdpsnd_context) {
        RdpsndServerContext *rdpsnd = ctx->rdpsnd_context;
        if (ctx->rdpsnd_opened) {
            rdpsnd->Stop(rdpsnd);
        }
        rdpsnd_server_context_free(rdpsnd);
        ctx->rdpsnd_context = NULL;
        ctx->rdpsnd_opened = false;
        ctx->rdpsnd_ready = false;
    }

    if (ctx->cliprdr_opened && ctx->clipboard) {
        clipboard_close_cliprdr(ctx->clipboard);
        ctx->cliprdr_context = NULL;
        ctx->cliprdr_opened = false;
    }

    if (ctx->disp_context) {
        DispServerContext *disp = ctx->disp_context;
        if (ctx->disp_opened) {
            disp->Close(disp);
        }
        disp_server_context_free(disp);
        ctx->disp_context = NULL;
    }

    if (ctx->gfx_vcm) {
        WTSCloseServer(ctx->gfx_vcm);
        ctx->gfx_vcm = NULL;
    }
    ctx->gfx_opened = false;
    ctx->gfx_ready = false;
    ctx->gfx_surface_id = 0;
    ctx->disp_opened = false;
}

/* --- DISP channel callbacks --- */

static UINT on_disp_monitor_layout(DispServerContext *context,
                                   const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu)
{
    freerdp_peer *client = context->rdpcontext->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (pdu->NumMonitors < 1) return CHANNEL_RC_OK;

    const DISPLAY_CONTROL_MONITOR_LAYOUT *mon = &pdu->Monitors[0];
    uint32_t width = mon->Width;
    uint32_t height = mon->Height;
    uint32_t scale = mon->DesktopScaleFactor;
    if (scale == 0) scale = 100;

    WLRDP_LOG_INFO("DISP: client requested resize to %ux%u (scale %u%%)",
                    width, height, scale);

    if (ctx->on_resize) {
        ctx->on_resize(ctx->on_resize_data, width, height, scale);
    }

    return CHANNEL_RC_OK;
}

static bool disp_open(struct wlrdp_peer_context *ctx)
{
    DispServerContext *disp = disp_server_context_new(ctx->gfx_vcm);
    if (!disp) {
        WLRDP_LOG_WARN("failed to create DISP server context");
        return false;
    }

    disp->rdpcontext = &ctx->base;
    disp->DispMonitorLayout = on_disp_monitor_layout;

    if (!disp->Open(disp)) {
        WLRDP_LOG_WARN("DISP channel Open failed");
        disp_server_context_free(disp);
        return false;
    }

    ctx->disp_context = disp;
    ctx->disp_opened = true;
    WLRDP_LOG_INFO("DISP channel opened");
    return true;
}

/* --- RDPSND channel --- */

static void on_rdpsnd_activated(RdpsndServerContext *context)
{
    freerdp_peer *client = context->rdpcontext->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    /* Find a PCM format the client supports */
    bool found = false;
    for (UINT16 i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT *fmt = &context->client_formats[i];
        if (fmt->wFormatTag == WAVE_FORMAT_PCM &&
            fmt->nChannels == 2 &&
            fmt->nSamplesPerSec == 44100 &&
            fmt->wBitsPerSample == 16) {
            context->SelectFormat(context, i);
            found = true;
            WLRDP_LOG_INFO("RDPSND: selected client format %u (PCM 44100/16/2)", i);
            break;
        }
    }

    if (!found) {
        WLRDP_LOG_WARN("RDPSND: no compatible PCM format found");
        return;
    }

    ctx->rdpsnd_ready = true;
    WLRDP_LOG_INFO("RDPSND: activated and ready");
}

static bool rdpsnd_open(struct wlrdp_peer_context *ctx)
{
    RdpsndServerContext *rdpsnd = rdpsnd_server_context_new(ctx->gfx_vcm);
    if (!rdpsnd) {
        WLRDP_LOG_WARN("failed to create RDPSND server context");
        return false;
    }

    rdpsnd->rdpcontext = &ctx->base;
    rdpsnd->use_dynamic_virtual_channel = TRUE;

    /* Advertise PCM S16LE 44100 Hz stereo */
    static AUDIO_FORMAT server_formats[] = {
        {
            .wFormatTag = WAVE_FORMAT_PCM,
            .nChannels = 2,
            .nSamplesPerSec = 44100,
            .nAvgBytesPerSec = 44100 * 2 * 2,
            .nBlockAlign = 4,
            .wBitsPerSample = 16,
            .cbSize = 0,
            .data = NULL,
        },
    };
    rdpsnd->server_formats = server_formats;
    rdpsnd->num_server_formats = 1;

    static AUDIO_FORMAT src_format = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = 2,
        .nSamplesPerSec = 44100,
        .nAvgBytesPerSec = 44100 * 2 * 2,
        .nBlockAlign = 4,
        .wBitsPerSample = 16,
        .cbSize = 0,
        .data = NULL,
    };
    rdpsnd->src_format = &src_format;
    rdpsnd->latency = 50; /* 50ms buffer */

    rdpsnd->Activated = on_rdpsnd_activated;

    if (rdpsnd->Initialize(rdpsnd, FALSE) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("RDPSND Initialize failed");
        rdpsnd_server_context_free(rdpsnd);
        return false;
    }

    if (rdpsnd->Start(rdpsnd) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("RDPSND Start failed");
        rdpsnd_server_context_free(rdpsnd);
        return false;
    }

    ctx->rdpsnd_context = rdpsnd;
    ctx->rdpsnd_opened = true;
    WLRDP_LOG_INFO("RDPSND channel opened");
    return true;
}

/* --- RDP input callbacks --- */

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

static uint32_t get_desktop_scale(rdpSettings *settings)
{
    uint32_t scale = freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
    if (scale == 0) scale = 100;
    return scale;
}

static BOOL on_desktop_resize(rdpContext *context)
{
    freerdp_peer *client = context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    rdpSettings *settings = client->context->settings;

    uint32_t width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    uint32_t height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    uint32_t scale = get_desktop_scale(settings);

    WLRDP_LOG_INFO("client requested desktop resize to %ux%u (scale %u%%)",
                    width, height, scale);

    if (ctx->on_resize) {
        ctx->on_resize(ctx->on_resize_data, width, height, scale);
    }

    return TRUE;
}

static BOOL on_remote_monitors(rdpContext *context, UINT32 count,
                               const MONITOR_DEF *monitors)
{
    freerdp_peer *client = context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    rdpSettings *settings = client->context->settings;

    if (count < 1) return TRUE;

    uint32_t width = monitors[0].right - monitors[0].left + 1;
    uint32_t height = monitors[0].bottom - monitors[0].top + 1;
    uint32_t scale = get_desktop_scale(settings);

    WLRDP_LOG_INFO("client requested remote monitors change to %ux%u (scale %u%%)",
                    width, height, scale);

    if (ctx->on_resize) {
        ctx->on_resize(ctx->on_resize_data, width, height, scale);
    }

    return TRUE;
}

static BOOL on_post_connect(freerdp_peer *client)
{
    rdpSettings *settings = client->context->settings;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    WLRDP_LOG_INFO("RDP client connected: %ux%u %ubpp",
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
                    freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth));

    WLRDP_LOG_INFO("client scale factors: desktop=%u, device=%u",
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor),
                    freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor));

    WLRDP_LOG_INFO("negotiated: NSCodec=%d SurfaceCommands=%d GfxH264=%d",
        freerdp_settings_get_bool(settings, FreeRDP_NSCodec),
        freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled),
        freerdp_settings_get_bool(settings, FreeRDP_GfxH264));

    uint32_t depth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
    switch (depth) {
    case 32: ctx->pixel_format = PIXEL_FORMAT_BGRX32; break;
    case 24: ctx->pixel_format = PIXEL_FORMAT_BGR24; break;
    case 16: ctx->pixel_format = PIXEL_FORMAT_RGB16; break;
    default: ctx->pixel_format = PIXEL_FORMAT_BGRX32; break;
    }
    WLRDP_LOG_INFO("negotiated pixel format: 0x%08x (%u bpp)", ctx->pixel_format, depth);

    /* Start VCM + DRDYNVC handshake — GFX opens later via rdp_peer_check_vcm */
    if (freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline)) {
        vcm_init(ctx);
    }

    return TRUE;
}

static BOOL on_activate(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    rdpSettings *settings = client->context->settings;

    ctx->activated = true;

    uint32_t client_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    uint32_t client_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    uint32_t scale = get_desktop_scale(settings);

    const char *mode_str;
    switch (ctx->send_mode) {
    case WLRDP_SEND_GFX_AVC444V2: mode_str = "GFX_AVC444v2"; break;
    case WLRDP_SEND_GFX_AVC444:   mode_str = "GFX_AVC444"; break;
    case WLRDP_SEND_GFX_AVC420:   mode_str = "GFX_AVC420"; break;
    default:                      mode_str = "SurfaceBits"; break;
    }

    WLRDP_LOG_INFO("RDP peer activated (send_mode=%s, res=%ux%u, scale=%u%%)",
                    mode_str, client_width, client_height, scale);

    /* Hide the client's local cursor. The server cursor is composited
     * into the frame by cage, so the client must not draw its own. */
    rdpPointerUpdate *pointer = client->context->update->pointer;
    POINTER_SYSTEM_UPDATE system_pointer = { .type = SYSPTR_NULL };
    pointer->PointerSystem(client->context, &system_pointer);

    /* Adopt client resolution if different from our current resolution */
    if (client_width != ctx->width || client_height != ctx->height) {
        WLRDP_LOG_INFO("adopting client resolution on activation: %ux%u (scale %u%%)",
                        client_width, client_height, scale);
        if (ctx->on_resize) {
            ctx->on_resize(ctx->on_resize_data, client_width, client_height, scale);
        }
    }

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

    /* Enable NSCodec for SurfaceBits fallback path */
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodecAllowSubsampling, TRUE);

    /* Advertise GFX pipeline support */
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE);

    /* Dynamic Resolution / Monitor Layout */
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE);

    /* Pointer settings */
    freerdp_settings_set_uint32(settings, FreeRDP_LargePointerFlag, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SuppressOutput, TRUE);

    client->PostConnect = on_post_connect;
    client->Activate = on_activate;
    client->context->update->DesktopResize = on_desktop_resize;
    client->context->update->RemoteMonitors = on_remote_monitors;
    client->context->input->KeyboardEvent = on_keyboard_event;
    client->context->input->MouseEvent = on_mouse_event;
    client->context->input->ExtendedMouseEvent = on_extended_mouse_event;

    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    ctx->input = input;
    ctx->send_mode = WLRDP_SEND_SURFACE_BITS;

    return true;
}

bool rdp_peer_supports_gfx_h264(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    return ctx->gfx_ready && ctx->send_mode == WLRDP_SEND_GFX_AVC420;
}

enum wlrdp_send_mode rdp_peer_get_send_mode(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    if (!ctx->gfx_ready)
        return WLRDP_SEND_SURFACE_BITS;
    return ctx->send_mode;
}

int rdp_peer_get_vcm_fd(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->gfx_vcm)
        return -1;

    HANDLE event = WTSVirtualChannelManagerGetEventHandle(ctx->gfx_vcm);
    if (!event || event == INVALID_HANDLE_VALUE)
        return -1;

    return GetEventFileDescriptor(event);
}

bool rdp_peer_check_vcm(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->gfx_vcm)
        return true;

    /* Already opened (or fully set up) — just pump the VCM */
    if (ctx->gfx_opened || ctx->gfx_ready)
        return WTSVirtualChannelManagerCheckFileDescriptor(ctx->gfx_vcm);

    if (!WTSVirtualChannelManagerCheckFileDescriptor(ctx->gfx_vcm)) {
        WLRDP_LOG_WARN("VCM check failed");
        gfx_cleanup(ctx);
        return true; /* non-fatal: fall back to SurfaceBits */
    }

    BYTE state = WTSVirtualChannelManagerGetDrdynvcState(ctx->gfx_vcm);

    if (state == DRDYNVC_STATE_FAILED) {
        WLRDP_LOG_WARN("DRDYNVC handshake failed, falling back to SurfaceBits");
        gfx_cleanup(ctx);
        return true;
    }

    if (state != DRDYNVC_STATE_READY)
        return true; /* still handshaking */

    /* DRDYNVC is ready — open the channels */
    WLRDP_LOG_INFO("DRDYNVC ready, opening dynamic channels");

    if (!gfx_open(ctx)) {
        WLRDP_LOG_WARN("GFX open failed, falling back to SurfaceBits");
    }

    if (!disp_open(ctx)) {
        WLRDP_LOG_WARN("DISP open failed, dynamic resizing may not work");
    }

    if (ctx->clipboard) {
        if (clipboard_open_cliprdr(ctx->clipboard, ctx->gfx_vcm)) {
            ctx->cliprdr_context = ctx->clipboard->cliprdr_context;
            ctx->cliprdr_opened = true;
        } else {
            WLRDP_LOG_WARN("CLIPRDR open failed, clipboard will not work");
        }
    }

    if (!rdpsnd_open(ctx)) {
        WLRDP_LOG_WARN("RDPSND open failed, audio will not work");
    }

    /* Surface creation is deferred until gfx_caps_advertise completes */
    return true;
}

static bool send_gfx_avc420(struct wlrdp_peer_context *ctx,
                             uint8_t *data, uint32_t len,
                             uint32_t width, uint32_t height)
{
    RdpgfxServerContext *gfx = ctx->gfx_context;

    RECTANGLE_16 region_rect = {
        .left = 0, .top = 0,
        .right = (UINT16)width, .bottom = (UINT16)height,
    };

    RDPGFX_H264_QUANT_QUALITY quant_qual = {
        .qp = 22,
        .qualityVal = 100,
        .qpVal = 22,
    };

    RDPGFX_AVC420_BITMAP_STREAM avc420 = {
        .meta = {
            .numRegionRects = 1,
            .regionRects = &region_rect,
            .quantQualityVals = &quant_qual,
        },
        .length = len,
        .data = data,
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
        .format = ctx->pixel_format,
        .left = 0,
        .top = 0,
        .right = width,
        .bottom = height,
        .length = len,
        .data = data,
        .extra = &avc420,
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

static bool send_gfx_avc444(struct wlrdp_peer_context *ctx,
                             uint8_t *main_data, uint32_t main_len,
                             uint8_t *aux_data, uint32_t aux_len,
                             uint32_t width, uint32_t height,
                             bool is_avc444v2)
{
    RdpgfxServerContext *gfx = ctx->gfx_context;

    RECTANGLE_16 region_rect = {
        .left = 0, .top = 0,
        .right = (UINT16)width, .bottom = (UINT16)height,
    };

    RDPGFX_H264_QUANT_QUALITY quant_qual = {
        .qp = 22,
        .qualityVal = 100,
        .qpVal = 22,
    };

    /* LC per MS-RDPEGFX: 0 = both luma+chroma streams, 1 = luma only, 2 = chroma only */
    BYTE lc;
    if (main_len > 0 && aux_len > 0) lc = 0;
    else if (main_len > 0)           lc = 1;
    else                             lc = 2;

    /* cbAvc420EncodedBitstream1 = metablock1 bytes + H.264 stream 1 bytes.
     * Metablock size = 4 (numRegionRects) + numRegionRects * (8 rect + 2 qp/qual). */
    const uint32_t metablock1_size = 4 + 1 * 10;

    RDPGFX_AVC444_BITMAP_STREAM avc444 = {
        .cbAvc420EncodedBitstream1 = metablock1_size + main_len,
        .LC = lc,
        .bitstream = {
            [0] = {
                .meta = {
                    .numRegionRects = 1,
                    .regionRects = &region_rect,
                    .quantQualityVals = &quant_qual,
                },
                .length = main_len,
                .data = main_data,
            },
            [1] = {
                .meta = {
                    .numRegionRects = 1,
                    .regionRects = &region_rect,
                    .quantQualityVals = &quant_qual,
                },
                .length = aux_len,
                .data = aux_data,
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
        .codecId = is_avc444v2 ? RDPGFX_CODECID_AVC444v2
                               : RDPGFX_CODECID_AVC444,
        .format = ctx->pixel_format,
        .left = 0,
        .top = 0,
        .right = width,
        .bottom = height,
        .length = main_len + aux_len,
        .data = main_data,
        .extra = &avc444,
    };

    if (gfx->SurfaceCommand(gfx, &cmd) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("GFX SurfaceCommand (AVC444) failed");
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
    rdpSettings *settings = client->context->settings;
    struct wlrdp_peer_context *ctx = (struct wlrdp_peer_context *)client->context;

    SURFACE_BITS_COMMAND cmd = { 0 };
    cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
    cmd.destLeft = 0;
    cmd.destTop = 0;
    cmd.destRight = width;
    cmd.destBottom = height;
    cmd.bmp.bpp = FreeRDPGetBitsPerPixel(ctx->pixel_format);
    cmd.bmp.width = width;
    cmd.bmp.height = height;

    if (freerdp_settings_get_bool(settings, FreeRDP_NSCodec)) {
        cmd.bmp.codecID = RDP_CODEC_ID_NSCODEC;
    } else {
        cmd.bmp.codecID = RDP_CODEC_ID_NONE;
    }

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
                         uint8_t *aux_data, uint32_t aux_len,
                         uint32_t width, uint32_t height,
                         bool is_keyframe)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    (void)is_keyframe; /* AVC420 metadata carries QP, not per-frame keyframe flag */

    if (ctx->gfx_ready) {
        switch (ctx->send_mode) {
        case WLRDP_SEND_GFX_AVC444V2:
            return send_gfx_avc444(ctx, data, len, aux_data, aux_len,
                                   width, height, true);
        case WLRDP_SEND_GFX_AVC444:
            return send_gfx_avc444(ctx, data, len, aux_data, aux_len,
                                   width, height, false);
        case WLRDP_SEND_GFX_AVC420:
            return send_gfx_avc420(ctx, data, len, width, height);
        default:
            break;
        }
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

void rdp_peer_update_size(freerdp_peer *client, uint32_t width, uint32_t height)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    ctx->width = width;
    ctx->height = height;

    if (ctx->gfx_ready && ctx->gfx_context) {
        gfx_create_surface(ctx, width, height);
    }
}

bool rdp_peer_send_audio(freerdp_peer *client, const int16_t *samples,
                         uint32_t n_frames)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->rdpsnd_ready || !ctx->rdpsnd_context) return false;

    RdpsndServerContext *rdpsnd = ctx->rdpsnd_context;

    /* Process any pending RDPSND messages */
    rdpsnd_server_handle_messages(rdpsnd);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    UINT16 timestamp = (UINT16)((ts.tv_sec * 1000 + ts.tv_nsec / 1000000) & 0xFFFF);

    UINT rc = rdpsnd->SendSamples(rdpsnd, samples, n_frames, timestamp);
    return (rc == CHANNEL_RC_OK);
}
