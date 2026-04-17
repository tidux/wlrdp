#ifndef WLRDP_RDP_PEER_H
#define WLRDP_RDP_PEER_H

#include <stdbool.h>
#include <stdint.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

struct wlrdp_input;

enum wlrdp_send_mode {
    WLRDP_SEND_SURFACE_BITS,  /* Phase 1/2 fallback: SurfaceBits + NSCodec */
    WLRDP_SEND_GFX_AVC420,    /* Phase 3: RDPGFX + H.264 */
};

struct wlrdp_peer_context {
    rdpContext base;

    struct wlrdp_input *input;
    uint32_t width;
    uint32_t height;
    bool activated;

    /* RDPGFX state */
    void *gfx_context;       /* RdpgfxServerContext* */
    uint16_t gfx_surface_id;
    bool gfx_opened;
    uint32_t gfx_frame_id;

    enum wlrdp_send_mode send_mode;
};

bool rdp_peer_init(freerdp_peer *client, const char *cert_file,
                   const char *key_file, struct wlrdp_input *input);

/*
 * Send a frame to the RDP client.
 * For GFX mode: data is H.264 NAL units, len is byte count.
 * For SurfaceBits mode: data is raw BGRX pixels, len is byte count.
 * is_keyframe is only meaningful for GFX mode.
 */
bool rdp_peer_send_frame(freerdp_peer *client,
                         uint8_t *data, uint32_t len,
                         uint32_t width, uint32_t height,
                         bool is_keyframe);

/*
 * Query whether the client supports GFX+AVC420.
 * Only valid after the peer is activated.
 */
bool rdp_peer_supports_gfx_h264(freerdp_peer *client);

bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input);

#endif /* WLRDP_RDP_PEER_H */
