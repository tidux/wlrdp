#include "encoder.h"
#include "common.h"

#include <freerdp/codec/nsc.h>
#include <winpr/stream.h>

bool encoder_init(struct wlrdp_encoder *enc, uint32_t width, uint32_t height)
{
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;

    NSC_CONTEXT *nsc = nsc_context_new();
    if (!nsc) {
        WLRDP_LOG_ERROR("failed to create NSCodec context");
        return false;
    }

    if (!nsc_context_set_parameters(nsc, NSC_COLOR_LOSS_LEVEL, 0)) {
        WLRDP_LOG_WARN("failed to set NSCodec color loss level");
    }
    if (!nsc_context_set_parameters(nsc, NSC_ALLOW_SUBSAMPLING, 1)) {
        WLRDP_LOG_WARN("failed to set NSCodec subsampling");
    }
    if (!nsc_context_set_parameters(nsc, NSC_DYNAMIC_COLOR_FIDELITY, 1)) {
        WLRDP_LOG_WARN("failed to set NSCodec dynamic color fidelity");
    }

    enc->nsc_ctx = nsc;

    WLRDP_LOG_INFO("encoder initialized (%ux%u, NSCodec)", width, height);
    return true;
}

bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride)
{
    NSC_CONTEXT *nsc = enc->nsc_ctx;

    if (!nsc_compose_message(nsc, enc->width, enc->height, pixels, stride)) {
        WLRDP_LOG_ERROR("nsc_compose_message failed");
        return false;
    }

    wStream *s = nsc->BitmapData;
    enc->out_buf = Stream_Buffer(s);
    enc->out_len = Stream_GetPosition(s);

    return true;
}

void encoder_destroy(struct wlrdp_encoder *enc)
{
    if (enc->nsc_ctx) {
        nsc_context_free(enc->nsc_ctx);
    }
    memset(enc, 0, sizeof(*enc));
}
