#ifndef WLRDP_ENCODER_H
#define WLRDP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

enum wlrdp_encoder_mode {
    WLRDP_ENCODER_RAW,     /* Raw pixels (nocodec) */
    WLRDP_ENCODER_NSC,     /* NSCodec fallback (Phase 1 behavior) */
    WLRDP_ENCODER_H264,    /* H.264 via FFmpeg (AVC420) */
    WLRDP_ENCODER_AVC444,  /* H.264 AVC444/AVC444v2 dual-stream */
};

/*
 * Single H.264 encoder context (codec + frame + packet + color converter).
 * AVC420 uses one of these; AVC444 uses two (main + aux).
 */
struct h264_ctx {
    void *codec_ctx;    /* AVCodecContext* */
    void *frame;        /* AVFrame* (YUV420P) */
    void *packet;       /* AVPacket* */
    void *sws_ctx;      /* SwsContext* for pixel format conversion */
};

struct wlrdp_encoder {
    enum wlrdp_encoder_mode mode;
    uint32_t width;
    uint32_t height;

    /* NSCodec state (used when mode == WLRDP_ENCODER_NSC) */
    void *nsc_ctx;
    void *nsc_stream;

    /* H.264 state: h264[0] = main stream, h264[1] = aux/chroma (AVC444 only) */
    struct h264_ctx h264[2];

    /* AVC444 intermediate YUV plane buffers (allocated by avc444_init) */
    uint8_t *yuv_main[3];   /* main YUV420 planes (Y, U, V) */
    uint8_t *yuv_aux[3];    /* aux YUV420 planes (Y, U, V) */
    uint32_t yuv_stride[3]; /* plane strides */
    bool avc444v2;          /* true = use AVC444v2 chroma layout */
    uint32_t format;        /* RDP pixel format (PIXEL_FORMAT_XXX) */

    /* Aligned copy of capture buffer for RGBToAVC444YUV primitive (fixes ARM bus error on unaligned screencopy buffers). */
    uint8_t *aligned_buf;
    uint32_t aligned_stride;

    /* Conversion buffer for non-32bit formats */
    uint8_t *conv_buf;
    uint32_t conv_size;

    /* Output — valid after encoder_encode until next call or destroy */
    uint8_t *out_buf;       /* main stream (AVC420 or AVC444 base) */
    uint32_t out_len;
    uint8_t *aux_buf;       /* aux stream (AVC444 chroma, NULL for AVC420/NSC) */
    uint32_t aux_len;
    bool is_keyframe;
    uint32_t frame_count;   /* Tracks initial frames for forced keyframes (fixes init/decode issues) */
};

/*
 * Initialize the encoder. Tries H.264 if mode is WLRDP_ENCODER_H264
 * and WLRDP_HAVE_H264 is defined; falls back to NSC on failure.
 * Returns true on success. After init, check enc->mode for actual mode.
 */
bool encoder_init(struct wlrdp_encoder *enc, enum wlrdp_encoder_mode mode,
                  uint32_t width, uint32_t height, bool avc444v2, uint32_t format);

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
