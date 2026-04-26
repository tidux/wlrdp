#include "encoder.h"
#include "common.h"

#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>

#include <stdlib.h>
#include <string.h>

#ifndef PREFER_HW_H264
#define PREFER_HW_H264 1
#endif

static void
encoder_clear_h264_metadata(struct wlrdp_encoder *enc)
{
    free_h264_metablock(&enc->h264_meta);
    free_h264_metablock(&enc->h264_aux_meta);
    memset(&enc->h264_meta, 0, sizeof(enc->h264_meta));
    memset(&enc->h264_aux_meta, 0, sizeof(enc->h264_aux_meta));
}

static bool
raw_init(struct wlrdp_encoder *enc)
{
    enc->mode = WLRDP_ENCODER_RAW;
    WLRDP_LOG_INFO("encoder initialized (%ux%u, RAW)", enc->width, enc->height);
    return true;
}

static bool
raw_encode(struct wlrdp_encoder *enc, const uint8_t *pixels, uint32_t stride)
{
    uint32_t bpp = FreeRDPGetBitsPerPixel(enc->format);
    uint32_t dst_stride = (enc->width * bpp + 7) / 8;
    uint32_t needed = dst_stride * enc->height;

    if (enc->conv_size < needed) {
        uint8_t *tmp = realloc(enc->conv_buf, needed);
        if (!tmp)
            return false;
        enc->conv_buf = tmp;
        enc->conv_size = needed;
    }

    if (!freerdp_image_copy(enc->conv_buf, enc->format, dst_stride, 0, 0,
                            enc->width, enc->height, pixels, PIXEL_FORMAT_BGRX32,
                            stride, 0, 0, NULL, FREERDP_FLIP_VERTICAL)) {
        WLRDP_LOG_ERROR("freerdp_image_copy failed for raw conversion");
        return false;
    }

    enc->out_buf = enc->conv_buf;
    enc->out_len = needed;
    enc->aux_buf = NULL;
    enc->aux_len = 0;
    enc->is_keyframe = true;
    return true;
}

static void
raw_cleanup(struct wlrdp_encoder *enc)
{
    free(enc->conv_buf);
    enc->conv_buf = NULL;
    enc->conv_size = 0;
}

static bool
nsc_init(struct wlrdp_encoder *enc)
{
    NSC_CONTEXT *nsc = nsc_context_new();
    if (!nsc) {
        WLRDP_LOG_ERROR("failed to create NSCodec context");
        return false;
    }

    nsc_context_set_parameters(nsc, NSC_COLOR_LOSS_LEVEL, 0);
    nsc_context_set_parameters(nsc, NSC_ALLOW_SUBSAMPLING, 1);
    nsc_context_set_parameters(nsc, NSC_DYNAMIC_COLOR_FIDELITY, 1);

    if (!nsc_context_set_parameters(nsc, NSC_COLOR_FORMAT, enc->format)) {
        WLRDP_LOG_ERROR("failed to set NSCodec pixel format 0x%08x", enc->format);
        nsc_context_free(nsc);
        return false;
    }

    wStream *stream = Stream_New(NULL, enc->width * enc->height * 4);
    if (!stream) {
        WLRDP_LOG_ERROR("failed to create NSCodec stream");
        nsc_context_free(nsc);
        return false;
    }

    enc->nsc_ctx = nsc;
    enc->nsc_stream = stream;
    enc->mode = WLRDP_ENCODER_NSAC;
    WLRDP_LOG_INFO("encoder initialized (%ux%u, NSCodec)", enc->width, enc->height);
    return true;
}

static bool
nsc_encode(struct wlrdp_encoder *enc, const uint8_t *pixels, uint32_t stride)
{
    uint32_t bpp = FreeRDPGetBitsPerPixel(enc->format);
    uint32_t dst_stride = (enc->width * bpp + 7) / 8;
    uint32_t needed = dst_stride * enc->height;

    if (enc->conv_size < needed) {
        uint8_t *tmp = realloc(enc->conv_buf, needed);
        if (!tmp)
            return false;
        enc->conv_buf = tmp;
        enc->conv_size = needed;
    }

    if (!freerdp_image_copy(enc->conv_buf, enc->format, dst_stride, 0, 0,
                            enc->width, enc->height, pixels, PIXEL_FORMAT_BGRX32,
                            stride, 0, 0, NULL, FREERDP_FLIP_VERTICAL)) {
        WLRDP_LOG_ERROR("freerdp_image_copy failed for NSCodec conversion");
        return false;
    }

    Stream_SetPosition(enc->nsc_stream, 0);
    if (!nsc_compose_message(enc->nsc_ctx, enc->nsc_stream, enc->conv_buf,
                             enc->width, enc->height, dst_stride)) {
        WLRDP_LOG_ERROR("nsc_compose_message failed");
        return false;
    }

    enc->out_buf = Stream_Buffer(enc->nsc_stream);
    enc->out_len = (uint32_t)Stream_GetPosition(enc->nsc_stream);
    enc->aux_buf = NULL;
    enc->aux_len = 0;
    enc->is_keyframe = true;
    return true;
}

static void
nsc_cleanup(struct wlrdp_encoder *enc)
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

static bool
h264_init(struct wlrdp_encoder *enc)
{
    enc->h264_ctx = h264_context_new(TRUE);
    if (!enc->h264_ctx) {
        WLRDP_LOG_ERROR("failed to create FreeRDP H.264 context");
        return false;
    }

    h264_context_set_option(enc->h264_ctx, H264_CONTEXT_OPTION_HW_ACCEL,
                            PREFER_HW_H264 ? TRUE : FALSE);
    h264_context_set_option(enc->h264_ctx, H264_CONTEXT_OPTION_RATECONTROL,
                            H264_RATECONTROL_CQP);
    h264_context_set_option(enc->h264_ctx, H264_CONTEXT_OPTION_BITRATE, 4000000);
    h264_context_set_option(enc->h264_ctx, H264_CONTEXT_OPTION_FRAMERATE, 30);
    h264_context_set_option(enc->h264_ctx, H264_CONTEXT_OPTION_QP, 22);
    h264_context_set_option(enc->h264_ctx, H264_CONTEXT_OPTION_USAGETYPE,
                            H264_SCREEN_CONTENT_REAL_TIME);

    if (!h264_context_reset(enc->h264_ctx, enc->width, enc->height)) {
        WLRDP_LOG_ERROR("failed to reset FreeRDP H.264 context");
        h264_context_free(enc->h264_ctx);
        enc->h264_ctx = NULL;
        return false;
    }

    enc->mode = WLRDP_ENCODER_H264_FREERDP;
    WLRDP_LOG_INFO("encoder initialized (%ux%u, FreeRDP H.264, hw preference: %s)",
                   enc->width, enc->height, PREFER_HW_H264 ? "enabled" : "disabled");
    return true;
}

static bool
h264_encode(struct wlrdp_encoder *enc, const uint8_t *pixels, uint32_t stride)
{
    RECTANGLE_16 region = {
        .left = 0,
        .top = 0,
        .right = (UINT16)enc->width,
        .bottom = (UINT16)enc->height,
    };
    INT32 rc;

    encoder_clear_h264_metadata(enc);
    enc->out_buf = NULL;
    enc->out_len = 0;
    enc->aux_buf = NULL;
    enc->aux_len = 0;
    enc->avc444_lc = 0;

    if (enc->avc444_version != 0) {
        rc = avc444_compress(enc->h264_ctx, pixels, PIXEL_FORMAT_BGRX32,
                             stride, enc->width, enc->height,
                             enc->avc444_version, &region,
                             &enc->avc444_lc, &enc->out_buf, &enc->out_len,
                             &enc->aux_buf, &enc->aux_len, &enc->h264_meta,
                             &enc->h264_aux_meta);
    } else {
        rc = avc420_compress(enc->h264_ctx, pixels, PIXEL_FORMAT_BGRX32,
                             stride, enc->width, enc->height, &region,
                             &enc->out_buf, &enc->out_len, &enc->h264_meta);
    }

    if (rc < 0) {
        WLRDP_LOG_ERROR("FreeRDP H.264 compression failed");
        encoder_clear_h264_metadata(enc);
        return false;
    }

    enc->is_keyframe = true;
    return true;
}

static bool
progressive_init(struct wlrdp_encoder *enc)
{
    enc->prog_ctx = progressive_context_new(TRUE);
    if (!enc->prog_ctx) {
        WLRDP_LOG_ERROR("failed to create FreeRDP Progressive context");
        return false;
    }

    if (progressive_create_surface_context(enc->prog_ctx, 1, enc->width,
                                           enc->height) < 0) {
        WLRDP_LOG_ERROR("failed to create Progressive surface context");
        progressive_context_free(enc->prog_ctx);
        enc->prog_ctx = NULL;
        return false;
    }

    enc->mode = WLRDP_ENCODER_PROGRESSIVE;
    WLRDP_LOG_INFO("encoder initialized (%ux%u, Progressive)", enc->width,
                   enc->height);
    return true;
}

static bool
progressive_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                   uint32_t stride)
{
    REGION16 invalid;
    RECTANGLE_16 rect = {
        .left = 0,
        .top = 0,
        .right = (UINT16)enc->width,
        .bottom = (UINT16)enc->height,
    };

    region16_init(&invalid);
    if (!region16_union_rect(&invalid, &invalid, &rect)) {
        region16_uninit(&invalid);
        return false;
    }

    INT32 rc = progressive_compress(enc->prog_ctx, pixels, stride * enc->height,
                                    PIXEL_FORMAT_BGRX32, enc->width, enc->height,
                                    stride, &invalid, &enc->out_buf,
                                    &enc->out_len);
    region16_uninit(&invalid);

    if (rc < 0) {
        WLRDP_LOG_ERROR("Progressive compression failed");
        enc->out_buf = NULL;
        enc->out_len = 0;
        return false;
    }

    enc->aux_buf = NULL;
    enc->aux_len = 0;
    enc->is_keyframe = true;
    return true;
}

bool
encoder_init(struct wlrdp_encoder *enc, enum wlrdp_encoder_mode mode,
             uint32_t width, uint32_t height, uint8_t avc444_version,
             uint32_t format)
{
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;
    enc->format = format;
    enc->avc444_version = avc444_version;

    if (mode == WLRDP_ENCODER_H264_FREERDP && h264_init(enc))
        return true;

    if (mode == WLRDP_ENCODER_PROGRESSIVE && progressive_init(enc))
        return true;

    if (mode == WLRDP_ENCODER_NSAC && nsc_init(enc))
        return true;

    return raw_init(enc);
}

bool
encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels, uint32_t stride)
{
    switch (enc->mode) {
    case WLRDP_ENCODER_H264_FREERDP:
        return h264_encode(enc, pixels, stride);
    case WLRDP_ENCODER_PROGRESSIVE:
        return progressive_encode(enc, pixels, stride);
    case WLRDP_ENCODER_NSAC:
        return nsc_encode(enc, pixels, stride);
    case WLRDP_ENCODER_RAW:
    default:
        return raw_encode(enc, pixels, stride);
    }
}

void
encoder_request_keyframe(struct wlrdp_encoder *enc)
{
    if (enc->mode == WLRDP_ENCODER_H264_FREERDP && enc->h264_ctx)
        h264_context_reset(enc->h264_ctx, enc->width, enc->height);
    else if (enc->mode == WLRDP_ENCODER_PROGRESSIVE && enc->prog_ctx)
        progressive_context_reset(enc->prog_ctx);
}

void
encoder_destroy(struct wlrdp_encoder *enc)
{
    encoder_clear_h264_metadata(enc);

    if (enc->h264_ctx) {
        h264_context_free(enc->h264_ctx);
        enc->h264_ctx = NULL;
    }
    if (enc->prog_ctx) {
        progressive_delete_surface_context(enc->prog_ctx, 1);
        progressive_context_free(enc->prog_ctx);
        enc->prog_ctx = NULL;
    }

    nsc_cleanup(enc);
    raw_cleanup(enc);
    memset(enc, 0, sizeof(*enc));
}
