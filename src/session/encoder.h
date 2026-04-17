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
