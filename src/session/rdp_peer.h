#ifndef WLRDP_RDP_PEER_H
#define WLRDP_RDP_PEER_H

#include <stdbool.h>
#include <stdint.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

struct wlrdp_input;
struct wlrdp_encoder;

struct wlrdp_peer_context {
    rdpContext base;

    struct wlrdp_input *input;
    uint32_t width;
    uint32_t height;
    bool activated;
};

bool rdp_peer_init(freerdp_peer *client, const char *cert_file,
                   const char *key_file, struct wlrdp_input *input);
bool rdp_peer_send_frame(freerdp_peer *client,
                         uint8_t *data, uint32_t len,
                         uint32_t width, uint32_t height);

/*
 * Initialize a peer from a pre-authenticated socket fd received via IPC.
 * The cert/key are needed for the peer's TLS context.
 */
bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input);

#endif /* WLRDP_RDP_PEER_H */
