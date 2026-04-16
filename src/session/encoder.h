#ifndef WLRDP_ENCODER_H
#define WLRDP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

struct wlrdp_encoder {
    uint32_t width;
    uint32_t height;
    void *nsc_ctx;
    uint8_t *out_buf;
    uint32_t out_len;
};

bool encoder_init(struct wlrdp_encoder *enc, uint32_t width, uint32_t height);
bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride);
void encoder_destroy(struct wlrdp_encoder *enc);

#endif /* WLRDP_ENCODER_H */
