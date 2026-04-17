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
    ctx->send_mode = WLRDP_SEND_SURFACE_BITS;

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
    cmd.bmp.codecID = 0;
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
