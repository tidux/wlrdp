#ifndef WLRDP_ENCODER_H
#define WLRDP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/h264.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/codec/progressive.h>
#include <winpr/stream.h>

enum wlrdp_encoder_mode {
    WLRDP_ENCODER_RAW = 0,
    WLRDP_ENCODER_NSAC,
    WLRDP_ENCODER_H264_FFMPEG,
    WLRDP_ENCODER_H264_FREERDP,
    WLRDP_ENCODER_PROGRESSIVE,
    WLRDP_ENCODER_MAX
};

struct wlrdp_encoder {
    enum wlrdp_encoder_mode mode;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint8_t avc444_version;

    uint8_t *out_buf;
    uint32_t out_len;
    uint8_t *aux_buf;
    uint32_t aux_len;
    bool is_keyframe;

    uint8_t avc444_lc;
    RDPGFX_H264_METABLOCK h264_meta;
    RDPGFX_H264_METABLOCK h264_aux_meta;

    H264_CONTEXT *h264_ctx;
    PROGRESSIVE_CONTEXT *prog_ctx;
    NSC_CONTEXT *nsc_ctx;
    wStream *nsc_stream;

    uint8_t *conv_buf;
    uint32_t conv_size;
};

bool encoder_init(struct wlrdp_encoder *enc, enum wlrdp_encoder_mode mode,
                  uint32_t width, uint32_t height, uint8_t avc444_version,
                  uint32_t format);
bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride);
void encoder_request_keyframe(struct wlrdp_encoder *enc);
void encoder_destroy(struct wlrdp_encoder *enc);

#endif /* WLRDP_ENCODER_H */
