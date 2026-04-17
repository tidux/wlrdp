# wlrdp Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a minimal single-process RDP server that launches a nested cage Wayland compositor and streams its display to an RDP client with keyboard/mouse input.

**Architecture:** A single `wlrdp-session` binary launches cage (wlroots kiosk compositor) in headless mode, captures frames via wlr-screencopy protocol, encodes them with NSCodec, and sends them over FreeRDP's SurfaceBits interface. RDP input events are translated to Wayland virtual pointer/keyboard protocol calls injected into cage.

**Tech Stack:** C11, Meson, FreeRDP 3.x (libfreerdp-server3), libwayland-client, wlr-protocols (screencopy, virtual-pointer, virtual-keyboard), libxkbcommon, OpenSSL, cage

**Spec:** `docs/superpowers/specs/2026-04-16-wlrdp-design.md`

---

## File Map

| File | Responsibility |
|------|---------------|
| `.devcontainer/Dockerfile` | Fedora 43 dev container with all build deps |
| `.devcontainer/devcontainer.json` | DevContainer configuration |
| `meson.build` | Top-level build: project, deps, subdirs |
| `meson_options.txt` | Build options (future phases) |
| `src/common/meson.build` | Builds libwlrdp-common.a |
| `src/common/common.h` | Log macros, shared types, error codes |
| `src/protocols/meson.build` | wayland-scanner generation rules |
| `src/protocols/wlr-screencopy-unstable-v1.xml` | Screen capture protocol definition |
| `src/protocols/wlr-virtual-pointer-unstable-v1.xml` | Virtual pointer protocol definition |
| `src/protocols/virtual-keyboard-unstable-v1.xml` | Virtual keyboard protocol definition |
| `src/session/meson.build` | Builds wlrdp-session binary |
| `src/session/compositor.h` | Compositor launcher interface |
| `src/session/compositor.c` | Fork+exec cage with headless backend |
| `src/session/capture.h` | Screen capture interface |
| `src/session/capture.c` | wlr-screencopy client, frame management |
| `src/session/input.h` | Input injector interface |
| `src/session/input.c` | Virtual pointer + keyboard injection |
| `src/session/encoder.h` | Frame encoder interface |
| `src/session/encoder.c` | NSCodec encoding via FreeRDP |
| `src/session/rdp_peer.h` | RDP peer handler interface |
| `src/session/rdp_peer.c` | freerdp_peer lifecycle, frame send, input dispatch |
| `src/session/main.c` | Entry point, CLI args, event loop, orchestration |

---

### Task 1: DevContainer and Project Scaffolding

**Files:**
- Create: `.devcontainer/Dockerfile`
- Create: `.devcontainer/devcontainer.json`
- Create: `meson.build`
- Create: `meson_options.txt`
- Create: `src/common/meson.build`
- Create: `src/common/common.h`
- Create: `src/protocols/meson.build`
- Create: `src/session/meson.build`

- [ ] **Step 1: Create DevContainer Dockerfile**

Create `.devcontainer/Dockerfile`:

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
    cage \
    foot \
    xkeyboard-config \
    git \
    gdb \
    && dnf clean all

RUN useradd -m developer
USER developer
WORKDIR /workspace
```

- [ ] **Step 2: Create devcontainer.json**

Create `.devcontainer/devcontainer.json`:

```json
{
    "name": "wlrdp",
    "build": {
        "dockerfile": "Dockerfile",
        "context": ".."
    },
    "workspaceFolder": "/workspace",
    "workspaceMount": "source=${localWorkspaceFolder},target=/workspace,type=bind",
    "customizations": {
        "vscode": {
            "extensions": [
                "ms-vscode.cpptools",
                "mesonbuild.mesonbuild"
            ],
            "settings": {
                "C_Cpp.default.compilerPath": "/usr/bin/gcc",
                "C_Cpp.default.cStandard": "c11"
            }
        }
    },
    "remoteUser": "developer"
}
```

- [ ] **Step 3: Create top-level meson.build**

Create `meson.build`:

```meson
project('wlrdp', 'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ],
)

freerdp_dep = dependency('freerdp3', version: '>= 3.0')
freerdp_server_dep = dependency('freerdp-server3', version: '>= 3.0')
winpr_dep = dependency('winpr3', version: '>= 3.0')
wayland_client_dep = dependency('wayland-client')
wayland_scanner_dep = dependency('wayland-scanner', native: true)
xkbcommon_dep = dependency('xkbcommon')

wayland_scanner = find_program(
    wayland_scanner_dep.get_variable('wayland_scanner'),
)

subdir('src/protocols')
subdir('src/common')
subdir('src/session')
```

- [ ] **Step 4: Create meson_options.txt**

Create `meson_options.txt`:

```meson
option('enable-h264', type: 'feature', value: 'disabled',
    description: 'H.264 encoding via FFmpeg (Phase 3)')
option('enable-pipewire', type: 'feature', value: 'disabled',
    description: 'PipeWire capture and audio (Phase 3+)')
```

- [ ] **Step 5: Create src/common/common.h**

Create `src/common/common.h`:

```c
#ifndef WLRDP_COMMON_H
#define WLRDP_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define WLRDP_LOG_INFO(fmt, ...) \
    fprintf(stderr, "[wlrdp] INFO: " fmt "\n", ##__VA_ARGS__)
#define WLRDP_LOG_WARN(fmt, ...) \
    fprintf(stderr, "[wlrdp] WARN: " fmt "\n", ##__VA_ARGS__)
#define WLRDP_LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[wlrdp] ERROR: " fmt "\n", ##__VA_ARGS__)

#define WLRDP_DEFAULT_PORT 3389
#define WLRDP_DEFAULT_WIDTH 1920
#define WLRDP_DEFAULT_HEIGHT 1080

#endif /* WLRDP_COMMON_H */
```

- [ ] **Step 6: Create src/common/meson.build**

Create `src/common/meson.build`:

```meson
common_inc = include_directories('.')
```

- [ ] **Step 7: Create src/protocols/meson.build**

Create `src/protocols/meson.build`:

```meson
protocols = [
    'wlr-screencopy-unstable-v1',
    'wlr-virtual-pointer-unstable-v1',
    'virtual-keyboard-unstable-v1',
]

protocol_sources = []
protocol_headers = []

foreach p : protocols
    xml = files(p + '.xml')
    protocol_sources += custom_target(
        p + '-protocol-code',
        input: xml,
        output: p + '-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
    protocol_headers += custom_target(
        p + '-client-header',
        input: xml,
        output: p + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
endforeach

protocols_lib = static_library('wlrdp-protocols',
    protocol_sources + protocol_headers,
    dependencies: [wayland_client_dep],
)

protocols_dep = declare_dependency(
    link_with: protocols_lib,
    sources: protocol_headers,
)
```

- [ ] **Step 8: Create src/session/meson.build (stub)**

Create `src/session/meson.build`:

```meson
session_sources = []

# Populated by subsequent tasks; starts empty so the build system is valid.
```

- [ ] **Step 9: Download Wayland protocol XML files**

Download the three protocol XML files from the wlr-protocols and wayland-protocols repositories into `src/protocols/`:

```bash
# wlr-screencopy
curl -Lo src/protocols/wlr-screencopy-unstable-v1.xml \
    'https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-screencopy-unstable-v1.xml'

# wlr-virtual-pointer
curl -Lo src/protocols/wlr-virtual-pointer-unstable-v1.xml \
    'https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-virtual-pointer-unstable-v1.xml'

# virtual-keyboard (from wayland-protocols or wlroots)
curl -Lo src/protocols/virtual-keyboard-unstable-v1.xml \
    'https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/virtual-keyboard-unstable-v1.xml'
```

- [ ] **Step 10: Verify the build system configures**

```bash
meson setup build
```

Expected: Meson finds all dependencies, configures successfully. No compilation yet (no C sources to compile).

- [ ] **Step 11: Commit**

```bash
git add .devcontainer/ meson.build meson_options.txt src/
git commit -m "feat: project scaffolding with DevContainer, Meson build, and protocol XMLs"
```

---

### Task 2: Compositor Launcher

**Files:**
- Create: `src/session/compositor.h`
- Create: `src/session/compositor.c`
- Modify: `src/session/meson.build`

- [ ] **Step 1: Create src/session/compositor.h**

```c
#ifndef WLRDP_COMPOSITOR_H
#define WLRDP_COMPOSITOR_H

#include <sys/types.h>
#include <stdbool.h>

struct wlrdp_compositor {
    pid_t pid;
    char display_name[64];
};

/*
 * Launch cage in headless mode running desktop_cmd at the given resolution.
 * Blocks until the Wayland display socket appears (up to 5 seconds).
 * Returns true on success, false on failure.
 */
bool compositor_launch(struct wlrdp_compositor *comp,
                       const char *desktop_cmd,
                       int width, int height);

/*
 * Kill cage and wait for it to exit.
 */
void compositor_destroy(struct wlrdp_compositor *comp);

#endif /* WLRDP_COMPOSITOR_H */
```

- [ ] **Step 2: Create src/session/compositor.c**

```c
#include "compositor.h"
#include "common.h"

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

static bool wait_for_socket(const char *display_name, int timeout_ms)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) {
        WLRDP_LOG_ERROR("XDG_RUNTIME_DIR not set");
        return false;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", xdg, display_name);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000 }; /* 50ms */
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        if (access(path, F_OK) == 0) {
            return true;
        }
        nanosleep(&ts, NULL);
        elapsed += 50;
    }

    WLRDP_LOG_ERROR("timed out waiting for %s", path);
    return false;
}

bool compositor_launch(struct wlrdp_compositor *comp,
                       const char *desktop_cmd,
                       int width, int height)
{
    snprintf(comp->display_name, sizeof(comp->display_name),
             "wlrdp-%d", getpid());

    char mode[32];
    snprintf(mode, sizeof(mode), "%dx%d", width, height);

    pid_t pid = fork();
    if (pid < 0) {
        WLRDP_LOG_ERROR("fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        /* Child: exec cage */
        setenv("WLR_BACKENDS", "headless", 1);
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
        setenv("WAYLAND_DISPLAY", comp->display_name, 1);

        /* Set headless output resolution via WLR_HEADLESS_OUTPUT_MODE
         * if supported, otherwise cage uses its default. */
        setenv("WLR_HEADLESS_OUTPUT_MODE", mode, 1);

        execlp("cage", "cage", "--", desktop_cmd, NULL);
        /* exec failed */
        fprintf(stderr, "[wlrdp] ERROR: exec cage failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    comp->pid = pid;
    WLRDP_LOG_INFO("launched cage (pid %d) on %s at %s",
                    pid, comp->display_name, mode);

    if (!wait_for_socket(comp->display_name, 5000)) {
        compositor_destroy(comp);
        return false;
    }

    return true;
}

void compositor_destroy(struct wlrdp_compositor *comp)
{
    if (comp->pid <= 0) {
        return;
    }

    kill(comp->pid, SIGTERM);

    /* Give it 2 seconds to exit gracefully */
    int status;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000 };
    for (int i = 0; i < 20; i++) {
        if (waitpid(comp->pid, &status, WNOHANG) > 0) {
            goto done;
        }
        nanosleep(&ts, NULL);
    }

    /* Force kill */
    kill(comp->pid, SIGKILL);
    waitpid(comp->pid, &status, 0);

done:
    WLRDP_LOG_INFO("cage (pid %d) exited", comp->pid);
    comp->pid = 0;
}
```

- [ ] **Step 3: Update src/session/meson.build**

Replace `src/session/meson.build` with:

```meson
session_sources = files(
    'compositor.c',
)
```

- [ ] **Step 4: Verify it compiles**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Compiles without errors. No binary yet (no main.c).

- [ ] **Step 5: Commit**

```bash
git add src/session/compositor.h src/session/compositor.c src/session/meson.build
git commit -m "feat: add compositor launcher (cage fork+exec in headless mode)"
```

---

### Task 3: Screen Capture

**Files:**
- Create: `src/session/capture.h`
- Create: `src/session/capture.c`
- Modify: `src/session/meson.build`

- [ ] **Step 1: Create src/session/capture.h**

```c
#ifndef WLRDP_CAPTURE_H
#define WLRDP_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

/* Forward declarations for generated protocol types */
struct zwlr_screencopy_manager_v1;
struct zwlr_screencopy_frame_v1;

/* Called when a frame is ready. pixels points to XRGB8888 data. */
typedef void (*capture_frame_cb)(void *data, uint8_t *pixels,
                                 uint32_t width, uint32_t height,
                                 uint32_t stride);

struct wlrdp_capture {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_output *output;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;

    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    uint8_t *pixels;       /* mmap'd shm buffer */
    int shm_fd;
    uint32_t buf_size;

    uint32_t width;
    uint32_t height;
    uint32_t stride;

    bool capturing;
    bool frame_ready;

    capture_frame_cb on_frame;
    void *on_frame_data;
};

/*
 * Connect to the Wayland display and bind screencopy, output, and shm.
 * display_name is the WAYLAND_DISPLAY of cage.
 */
bool capture_init(struct wlrdp_capture *cap, const char *display_name,
                  capture_frame_cb cb, void *cb_data);

/*
 * Request the next frame. Non-blocking; the frame arrives via the
 * Wayland event loop (call wl_display_dispatch or poll the fd).
 */
bool capture_request_frame(struct wlrdp_capture *cap);

/*
 * Get the Wayland display fd for polling.
 */
int capture_get_fd(struct wlrdp_capture *cap);

/*
 * Dispatch pending Wayland events. Call when the fd is readable.
 * Returns -1 on error.
 */
int capture_dispatch(struct wlrdp_capture *cap);

void capture_destroy(struct wlrdp_capture *cap);

#endif /* WLRDP_CAPTURE_H */
```

- [ ] **Step 2: Create src/session/capture.c**

```c
#include "capture.h"
#include "common.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

static int create_shm_file(size_t size)
{
    char name[] = "/wlrdp-shm-XXXXXX";
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    shm_unlink(name);

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool alloc_buffer(struct wlrdp_capture *cap,
                         uint32_t width, uint32_t height, uint32_t stride)
{
    /* Clean up old buffer if resizing */
    if (cap->buffer) {
        wl_buffer_destroy(cap->buffer);
        cap->buffer = NULL;
    }
    if (cap->pool) {
        wl_shm_pool_destroy(cap->pool);
        cap->pool = NULL;
    }
    if (cap->pixels) {
        munmap(cap->pixels, cap->buf_size);
        cap->pixels = NULL;
    }
    if (cap->shm_fd >= 0) {
        close(cap->shm_fd);
    }

    cap->width = width;
    cap->height = height;
    cap->stride = stride;
    cap->buf_size = stride * height;

    cap->shm_fd = create_shm_file(cap->buf_size);
    if (cap->shm_fd < 0) {
        WLRDP_LOG_ERROR("failed to create shm file: %s", strerror(errno));
        return false;
    }

    cap->pixels = mmap(NULL, cap->buf_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, cap->shm_fd, 0);
    if (cap->pixels == MAP_FAILED) {
        WLRDP_LOG_ERROR("mmap failed: %s", strerror(errno));
        cap->pixels = NULL;
        close(cap->shm_fd);
        return false;
    }

    cap->pool = wl_shm_pool_create(cap->shm, cap->shm_fd, cap->buf_size);
    cap->buffer = wl_shm_pool_create_buffer(cap->pool, 0,
                                             width, height, stride,
                                             WL_SHM_FORMAT_XRGB8888);
    return true;
}

/* --- screencopy frame callbacks --- */

static void frame_buffer(void *data,
                         struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width,
                         uint32_t height, uint32_t stride)
{
    struct wlrdp_capture *cap = data;

    if (format != WL_SHM_FORMAT_XRGB8888) {
        WLRDP_LOG_WARN("unexpected shm format 0x%x, expected XRGB8888",
                        format);
    }

    if (width != cap->width || height != cap->height ||
        stride != cap->stride) {
        if (!alloc_buffer(cap, width, height, stride)) {
            WLRDP_LOG_ERROR("failed to reallocate capture buffer");
            zwlr_screencopy_frame_v1_destroy(frame);
            return;
        }
    }
}

static void frame_linux_dmabuf(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width,
                               uint32_t height)
{
    /* Phase 1: ignore DMA-BUF, we use shm */
    (void)data; (void)frame; (void)format;
    (void)width; (void)height;
}

static void frame_buffer_done(void *data,
                              struct zwlr_screencopy_frame_v1 *frame)
{
    struct wlrdp_capture *cap = data;
    zwlr_screencopy_frame_v1_copy(frame, cap->buffer);
}

static void frame_flags(void *data,
                        struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags)
{
    (void)data; (void)frame; (void)flags;
    /* flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT means
     * the image is y-flipped. We ignore this for now. */
}

static void frame_ready(void *data,
                        struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                        uint32_t tv_nsec)
{
    struct wlrdp_capture *cap = data;
    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;

    zwlr_screencopy_frame_v1_destroy(frame);

    if (cap->on_frame) {
        cap->on_frame(cap->on_frame_data, cap->pixels,
                      cap->width, cap->height, cap->stride);
    }
}

static void frame_failed(void *data,
                         struct zwlr_screencopy_frame_v1 *frame)
{
    struct wlrdp_capture *cap = data;
    (void)cap;
    WLRDP_LOG_WARN("screencopy frame capture failed");
    zwlr_screencopy_frame_v1_destroy(frame);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
};

/* --- registry callbacks --- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct wlrdp_capture *cap = data;

    if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0) {
        cap->screencopy_mgr = wl_registry_bind(registry, name,
            &zwlr_screencopy_manager_v1_interface, 3);
    } else if (strcmp(interface, "wl_output") == 0) {
        if (!cap->output) {
            cap->output = wl_registry_bind(registry, name,
                &wl_output_interface, 1);
        }
    } else if (strcmp(interface, "wl_shm") == 0) {
        cap->shm = wl_registry_bind(registry, name,
            &wl_shm_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- public API --- */

bool capture_init(struct wlrdp_capture *cap, const char *display_name,
                  capture_frame_cb cb, void *cb_data)
{
    memset(cap, 0, sizeof(*cap));
    cap->shm_fd = -1;
    cap->on_frame = cb;
    cap->on_frame_data = cb_data;

    cap->display = wl_display_connect(display_name);
    if (!cap->display) {
        WLRDP_LOG_ERROR("failed to connect to Wayland display '%s'",
                        display_name);
        return false;
    }

    cap->registry = wl_display_get_registry(cap->display);
    wl_registry_add_listener(cap->registry, &registry_listener, cap);

    /* Round-trip to bind globals */
    wl_display_roundtrip(cap->display);

    if (!cap->screencopy_mgr) {
        WLRDP_LOG_ERROR("compositor does not support wlr-screencopy-v1");
        capture_destroy(cap);
        return false;
    }
    if (!cap->output) {
        WLRDP_LOG_ERROR("no wl_output found");
        capture_destroy(cap);
        return false;
    }
    if (!cap->shm) {
        WLRDP_LOG_ERROR("no wl_shm found");
        capture_destroy(cap);
        return false;
    }

    WLRDP_LOG_INFO("capture initialized on display '%s'", display_name);
    return true;
}

bool capture_request_frame(struct wlrdp_capture *cap)
{
    struct zwlr_screencopy_frame_v1 *frame;
    frame = zwlr_screencopy_manager_v1_capture_output(
        cap->screencopy_mgr, 0, cap->output);
    if (!frame) {
        WLRDP_LOG_ERROR("failed to create screencopy frame");
        return false;
    }

    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, cap);
    wl_display_flush(cap->display);
    return true;
}

int capture_get_fd(struct wlrdp_capture *cap)
{
    return wl_display_get_fd(cap->display);
}

int capture_dispatch(struct wlrdp_capture *cap)
{
    return wl_display_dispatch(cap->display);
}

void capture_destroy(struct wlrdp_capture *cap)
{
    if (cap->buffer) {
        wl_buffer_destroy(cap->buffer);
    }
    if (cap->pool) {
        wl_shm_pool_destroy(cap->pool);
    }
    if (cap->pixels) {
        munmap(cap->pixels, cap->buf_size);
    }
    if (cap->shm_fd >= 0) {
        close(cap->shm_fd);
    }
    if (cap->screencopy_mgr) {
        zwlr_screencopy_manager_v1_destroy(cap->screencopy_mgr);
    }
    if (cap->output) {
        wl_output_destroy(cap->output);
    }
    if (cap->shm) {
        wl_shm_destroy(cap->shm);
    }
    if (cap->registry) {
        wl_registry_destroy(cap->registry);
    }
    if (cap->display) {
        wl_display_disconnect(cap->display);
    }
    memset(cap, 0, sizeof(*cap));
    cap->shm_fd = -1;
}
```

- [ ] **Step 3: Update src/session/meson.build**

```meson
session_sources = files(
    'compositor.c',
    'capture.c',
)
```

- [ ] **Step 4: Verify it compiles**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Compiles without errors. Generated protocol headers are picked up.

- [ ] **Step 5: Commit**

```bash
git add src/session/capture.h src/session/capture.c src/session/meson.build
git commit -m "feat: add screen capture via wlr-screencopy-v1 protocol"
```

---

### Task 4: Input Injector

**Files:**
- Create: `src/session/input.h`
- Create: `src/session/input.c`
- Modify: `src/session/meson.build`

- [ ] **Step 1: Create src/session/input.h**

```c
#ifndef WLRDP_INPUT_H
#define WLRDP_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_v1;

struct wlrdp_input {
    struct wl_display *display;  /* borrowed from capture or own connection */
    struct wl_registry *registry;
    struct wl_seat *seat;

    /* Virtual pointer manager + instance */
    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zwlr_virtual_pointer_v1 *vptr;

    /* Virtual keyboard manager + instance */
    struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
    struct zwp_virtual_keyboard_v1 *vkbd;

    uint32_t width;
    uint32_t height;
};

/*
 * Connect to the Wayland display and bind virtual input protocols.
 * Uses its own wl_display connection to display_name.
 */
bool input_init(struct wlrdp_input *inp, const char *display_name,
                uint32_t width, uint32_t height);

/*
 * Inject a pointer motion (absolute coordinates).
 */
void input_pointer_motion(struct wlrdp_input *inp, uint32_t x, uint32_t y);

/*
 * Inject a pointer button press/release.
 * button: Linux input event code (BTN_LEFT, BTN_RIGHT, etc.)
 */
void input_pointer_button(struct wlrdp_input *inp, uint32_t button,
                          bool pressed);

/*
 * Inject a pointer axis (scroll wheel) event.
 * value: scroll amount in Wayland axis units (positive = scroll down).
 */
void input_pointer_axis(struct wlrdp_input *inp, int32_t value);

/*
 * Inject a keyboard key press/release.
 * key: evdev keycode (e.g., KEY_A = 30)
 */
void input_keyboard_key(struct wlrdp_input *inp, uint32_t key, bool pressed);

/*
 * Get the Wayland display fd for this input connection.
 */
int input_get_fd(struct wlrdp_input *inp);

/*
 * Dispatch pending events on the input Wayland connection.
 */
int input_dispatch(struct wlrdp_input *inp);

void input_destroy(struct wlrdp_input *inp);

#endif /* WLRDP_INPUT_H */
```

- [ ] **Step 2: Create src/session/input.c**

```c
#include "input.h"
#include "common.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

static uint32_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static bool setup_keymap(struct wlrdp_input *inp)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        WLRDP_LOG_ERROR("failed to create xkb context");
        return false;
    }

    struct xkb_rule_names names = {
        .rules = "evdev",
        .model = "pc105",
        .layout = "us",
        .variant = NULL,
        .options = NULL,
    };

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(
        ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        WLRDP_LOG_ERROR("failed to create xkb keymap");
        xkb_context_unref(ctx);
        return false;
    }

    char *keymap_str = xkb_keymap_get_as_string(
        keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!keymap_str) {
        WLRDP_LOG_ERROR("failed to serialize xkb keymap");
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return false;
    }

    size_t keymap_size = strlen(keymap_str) + 1;

    char name[] = "/wlrdp-keymap-XXXXXX";
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        WLRDP_LOG_ERROR("failed to create keymap shm: %s", strerror(errno));
        free(keymap_str);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return false;
    }
    shm_unlink(name);

    if (ftruncate(fd, keymap_size) < 0) {
        close(fd);
        free(keymap_str);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return false;
    }

    void *map = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        free(keymap_str);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        return false;
    }

    memcpy(map, keymap_str, keymap_size);
    munmap(map, keymap_size);

    zwp_virtual_keyboard_v1_keymap(inp->vkbd,
                                    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                    fd, keymap_size);
    close(fd);
    free(keymap_str);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    WLRDP_LOG_INFO("loaded XKB keymap (us/pc105)");
    return true;
}

/* --- registry callbacks --- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct wlrdp_input *inp = data;

    if (strcmp(interface, "zwlr_virtual_pointer_manager_v1") == 0) {
        inp->vptr_mgr = wl_registry_bind(registry, name,
            &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_virtual_keyboard_manager_v1") == 0) {
        inp->vkbd_mgr = wl_registry_bind(registry, name,
            &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        if (!inp->seat) {
            inp->seat = wl_registry_bind(registry, name,
                &wl_seat_interface, 1);
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- public API --- */

bool input_init(struct wlrdp_input *inp, const char *display_name,
                uint32_t width, uint32_t height)
{
    memset(inp, 0, sizeof(*inp));
    inp->width = width;
    inp->height = height;

    inp->display = wl_display_connect(display_name);
    if (!inp->display) {
        WLRDP_LOG_ERROR("input: failed to connect to '%s'", display_name);
        return false;
    }

    inp->registry = wl_display_get_registry(inp->display);
    wl_registry_add_listener(inp->registry, &registry_listener, inp);
    wl_display_roundtrip(inp->display);

    if (!inp->vptr_mgr) {
        WLRDP_LOG_ERROR("compositor lacks zwlr_virtual_pointer_manager_v1");
        input_destroy(inp);
        return false;
    }
    if (!inp->vkbd_mgr) {
        WLRDP_LOG_ERROR("compositor lacks zwp_virtual_keyboard_manager_v1");
        input_destroy(inp);
        return false;
    }
    if (!inp->seat) {
        WLRDP_LOG_ERROR("no wl_seat found");
        input_destroy(inp);
        return false;
    }

    inp->vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        inp->vptr_mgr, inp->seat);
    inp->vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        inp->vkbd_mgr, inp->seat);

    if (!setup_keymap(inp)) {
        input_destroy(inp);
        return false;
    }

    wl_display_flush(inp->display);
    WLRDP_LOG_INFO("input initialized on display '%s'", display_name);
    return true;
}

void input_pointer_motion(struct wlrdp_input *inp, uint32_t x, uint32_t y)
{
    zwlr_virtual_pointer_v1_motion_absolute(inp->vptr, get_time_ms(),
                                             x, y,
                                             inp->width, inp->height);
    zwlr_virtual_pointer_v1_frame(inp->vptr);
    wl_display_flush(inp->display);
}

void input_pointer_button(struct wlrdp_input *inp, uint32_t button,
                          bool pressed)
{
    zwlr_virtual_pointer_v1_button(inp->vptr, get_time_ms(), button,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(inp->vptr);
    wl_display_flush(inp->display);
}

void input_pointer_axis(struct wlrdp_input *inp, int32_t value)
{
    /* wl_fixed_t uses 24.8 format. Multiply by 256 (wl_fixed_from_int). */
    zwlr_virtual_pointer_v1_axis(inp->vptr, get_time_ms(),
                                  WL_POINTER_AXIS_VERTICAL_SCROLL,
                                  value * 256);
    zwlr_virtual_pointer_v1_frame(inp->vptr);
    wl_display_flush(inp->display);
}

void input_keyboard_key(struct wlrdp_input *inp, uint32_t key, bool pressed)
{
    zwp_virtual_keyboard_v1_key(inp->vkbd, get_time_ms(), key,
        pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                : WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_flush(inp->display);
}

int input_get_fd(struct wlrdp_input *inp)
{
    return wl_display_get_fd(inp->display);
}

int input_dispatch(struct wlrdp_input *inp)
{
    return wl_display_dispatch(inp->display);
}

void input_destroy(struct wlrdp_input *inp)
{
    if (inp->vkbd) {
        zwp_virtual_keyboard_v1_destroy(inp->vkbd);
    }
    if (inp->vptr) {
        zwlr_virtual_pointer_v1_destroy(inp->vptr);
    }
    if (inp->vkbd_mgr) {
        zwp_virtual_keyboard_manager_v1_destroy(inp->vkbd_mgr);
    }
    if (inp->vptr_mgr) {
        zwlr_virtual_pointer_manager_v1_destroy(inp->vptr_mgr);
    }
    if (inp->seat) {
        wl_seat_destroy(inp->seat);
    }
    if (inp->registry) {
        wl_registry_destroy(inp->registry);
    }
    if (inp->display) {
        wl_display_disconnect(inp->display);
    }
    memset(inp, 0, sizeof(*inp));
}
```

- [ ] **Step 3: Update src/session/meson.build**

```meson
session_sources = files(
    'compositor.c',
    'capture.c',
    'input.c',
)
```

- [ ] **Step 4: Verify it compiles**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Compiles without errors.

- [ ] **Step 5: Commit**

```bash
git add src/session/input.h src/session/input.c src/session/meson.build
git commit -m "feat: add input injector via virtual pointer/keyboard protocols"
```

---

### Task 5: Frame Encoder

**Files:**
- Create: `src/session/encoder.h`
- Create: `src/session/encoder.c`
- Modify: `src/session/meson.build`

- [ ] **Step 1: Create src/session/encoder.h**

```c
#ifndef WLRDP_ENCODER_H
#define WLRDP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

struct wlrdp_encoder {
    uint32_t width;
    uint32_t height;
    void *nsc_ctx;     /* NSC_CONTEXT* from FreeRDP */
    uint8_t *out_buf;
    uint32_t out_len;
};

bool encoder_init(struct wlrdp_encoder *enc, uint32_t width, uint32_t height);

/*
 * Encode a frame of XRGB8888 pixels using NSCodec.
 * After calling, enc->out_buf and enc->out_len contain the encoded data.
 * The output buffer is owned by the encoder and valid until the next
 * encode call or encoder_destroy.
 */
bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride);

void encoder_destroy(struct wlrdp_encoder *enc);

#endif /* WLRDP_ENCODER_H */
```

- [ ] **Step 2: Create src/session/encoder.c**

```c
#include "encoder.h"
#include "common.h"

#include <freerdp/codec/nsc.h>
#include <winpr/stream.h>

bool encoder_init(struct wlrdp_encoder *enc, uint32_t width, uint32_t height)
{
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;

    NSC_CONTEXT *nsc = nsc_context_new();
    if (!nsc) {
        WLRDP_LOG_ERROR("failed to create NSCodec context");
        return false;
    }

    if (!nsc_context_set_parameters(nsc, NSC_COLOR_LOSS_LEVEL, 0)) {
        WLRDP_LOG_WARN("failed to set NSCodec color loss level");
    }
    if (!nsc_context_set_parameters(nsc, NSC_ALLOW_SUBSAMPLING, 1)) {
        WLRDP_LOG_WARN("failed to set NSCodec subsampling");
    }
    if (!nsc_context_set_parameters(nsc, NSC_DYNAMIC_COLOR_FIDELITY, 1)) {
        WLRDP_LOG_WARN("failed to set NSCodec dynamic color fidelity");
    }

    enc->nsc_ctx = nsc;

    WLRDP_LOG_INFO("encoder initialized (%ux%u, NSCodec)", width, height);
    return true;
}

bool encoder_encode(struct wlrdp_encoder *enc, const uint8_t *pixels,
                    uint32_t stride)
{
    NSC_CONTEXT *nsc = enc->nsc_ctx;

    if (!nsc_compose_message(nsc, enc->width, enc->height, pixels, stride)) {
        WLRDP_LOG_ERROR("nsc_compose_message failed");
        return false;
    }

    wStream *s = nsc->BitmapData;
    enc->out_buf = Stream_Buffer(s);
    enc->out_len = Stream_GetPosition(s);

    return true;
}

void encoder_destroy(struct wlrdp_encoder *enc)
{
    if (enc->nsc_ctx) {
        nsc_context_free(enc->nsc_ctx);
    }
    memset(enc, 0, sizeof(*enc));
}
```

- [ ] **Step 3: Update src/session/meson.build**

```meson
session_sources = files(
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
)
```

- [ ] **Step 4: Verify it compiles**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Compiles. May need to check FreeRDP 3.x NSCodec API — if `nsc_compose_message` signature differs in 3.x, adjust accordingly. The FreeRDP 3.x API changed some function signatures; check `freerdp/codec/nsc.h` in the installed headers.

- [ ] **Step 5: Commit**

```bash
git add src/session/encoder.h src/session/encoder.c src/session/meson.build
git commit -m "feat: add NSCodec frame encoder using FreeRDP"
```

---

### Task 6: RDP Peer Handler

**Files:**
- Create: `src/session/rdp_peer.h`
- Create: `src/session/rdp_peer.c`
- Modify: `src/session/meson.build`

- [ ] **Step 1: Create src/session/rdp_peer.h**

```c
#ifndef WLRDP_RDP_PEER_H
#define WLRDP_RDP_PEER_H

#include <stdbool.h>
#include <stdint.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

struct wlrdp_input;
struct wlrdp_encoder;

struct wlrdp_peer_context {
    rdpContext base;  /* must be first */

    struct wlrdp_input *input;
    uint32_t width;
    uint32_t height;
    bool activated;
};

/*
 * Initialize a freerdp_peer with our callbacks.
 * cert_file and key_file are paths to the TLS certificate and key.
 * input is borrowed (not owned).
 */
bool rdp_peer_init(freerdp_peer *client, const char *cert_file,
                   const char *key_file, struct wlrdp_input *input);

/*
 * Send an encoded frame to the RDP client.
 * codec_id should be RDP_CODEC_ID_NSCODEC.
 */
bool rdp_peer_send_frame(freerdp_peer *client,
                         uint8_t *data, uint32_t len,
                         uint32_t width, uint32_t height);

#endif /* WLRDP_RDP_PEER_H */
```

- [ ] **Step 2: Create src/session/rdp_peer.c**

```c
#include "rdp_peer.h"
#include "input.h"
#include "common.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/codec/nsc.h>
#include <linux/input-event-codes.h>

static BOOL on_post_connect(freerdp_peer *client)
{
    rdpSettings *settings = client->context->settings;

    WLRDP_LOG_INFO("RDP client connected: %ux%u %ubpp",
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
                    freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
                    freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth));

    if (!freerdp_settings_get_bool(settings, FreeRDP_NSCodec)) {
        WLRDP_LOG_WARN("client does not support NSCodec, falling back to raw bitmap");
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

    if (!ctx->input) {
        return TRUE;
    }

    /*
     * RDP scancode to evdev keycode:
     * For standard (non-extended) keys: evdev = rdp_scancode + 8
     * Extended keys (flags & KBD_FLAGS_EXTENDED): add 0 for now,
     * a full mapping table is needed for complete coverage.
     */
    uint32_t evdev_key;
    if (flags & KBD_FLAGS_EXTENDED) {
        /* Extended key mapping for common keys */
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
        default:   evdev_key = code + 8; break;
        }
    } else {
        evdev_key = code + 8;
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

    if (!ctx->input) {
        return TRUE;
    }

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
        /* RDP wheel units are 120ths; normalize to a reasonable Wayland value */
        input_pointer_axis(ctx->input, value / 120);
    }

    return TRUE;
}

static BOOL on_extended_mouse_event(rdpInput *input, UINT16 flags,
                                    UINT16 x, UINT16 y)
{
    /* Extended mouse events include absolute position with button state.
     * For Phase 1, just handle the motion. */
    freerdp_peer *client = input->context->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->input) {
        return TRUE;
    }

    input_pointer_motion(ctx->input, x, y);
    return TRUE;
}

bool rdp_peer_init(freerdp_peer *client, const char *cert_file,
                   const char *key_file, struct wlrdp_input *input)
{
    rdpSettings *settings = client->context->settings;

    /* TLS configuration */
    if (!freerdp_settings_set_string(settings, FreeRDP_CertificateFile,
                                     cert_file)) {
        WLRDP_LOG_ERROR("failed to set certificate file");
        return false;
    }
    if (!freerdp_settings_set_string(settings, FreeRDP_PrivateKeyFile,
                                     key_file)) {
        WLRDP_LOG_ERROR("failed to set private key file");
        return false;
    }

    /* Security settings: TLS only, no NLA in Phase 1 */
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);

    /* Enable NSCodec */
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    /* Callbacks */
    client->PostConnect = on_post_connect;
    client->Activate = on_activate;
    client->context->input->KeyboardEvent = on_keyboard_event;
    client->context->input->MouseEvent = on_mouse_event;
    client->context->input->ExtendedMouseEvent = on_extended_mouse_event;

    /* Store input reference in our extended context */
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
    cmd.destLeft = 0;
    cmd.destTop = 0;
    cmd.destRight = width;
    cmd.destBottom = height;
    cmd.bmp.bpp = 32;
    cmd.bmp.width = width;
    cmd.bmp.height = height;

    if (freerdp_settings_get_bool(settings, FreeRDP_NSCodec)) {
        cmd.bmp.codecID = freerdp_settings_get_uint32(settings,
                            FreeRDP_NSCodecId);
    } else {
        cmd.bmp.codecID = 0; /* raw bitmap */
    }

    cmd.bmp.bitmapDataLength = len;
    cmd.bmp.bitmapData = data;

    BOOL ret = update->SurfaceBits(update->context, &cmd);
    if (!ret) {
        WLRDP_LOG_WARN("SurfaceBits failed");
    }

    return ret;
}
```

- [ ] **Step 3: Update src/session/meson.build**

```meson
session_sources = files(
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
    'rdp_peer.c',
)
```

- [ ] **Step 4: Verify it compiles**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Compiles. If any FreeRDP 3.x API differences are found (e.g., `freerdp_settings_get_uint32` vs direct struct access), fix them. FreeRDP 3.x uses accessor functions rather than direct field access.

- [ ] **Step 5: Commit**

```bash
git add src/session/rdp_peer.h src/session/rdp_peer.c src/session/meson.build
git commit -m "feat: add RDP peer handler with TLS, NSCodec, and input dispatch"
```

---

### Task 7: Main Entry Point and Event Loop

**Files:**
- Create: `src/session/main.c`
- Modify: `src/session/meson.build`

- [ ] **Step 1: Create src/session/main.c**

```c
#include "common.h"
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

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct wlrdp_server {
    /* Config */
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;

    /* State */
    struct wlrdp_compositor comp;
    struct wlrdp_capture capture;
    struct wlrdp_input input;
    struct wlrdp_encoder encoder;
    freerdp_listener *listener;
    freerdp_peer *client;

    int epoll_fd;
    bool client_active;
};

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGCHLD) {
        g_child_exited = 1;
    }
}

static void on_frame_ready(void *data, uint8_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t stride)
{
    struct wlrdp_server *srv = data;

    if (!srv->client_active || !srv->client) {
        return;
    }

    if (!encoder_encode(&srv->encoder, pixels, stride)) {
        return;
    }

    rdp_peer_send_frame(srv->client, srv->encoder.out_buf,
                        srv->encoder.out_len, width, height);

    /* Request next frame */
    capture_request_frame(&srv->capture);
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

    /* Allocate context with our extended struct */
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
        "  --port PORT          RDP listen port (default: 3389)\n"
        "  --width WIDTH        Display width (default: 1920)\n"
        "  --height HEIGHT      Display height (default: 1080)\n"
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
        case 'p': srv.port = atoi(optarg); break;
        case 'W': srv.width = atoi(optarg); break;
        case 'H': srv.height = atoi(optarg); break;
        case 'd': srv.desktop_cmd = optarg; break;
        case 'c': srv.cert_file = optarg; break;
        case 'k': srv.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Auto-generate cert if not provided */
    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (!srv.cert_file || !srv.key_file) {
        generate_self_signed_cert(auto_cert, auto_key);
        if (!srv.cert_file) srv.cert_file = auto_cert;
        if (!srv.key_file) srv.key_file = auto_key;
    }

    /* Set up signal handlers */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    /* Launch cage */
    WLRDP_LOG_INFO("launching compositor: cage -- %s (%dx%d)",
                    srv.desktop_cmd, srv.width, srv.height);
    if (!compositor_launch(&srv.comp, srv.desktop_cmd,
                           srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to launch compositor");
        return 1;
    }

    /* Connect to cage for screen capture */
    if (!capture_init(&srv.capture, srv.comp.display_name,
                      on_frame_ready, &srv)) {
        WLRDP_LOG_ERROR("failed to initialize capture");
        compositor_destroy(&srv.comp);
        return 1;
    }

    /* Connect to cage for input injection */
    if (!input_init(&srv.input, srv.comp.display_name,
                    srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize input");
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    /* Initialize encoder */
    if (!encoder_init(&srv.encoder, srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize encoder");
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    /* Create FreeRDP listener */
    srv.listener = freerdp_listener_new();
    if (!srv.listener) {
        WLRDP_LOG_ERROR("freerdp_listener_new failed");
        encoder_destroy(&srv.encoder);
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    srv.listener->info = &srv;
    srv.listener->PeerAccepted = on_peer_accepted;

    if (!srv.listener->Open(srv.listener, "0.0.0.0", srv.port)) {
        WLRDP_LOG_ERROR("failed to listen on port %d", srv.port);
        freerdp_listener_free(srv.listener);
        encoder_destroy(&srv.encoder);
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    WLRDP_LOG_INFO("listening on port %d", srv.port);

    /* Set up epoll */
    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    /* Add listener fds */
    DWORD count = 0;
    HANDLE *handles = srv.listener->GetEventHandles(srv.listener, &count);
    for (DWORD i = 0; i < count; i++) {
        int fd = GetEventFileDescriptor(handles[i]);
        if (fd >= 0) {
            epoll_add_fd(srv.epoll_fd, fd);
        }
    }

    /* Add Wayland capture fd */
    int capture_fd = capture_get_fd(&srv.capture);
    epoll_add_fd(srv.epoll_fd, capture_fd);

    /* Main event loop */
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

        /* Flush Wayland before polling */
        wl_display_flush(srv.capture.display);

        struct epoll_event events[16];
        int nfds = epoll_wait(srv.epoll_fd, events, 16, 16); /* 16ms ~ 60fps */
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
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

            /* Must be a listener fd */
            if (!srv.listener->CheckFileDescriptor(srv.listener)) {
                WLRDP_LOG_ERROR("listener check failed");
                g_running = 0;
            }
        }

        /* Check peer fds if we have a client */
        if (srv.client) {
            struct wlrdp_peer_context *ctx =
                (struct wlrdp_peer_context *)srv.client->context;

            if (!srv.client->CheckFileDescriptor(srv.client)) {
                WLRDP_LOG_INFO("client disconnected");
                freerdp_peer_context_free(srv.client);
                freerdp_peer_free(srv.client);
                srv.client = NULL;
                srv.client_active = false;
                continue;
            }

            /* Start frame capture once activated */
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

- [ ] **Step 2: Finalize src/session/meson.build**

Replace `src/session/meson.build` with:

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
    ],
    include_directories: [common_inc],
    install: true,
)
```

- [ ] **Step 3: Build the complete binary**

```bash
meson setup build --wipe && ninja -C build
```

Expected: Compiles and links successfully, producing `build/src/session/wlrdp-session`.

- [ ] **Step 4: Verify the binary runs (help output)**

```bash
./build/src/session/wlrdp-session --help
```

Expected output:
```
Usage: wlrdp-session [options]
  --port PORT          RDP listen port (default: 3389)
  --width WIDTH        Display width (default: 1920)
  --height HEIGHT      Display height (default: 1080)
  --desktop-cmd CMD    Command to run inside cage (default: foot)
  --cert FILE          TLS certificate file
  --key FILE           TLS private key file
  --help               Show this help
```

- [ ] **Step 5: Commit**

```bash
git add src/session/main.c src/session/meson.build
git commit -m "feat: add main entry point with event loop tying all components together"
```

---

### Task 8: Build Verification and API Fixes

This task handles any compilation errors from FreeRDP 3.x API differences. FreeRDP 3.x changed several APIs from version 2.x, and exact signatures vary between 3.x patch versions.

**Files:**
- Modify: any files with compilation errors

- [ ] **Step 1: Attempt a clean build in the DevContainer**

```bash
meson setup build --wipe && ninja -C build 2>&1
```

If this succeeds with zero errors and zero warnings, skip to Step 4.

- [ ] **Step 2: Fix any FreeRDP API issues**

Common FreeRDP 3.x issues to check:

1. `nsc_compose_message` may have a different signature in 3.x. Check:
   ```bash
   grep -r 'nsc_compose_message' /usr/include/freerdp3/
   ```
   Adjust `encoder.c` to match the installed signature.

2. `listener->GetEventHandles` and `GetEventFileDescriptor` may differ. Check:
   ```bash
   grep -r 'GetEventHandles\|GetEventFileDescriptor' /usr/include/freerdp3/ /usr/include/winpr3/
   ```

3. `NSC_COLOR_LOSS_LEVEL` and other NSCodec constants may be renamed. Check:
   ```bash
   grep -r 'NSC_COLOR_LOSS_LEVEL\|nsc_context_set_parameters' /usr/include/freerdp3/
   ```

4. `SurfaceBits` callback signature. Check:
   ```bash
   grep -r 'SurfaceBits' /usr/include/freerdp3/
   ```

Fix each issue in the relevant source file.

- [ ] **Step 3: Rebuild and verify zero errors**

```bash
ninja -C build
```

Expected: Clean build.

- [ ] **Step 4: Commit fixes (if any)**

```bash
git add -u
git commit -m "fix: adjust for FreeRDP 3.x API compatibility"
```

Skip this step if no fixes were needed.

---

### Task 9: Integration Test

Test the complete system end-to-end. This requires a Wayland-capable environment (the DevContainer or a Linux machine with Wayland).

- [ ] **Step 1: Generate a test certificate**

```bash
openssl req -x509 -newkey rsa:2048 \
    -keyout /tmp/wlrdp-test-key.pem \
    -out /tmp/wlrdp-test-cert.pem \
    -days 1 -nodes -subj '/CN=wlrdp-test'
```

- [ ] **Step 2: Start wlrdp-session**

```bash
./build/src/session/wlrdp-session \
    --port 3389 \
    --cert /tmp/wlrdp-test-cert.pem \
    --key /tmp/wlrdp-test-key.pem \
    --desktop-cmd foot \
    --width 1280 --height 720
```

Expected: Logs show cage launching, capture and input initializing, listener ready.

- [ ] **Step 3: Connect with an RDP client**

From another terminal or machine:

```bash
xfreerdp /v:localhost:3389 /cert:ignore /w:1280 /h:720
```

Expected: A window appears showing the foot terminal running inside cage.

- [ ] **Step 4: Test keyboard input**

Type characters in the xfreerdp window. Expected: Characters appear in the foot terminal.

- [ ] **Step 5: Test mouse input**

Click inside the foot terminal, scroll up/down. Expected: Terminal responds to clicks and scrolling.

- [ ] **Step 6: Test disconnect**

Close the xfreerdp window. Expected: wlrdp-session logs "client disconnected" and continues running, ready for a new connection.

- [ ] **Step 7: Commit any final adjustments**

```bash
git add -u
git commit -m "chore: integration test adjustments"
```

Skip if no changes needed.
