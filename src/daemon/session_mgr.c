#include "session_mgr.h"
#include "ipc.h"
#include "common.h"

#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

void session_mgr_init(struct wlrdp_session_mgr *mgr,
                      const char *desktop_cmd,
                      const char *cert_file, const char *key_file,
                      int width, int height)
{
    memset(mgr, 0, sizeof(*mgr));
    mgr->desktop_cmd = desktop_cmd;
    mgr->cert_file = cert_file;
    mgr->key_file = key_file;
    mgr->default_width = width;
    mgr->default_height = height;
}

static struct wlrdp_session_entry *find_session(
    struct wlrdp_session_mgr *mgr, const char *username)
{
    for (int i = 0; i < WLRDP_MAX_SESSIONS; i++) {
        if (mgr->sessions[i].active &&
            strcmp(mgr->sessions[i].username, username) == 0) {
            return &mgr->sessions[i];
        }
    }
    return NULL;
}

static struct wlrdp_session_entry *alloc_slot(struct wlrdp_session_mgr *mgr)
{
    for (int i = 0; i < WLRDP_MAX_SESSIONS; i++) {
        if (!mgr->sessions[i].active) {
            return &mgr->sessions[i];
        }
    }
    return NULL;
}

static bool fork_session(struct wlrdp_session_mgr *mgr,
                         struct wlrdp_session_entry *entry,
                         const struct wlrdp_auth_result *auth)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        WLRDP_LOG_ERROR("socketpair failed: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        WLRDP_LOG_ERROR("fork failed: %s", strerror(errno));
        close(sv[0]);
        close(sv[1]);
        return false;
    }

    if (pid == 0) {
        /* Child: become the session worker */
        close(sv[0]); /* daemon's end */

        /* Drop privileges */
        if (setgid(auth->gid) < 0) {
            fprintf(stderr, "[wlrdp] ERROR: setgid(%d) failed: %s\n",
                    auth->gid, strerror(errno));
            _exit(1);
        }
        if (initgroups(entry->username, auth->gid) < 0) {
            fprintf(stderr, "[wlrdp] ERROR: initgroups failed: %s\n",
                    strerror(errno));
            _exit(1);
        }
        if (setuid(auth->uid) < 0) {
            fprintf(stderr, "[wlrdp] ERROR: setuid(%d) failed: %s\n",
                    auth->uid, strerror(errno));
            _exit(1);
        }

        /* Set up user environment */
        setenv("HOME", auth->home, 1);
        setenv("USER", entry->username, 1);
        setenv("LOGNAME", entry->username, 1);
        setenv("SHELL", auth->shell, 1);

        char xdg_dir[128];
        snprintf(xdg_dir, sizeof(xdg_dir), "/run/user/%d", auth->uid);
        setenv("XDG_RUNTIME_DIR", xdg_dir, 1);

        /* Convert IPC fd and params to strings for exec */
        char ipc_fd_str[16];
        snprintf(ipc_fd_str, sizeof(ipc_fd_str), "%d", sv[1]);

        char width_str[16], height_str[16];
        snprintf(width_str, sizeof(width_str), "%d", mgr->default_width);
        snprintf(height_str, sizeof(height_str), "%d", mgr->default_height);

        execlp("wlrdp-session", "wlrdp-session",
               "--ipc-fd", ipc_fd_str,
               "--width", width_str,
               "--height", height_str,
               "--desktop-cmd", mgr->desktop_cmd,
               "--cert", mgr->cert_file,
               "--key", mgr->key_file,
               NULL);

        fprintf(stderr, "[wlrdp] ERROR: exec wlrdp-session failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    /* Parent: daemon */
    close(sv[1]); /* session's end */

    entry->active = true;
    entry->pid = pid;
    entry->ipc_fd = sv[0];
    entry->status = WLRDP_STATUS_READY;
    mgr->count++;

    WLRDP_LOG_INFO("forked session for '%s' (pid %d)",
                    entry->username, pid);
    return true;
}

struct wlrdp_session_entry *session_mgr_get_session(
    struct wlrdp_session_mgr *mgr,
    const struct wlrdp_auth_result *auth)
{
    struct passwd *pw = getpwuid(auth->uid);
    if (!pw) return NULL;

    const char *username = pw->pw_name;

    /* Check for existing session */
    struct wlrdp_session_entry *entry = find_session(mgr, username);
    if (entry) {
        WLRDP_LOG_INFO("found existing session for '%s' (pid %d)",
                        username, entry->pid);
        return entry;
    }

    /* Allocate new slot */
    entry = alloc_slot(mgr);
    if (!entry) {
        WLRDP_LOG_ERROR("max sessions reached (%d)", WLRDP_MAX_SESSIONS);
        return NULL;
    }

    snprintf(entry->username, sizeof(entry->username), "%s", username);

    if (!fork_session(mgr, entry, auth)) {
        memset(entry, 0, sizeof(*entry));
        return NULL;
    }

    return entry;
}

bool session_mgr_send_client(struct wlrdp_session_entry *entry, int peer_fd)
{
    struct wlrdp_ipc_msg msg = {
        .type = WLRDP_MSG_NEW_CLIENT,
        .payload_len = 0,
    };

    if (!ipc_send_msg(entry->ipc_fd, &msg, peer_fd)) {
        WLRDP_LOG_ERROR("failed to send client fd to session '%s'",
                        entry->username);
        return false;
    }

    WLRDP_LOG_INFO("sent client fd to session '%s' (pid %d)",
                    entry->username, entry->pid);
    return true;
}

void session_mgr_reap(struct wlrdp_session_mgr *mgr)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < WLRDP_MAX_SESSIONS; i++) {
            if (mgr->sessions[i].active && mgr->sessions[i].pid == pid) {
                WLRDP_LOG_INFO("session '%s' (pid %d) exited",
                                mgr->sessions[i].username, pid);
                close(mgr->sessions[i].ipc_fd);
                memset(&mgr->sessions[i], 0, sizeof(mgr->sessions[i]));
                mgr->count--;
                break;
            }
        }
    }
}

void session_mgr_destroy(struct wlrdp_session_mgr *mgr)
{
    for (int i = 0; i < WLRDP_MAX_SESSIONS; i++) {
        if (mgr->sessions[i].active) {
            struct wlrdp_ipc_msg msg = {
                .type = WLRDP_MSG_SHUTDOWN,
                .payload_len = 0,
            };
            ipc_send_msg(mgr->sessions[i].ipc_fd, &msg, -1);
            kill(mgr->sessions[i].pid, SIGTERM);
            close(mgr->sessions[i].ipc_fd);
        }
    }

    /* Wait for children to exit */
    for (int i = 0; i < WLRDP_MAX_SESSIONS; i++) {
        if (mgr->sessions[i].active) {
            waitpid(mgr->sessions[i].pid, NULL, 0);
        }
    }

    memset(mgr, 0, sizeof(*mgr));
}
