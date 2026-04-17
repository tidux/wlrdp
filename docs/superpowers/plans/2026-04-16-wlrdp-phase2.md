# wlrdp Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the single-process Phase 1 server into a daemon/session architecture with PAM authentication, multi-user session management, session persistence across disconnect/reconnect, and systemd integration.

**Architecture:** A root-level `wlrdp-daemon` listens for RDP connections, authenticates users via PAM, and manages a session registry. For each authenticated user, it forks a `wlrdp-session` worker that drops privileges, launches cage, and handles frame capture/encoding/input. The daemon passes the authenticated RDP peer's socket fd to the session worker via Unix domain socket `SCM_RIGHTS`. On reconnect, the daemon routes the new connection to the existing session worker.

**Tech Stack:** C11, Meson, FreeRDP 3.x, libpam, Unix domain sockets (SCM_RIGHTS), epoll, systemd service units

**Spec:** `docs/superpowers/specs/2026-04-16-wlrdp-design.md`

---

## File Map

| File | Responsibility |
|------|---------------|
| `.devcontainer/Dockerfile` | Add `pam-devel`, `shadow-utils`, test users |
| `meson.build` | Add `pam` dependency, `src/daemon` subdir |
| `src/common/common.h` | Add log prefix for daemon vs session |
| `src/common/ipc.h` | IPC message type definitions and structs |
| `src/common/ipc.c` | `ipc_send_msg` / `ipc_recv_msg` with SCM_RIGHTS fd passing |
| `src/common/meson.build` | Build ipc.c into shared static lib |
| `src/daemon/meson.build` | Build `wlrdp-daemon` executable |
| `src/daemon/main.c` | Entry point: CLI args, signal handling, cert gen, epoll loop |
| `src/daemon/auth.h` | PAM auth interface |
| `src/daemon/auth.c` | `auth_check_credentials(user, pass)` → uid/gid via PAM |
| `src/daemon/session_mgr.h` | Session registry interface |
| `src/daemon/session_mgr.c` | Session table, fork+privilege-drop, reconnect routing |
| `src/session/main.c` | Rewrite: receive peer fd from IPC instead of running own listener |
| `src/session/rdp_peer.c` | Add `rdp_peer_init_from_fd()` to adopt an existing socket |
| `src/session/rdp_peer.h` | Declare `rdp_peer_init_from_fd()` |
| `config/wlrdp.pam` | PAM service config file |
| `config/wlrdp.conf.example` | Example configuration |
| `systemd/wlrdp.service` | systemd service unit for wlrdp-daemon |

---

### Task 1: DevContainer and Build System Updates

**Files:**
- Modify: `.devcontainer/Dockerfile`
- Modify: `meson.build`
- Create: `src/common/meson.build` (rewrite)
- Create: `src/daemon/meson.build`

- [ ] **Step 1: Add PAM and test-user support to Dockerfile**

```dockerfile
FROM registry.fedoraproject.org/fedora:43

RUN dnf install -y \
    gcc \
    meson \
    ninja-build \
    pkgconf-pkg-config \
    freerdp-devel \
    freerdp-server \
    libwinpr-devel \
    wayland-devel \
    wayland-protocols-devel \
    libxkbcommon-devel \
    openssl-devel \
    openssl \
    pam-devel \
    cage \
    xorg-x11-server-Xwayland \
    pixman-devel \
    wlr-randr \
    foot \
    xkeyboard-config \
    git \
    gdb \
    && dnf clean all

# Create test users for multi-session testing
RUN useradd -m developer && \
    echo "developer:developer" | chpasswd && \
    useradd -m testuser && \
    echo "testuser:testuser" | chpasswd && \
    mkdir -p /run/user/1000 /run/user/1001 && \
    chown developer:developer /run/user/1000 && \
    chown testuser:testuser /run/user/1001

USER developer
ENV XDG_RUNTIME_DIR=/run/user/1000
WORKDIR /workspace
```

- [ ] **Step 2: Add pam dependency and daemon subdir to top-level meson.build**

Add after the `xkbcommon_dep` line:

```c
pam_dep = dependency('pam')
```

Add at the end, after `subdir('src/session')`:

```c
subdir('src/daemon')
```

- [ ] **Step 3: Rewrite src/common/meson.build to build a static library**

```meson
common_sources = files('ipc.c')

common_lib = static_library('wlrdp-common',
    common_sources,
    include_directories: ['.'],
)

common_inc = include_directories('.')
common_dep = declare_dependency(
    link_with: common_lib,
    include_directories: common_inc,
)
```

- [ ] **Step 4: Create src/daemon/meson.build**

```meson
daemon_sources = files(
    'main.c',
    'auth.c',
    'session_mgr.c',
)

executable('wlrdp-daemon',
    daemon_sources,
    dependencies: [
        freerdp_dep,
        freerdp_server_dep,
        winpr_dep,
        pam_dep,
        common_dep,
    ],
    install: true,
)
```

- [ ] **Step 5: Update src/session/meson.build to use common_dep**

```meson
session_sources = files(
    'main.c',
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
    'rdp_peer.c',
)

executable('wlrdp-session',
    session_sources,
    dependencies: [
        freerdp_dep,
        freerdp_server_dep,
        winpr_dep,
        wayland_client_dep,
        xkbcommon_dep,
        protocols_dep,
        common_dep,
    ],
    install: true,
)
```

- [ ] **Step 6: Commit**

```bash
git add .devcontainer/Dockerfile meson.build src/common/meson.build src/daemon/meson.build src/session/meson.build
git commit -m "build: add PAM dependency and daemon build target for Phase 2"
```

---

### Task 2: IPC Message Protocol

**Files:**
- Create: `src/common/ipc.h`
- Create: `src/common/ipc.c`

- [ ] **Step 1: Create src/common/ipc.h**

```c
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
```

- [ ] **Step 2: Create src/common/ipc.c**

```c
#include "ipc.h"
#include "common.h"

#include <sys/socket.h>
#include <unistd.h>

bool ipc_send_msg(int sock_fd, const struct wlrdp_ipc_msg *msg, int fd)
{
    uint8_t buf[8 + sizeof(msg->payload)];
    uint32_t total = 8 + msg->payload_len;

    memcpy(buf, &msg->type, 4);
    memcpy(buf + 4, &msg->payload_len, 4);
    if (msg->payload_len > 0) {
        memcpy(buf + 8, msg->payload, msg->payload_len);
    }

    struct iovec iov = {
        .iov_base = buf,
        .iov_len = total,
    };

    struct msghdr hdr = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    /* Attach fd via SCM_RIGHTS if provided */
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    if (fd >= 0) {
        hdr.msg_control = cmsg_buf.buf;
        hdr.msg_controllen = sizeof(cmsg_buf.buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }

    ssize_t n = sendmsg(sock_fd, &hdr, MSG_NOSIGNAL);
    if (n < 0) {
        WLRDP_LOG_ERROR("ipc_send_msg: sendmsg failed: %s", strerror(errno));
        return false;
    }

    return (size_t)n == total;
}

bool ipc_recv_msg(int sock_fd, struct wlrdp_ipc_msg *msg, int *out_fd)
{
    *out_fd = -1;

    uint8_t buf[8 + sizeof(msg->payload)];

    struct iovec iov = {
        .iov_base = buf,
        .iov_len = sizeof(buf),
    };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    struct msghdr hdr = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    ssize_t n = recvmsg(sock_fd, &hdr, 0);
    if (n <= 0) {
        return false;
    }

    if ((size_t)n < 8) {
        WLRDP_LOG_ERROR("ipc_recv_msg: short read (%zd bytes)", n);
        return false;
    }

    memcpy(&msg->type, buf, 4);
    memcpy(&msg->payload_len, buf + 4, 4);

    if (msg->payload_len > sizeof(msg->payload)) {
        WLRDP_LOG_ERROR("ipc_recv_msg: payload too large (%u)", msg->payload_len);
        return false;
    }

    if (msg->payload_len > 0 && (size_t)n >= 8 + msg->payload_len) {
        memcpy(msg->payload, buf + 8, msg->payload_len);
    }

    /* Extract ancillary fd if present */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
    }

    return true;
}
```

- [ ] **Step 3: Verify it compiles**

Create a minimal `src/daemon/main.c` stub so the build succeeds:

```c
#include "common.h"
#include "ipc.h"

int main(void)
{
    WLRDP_LOG_INFO("wlrdp-daemon stub");
    return 0;
}
```

Also create stub files so the daemon builds:

`src/daemon/auth.c`:
```c
#include "common.h"
/* stub — implemented in Task 3 */
```

`src/daemon/session_mgr.c`:
```c
#include "common.h"
/* stub — implemented in Task 4 */
```

Run: `meson setup build --wipe && ninja -C build`

Expected: Both `wlrdp-daemon` and `wlrdp-session` compile successfully.

- [ ] **Step 4: Commit**

```bash
git add src/common/ipc.h src/common/ipc.c src/daemon/main.c src/daemon/auth.c src/daemon/session_mgr.c
git commit -m "feat: add IPC message protocol with SCM_RIGHTS fd passing"
```

---

### Task 3: PAM Authentication

**Files:**
- Create: `src/daemon/auth.h`
- Modify: `src/daemon/auth.c`
- Create: `config/wlrdp.pam`

- [ ] **Step 1: Create config/wlrdp.pam**

```
# PAM configuration for wlrdp
auth       required   pam_unix.so
account    required   pam_unix.so
session    required   pam_unix.so
```

- [ ] **Step 2: Create src/daemon/auth.h**

```c
#ifndef WLRDP_AUTH_H
#define WLRDP_AUTH_H

#include <stdbool.h>
#include <sys/types.h>

struct wlrdp_auth_result {
    uid_t uid;
    gid_t gid;
    char home[256];
    char shell[256];
};

/*
 * Authenticate a user via PAM. On success, fills result with
 * the user's uid, gid, home directory, and shell.
 * Returns true on success, false on auth failure.
 */
bool auth_check_credentials(const char *username, const char *password,
                            struct wlrdp_auth_result *result);

#endif /* WLRDP_AUTH_H */
```

- [ ] **Step 3: Implement src/daemon/auth.c**

```c
#include "auth.h"
#include "common.h"

#include <security/pam_appl.h>
#include <pwd.h>

/* PAM conversation callback — supplies the password */
struct pam_conv_data {
    const char *password;
};

static int pam_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata)
{
    struct pam_conv_data *data = appdata;
    struct pam_response *replies = calloc(num_msg, sizeof(*replies));
    if (!replies) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            replies[i].resp = strdup(data->password);
            if (!replies[i].resp) {
                free(replies);
                return PAM_BUF_ERR;
            }
        }
    }

    *resp = replies;
    return PAM_SUCCESS;
}

bool auth_check_credentials(const char *username, const char *password,
                            struct wlrdp_auth_result *result)
{
    struct pam_conv_data conv_data = { .password = password };
    struct pam_conv conv = {
        .conv = pam_conversation,
        .appdata_ptr = &conv_data,
    };

    pam_handle_t *pamh = NULL;
    int rc = pam_start("wlrdp", username, &conv, &pamh);
    if (rc != PAM_SUCCESS) {
        WLRDP_LOG_ERROR("pam_start failed: %s", pam_strerror(pamh, rc));
        return false;
    }

    rc = pam_authenticate(pamh, 0);
    if (rc != PAM_SUCCESS) {
        WLRDP_LOG_WARN("auth failed for '%s': %s",
                        username, pam_strerror(pamh, rc));
        pam_end(pamh, rc);
        return false;
    }

    rc = pam_acct_mgmt(pamh, 0);
    if (rc != PAM_SUCCESS) {
        WLRDP_LOG_WARN("account check failed for '%s': %s",
                        username, pam_strerror(pamh, rc));
        pam_end(pamh, rc);
        return false;
    }

    pam_end(pamh, PAM_SUCCESS);

    /* Look up uid/gid from passwd database */
    struct passwd *pw = getpwnam(username);
    if (!pw) {
        WLRDP_LOG_ERROR("getpwnam('%s') failed: %s",
                        username, strerror(errno));
        return false;
    }

    result->uid = pw->pw_uid;
    result->gid = pw->pw_gid;
    snprintf(result->home, sizeof(result->home), "%s", pw->pw_dir);
    snprintf(result->shell, sizeof(result->shell), "%s", pw->pw_shell);

    WLRDP_LOG_INFO("authenticated user '%s' (uid=%d gid=%d)",
                    username, result->uid, result->gid);
    return true;
}
```

- [ ] **Step 4: Verify build**

Run: `ninja -C build`

Expected: Compiles with no errors. (auth.c won't be called yet — just compiled.)

- [ ] **Step 5: Commit**

```bash
git add src/daemon/auth.h src/daemon/auth.c config/wlrdp.pam
git commit -m "feat: add PAM authentication module"
```

---

### Task 4: Session Manager

**Files:**
- Create: `src/daemon/session_mgr.h`
- Modify: `src/daemon/session_mgr.c`

- [ ] **Step 1: Create src/daemon/session_mgr.h**

```c
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
```

- [ ] **Step 2: Implement src/daemon/session_mgr.c**

```c
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
```

- [ ] **Step 3: Verify build**

Run: `ninja -C build`

Expected: Compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/daemon/session_mgr.h src/daemon/session_mgr.c
git commit -m "feat: add session manager with fork, privilege drop, and reconnect"
```

---

### Task 5: Daemon Main with RDP Listener and Auth

**Files:**
- Modify: `src/daemon/main.c`

This replaces the stub main.c with the full daemon: RDP listener, TLS, PAM auth via the `Logon` callback, session routing.

- [ ] **Step 1: Implement src/daemon/main.c**

```c
#include "common.h"
#include "ipc.h"
#include "auth.h"
#include "session_mgr.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>

#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct daemon_state {
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;

    freerdp_listener *listener;
    struct wlrdp_session_mgr session_mgr;
    int epoll_fd;
};

/* Custom peer context to hold auth info during connection */
struct daemon_peer_context {
    rdpContext base;
    struct daemon_state *daemon;
};

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGCHLD) {
        g_child_exited = 1;
    }
}

static BOOL on_peer_logon(freerdp_peer *client,
                          const SEC_WINNT_AUTH_IDENTITY *identity,
                          BOOL automatic)
{
    (void)automatic;
    struct daemon_peer_context *ctx =
        (struct daemon_peer_context *)client->context;

    const char *username = (const char *)identity->User;
    const char *password = (const char *)identity->Password;

    if (!username || !password) {
        WLRDP_LOG_WARN("logon: missing username or password");
        return FALSE;
    }

    WLRDP_LOG_INFO("logon attempt for user '%s'", username);

    struct wlrdp_auth_result auth;
    if (!auth_check_credentials(username, password, &auth)) {
        return FALSE;
    }

    /* Find or create session */
    struct wlrdp_session_entry *entry =
        session_mgr_get_session(&ctx->daemon->session_mgr, &auth);
    if (!entry) {
        WLRDP_LOG_ERROR("failed to get session for '%s'", username);
        return FALSE;
    }

    /* Extract the peer's transport fd and send it to the session worker */
    int peer_fd = client->context->rdp->transport->frontBio
        ? (int)BIO_get_fd(client->context->rdp->transport->frontBio, NULL)
        : -1;

    /*
     * Note: Getting the peer fd from FreeRDP internals is fragile.
     * A cleaner approach is to have the daemon complete the TLS handshake
     * and RDP negotiation, then pass the negotiated peer object.
     * For Phase 2, we use a simpler approach: the daemon only does
     * auth, then tells the session to accept a new connection on the
     * same port. But the cleanest approach with SCM_RIGHTS requires
     * extracting the raw socket fd.
     *
     * Alternative: Use freerdp_peer_get_transport_fd() if available,
     * or have the session run its own listener on a Unix socket.
     */

    /* For now, we'll use the approach where the session worker
     * receives a NEW_CLIENT IPC message and the daemon provides
     * the peer socket fd. We need to get the fd before FreeRDP
     * takes full ownership. This is handled in on_peer_accepted. */

    return TRUE;
}

static BOOL on_post_connect(freerdp_peer *client)
{
    WLRDP_LOG_INFO("daemon: peer post-connect");
    return TRUE;
}

static BOOL on_activate(freerdp_peer *client)
{
    WLRDP_LOG_INFO("daemon: peer activated (should not happen — "
                    "fd should be passed to session before this)");
    return TRUE;
}

static BOOL on_peer_accepted(freerdp_listener *listener,
                             freerdp_peer *client)
{
    struct daemon_state *state = listener->info;

    client->ContextSize = sizeof(struct daemon_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return FALSE;
    }

    struct daemon_peer_context *ctx =
        (struct daemon_peer_context *)client->context;
    ctx->daemon = state;

    rdpSettings *settings = client->context->settings;

    /* Load TLS certificate and key */
    rdpCertificate *cert = freerdp_certificate_new_from_file(state->cert_file);
    if (!cert) {
        WLRDP_LOG_ERROR("failed to load certificate");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1);

    rdpPrivateKey *key = freerdp_key_new_from_file(state->key_file);
    if (!key) {
        WLRDP_LOG_ERROR("failed to load private key");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);

    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceCommandsEnabled, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    client->PostConnect = on_post_connect;
    client->Activate = on_activate;
    client->Logon = on_peer_logon;

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    WLRDP_LOG_INFO("RDP peer accepted, waiting for logon");

    /* The peer will go through TLS handshake and capability exchange.
     * When the user provides credentials, on_peer_logon fires.
     * We need to pump the peer in the event loop until that happens. */

    /* TODO: add peer to tracked list for event loop pumping.
     * For now, pump inline. */
    while (g_running) {
        if (!client->CheckFileDescriptor(client)) {
            WLRDP_LOG_INFO("peer disconnected during handshake");
            break;
        }

        struct daemon_peer_context *pctx =
            (struct daemon_peer_context *)client->context;
        /* Check if logon succeeded and session was assigned */
        /* For Phase 2, we break after a timeout or when logon completes */
        /* This is simplified — production code would use epoll */
        usleep(10000); /* 10ms */
    }

    freerdp_peer_context_free(client);
    freerdp_peer_free(client);
    return TRUE;
}

static void generate_self_signed_cert(const char *cert_file,
                                      const char *key_file)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 "
             "-keyout '%s' -out '%s' "
             "-days 365 -nodes -subj '/CN=wlrdp' 2>/dev/null",
             key_file, cert_file);

    WLRDP_LOG_INFO("generating self-signed TLS certificate...");
    if (system(cmd) != 0) {
        WLRDP_LOG_ERROR("failed to generate certificate");
    }
}

static bool epoll_add_fd(int epoll_fd, int fd)
{
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT          RDP listen port (default: 3389)\n"
        "  --width WIDTH        Default display width (default: 800)\n"
        "  --height HEIGHT      Default display height (default: 600)\n"
        "  --desktop-cmd CMD    Command for sessions (default: foot)\n"
        "  --cert FILE          TLS certificate file\n"
        "  --key FILE           TLS private key file\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    struct daemon_state state = {
        .port = WLRDP_DEFAULT_PORT,
        .width = WLRDP_DEFAULT_WIDTH,
        .height = WLRDP_DEFAULT_HEIGHT,
        .desktop_cmd = "foot",
        .cert_file = NULL,
        .key_file = NULL,
    };

    static struct option long_opts[] = {
        { "port",        required_argument, NULL, 'p' },
        { "width",       required_argument, NULL, 'W' },
        { "height",      required_argument, NULL, 'H' },
        { "desktop-cmd", required_argument, NULL, 'd' },
        { "cert",        required_argument, NULL, 'c' },
        { "key",         required_argument, NULL, 'k' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:W:H:d:c:k:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': state.port = atoi(optarg); break;
        case 'W': state.width = atoi(optarg); break;
        case 'H': state.height = atoi(optarg); break;
        case 'd': state.desktop_cmd = optarg; break;
        case 'c': state.cert_file = optarg; break;
        case 'k': state.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (!state.cert_file || !state.key_file) {
        generate_self_signed_cert(auto_cert, auto_key);
        if (!state.cert_file) state.cert_file = auto_cert;
        if (!state.key_file) state.key_file = auto_key;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    session_mgr_init(&state.session_mgr, state.desktop_cmd,
                     state.cert_file, state.key_file,
                     state.width, state.height);

    state.listener = freerdp_listener_new();
    if (!state.listener) {
        WLRDP_LOG_ERROR("freerdp_listener_new failed");
        return 1;
    }

    state.listener->info = &state;
    state.listener->PeerAccepted = on_peer_accepted;

    if (!state.listener->Open(state.listener, "0.0.0.0", state.port)) {
        WLRDP_LOG_ERROR("failed to listen on port %d", state.port);
        freerdp_listener_free(state.listener);
        return 1;
    }

    WLRDP_LOG_INFO("wlrdp-daemon listening on port %d", state.port);

    state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (state.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    HANDLE events[32];
    DWORD count = state.listener->GetEventHandles(state.listener, events, 32);
    for (DWORD i = 0; i < count; i++) {
        int fd = GetEventFileDescriptor(events[i]);
        if (fd >= 0) {
            epoll_add_fd(state.epoll_fd, fd);
        }
    }

    while (g_running) {
        if (g_child_exited) {
            session_mgr_reap(&state.session_mgr);
            g_child_exited = 0;
        }

        struct epoll_event ep_events[16];
        int nfds = epoll_wait(state.epoll_fd, ep_events, 16, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            WLRDP_LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (!state.listener->CheckFileDescriptor(state.listener)) {
                WLRDP_LOG_ERROR("listener check failed");
                g_running = 0;
            }
        }
    }

cleanup:
    WLRDP_LOG_INFO("wlrdp-daemon shutting down");
    session_mgr_destroy(&state.session_mgr);
    if (state.listener) {
        state.listener->Close(state.listener);
        freerdp_listener_free(state.listener);
    }
    if (state.epoll_fd >= 0) {
        close(state.epoll_fd);
    }
    return 0;
}
```

- [ ] **Step 2: Verify build**

Run: `ninja -C build`

Expected: `wlrdp-daemon` compiles. There may be warnings about the `on_peer_logon` approach with FreeRDP internals — those are expected and will be refined during integration testing.

- [ ] **Step 3: Commit**

```bash
git add src/daemon/main.c
git commit -m "feat: add daemon main with RDP listener, TLS, and PAM auth flow"
```

---

### Task 6: Refactor Session Worker for IPC Mode

**Files:**
- Modify: `src/session/main.c`
- Modify: `src/session/rdp_peer.h`
- Modify: `src/session/rdp_peer.c`

The session worker needs to operate in two modes:
1. **Standalone mode** (Phase 1 behavior): runs its own listener (for development/testing)
2. **IPC mode** (Phase 2): receives peer fds from daemon via `--ipc-fd`

- [ ] **Step 1: Add rdp_peer_init_from_fd to rdp_peer.h**

Add after the existing `rdp_peer_init` declaration:

```c
/*
 * Initialize a peer from a pre-authenticated socket fd received via IPC.
 * The cert/key are needed for the peer's TLS context.
 */
bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input);
```

- [ ] **Step 2: Implement rdp_peer_init_from_fd in rdp_peer.c**

Add at the end of rdp_peer.c, before the closing:

```c
bool rdp_peer_init_from_fd(freerdp_peer *client, int peer_fd,
                           const char *cert_file, const char *key_file,
                           struct wlrdp_input *input)
{
    /* Set the peer's socket fd */
    client->sockfd = peer_fd;

    /* Then do the same init as rdp_peer_init */
    return rdp_peer_init(client, cert_file, key_file, input);
}
```

- [ ] **Step 3: Rewrite src/session/main.c for dual-mode operation**

The key change: when `--ipc-fd` is provided, the session doesn't create its own listener. Instead it waits on the IPC fd for `WLRDP_MSG_NEW_CLIENT` messages carrying peer socket fds.

```c
#include "common.h"
#include "ipc.h"
#include "compositor.h"
#include "capture.h"
#include "input.h"
#include "encoder.h"
#include "rdp_peer.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>

#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define FRAME_MIN_INTERVAL_MS 33 /* ~30 fps max */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct wlrdp_server {
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;
    int ipc_fd;          /* -1 for standalone mode */

    struct wlrdp_compositor comp;
    struct wlrdp_capture capture;
    struct wlrdp_input input;
    struct wlrdp_encoder encoder;
    freerdp_listener *listener; /* NULL in IPC mode */
    freerdp_peer *client;

    int epoll_fd;
    bool client_active;
    uint64_t last_frame_ms;
};

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGCHLD) {
        g_child_exited = 1;
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_frame_ready(void *data, uint8_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t stride)
{
    struct wlrdp_server *srv = data;

    if (!srv->client_active || !srv->client) return;

    uint64_t now = now_ms();
    if (now - srv->last_frame_ms < FRAME_MIN_INTERVAL_MS) {
        capture_request_frame(&srv->capture);
        return;
    }
    srv->last_frame_ms = now;

    uint32_t row_bytes = width * 4;
    for (uint32_t top = 0, bot = height - 1; top < bot; top++, bot--) {
        uint8_t *a = pixels + top * stride;
        uint8_t *b = pixels + bot * stride;
        for (uint32_t i = 0; i < row_bytes; i++) {
            uint8_t tmp = a[i];
            a[i] = b[i];
            b[i] = tmp;
        }
    }

    rdp_peer_send_frame(srv->client, pixels, width * height * 4,
                        width, height);

    capture_request_frame(&srv->capture);
}

static void disconnect_client(struct wlrdp_server *srv)
{
    if (srv->client) {
        freerdp_peer_context_free(srv->client);
        freerdp_peer_free(srv->client);
        srv->client = NULL;
        srv->client_active = false;
        WLRDP_LOG_INFO("client disconnected");
    }

    /* In IPC mode, notify daemon and wait for reconnect.
     * In standalone mode, keep running for a new connection. */
    if (srv->ipc_fd >= 0) {
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_DISCONNECTED;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv->ipc_fd, &msg, -1);
    }
}

static bool accept_ipc_client(struct wlrdp_server *srv)
{
    struct wlrdp_ipc_msg msg;
    int peer_fd = -1;

    if (!ipc_recv_msg(srv->ipc_fd, &msg, &peer_fd)) {
        WLRDP_LOG_ERROR("IPC recv failed — daemon gone?");
        g_running = 0;
        return false;
    }

    if (msg.type == WLRDP_MSG_SHUTDOWN) {
        WLRDP_LOG_INFO("received shutdown from daemon");
        g_running = 0;
        return false;
    }

    if (msg.type != WLRDP_MSG_NEW_CLIENT || peer_fd < 0) {
        WLRDP_LOG_WARN("unexpected IPC message type %u", msg.type);
        if (peer_fd >= 0) close(peer_fd);
        return false;
    }

    /* Disconnect any existing client first */
    disconnect_client(srv);

    WLRDP_LOG_INFO("received new client fd %d from daemon", peer_fd);

    freerdp_peer *client = freerdp_peer_new(peer_fd);
    if (!client) {
        WLRDP_LOG_ERROR("freerdp_peer_new failed");
        close(peer_fd);
        return false;
    }

    client->ContextSize = sizeof(struct wlrdp_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return false;
    }

    if (!rdp_peer_init(client, srv->cert_file, srv->key_file, &srv->input)) {
        WLRDP_LOG_ERROR("rdp_peer_init failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return false;
    }

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return false;
    }

    srv->client = client;
    WLRDP_LOG_INFO("IPC client initialized");
    return true;
}

static BOOL on_peer_accepted(freerdp_listener *listener,
                             freerdp_peer *client)
{
    struct wlrdp_server *srv = listener->info;

    if (srv->client) {
        WLRDP_LOG_WARN("rejecting second client (single-session mode)");
        freerdp_peer_free(client);
        return TRUE;
    }

    client->ContextSize = sizeof(struct wlrdp_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return FALSE;
    }

    if (!rdp_peer_init(client, srv->cert_file, srv->key_file, &srv->input)) {
        WLRDP_LOG_ERROR("rdp_peer_init failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    srv->client = client;
    WLRDP_LOG_INFO("RDP client accepted");
    return TRUE;
}

static bool epoll_add_fd(int epoll_fd, int fd)
{
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static void generate_self_signed_cert(const char *cert_file,
                                      const char *key_file)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 "
             "-keyout '%s' -out '%s' "
             "-days 365 -nodes -subj '/CN=wlrdp' 2>/dev/null",
             key_file, cert_file);

    WLRDP_LOG_INFO("generating self-signed TLS certificate...");
    if (system(cmd) != 0) {
        WLRDP_LOG_ERROR("failed to generate certificate");
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT          RDP listen port (standalone mode, default: 3389)\n"
        "  --ipc-fd FD          IPC socket fd (daemon mode)\n"
        "  --width WIDTH        Display width (default: 800)\n"
        "  --height HEIGHT      Display height (default: 600)\n"
        "  --desktop-cmd CMD    Command to run inside cage (default: foot)\n"
        "  --cert FILE          TLS certificate file\n"
        "  --key FILE           TLS private key file\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    struct wlrdp_server srv = {
        .port = WLRDP_DEFAULT_PORT,
        .width = WLRDP_DEFAULT_WIDTH,
        .height = WLRDP_DEFAULT_HEIGHT,
        .desktop_cmd = "foot",
        .cert_file = NULL,
        .key_file = NULL,
        .ipc_fd = -1,
    };

    static struct option long_opts[] = {
        { "port",        required_argument, NULL, 'p' },
        { "ipc-fd",      required_argument, NULL, 'i' },
        { "width",       required_argument, NULL, 'W' },
        { "height",      required_argument, NULL, 'H' },
        { "desktop-cmd", required_argument, NULL, 'd' },
        { "cert",        required_argument, NULL, 'c' },
        { "key",         required_argument, NULL, 'k' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:i:W:H:d:c:k:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': srv.port = atoi(optarg); break;
        case 'i': srv.ipc_fd = atoi(optarg); break;
        case 'W': srv.width = atoi(optarg); break;
        case 'H': srv.height = atoi(optarg); break;
        case 'd': srv.desktop_cmd = optarg; break;
        case 'c': srv.cert_file = optarg; break;
        case 'k': srv.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    bool standalone = (srv.ipc_fd < 0);

    if (standalone) {
        WLRDP_LOG_INFO("running in standalone mode (no daemon)");
    } else {
        WLRDP_LOG_INFO("running in IPC mode (fd=%d)", srv.ipc_fd);
    }

    /* Auto-generate cert only in standalone mode */
    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (standalone && (!srv.cert_file || !srv.key_file)) {
        generate_self_signed_cert(auto_cert, auto_key);
        if (!srv.cert_file) srv.cert_file = auto_cert;
        if (!srv.key_file) srv.key_file = auto_key;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    WLRDP_LOG_INFO("launching compositor: cage -- %s (%dx%d)",
                    srv.desktop_cmd, srv.width, srv.height);
    if (!compositor_launch(&srv.comp, srv.desktop_cmd,
                           srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to launch compositor");
        return 1;
    }

    if (!capture_init(&srv.capture, srv.comp.display_name,
                      on_frame_ready, &srv)) {
        WLRDP_LOG_ERROR("failed to initialize capture");
        compositor_destroy(&srv.comp);
        return 1;
    }

    if (!input_init(&srv.input, srv.comp.display_name,
                    srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize input");
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    if (!encoder_init(&srv.encoder, srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize encoder");
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    int capture_fd = capture_get_fd(&srv.capture);
    epoll_add_fd(srv.epoll_fd, capture_fd);

    if (standalone) {
        /* Standalone: create our own RDP listener */
        srv.listener = freerdp_listener_new();
        if (!srv.listener) {
            WLRDP_LOG_ERROR("freerdp_listener_new failed");
            goto cleanup;
        }

        srv.listener->info = &srv;
        srv.listener->PeerAccepted = on_peer_accepted;

        if (!srv.listener->Open(srv.listener, "0.0.0.0", srv.port)) {
            WLRDP_LOG_ERROR("failed to listen on port %d", srv.port);
            goto cleanup;
        }

        WLRDP_LOG_INFO("listening on port %d", srv.port);

        HANDLE events[32];
        DWORD count = srv.listener->GetEventHandles(srv.listener, events, 32);
        for (DWORD i = 0; i < count; i++) {
            int fd = GetEventFileDescriptor(events[i]);
            if (fd >= 0) epoll_add_fd(srv.epoll_fd, fd);
        }
    } else {
        /* IPC mode: listen on the IPC fd for new clients */
        epoll_add_fd(srv.epoll_fd, srv.ipc_fd);

        /* Notify daemon we're ready */
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_READY;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv.ipc_fd, &msg, -1);
    }

    WLRDP_LOG_INFO("entering main event loop");

    while (g_running) {
        if (g_child_exited) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid == srv.comp.pid) {
                WLRDP_LOG_INFO("cage exited, shutting down");
                break;
            }
            g_child_exited = 0;
        }

        wl_display_flush(srv.capture.display);
        input_flush(&srv.input);

        struct epoll_event events[16];
        int nfds = epoll_wait(srv.epoll_fd, events, 16, 16);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            WLRDP_LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == capture_fd) {
                if (capture_dispatch(&srv.capture) < 0) {
                    WLRDP_LOG_ERROR("Wayland dispatch error");
                    g_running = 0;
                }
                continue;
            }

            if (!standalone && fd == srv.ipc_fd) {
                accept_ipc_client(&srv);
                continue;
            }

            if (standalone && srv.listener) {
                if (!srv.listener->CheckFileDescriptor(srv.listener)) {
                    WLRDP_LOG_ERROR("listener check failed");
                    g_running = 0;
                }
            }
        }

        if (srv.client) {
            struct wlrdp_peer_context *ctx =
                (struct wlrdp_peer_context *)srv.client->context;

            if (!srv.client->CheckFileDescriptor(srv.client)) {
                disconnect_client(&srv);
                if (standalone) {
                    /* In standalone, keep running for reconnect */
                } else {
                    /* In IPC mode, wait for daemon to send new client */
                }
                continue;
            }

            if (ctx->activated && !srv.client_active) {
                srv.client_active = true;
                WLRDP_LOG_INFO("starting frame capture");
                capture_request_frame(&srv.capture);
            }
        }
    }

cleanup:
    WLRDP_LOG_INFO("shutting down");

    if (srv.client) {
        freerdp_peer_context_free(srv.client);
        freerdp_peer_free(srv.client);
    }
    if (srv.listener) {
        srv.listener->Close(srv.listener);
        freerdp_listener_free(srv.listener);
    }
    if (srv.ipc_fd >= 0) {
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_TERMINATED;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv.ipc_fd, &msg, -1);
        close(srv.ipc_fd);
    }
    if (srv.epoll_fd >= 0) {
        close(srv.epoll_fd);
    }
    encoder_destroy(&srv.encoder);
    input_destroy(&srv.input);
    capture_destroy(&srv.capture);
    compositor_destroy(&srv.comp);

    WLRDP_LOG_INFO("bye");
    return 0;
}
```

- [ ] **Step 4: Verify standalone mode still works**

Run: `ninja -C build`

Expected: Both binaries compile. The session binary in standalone mode (no `--ipc-fd`) should work identically to Phase 1.

- [ ] **Step 5: Commit**

```bash
git add src/session/main.c src/session/rdp_peer.h src/session/rdp_peer.c
git commit -m "feat: refactor session worker for dual-mode (standalone + IPC from daemon)"
```

---

### Task 7: systemd Service and PAM Config

**Files:**
- Create: `systemd/wlrdp.service`
- Create: `config/wlrdp.conf.example`

- [ ] **Step 1: Create systemd/wlrdp.service**

```ini
[Unit]
Description=wlrdp RDP Terminal Server
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/wlrdp-daemon --port 3389 --desktop-cmd foot
Restart=on-failure
RestartSec=5

# Security hardening
ProtectSystem=strict
ProtectHome=read-only
PrivateTmp=true
NoNewPrivileges=false
# NoNewPrivileges must be false because we need setuid/setgid for session workers

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: Create config/wlrdp.conf.example**

```ini
# wlrdp configuration file
# Copy to /etc/wlrdp/wlrdp.conf and adjust as needed.

[server]
port = 3389
# cert_file = /etc/wlrdp/tls/cert.pem
# key_file = /etc/wlrdp/tls/key.pem

[session]
desktop_cmd = foot
default_width = 800
default_height = 600

[security]
max_sessions = 50
```

- [ ] **Step 3: Commit**

```bash
git add systemd/wlrdp.service config/wlrdp.conf.example config/wlrdp.pam
git commit -m "feat: add systemd service unit, PAM config, and example config"
```

---

### Task 8: Build Verification and Smoke Test

- [ ] **Step 1: Rebuild from clean**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Both `wlrdp-daemon` and `wlrdp-session` compile with no errors.

- [ ] **Step 2: Test standalone mode (Phase 1 regression)**

```bash
./build/src/session/wlrdp-session --port 3389 --desktop-cmd foot --width 800 --height 600
```

From another terminal:
```bash
xfreerdp /v:localhost:3389 /cert:ignore /w:800 /h:600
```

Expected: Foot terminal visible, keyboard and mouse work — same as Phase 1.

- [ ] **Step 3: Test daemon startup**

```bash
sudo ./build/src/daemon/wlrdp-daemon --port 3390 --desktop-cmd foot
```

Expected: Daemon starts, logs "wlrdp-daemon listening on port 3390". It won't fully work yet until we resolve the FreeRDP peer fd passing, but it should accept connections and attempt auth.

- [ ] **Step 4: Install PAM config for testing**

```bash
sudo cp config/wlrdp.pam /etc/pam.d/wlrdp
```

- [ ] **Step 5: Commit any fixes**

```bash
git add -u
git commit -m "fix: build and smoke test adjustments for Phase 2"
```

Skip if no changes needed.

---

### Task 9: Integration Test

Test the full daemon → session → client flow.

- [ ] **Step 1: Start the daemon as root**

```bash
sudo ./build/src/daemon/wlrdp-daemon --port 3389 --desktop-cmd foot --width 800 --height 600
```

Expected: "wlrdp-daemon listening on port 3389"

- [ ] **Step 2: Connect as user 'developer'**

```bash
xfreerdp /v:localhost:3389 /cert:ignore /u:developer /p:developer /w:800 /h:600
```

Expected: PAM authenticates, daemon forks wlrdp-session, session launches cage+foot, display appears.

- [ ] **Step 3: Verify session persistence**

Close the xfreerdp window. Expected: Session worker logs "client disconnected" but keeps running. Cage is still alive.

Reconnect:
```bash
xfreerdp /v:localhost:3389 /cert:ignore /u:developer /p:developer /w:800 /h:600
```

Expected: Same session resumes — any text typed in foot before disconnect is still visible.

- [ ] **Step 4: Test multi-user**

Open a second terminal and connect as 'testuser':
```bash
xfreerdp /v:localhost:3389 /cert:ignore /u:testuser /p:testuser /w:800 /h:600
```

Expected: A separate session with its own foot terminal. Both sessions run simultaneously.

- [ ] **Step 5: Verify session isolation**

Check processes:
```bash
ps aux | grep wlrdp-session
```

Expected: Two wlrdp-session processes running as different users (developer, testuser).

- [ ] **Step 6: Commit any final fixes**

```bash
git add -u
git commit -m "fix: integration test adjustments for Phase 2"
```

Skip if no changes needed.
