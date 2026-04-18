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

    enc->av_codec_ctx = ctx;
    enc->av_frame = frame;
    enc->av_packet = pkt;
    enc->sws_ctx = sws;
    enc->mode = WLRDP_ENCODER_H264;

    WLRDP_LOG_INFO("encoder initialized (%ux%u, aligned to %ux%u, H.264 via %s)",
                    enc->width, enc->height, ctx->width, ctx->height, name);
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
