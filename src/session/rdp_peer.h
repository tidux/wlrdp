#ifndef WLRDP_RDP_PEER_H
#define WLRDP_RDP_PEER_H

#include <stdbool.h>
#include <stdint.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

struct wlrdp_input;
struct wlrdp_clipboard;
struct wlrdp_audio;

enum wlrdp_send_mode {
    WLRDP_SEND_SURFACE_BITS,  /* Phase 1/2 fallback: SurfaceBits + NSCodec */
    WLRDP_SEND_GFX_AVC420,    /* Phase 3: RDPGFX + H.264 */
};

typedef void (*rdp_peer_resize_cb)(void *data, uint32_t width, uint32_t height, uint32_t scale);

struct wlrdp_peer_context {
    rdpContext base;

    struct wlrdp_input *input;
    uint32_t width;
    uint32_t height;
    bool activated;

    /* Resize callback */
    rdp_peer_resize_cb on_resize;
    void *on_resize_data;

    /* RDPGFX state */
    void *gfx_context;       /* RdpgfxServerContext* */
    void *gfx_vcm;           /* HANDLE from WTSOpenServerA */
    uint16_t gfx_surface_id;
    bool gfx_opened;
    bool gfx_ready;          /* true once DRDYNVC ready + GFX opened + surface created */
    uint32_t gfx_frame_id;

    /* DISP state */
    void *disp_context;      /* DispServerContext* */
    bool disp_opened;

    /* CLIPRDR state */
    void *cliprdr_context;     /* CliprdrServerContext* */
    bool cliprdr_opened;
    struct wlrdp_clipboard *clipboard;  /* set by main.c before activation */

    /* RDPSND state */
    void *rdpsnd_context;      /* RdpsndServerContext* */
    bool rdpsnd_opened;
    bool rdpsnd_ready;         /* true after Activated callback */
    struct wlrdp_audio *audio; /* set by main.c before activation */

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
 * Only valid after GFX negotiation completes (gfx_ready == true).
 */
bool rdp_peer_supports_gfx_h264(freerdp_peer *client);

/*
 * Get the file descriptor for the VCM event handle, for epoll.
 * Returns -1 if no VCM is active.
 */
int rdp_peer_get_vcm_fd(freerdp_peer *client);

/*
 * Poll the VCM for DRDYNVC progress. Call this from the event loop
 * whenever the VCM fd is readable. Returns false on fatal error.
 * When DRDYNVC becomes ready, this opens the GFX channel and creates
 * the surface automatically.
 */
bool rdp_peer_check_vcm(freerdp_peer *client);

bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input);

/*
 * Update the RDP peer's desktop size and recreate GFX surfaces if needed.
 */
void rdp_peer_update_size(freerdp_peer *client, uint32_t width, uint32_t height);

/*
 * Send audio samples to the RDP client via RDPSND.
 * Returns true on success.
 */
bool rdp_peer_send_audio(freerdp_peer *client, const int16_t *samples,
                         uint32_t n_frames);

#endif /* WLRDP_RDP_PEER_H */
