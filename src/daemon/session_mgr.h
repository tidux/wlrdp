#ifndef WLRDP_SESSION_MGR_H
#define WLRDP_SESSION_MGR_H

#include "auth.h"
#include <stdbool.h>
#include <sys/types.h>

#define WLRDP_MAX_SESSIONS 50

struct wlrdp_session_entry {
    bool active;
    char username[64];
    pid_t pid;
    int ipc_fd;        /* daemon's end of the socketpair */
    int status;        /* last reported wlrdp_session_status */
};

struct wlrdp_session_mgr {
    struct wlrdp_session_entry sessions[WLRDP_MAX_SESSIONS];
    int count;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;
    int default_width;
    int default_height;
};

/* Initialize the session manager */
void session_mgr_init(struct wlrdp_session_mgr *mgr,
                      const char *desktop_cmd,
                      const char *cert_file, const char *key_file,
                      int width, int height);

/*
 * Find or create a session for the given user. If a session already
 * exists and is alive, returns its entry for reconnection. Otherwise
 * forks a new wlrdp-session process.
 *
 * Returns the session entry, or NULL on failure.
 */
struct wlrdp_session_entry *session_mgr_get_session(
    struct wlrdp_session_mgr *mgr,
    const struct wlrdp_auth_result *auth);

/*
 * Send a new client fd to the session worker via IPC.
 * The peer_fd is the RDP peer's socket obtained after TLS+auth.
 */
bool session_mgr_send_client(struct wlrdp_session_entry *entry, int peer_fd);

/*
 * Reap any dead session workers. Call after SIGCHLD.
 */
void session_mgr_reap(struct wlrdp_session_mgr *mgr);

/*
 * Shut down all sessions.
 */
void session_mgr_destroy(struct wlrdp_session_mgr *mgr);

#endif /* WLRDP_SESSION_MGR_H */
