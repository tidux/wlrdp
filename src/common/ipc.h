#ifndef WLRDP_IPC_H
#define WLRDP_IPC_H

#include <stdbool.h>
#include <stdint.h>

/* IPC message types between daemon and session worker */
enum wlrdp_msg_type {
    WLRDP_MSG_NEW_CLIENT = 1,  /* daemon->session: new peer fd (SCM_RIGHTS) */
    WLRDP_MSG_DISCONNECT = 2,  /* daemon->session: client disconnected */
    WLRDP_MSG_RESIZE     = 3,  /* daemon->session: resolution change */
    WLRDP_MSG_STATUS     = 4,  /* session->daemon: session state report */
    WLRDP_MSG_SHUTDOWN   = 5,  /* daemon->session: terminate */
};

/* Session status values for WLRDP_MSG_STATUS */
enum wlrdp_session_status {
    WLRDP_STATUS_READY        = 0,  /* session ready for client */
    WLRDP_STATUS_ACTIVE       = 1,  /* client connected and streaming */
    WLRDP_STATUS_DISCONNECTED = 2,  /* client disconnected, session alive */
    WLRDP_STATUS_TERMINATED   = 3,  /* session ending */
};

/* Wire format: [4 bytes type][4 bytes payload_len][payload][optional fd] */
struct wlrdp_ipc_msg {
    uint32_t type;
    uint32_t payload_len;
    uint8_t payload[256]; /* inline payload for small messages */
};

/* Resize payload */
struct wlrdp_resize_payload {
    uint32_t width;
    uint32_t height;
};

/*
 * Send an IPC message, optionally with an ancillary fd.
 * Pass fd=-1 to send without an fd.
 * Returns true on success.
 */
bool ipc_send_msg(int sock_fd, const struct wlrdp_ipc_msg *msg, int fd);

/*
 * Receive an IPC message, optionally receiving an ancillary fd.
 * *out_fd is set to the received fd, or -1 if none.
 * Returns true on success, false on error or EOF.
 */
bool ipc_recv_msg(int sock_fd, struct wlrdp_ipc_msg *msg, int *out_fd);

#endif /* WLRDP_IPC_H */
