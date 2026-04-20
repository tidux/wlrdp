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
#include <freerdp/primitives.h>
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

static bool h264_try_encoder(struct wlrdp_encoder *enc, const char *name,
                             struct h264_ctx *hctx)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
        WLRDP_LOG_INFO("H.264 encoder '%s' not found", name);
        return false;
    }

    /* Many H.264 encoders (especially libx264 and hardware ones)
     * require even dimensions for YUV420. */
    uint32_t enc_width = enc->width & ~1;
    uint32_t enc_height = enc->height & ~1;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    ctx->width = enc_width;
    ctx->height = enc_height;
    ctx->time_base = (AVRational){1, 30};
    ctx->framerate = (AVRational){30, 1};
    ctx->gop_size = 60;        /* keyframe every 2 seconds at 30fps */
    ctx->max_b_frames = 0;     /* RDP doesn't benefit from B-frames */
    ctx->bit_rate = enc_width * enc_height * 2; /* ~2 bits/pixel */
    ctx->thread_count = 1;     /* single-threaded for low latency */

    if (strcmp(name, "h264_vaapi") == 0) {
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    } else {
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    /* Apply encoder-specific low-latency tuning */
    if (strcmp(name, "libx264") == 0) {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "profile", "baseline", 0);
    } else if (strcmp(name, "h264_nvenc") == 0) {
        av_opt_set(ctx->priv_data, "preset", "p1", 0); /* p1 = fastest */
        av_opt_set(ctx->priv_data, "tune", "ull", 0);  /* ull = ultra low latency */
        av_opt_set(ctx->priv_data, "delay", "0", 0);
    } else if (strcmp(name, "h264_vaapi") == 0) {
        /* VAAPI often uses its own profile/entrypoint settings */
    }

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        WLRDP_LOG_INFO("H.264 encoder '%s' failed to open", name);
        avcodec_free_context(&ctx);
        return false;
    }

    /* VAAPI requires hardware frame allocation, which we don't support yet.
     * For now, if vaapi was selected, we fail and try the next one. */
    if (ctx->pix_fmt == AV_PIX_FMT_VAAPI) {
        WLRDP_LOG_INFO("H.264 encoder '%s' requires VAAPI hardware frames (unsupported)", name);
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
        ctx->width, ctx->height, ctx->pix_fmt,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }

    hctx->codec_ctx = ctx;
    hctx->frame = frame;
    hctx->packet = pkt;
    hctx->sws_ctx = sws;

    WLRDP_LOG_INFO("H.264 encoder '%s' opened (%ux%u -> %ux%u)",
                    name, enc->width, enc->height, ctx->width, ctx->height);
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
        if (h264_try_encoder(enc, encoders[i], &enc->h264[0])) {
            enc->mode = WLRDP_ENCODER_H264;
            WLRDP_LOG_INFO("encoder initialized (%ux%u, H.264 via %s)",
                            enc->width, enc->height, encoders[i]);
            return true;
        }
    }

    WLRDP_LOG_WARN("no H.264 encoder available, falling back to NSCodec");
    return false;
}

/*
 * Encode a frame using a single H.264 context. The caller must have already
 * filled hctx->frame->data with YUV420P planes. Sets *out and *out_len to
 * the encoded NAL units. Returns true on success.
 */
static bool h264_ctx_encode(struct h264_ctx *hctx, int64_t pts,
                            uint8_t **out, uint32_t *out_len, bool *keyframe)
{
    AVCodecContext *ctx = hctx->codec_ctx;
    AVFrame *frame = hctx->frame;
    AVPacket *pkt = hctx->packet;

    frame->pts = pts;

    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        WLRDP_LOG_ERROR("avcodec_send_frame failed: %d", ret);
        return false;
    }

    ret = avcodec_receive_packet(ctx, pkt);
    if (ret == AVERROR(EAGAIN)) {
        *out_len = 0;
        return true;
    }
    if (ret < 0) {
        WLRDP_LOG_ERROR("avcodec_receive_packet failed: %d", ret);
        return false;
    }

    *out = pkt->data;
    *out_len = pkt->size;
    *keyframe = !!(pkt->flags & AV_PKT_FLAG_KEY);
    return true;
}

static bool h264_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                        uint32_t stride)
{
    struct h264_ctx *hctx = &enc->h264[0];
    AVCodecContext *ctx = hctx->codec_ctx;
    AVFrame *frame = hctx->frame;
    struct SwsContext *sws = hctx->sws_ctx;

    /* Convert BGRX -> YUV420P */
    const uint8_t *src_data[1] = { pixels };
    int src_linesize[1] = { (int)stride };

    if (av_frame_make_writable(frame) < 0) {
        WLRDP_LOG_ERROR("av_frame_make_writable failed");
        return false;
    }

    sws_scale(sws, src_data, src_linesize, 0, enc->height,
              frame->data, frame->linesize);

    return h264_ctx_encode(hctx, ctx->frame_num,
                           &enc->out_buf, &enc->out_len, &enc->is_keyframe);
}

static void h264_ctx_cleanup(struct h264_ctx *hctx)
{
    if (hctx->sws_ctx) {
        sws_freeContext(hctx->sws_ctx);
        hctx->sws_ctx = NULL;
    }
    if (hctx->packet) {
        av_packet_free((AVPacket **)&hctx->packet);
    }
    if (hctx->frame) {
        av_frame_free((AVFrame **)&hctx->frame);
    }
    if (hctx->codec_ctx) {
        avcodec_free_context((AVCodecContext **)&hctx->codec_ctx);
    }
}

static void h264_cleanup(struct wlrdp_encoder *enc)
{
    h264_ctx_cleanup(&enc->h264[0]);
    h264_ctx_cleanup(&enc->h264[1]);
}

static void avc444_cleanup_buffers(struct wlrdp_encoder *enc)
{
    for (int i = 0; i < 3; i++) {
        free(enc->yuv_main[i]);
        enc->yuv_main[i] = NULL;
        free(enc->yuv_aux[i]);
        enc->yuv_aux[i] = NULL;
    }
}

static bool avc444_init(struct wlrdp_encoder *enc)
{
    /* Both streams use the same H.264 encoder.
     * Find a working encoder name first, then open two instances. */
    static const char *encoders[] = {
        "h264_vaapi",
        "h264_nvenc",
        "libx264",
        NULL,
    };

    const char *chosen = NULL;
    for (int i = 0; encoders[i]; i++) {
        if (h264_try_encoder(enc, encoders[i], &enc->h264[0])) {
            chosen = encoders[i];
            break;
        }
    }

    if (!chosen) {
        WLRDP_LOG_WARN("no H.264 encoder for AVC444 main stream");
        return false;
    }

    /* Open a second encoder instance for the aux/chroma stream */
    if (!h264_try_encoder(enc, chosen, &enc->h264[1])) {
        WLRDP_LOG_WARN("failed to open second H.264 encoder for AVC444 aux stream");
        h264_ctx_cleanup(&enc->h264[0]);
        return false;
    }

    /* Allocate YUV plane buffers for RGBToAVC444YUV output.
     * Y plane: full resolution. U,V planes: half resolution (YUV420). */
    uint32_t w = enc->width & ~1;
    uint32_t h = enc->height & ~1;
    uint32_t y_stride = w;
    uint32_t uv_stride = w / 2;

    enc->yuv_stride[0] = y_stride;
    enc->yuv_stride[1] = uv_stride;
    enc->yuv_stride[2] = uv_stride;

    for (int i = 0; i < 3; i++) {
        uint32_t plane_stride = enc->yuv_stride[i];
        uint32_t plane_height = (i == 0) ? h : h / 2;
        size_t plane_size = (size_t)plane_stride * plane_height;

        enc->yuv_main[i] = calloc(1, plane_size);
        enc->yuv_aux[i] = calloc(1, plane_size);

        if (!enc->yuv_main[i] || !enc->yuv_aux[i]) {
            WLRDP_LOG_ERROR("failed to allocate AVC444 YUV buffers");
            avc444_cleanup_buffers(enc);
            h264_ctx_cleanup(&enc->h264[0]);
            h264_ctx_cleanup(&enc->h264[1]);
            return false;
        }
    }

    enc->mode = WLRDP_ENCODER_AVC444;
    WLRDP_LOG_INFO("encoder initialized (%ux%u, AVC444%s via %s)",
                    enc->width, enc->height,
                    enc->avc444v2 ? "v2" : "", chosen);
    return true;
}

static bool avc444_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                          uint32_t stride)
{
    primitives_t *prims = primitives_get();
    uint32_t w = enc->width & ~1;
    uint32_t h = enc->height & ~1;

    prim_size_t roi = { .width = w, .height = h };
    UINT32 main_step[3] = { enc->yuv_stride[0], enc->yuv_stride[1], enc->yuv_stride[2] };
    UINT32 aux_step[3]  = { enc->yuv_stride[0], enc->yuv_stride[1], enc->yuv_stride[2] };

    /* Split BGRX into main (base YUV420) + aux (chroma refinement YUV420) */
    pstatus_t prc;
    if (enc->avc444v2) {
        prc = prims->RGBToAVC444YUVv2(pixels, PIXEL_FORMAT_BGRX32, stride,
                                       enc->yuv_main, main_step,
                                       enc->yuv_aux, aux_step, &roi);
    } else {
        prc = prims->RGBToAVC444YUV(pixels, PIXEL_FORMAT_BGRX32, stride,
                                     enc->yuv_main, main_step,
                                     enc->yuv_aux, aux_step, &roi);
    }

    if (prc != PRIMITIVES_SUCCESS) {
        WLRDP_LOG_ERROR("RGBToAVC444YUV%s failed: %d",
                        enc->avc444v2 ? "v2" : "", (int)prc);
        return false;
    }

    /* Copy YUV planes into encoder frames */
    for (int stream = 0; stream < 2; stream++) {
        struct h264_ctx *hctx = &enc->h264[stream];
        AVFrame *frame = hctx->frame;
        uint8_t **yuv = (stream == 0) ? enc->yuv_main : enc->yuv_aux;

        if (av_frame_make_writable(frame) < 0) {
            WLRDP_LOG_ERROR("av_frame_make_writable failed (stream %d)", stream);
            return false;
        }

        /* Copy each plane (Y, U, V) row by row */
        for (int p = 0; p < 3; p++) {
            uint32_t plane_h = (p == 0) ? h : h / 2;
            uint32_t src_stride = enc->yuv_stride[p];
            uint32_t dst_stride = frame->linesize[p];
            uint32_t copy_width = (p == 0) ? w : w / 2;

            for (uint32_t row = 0; row < plane_h; row++) {
                memcpy(frame->data[p] + row * dst_stride,
                       yuv[p] + row * src_stride,
                       copy_width);
            }
        }
    }

    /* Encode main stream */
    AVCodecContext *main_ctx = enc->h264[0].codec_ctx;
    if (!h264_ctx_encode(&enc->h264[0], main_ctx->frame_num,
                         &enc->out_buf, &enc->out_len, &enc->is_keyframe)) {
        return false;
    }

    /* Encode aux stream */
    AVCodecContext *aux_ctx = enc->h264[1].codec_ctx;
    bool aux_key;
    if (!h264_ctx_encode(&enc->h264[1], aux_ctx->frame_num,
                         &enc->aux_buf, &enc->aux_len, &aux_key)) {
        return false;
    }

    return true;
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
    if (mode == WLRDP_ENCODER_AVC444) {
        if (avc444_init(enc)) {
            return true;
        }
        /* AVC444 failed — try plain AVC420 before NSCodec */
        mode = WLRDP_ENCODER_H264;
    }
    if (mode == WLRDP_ENCODER_H264) {
        if (h264_init(enc)) {
            return true;
        }
    }
#else
    if (mode == WLRDP_ENCODER_H264 || mode == WLRDP_ENCODER_AVC444) {
        WLRDP_LOG_WARN("H.264 not compiled in, falling back to NSCodec");
    }
#endif

    return nsc_init(enc);
}

bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride)
{
#ifdef WLRDP_HAVE_H264
    if (enc->mode == WLRDP_ENCODER_AVC444) {
        return avc444_encode(enc, pixels, stride);
    }
    if (enc->mode == WLRDP_ENCODER_H264) {
        return h264_encode(enc, pixels, stride);
    }
#endif
    return nsc_encode(enc, pixels, stride);
}

void encoder_request_keyframe(struct wlrdp_encoder *enc)
{
#ifdef WLRDP_HAVE_H264
    if ((enc->mode == WLRDP_ENCODER_H264 || enc->mode == WLRDP_ENCODER_AVC444)
        && enc->h264[0].frame) {
        ((AVFrame *)enc->h264[0].frame)->pict_type = AV_PICTURE_TYPE_I;
    }
    if (enc->mode == WLRDP_ENCODER_AVC444 && enc->h264[1].frame) {
        ((AVFrame *)enc->h264[1].frame)->pict_type = AV_PICTURE_TYPE_I;
    }
#else
    (void)enc;
#endif
}

void encoder_destroy(struct wlrdp_encoder *enc)
{
#ifdef WLRDP_HAVE_H264
    h264_cleanup(enc);
    avc444_cleanup_buffers(enc);
#endif
    nsc_cleanup(enc);
    memset(enc, 0, sizeof(*enc));
}
