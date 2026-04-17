#include "rdp_peer.h"
#include "input.h"
#include "common.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <linux/input-event-codes.h>

static BOOL on_post_connect(freerdp_peer *client)
{
    rdpSettings *settings = client->context->settings;

    WLRDP_LOG_INFO("RDP client connected: %ux%u %ubpp",
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
                    freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth));

    WLRDP_LOG_INFO("negotiated: NSCodec=%d NSCodecId=%u "
                    "RemoteFxCodec=%d SurfaceCommands=%d "
                    "FrameMarker=%d BitmapCacheV3=%d",
        freerdp_settings_get_bool(settings, FreeRDP_NSCodec),
        freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId),
        freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec),
        freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled),
        freerdp_settings_get_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled),
        freerdp_settings_get_bool(settings, FreeRDP_BitmapCacheV3Enabled));

    if (!freerdp_settings_get_bool(settings, FreeRDP_NSCodec)) {
        WLRDP_LOG_WARN("client does not support NSCodec, using raw bitmaps");
    }

    return TRUE;
}

static BOOL on_activate(freerdp_peer *client)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    ctx->activated = true;
    WLRDP_LOG_INFO("RDP peer activated");
    return TRUE;
}

static BOOL on_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code)
{
    freerdp_peer *client = input->context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->input) return TRUE;

    /* RDP scancodes are XT set 1. The virtual keyboard protocol expects
     * evdev keycodes, which equal the XT scancode for non-extended keys.
     * Extended keys need a lookup table. No +8 offset — the virtual
     * keyboard protocol handles the XKB offset internally. */
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
        bool pressed = !!(flags & PTR_FLAGS_DOWN);
        input_pointer_button(ctx->input, BTN_LEFT, pressed);
    }
    if (flags & PTR_FLAGS_BUTTON2) {
        bool pressed = !!(flags & PTR_FLAGS_DOWN);
        input_pointer_button(ctx->input, BTN_RIGHT, pressed);
    }
    if (flags & PTR_FLAGS_BUTTON3) {
        bool pressed = !!(flags & PTR_FLAGS_DOWN);
        input_pointer_button(ctx->input, BTN_MIDDLE, pressed);
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

    client->PostConnect = on_post_connect;
    client->Activate = on_activate;
    client->context->input->KeyboardEvent = on_keyboard_event;
    client->context->input->MouseEvent = on_mouse_event;
    client->context->input->ExtendedMouseEvent = on_extended_mouse_event;

    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;
    ctx->input = input;

    return true;
}

bool rdp_peer_send_frame(freerdp_peer *client,
                         uint8_t *data, uint32_t len,
                         uint32_t width, uint32_t height)
{
    rdpUpdate *update = client->context->update;
    rdpSettings *settings = client->context->settings;

    SURFACE_BITS_COMMAND cmd = { 0 };
    cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
    cmd.destLeft = 0;
    cmd.destTop = 0;
    cmd.destRight = width;
    cmd.destBottom = height;
    cmd.bmp.bpp = 32;
    cmd.bmp.width = width;
    cmd.bmp.height = height;

    (void)settings;
    cmd.bmp.codecID = 0; /* raw bitmap — NSCodec negotiation TBD */

    cmd.bmp.bitmapDataLength = len;
    cmd.bmp.bitmapData = data;

    BOOL ret = update->SurfaceBits(update->context, &cmd);
    if (!ret) {
        WLRDP_LOG_WARN("SurfaceBits failed (codecID=%u)", cmd.bmp.codecID);
    }

    return ret;
}

bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input)
{
    /* Set the peer's socket fd */
    client->sockfd = peer_fd;

    /* Then do the same init as rdp_peer_init */
    return rdp_peer_init(client, cert_file, key_file, input);
}
