# wlrdp Phase 4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add clipboard sharing, audio playback, and null system pointer (hide client cursor) to wlrdp sessions.

**Architecture:** Two independent subsystems added to the session worker, both opened as dynamic virtual channels via the existing VCM infrastructure. Clipboard uses FreeRDP's CLIPRDR server API bridged to the Wayland compositor via the `ext-data-control-v1` protocol. Audio output uses FreeRDP's RDPSND server API fed by a PipeWire stream that captures the session's audio. Each subsystem gets its own source file with a clean init/cleanup/poll interface that plugs into the existing epoll event loop. Microphone input (AUDIN) and webcam (RDPECAM) are deferred to a future phase to keep this plan focused and shippable.

**Tech Stack:** C11, Meson, FreeRDP 3.x CLIPRDR + RDPSND server APIs, PipeWire 0.3 (`libpipewire-0.3`), Wayland `ext-data-control-v1` protocol

**Spec:** `docs/superpowers/specs/2026-04-16-wlrdp-design.md` -- Phase 4 section

**Scope reduction from spec:** The spec lists AUDIN (microphone), RDPECAM (webcam), and RDPSND (audio). This plan implements **clipboard (CLIPRDR) and audio output (RDPSND)** only. Microphone and webcam are complex (they require injecting media *into* the compositor session) and are better tackled as a separate plan once clipboard and audio output are stable.

---

## File Map

| File | Responsibility |
|------|---------------|
| `.devcontainer/Dockerfile` | Add PipeWire dev packages and `wl-clipboard` for testing |
| `meson.build` | Add PipeWire dependency (optional, gated by `enable-pipewire`) |
| `meson_options.txt` | Change `enable-pipewire` default from `disabled` to `auto` |
| `src/session/meson.build` | Link PipeWire and add new source files |
| `src/protocols/meson.build` | Add `ext-data-control-v1` protocol generation |
| `src/protocols/ext-data-control-v1.xml` | Copy from system wayland-protocols |
| `src/session/clipboard.h` | Clipboard module interface: init, cleanup, poll |
| `src/session/clipboard.c` | CLIPRDR server channel + ext-data-control-v1 bridge |
| `src/session/audio.h` | Audio output module interface: init, cleanup, poll |
| `src/session/audio.c` | RDPSND server channel + PipeWire capture stream |
| `src/session/rdp_peer.h` | Add clipboard and audio context pointers |
| `src/session/rdp_peer.c` | Hide client cursor, open CLIPRDR and RDPSND channels after DRDYNVC ready |
| `src/session/main.c` | Wire clipboard and audio into epoll loop and lifecycle |

---

### Task 1: Build System -- PipeWire and Protocol Dependencies

**Files:**
- Modify: `.devcontainer/Dockerfile`
- Modify: `meson.build`
- Modify: `meson_options.txt`
- Modify: `src/session/meson.build`
- Modify: `src/protocols/meson.build`
- Create: `src/protocols/ext-data-control-v1.xml`

- [ ] **Step 1: Add PipeWire and wl-clipboard packages to Dockerfile**

Add to the `dnf install` list in `.devcontainer/Dockerfile`, after `labwc`:

```dockerfile
    pipewire-devel \
    wireplumber \
    wl-clipboard \
```

`pipewire-devel` provides `libpipewire-0.3`. `wireplumber` is the session manager needed at runtime. `wl-clipboard` provides `wl-copy`/`wl-paste` for manual testing.

- [ ] **Step 2: Change enable-pipewire default in meson_options.txt**

Change the existing line:

```meson
option('enable-pipewire', type: 'feature', value: 'auto',
    description: 'PipeWire audio capture (Phase 4)')
```

- [ ] **Step 3: Add PipeWire dependency to meson.build**

Add after the `have_h264` block:

```meson
pw_opt = get_option('enable-pipewire')
pipewire_dep = dependency('libpipewire-0.3', required: pw_opt)
have_pipewire = pipewire_dep.found()
if have_pipewire
    add_project_arguments('-DWLRDP_HAVE_PIPEWIRE=1', language: 'c')
endif
```

- [ ] **Step 4: Add ext-data-control-v1 protocol XML**

Copy from the system wayland-protocols directory into the project:

```bash
cp /usr/share/wayland-protocols/staging/ext-data-control/ext-data-control-v1.xml \
   src/protocols/ext-data-control-v1.xml
```

- [ ] **Step 5: Add protocol generation rule in src/protocols/meson.build**

Add `ext-data-control-v1` to the protocol list. The existing `meson.build` should have a pattern for generating client protocol headers -- add this protocol to it following the same pattern used for `wlr-screencopy-unstable-v1` and the other protocols.

Read `src/protocols/meson.build` to see the exact generation pattern, then add:

```meson
# ext-data-control-v1 (clipboard)
ext_data_control_xml = files('ext-data-control-v1.xml')
```

And add the corresponding `wayland_scanner` calls for client header and private code, following the existing pattern in that file.

- [ ] **Step 6: Update src/session/meson.build**

Add the new source files and PipeWire dependency:

```meson
session_sources = files(
    'main.c',
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
    'rdp_peer.c',
    'clipboard.c',
    'audio.c',
)
```

And for dependencies, add PipeWire conditionally:

```meson
if have_pipewire
    session_deps += [pipewire_dep]
endif
```

- [ ] **Step 7: Create stub files so the build succeeds**

Create `src/session/clipboard.h`:

```c
#ifndef WLRDP_CLIPBOARD_H
#define WLRDP_CLIPBOARD_H

#include <stdbool.h>
#include <stdint.h>

struct wlrdp_clipboard;

#endif /* WLRDP_CLIPBOARD_H */
```

Create `src/session/clipboard.c`:

```c
#include "clipboard.h"
```

Create `src/session/audio.h`:

```c
#ifndef WLRDP_AUDIO_H
#define WLRDP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct wlrdp_audio;

#endif /* WLRDP_AUDIO_H */
```

Create `src/session/audio.c`:

```c
#include "audio.h"
```

- [ ] **Step 8: Rebuild the devcontainer and verify the build**

Rebuild the devcontainer to pick up new packages, then:

```bash
meson setup --wipe build
meson compile -C build
```

Expected: builds successfully with `WLRDP_HAVE_PIPEWIRE=1` in the output.

- [ ] **Step 9: Commit**

```bash
git add .devcontainer/Dockerfile meson.build meson_options.txt \
    src/protocols/ext-data-control-v1.xml src/protocols/meson.build \
    src/session/meson.build src/session/clipboard.h src/session/clipboard.c \
    src/session/audio.h src/session/audio.c
git commit -m "build: add PipeWire, ext-data-control-v1 deps for Phase 4"
```

---

### Task 2: Hide Client Cursor (Null System Pointer)

**Files:**
- Modify: `src/session/rdp_peer.c`

The RDP client shows its own OS cursor on top of the server-rendered cursor, producing a "double cursor" effect. The fix is to send a `POINTER_SYSTEM_UPDATE` with `SYSPTR_NULL` after activation, which tells the client to hide its local cursor. The server's cursor is already composited into the frame by cage.

- [ ] **Step 1: Add null pointer send in on_activate**

In `rdp_peer.c`, add `#include <freerdp/pointer.h>` at the top (if not already present).

In `on_activate`, after the `WLRDP_LOG_INFO` line and before the resolution adoption block, add:

```c
    /* Hide the client's local cursor. The server cursor is composited
     * into the frame by cage, so the client must not draw its own. */
    rdpPointerUpdate *pointer = client->context->update->pointer;
    POINTER_SYSTEM_UPDATE system_pointer = { .type = SYSPTR_NULL };
    pointer->PointerSystem(client->context, &system_pointer);
```

- [ ] **Step 2: Verify the build compiles**

```bash
meson compile -C build
```

Expected: compiles with no errors.

- [ ] **Step 3: Test**

Connect with an RDP client. The client's local cursor should be hidden -- only the compositor's cursor (rendered in the frame) should be visible. There should be no "double cursor".

- [ ] **Step 4: Commit**

```bash
git add src/session/rdp_peer.c
git commit -m "fix: hide client cursor via SYSPTR_NULL on activation"
```

---

### Task 3: Clipboard -- Wayland Data Control Client

**Files:**
- Modify: `src/session/clipboard.h`
- Modify: `src/session/clipboard.c`

This task implements the Wayland side of clipboard: binding the `ext-data-control-v1` protocol to monitor and set the compositor's clipboard. The RDP bridge comes in Task 4.

- [ ] **Step 1: Define the clipboard module interface in clipboard.h**

```c
#ifndef WLRDP_CLIPBOARD_H
#define WLRDP_CLIPBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct wlrdp_clipboard {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;

    /* ext-data-control-v1 objects */
    struct ext_data_control_manager_v1 *dcm;
    struct ext_data_control_source_v1 *source;   /* our source when setting clipboard */
    struct ext_data_control_device_v1 *device;

    /* Current clipboard content from the Wayland side */
    char *wl_clipboard_text;       /* text/plain;charset=utf-8 content */
    size_t wl_clipboard_len;

    /* Pending clipboard content to set on Wayland side */
    char *pending_set_text;
    size_t pending_set_len;

    /* RDP-side callbacks */
    void (*on_wl_clipboard_changed)(void *data, const char *text, size_t len);
    void *cb_data;

    /* Track whether last clipboard set was from RDP (to avoid echo) */
    bool setting_from_rdp;
};

bool clipboard_init(struct wlrdp_clipboard *cb, const char *display_name,
                    void (*on_change)(void *data, const char *text, size_t len),
                    void *cb_data);
void clipboard_destroy(struct wlrdp_clipboard *cb);

/* Set the Wayland clipboard to the given text (called when RDP client pastes) */
void clipboard_set_text(struct wlrdp_clipboard *cb, const char *text, size_t len);

int clipboard_get_fd(struct wlrdp_clipboard *cb);
int clipboard_dispatch(struct wlrdp_clipboard *cb);
void clipboard_flush(struct wlrdp_clipboard *cb);

#endif /* WLRDP_CLIPBOARD_H */
```

- [ ] **Step 2: Implement the Wayland data control client in clipboard.c**

```c
#include "clipboard.h"
#include "common.h"
#include "ext-data-control-v1-client-protocol.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLIPBOARD_MIME_TYPE "text/plain;charset=utf-8"
#define CLIPBOARD_MAX_SIZE (16 * 1024 * 1024) /* 16 MiB limit */

/* --- data control source callbacks (for setting clipboard) --- */

static void source_send(void *data,
                        struct ext_data_control_source_v1 *source,
                        const char *mime_type, int32_t fd)
{
    struct wlrdp_clipboard *cb = data;

    if (cb->pending_set_text && strcmp(mime_type, CLIPBOARD_MIME_TYPE) == 0) {
        /* Write all data to the fd */
        size_t written = 0;
        while (written < cb->pending_set_len) {
            ssize_t n = write(fd, cb->pending_set_text + written,
                              cb->pending_set_len - written);
            if (n <= 0) break;
            written += n;
        }
    }
    close(fd);
}

static void source_cancelled(void *data,
                             struct ext_data_control_source_v1 *source)
{
    struct wlrdp_clipboard *cb = data;

    /* Our source was replaced by another client's clipboard */
    if (cb->source == source) {
        ext_data_control_source_v1_destroy(source);
        cb->source = NULL;
    }

    free(cb->pending_set_text);
    cb->pending_set_text = NULL;
    cb->pending_set_len = 0;
    cb->setting_from_rdp = false;
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

/* --- data control offer callbacks (for reading clipboard) --- */

struct clipboard_offer {
    struct wlrdp_clipboard *cb;
    struct ext_data_control_offer_v1 *offer;
    bool has_text;
};

static void offer_offer(void *data,
                        struct ext_data_control_offer_v1 *offer,
                        const char *mime_type)
{
    struct clipboard_offer *co = data;
    if (strcmp(mime_type, CLIPBOARD_MIME_TYPE) == 0) {
        co->has_text = true;
    }
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = offer_offer,
};

/* --- data control device callbacks --- */

static void read_offer_text(struct wlrdp_clipboard *cb,
                            struct ext_data_control_offer_v1 *offer)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        WLRDP_LOG_WARN("clipboard: pipe2 failed: %s", strerror(errno));
        return;
    }

    ext_data_control_offer_v1_receive(offer, CLIPBOARD_MIME_TYPE, fds[1]);
    close(fds[1]);

    /* Flush the request so the compositor starts writing */
    wl_display_flush(cb->display);

    /* Read synchronously from the pipe (blocking on read fd).
     * Switch to blocking mode for the read. */
    int flags = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, flags & ~O_NONBLOCK);

    char *buf = NULL;
    size_t total = 0;
    size_t capacity = 0;

    for (;;) {
        if (total >= CLIPBOARD_MAX_SIZE) break;
        if (total + 4096 > capacity) {
            capacity = (capacity == 0) ? 4096 : capacity * 2;
            if (capacity > CLIPBOARD_MAX_SIZE) capacity = CLIPBOARD_MAX_SIZE;
            char *newbuf = realloc(buf, capacity);
            if (!newbuf) break;
            buf = newbuf;
        }
        ssize_t n = read(fds[0], buf + total, capacity - total);
        if (n <= 0) break;
        total += n;
    }
    close(fds[0]);

    if (total > 0 && buf) {
        free(cb->wl_clipboard_text);
        cb->wl_clipboard_text = buf;
        cb->wl_clipboard_len = total;

        if (cb->on_wl_clipboard_changed && !cb->setting_from_rdp) {
            cb->on_wl_clipboard_changed(cb->cb_data, buf, total);
        }
    } else {
        free(buf);
    }

    cb->setting_from_rdp = false;
}

static void device_data_offer(void *data,
                              struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *offer)
{
    struct wlrdp_clipboard *cb = data;

    struct clipboard_offer *co = calloc(1, sizeof(*co));
    if (!co) return;
    co->cb = cb;
    co->offer = offer;
    co->has_text = false;

    ext_data_control_offer_v1_add_listener(offer, &offer_listener, co);
}

static void device_selection(void *data,
                             struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *offer)
{
    struct wlrdp_clipboard *cb = data;

    if (!offer) return;

    struct clipboard_offer *co = wl_proxy_get_user_data(
        (struct wl_proxy *)offer);
    if (!co) {
        ext_data_control_offer_v1_destroy(offer);
        return;
    }

    if (co->has_text) {
        read_offer_text(cb, offer);
    }

    ext_data_control_offer_v1_destroy(offer);
    free(co);
}

static void device_finished(void *data,
                            struct ext_data_control_device_v1 *device)
{
    struct wlrdp_clipboard *cb = data;
    if (cb->device == device) {
        ext_data_control_device_v1_destroy(device);
        cb->device = NULL;
    }
}

static void device_primary_selection(void *data,
                                     struct ext_data_control_device_v1 *device,
                                     struct ext_data_control_offer_v1 *offer)
{
    /* We ignore primary selection (middle-click paste) */
    if (offer) {
        ext_data_control_offer_v1_destroy(offer);
    }
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .finished = device_finished,
    .primary_selection = device_primary_selection,
};

/* --- registry --- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct wlrdp_clipboard *cb = data;
    (void)version;

    if (strcmp(interface, "ext_data_control_manager_v1") == 0) {
        cb->dcm = wl_registry_bind(registry, name,
            &ext_data_control_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        if (!cb->seat) {
            cb->seat = wl_registry_bind(registry, name,
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

bool clipboard_init(struct wlrdp_clipboard *cb, const char *display_name,
                    void (*on_change)(void *data, const char *text, size_t len),
                    void *cb_data)
{
    memset(cb, 0, sizeof(*cb));
    cb->on_wl_clipboard_changed = on_change;
    cb->cb_data = cb_data;

    cb->display = wl_display_connect(display_name);
    if (!cb->display) {
        WLRDP_LOG_ERROR("clipboard: failed to connect to '%s'", display_name);
        return false;
    }

    cb->registry = wl_display_get_registry(cb->display);
    wl_registry_add_listener(cb->registry, &registry_listener, cb);
    wl_display_roundtrip(cb->display);

    if (!cb->dcm) {
        WLRDP_LOG_WARN("compositor lacks ext_data_control_manager_v1, "
                        "clipboard disabled");
        clipboard_destroy(cb);
        return false;
    }
    if (!cb->seat) {
        WLRDP_LOG_WARN("clipboard: no wl_seat found");
        clipboard_destroy(cb);
        return false;
    }

    cb->device = ext_data_control_manager_v1_get_data_device(cb->dcm, cb->seat);
    ext_data_control_device_v1_add_listener(cb->device, &device_listener, cb);

    wl_display_roundtrip(cb->display);

    WLRDP_LOG_INFO("clipboard initialized on display '%s'", display_name);
    return true;
}

void clipboard_set_text(struct wlrdp_clipboard *cb, const char *text, size_t len)
{
    if (!cb->dcm || !cb->device) return;

    /* Destroy old source if any */
    if (cb->source) {
        ext_data_control_source_v1_destroy(cb->source);
        cb->source = NULL;
    }

    free(cb->pending_set_text);
    cb->pending_set_text = malloc(len);
    if (!cb->pending_set_text) return;
    memcpy(cb->pending_set_text, text, len);
    cb->pending_set_len = len;

    cb->source = ext_data_control_manager_v1_create_data_source(cb->dcm);
    ext_data_control_source_v1_add_listener(cb->source, &source_listener, cb);
    ext_data_control_source_v1_offer(cb->source, CLIPBOARD_MIME_TYPE);

    cb->setting_from_rdp = true;
    ext_data_control_device_v1_set_selection(cb->device, cb->source);

    wl_display_flush(cb->display);
}

int clipboard_get_fd(struct wlrdp_clipboard *cb)
{
    return wl_display_get_fd(cb->display);
}

int clipboard_dispatch(struct wlrdp_clipboard *cb)
{
    return wl_display_dispatch(cb->display);
}

void clipboard_flush(struct wlrdp_clipboard *cb)
{
    wl_display_flush(cb->display);
}

void clipboard_destroy(struct wlrdp_clipboard *cb)
{
    free(cb->wl_clipboard_text);
    free(cb->pending_set_text);
    if (cb->source) ext_data_control_source_v1_destroy(cb->source);
    if (cb->device) ext_data_control_device_v1_destroy(cb->device);
    if (cb->dcm) ext_data_control_manager_v1_destroy(cb->dcm);
    if (cb->seat) wl_seat_destroy(cb->seat);
    if (cb->registry) wl_registry_destroy(cb->registry);
    if (cb->display) wl_display_disconnect(cb->display);
    memset(cb, 0, sizeof(*cb));
}
```

- [ ] **Step 3: Verify the build compiles**

```bash
meson compile -C build
```

Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/session/clipboard.h src/session/clipboard.c
git commit -m "feat: add clipboard Wayland data-control client"
```

---

### Task 4: Clipboard -- CLIPRDR Server Channel Bridge

**Files:**
- Modify: `src/session/clipboard.h`
- Modify: `src/session/clipboard.c`
- Modify: `src/session/rdp_peer.h`
- Modify: `src/session/rdp_peer.c`

This task opens the CLIPRDR dynamic virtual channel and bridges it to the Wayland clipboard from Task 3.

- [ ] **Step 1: Add CLIPRDR context to rdp_peer.h**

Add to `struct wlrdp_peer_context`:

```c
    /* CLIPRDR state */
    void *cliprdr_context;     /* CliprdrServerContext* */
    bool cliprdr_opened;
```

- [ ] **Step 2: Add clipboard fields to clipboard.h**

Add a forward declaration and a field for the CLIPRDR context pointer so the clipboard module can send format lists and data responses:

```c
/* Add to struct wlrdp_clipboard */
    void *cliprdr_context;     /* CliprdrServerContext* -- set by rdp_peer after open */
```

- [ ] **Step 3: Add CLIPRDR callbacks and open function to clipboard.c**

Add the CLIPRDR server-side callback implementations at the top of `clipboard.c`, before the public API:

```c
#include <freerdp/channels/cliprdr.h>
#include <freerdp/server/cliprdr.h>

/* --- CLIPRDR server callbacks --- */

static UINT on_cliprdr_client_capabilities(CliprdrServerContext *context,
                                           const CLIPRDR_CAPABILITIES *caps)
{
    WLRDP_LOG_INFO("CLIPRDR: client capabilities received");
    return CHANNEL_RC_OK;
}

static UINT on_cliprdr_client_format_list(CliprdrServerContext *context,
                                          const CLIPRDR_FORMAT_LIST *list)
{
    struct wlrdp_clipboard *cb = context->custom;

    /* Check if client has text on its clipboard */
    bool has_text = false;
    uint32_t text_format_id = 0;
    for (uint32_t i = 0; i < list->numFormats; i++) {
        uint32_t id = list->formats[i].formatId;
        if (id == CF_TEXT || id == CF_UNICODETEXT || id == CF_OEMTEXT) {
            has_text = true;
            text_format_id = CF_UNICODETEXT; /* prefer Unicode */
            break;
        }
    }

    /* Acknowledge the format list */
    CLIPRDR_FORMAT_LIST_RESPONSE resp = {
        .common = { .msgFlags = CB_RESPONSE_OK },
    };
    context->ServerFormatListResponse(context, &resp);

    if (has_text) {
        /* Request the text data from the client */
        CLIPRDR_FORMAT_DATA_REQUEST req = {
            .requestedFormatId = text_format_id,
        };
        context->ServerFormatDataRequest(context, &req);
    }

    return CHANNEL_RC_OK;
}

static UINT on_cliprdr_client_format_data_response(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_DATA_RESPONSE *resp)
{
    struct wlrdp_clipboard *cb = context->custom;

    if (resp->common.msgFlags != CB_RESPONSE_OK) return CHANNEL_RC_OK;

    const BYTE *data = resp->requestedFormatData;
    uint32_t len = resp->common.dataLen;
    if (!data || len == 0) return CHANNEL_RC_OK;

    /* Convert from UTF-16LE (CF_UNICODETEXT) to UTF-8.
     * Simple conversion: use WinPR's ConvertWCharNToUtf8Alloc. */
    size_t utf8_len = 0;
    char *utf8 = ConvertWCharNToUtf8Alloc((const WCHAR *)data,
                                           len / sizeof(WCHAR), &utf8_len);
    if (utf8 && utf8_len > 0) {
        /* Strip trailing null if present */
        while (utf8_len > 0 && utf8[utf8_len - 1] == '\0') utf8_len--;
        clipboard_set_text(cb, utf8, utf8_len);
        WLRDP_LOG_INFO("CLIPRDR: received %zu bytes text from client", utf8_len);
    }
    free(utf8);

    return CHANNEL_RC_OK;
}

static UINT on_cliprdr_client_format_data_request(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_DATA_REQUEST *req)
{
    struct wlrdp_clipboard *cb = context->custom;

    CLIPRDR_FORMAT_DATA_RESPONSE resp = {
        .common = { .msgFlags = CB_RESPONSE_FAIL },
    };

    if (cb->wl_clipboard_text && cb->wl_clipboard_len > 0 &&
        (req->requestedFormatId == CF_UNICODETEXT ||
         req->requestedFormatId == CF_TEXT)) {

        /* Convert UTF-8 to UTF-16LE for CF_UNICODETEXT */
        if (req->requestedFormatId == CF_UNICODETEXT) {
            size_t wlen = 0;
            WCHAR *wstr = ConvertUtf8NToWCharAlloc(cb->wl_clipboard_text,
                                                    cb->wl_clipboard_len, &wlen);
            if (wstr) {
                resp.common.msgFlags = CB_RESPONSE_OK;
                resp.common.dataLen = (wlen + 1) * sizeof(WCHAR);
                resp.requestedFormatData = (const BYTE *)wstr;
                context->ServerFormatDataResponse(context, &resp);
                free(wstr);
                return CHANNEL_RC_OK;
            }
        } else {
            /* CF_TEXT: send as-is (ASCII subset) */
            resp.common.msgFlags = CB_RESPONSE_OK;
            resp.common.dataLen = cb->wl_clipboard_len + 1;
            resp.requestedFormatData = (const BYTE *)cb->wl_clipboard_text;
            context->ServerFormatDataResponse(context, &resp);
            return CHANNEL_RC_OK;
        }
    }

    context->ServerFormatDataResponse(context, &resp);
    return CHANNEL_RC_OK;
}
```

- [ ] **Step 4: Add clipboard_notify_rdp function to clipboard.c**

This is called when the Wayland clipboard changes and we need to tell the RDP client:

```c
/* Called by the on_wl_clipboard_changed callback to push a format list to the RDP client */
void clipboard_notify_rdp(struct wlrdp_clipboard *cb)
{
    CliprdrServerContext *cliprdr = cb->cliprdr_context;
    if (!cliprdr) return;

    CLIPRDR_FORMAT formats[] = {
        { .formatId = CF_UNICODETEXT, .formatName = NULL },
        { .formatId = CF_TEXT, .formatName = NULL },
    };
    CLIPRDR_FORMAT_LIST list = {
        .numFormats = 2,
        .formats = formats,
    };

    cliprdr->ServerFormatList(cliprdr, &list);
}
```

Add the declaration to `clipboard.h`:

```c
void clipboard_notify_rdp(struct wlrdp_clipboard *cb);
```

- [ ] **Step 5: Add CLIPRDR open/close to rdp_peer.c**

Add `#include "clipboard.h"` and `#include <freerdp/server/cliprdr.h>` at the top of `rdp_peer.c`.

Add a `cliprdr_open` function:

```c
static bool cliprdr_open(struct wlrdp_peer_context *ctx,
                         struct wlrdp_clipboard *clipboard)
{
    CliprdrServerContext *cliprdr = cliprdr_server_context_new(ctx->gfx_vcm);
    if (!cliprdr) {
        WLRDP_LOG_WARN("failed to create CLIPRDR server context");
        return false;
    }

    cliprdr->rdpcontext = &ctx->base;
    cliprdr->custom = clipboard;
    cliprdr->useLongFormatNames = TRUE;
    cliprdr->streamFileClipEnabled = FALSE;
    cliprdr->canLockClipData = FALSE;
    cliprdr->autoInitializationSequence = TRUE;

    cliprdr->ClientCapabilities = on_cliprdr_client_capabilities;
    cliprdr->ClientFormatList = on_cliprdr_client_format_list;
    cliprdr->ClientFormatDataResponse = on_cliprdr_client_format_data_response;
    cliprdr->ClientFormatDataRequest = on_cliprdr_client_format_data_request;

    if (cliprdr->Open(cliprdr) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("CLIPRDR channel Open failed");
        cliprdr_server_context_free(cliprdr);
        return false;
    }

    clipboard->cliprdr_context = cliprdr;
    ctx->cliprdr_context = cliprdr;
    ctx->cliprdr_opened = true;
    WLRDP_LOG_INFO("CLIPRDR channel opened");
    return true;
}
```

Add cleanup to `gfx_cleanup`:

```c
    if (ctx->cliprdr_context) {
        CliprdrServerContext *cliprdr = ctx->cliprdr_context;
        if (ctx->cliprdr_opened) {
            cliprdr->Close(cliprdr);
        }
        cliprdr_server_context_free(cliprdr);
        ctx->cliprdr_context = NULL;
        ctx->cliprdr_opened = false;
    }
```

- [ ] **Step 6: Open CLIPRDR after DRDYNVC ready**

In `rdp_peer_check_vcm`, after the `disp_open(ctx)` call, add:

```c
    /* Forward declaration needed -- add clipboard param to rdp_peer_check_vcm
     * or store clipboard pointer in context. For now, use the simpler approach
     * of adding a clipboard pointer to the peer context. */
```

Add to `struct wlrdp_peer_context` in `rdp_peer.h`:

```c
    struct wlrdp_clipboard *clipboard;  /* set by main.c before activation */
```

Then in `rdp_peer_check_vcm`, after `disp_open(ctx)`:

```c
    if (ctx->clipboard) {
        if (!cliprdr_open(ctx, ctx->clipboard)) {
            WLRDP_LOG_WARN("CLIPRDR open failed, clipboard may not work");
        }
    }
```

- [ ] **Step 7: Verify the build compiles**

```bash
meson compile -C build
```

Expected: compiles with no errors.

- [ ] **Step 8: Commit**

```bash
git add src/session/clipboard.h src/session/clipboard.c \
    src/session/rdp_peer.h src/session/rdp_peer.c
git commit -m "feat: add CLIPRDR channel bridged to Wayland clipboard"
```

---

### Task 5: Clipboard -- Wire Into Event Loop

**Files:**
- Modify: `src/session/main.c`

- [ ] **Step 1: Add clipboard to the server struct and includes**

Add `#include "clipboard.h"` at the top of `main.c`.

Add to `struct wlrdp_server`:

```c
    struct wlrdp_clipboard clipboard;
    bool clipboard_active;
```

- [ ] **Step 2: Initialize clipboard after input init**

After the `input_init` call in `main()`, add:

```c
    if (!clipboard_init(&srv.clipboard, srv.comp.display_name,
                        on_wl_clipboard_changed, &srv)) {
        WLRDP_LOG_WARN("clipboard init failed, continuing without clipboard");
    } else {
        srv.clipboard_active = true;
    }
```

- [ ] **Step 3: Add the Wayland clipboard change callback**

Add before `main()`:

```c
static void on_wl_clipboard_changed(void *data, const char *text, size_t len)
{
    struct wlrdp_server *srv = data;
    if (!srv->client_active) return;
    clipboard_notify_rdp(&srv->clipboard);
}
```

- [ ] **Step 4: Add clipboard fd to epoll and handle events**

After adding `input_fd` to epoll, add:

```c
    int clipboard_fd = -1;
    if (srv.clipboard_active) {
        clipboard_fd = clipboard_get_fd(&srv.clipboard);
        epoll_add_fd(srv.epoll_fd, clipboard_fd);
    }
```

In the event loop, add a handler for the clipboard fd after the input fd handler:

```c
            if (srv.clipboard_active && fd == clipboard_fd) {
                if (clipboard_dispatch(&srv.clipboard) < 0) {
                    WLRDP_LOG_WARN("clipboard dispatch error");
                    srv.clipboard_active = false;
                }
                continue;
            }
```

Add clipboard flush alongside `input_flush`:

```c
    if (srv.clipboard_active) clipboard_flush(&srv.clipboard);
```

- [ ] **Step 5: Set the clipboard pointer on the peer context**

In `on_peer_accepted` and `accept_ipc_client`, where the peer context is set up, add:

```c
        if (srv->clipboard_active) {
            pctx->clipboard = &srv->clipboard;
        }
```

(Add after the `pctx->on_resize_data = srv;` line in both functions.)

- [ ] **Step 6: Add clipboard cleanup**

In the `cleanup:` section of `main()`, before `input_destroy`:

```c
    if (srv.clipboard_active) {
        clipboard_destroy(&srv.clipboard);
    }
```

- [ ] **Step 7: Build and test**

```bash
meson compile -C build
```

Start the session, connect with an RDP client, copy text on the remote desktop, and try pasting on the local machine (and vice versa).

- [ ] **Step 8: Commit**

```bash
git add src/session/main.c
git commit -m "feat: wire clipboard into session event loop"
```

---

### Task 6: Audio Output -- PipeWire Capture Stream

**Files:**
- Modify: `src/session/audio.h`
- Modify: `src/session/audio.c`

This task implements the PipeWire side of audio: capturing the session's audio output into PCM buffers. The RDP bridge comes in Task 7.

- [ ] **Step 1: Define the audio module interface in audio.h**

```c
#ifndef WLRDP_AUDIO_H
#define WLRDP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef WLRDP_HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* Audio format: 16-bit signed LE, stereo, 44100 Hz (matches common RDP client support) */
#define WLRDP_AUDIO_RATE      44100
#define WLRDP_AUDIO_CHANNELS  2
#define WLRDP_AUDIO_FRAME_SIZE (sizeof(int16_t) * WLRDP_AUDIO_CHANNELS)

/* Callback when audio data is available */
typedef void (*audio_data_cb)(void *data, const int16_t *samples,
                              uint32_t n_frames);

struct wlrdp_audio {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    audio_data_cb on_data;
    void *cb_data;

    bool started;
};

bool audio_init(struct wlrdp_audio *audio, audio_data_cb on_data, void *cb_data);
void audio_destroy(struct wlrdp_audio *audio);

/* Get the PipeWire event loop fd for epoll integration */
int audio_get_fd(struct wlrdp_audio *audio);

/* Process pending PipeWire events (call when fd is readable) */
int audio_dispatch(struct wlrdp_audio *audio);

#else /* !WLRDP_HAVE_PIPEWIRE */

struct wlrdp_audio {
    bool started;
};

static inline bool audio_init(struct wlrdp_audio *audio, void *on_data, void *cb_data)
{
    (void)audio; (void)on_data; (void)cb_data;
    return false;
}
static inline void audio_destroy(struct wlrdp_audio *audio) { (void)audio; }
static inline int audio_get_fd(struct wlrdp_audio *audio) { (void)audio; return -1; }
static inline int audio_dispatch(struct wlrdp_audio *audio) { (void)audio; return 0; }

#endif /* WLRDP_HAVE_PIPEWIRE */

#endif /* WLRDP_AUDIO_H */
```

- [ ] **Step 2: Implement the PipeWire capture stream in audio.c**

```c
#include "audio.h"
#include "common.h"

#ifdef WLRDP_HAVE_PIPEWIRE

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

static void on_process(void *userdata)
{
    struct wlrdp_audio *audio = userdata;

    struct pw_buffer *b = pw_stream_dequeue_buffer(audio->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf->datas[0].data) {
        pw_stream_queue_buffer(audio->stream, b);
        return;
    }

    uint32_t n_bytes = buf->datas[0].chunk->size;
    uint32_t n_frames = n_bytes / WLRDP_AUDIO_FRAME_SIZE;
    const int16_t *samples = buf->datas[0].data;

    if (audio->on_data && n_frames > 0) {
        audio->on_data(audio->cb_data, samples, n_frames);
    }

    pw_stream_queue_buffer(audio->stream, b);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                     enum pw_stream_state state,
                                     const char *error)
{
    WLRDP_LOG_INFO("PipeWire stream state: %s -> %s",
                    pw_stream_state_as_string(old),
                    pw_stream_state_as_string(state));
    if (error) {
        WLRDP_LOG_WARN("PipeWire stream error: %s", error);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_stream_state_changed,
};

bool audio_init(struct wlrdp_audio *audio, audio_data_cb on_data, void *cb_data)
{
    memset(audio, 0, sizeof(*audio));
    audio->on_data = on_data;
    audio->cb_data = cb_data;

    pw_init(NULL, NULL);

    audio->loop = pw_main_loop_new(NULL);
    if (!audio->loop) {
        WLRDP_LOG_ERROR("failed to create PipeWire main loop");
        return false;
    }

    audio->context = pw_context_new(
        pw_main_loop_get_loop(audio->loop), NULL, 0);
    if (!audio->context) {
        WLRDP_LOG_ERROR("failed to create PipeWire context");
        pw_main_loop_destroy(audio->loop);
        return false;
    }

    audio->core = pw_context_connect(audio->context, NULL, 0);
    if (!audio->core) {
        WLRDP_LOG_ERROR("failed to connect to PipeWire daemon");
        pw_context_destroy(audio->context);
        pw_main_loop_destroy(audio->loop);
        return false;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_STREAM_CAPTURE_SINK, "true",  /* capture sink output (monitor) */
        PW_KEY_NODE_NAME, "wlrdp-audio-capture",
        NULL);

    audio->stream = pw_stream_new(audio->core, "wlrdp-audio", props);
    if (!audio->stream) {
        WLRDP_LOG_ERROR("failed to create PipeWire stream");
        audio_destroy(audio);
        return false;
    }

    pw_stream_add_listener(audio->stream, &audio->stream_listener,
                           &stream_events, audio);

    /* Build audio format params */
    uint8_t param_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(param_buf, sizeof(param_buf));
    const struct spa_pod *params[1];

    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .rate = WLRDP_AUDIO_RATE,
        .channels = WLRDP_AUDIO_CHANNELS,
    };
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(audio->stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1) < 0) {
        WLRDP_LOG_ERROR("failed to connect PipeWire stream");
        audio_destroy(audio);
        return false;
    }

    audio->started = true;
    WLRDP_LOG_INFO("audio capture initialized (S16LE, %u Hz, %u ch)",
                    WLRDP_AUDIO_RATE, WLRDP_AUDIO_CHANNELS);
    return true;
}

int audio_get_fd(struct wlrdp_audio *audio)
{
    if (!audio->loop) return -1;
    struct pw_loop *loop = pw_main_loop_get_loop(audio->loop);
    return pw_loop_get_fd(loop);
}

int audio_dispatch(struct wlrdp_audio *audio)
{
    if (!audio->loop) return 0;

    /* Process any pending PipeWire events without blocking */
    struct pw_loop *loop = pw_main_loop_get_loop(audio->loop);
    int result = pw_loop_iterate(loop, 0);
    return result;
}

void audio_destroy(struct wlrdp_audio *audio)
{
    if (audio->stream) {
        pw_stream_destroy(audio->stream);
        audio->stream = NULL;
    }
    if (audio->core) {
        pw_core_disconnect(audio->core);
        audio->core = NULL;
    }
    if (audio->context) {
        pw_context_destroy(audio->context);
        audio->context = NULL;
    }
    if (audio->loop) {
        pw_main_loop_destroy(audio->loop);
        audio->loop = NULL;
    }
    if (audio->started) {
        pw_deinit();
        audio->started = false;
    }
}

#endif /* WLRDP_HAVE_PIPEWIRE */
```

- [ ] **Step 3: Verify the build compiles**

```bash
meson compile -C build
```

Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/session/audio.h src/session/audio.c
git commit -m "feat: add PipeWire audio capture stream"
```

---

### Task 7: Audio Output -- RDPSND Server Channel

**Files:**
- Modify: `src/session/audio.h`
- Modify: `src/session/audio.c`
- Modify: `src/session/rdp_peer.h`
- Modify: `src/session/rdp_peer.c`

This task opens the RDPSND dynamic virtual channel and feeds it PCM samples from the PipeWire stream.

- [ ] **Step 1: Add RDPSND context to rdp_peer.h**

Add to `struct wlrdp_peer_context`:

```c
    /* RDPSND state */
    void *rdpsnd_context;      /* RdpsndServerContext* */
    bool rdpsnd_opened;
    bool rdpsnd_ready;         /* true after Activated callback */
```

And add a pointer for the audio module:

```c
    struct wlrdp_audio *audio;  /* set by main.c before activation */
```

- [ ] **Step 2: Add RDPSND open function to rdp_peer.c**

Add includes:

```c
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/codec/audio.h>
```

Add the Activated callback and open function:

```c
static void on_rdpsnd_activated(RdpsndServerContext *context)
{
    freerdp_peer *client = context->rdpcontext->peer;
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    /* Find a PCM format the client supports */
    bool found = false;
    for (UINT16 i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT *fmt = &context->client_formats[i];
        if (fmt->wFormatTag == WAVE_FORMAT_PCM &&
            fmt->nChannels == 2 &&
            fmt->nSamplesPerSec == 44100 &&
            fmt->wBitsPerSample == 16) {
            context->SelectFormat(context, i);
            found = true;
            WLRDP_LOG_INFO("RDPSND: selected client format %u (PCM 44100/16/2)", i);
            break;
        }
    }

    if (!found) {
        WLRDP_LOG_WARN("RDPSND: no compatible PCM format found");
        return;
    }

    ctx->rdpsnd_ready = true;
    WLRDP_LOG_INFO("RDPSND: activated and ready");
}

static bool rdpsnd_open(struct wlrdp_peer_context *ctx)
{
    RdpsndServerContext *rdpsnd = rdpsnd_server_context_new(ctx->gfx_vcm);
    if (!rdpsnd) {
        WLRDP_LOG_WARN("failed to create RDPSND server context");
        return false;
    }

    rdpsnd->rdpcontext = &ctx->base;
    rdpsnd->use_dynamic_virtual_channel = TRUE;

    /* Advertise PCM S16LE 44100 Hz stereo.
     * These must be static since the context outlives this function. */
    static AUDIO_FORMAT server_formats[] = {
        {
            .wFormatTag = WAVE_FORMAT_PCM,
            .nChannels = 2,
            .nSamplesPerSec = 44100,
            .nAvgBytesPerSec = 44100 * 2 * 2,
            .nBlockAlign = 4,
            .wBitsPerSample = 16,
            .cbSize = 0,
            .data = NULL,
        },
    };
    rdpsnd->server_formats = server_formats;
    rdpsnd->num_server_formats = 1;

    static AUDIO_FORMAT src_format = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = 2,
        .nSamplesPerSec = 44100,
        .nAvgBytesPerSec = 44100 * 2 * 2,
        .nBlockAlign = 4,
        .wBitsPerSample = 16,
        .cbSize = 0,
        .data = NULL,
    };
    rdpsnd->src_format = &src_format;
    rdpsnd->latency = 50; /* 50ms buffer */

    rdpsnd->Activated = on_rdpsnd_activated;

    if (rdpsnd->Initialize(rdpsnd, FALSE) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("RDPSND Initialize failed");
        rdpsnd_server_context_free(rdpsnd);
        return false;
    }

    if (rdpsnd->Start(rdpsnd) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("RDPSND Start failed");
        rdpsnd_server_context_free(rdpsnd);
        return false;
    }

    ctx->rdpsnd_context = rdpsnd;
    ctx->rdpsnd_opened = true;
    WLRDP_LOG_INFO("RDPSND channel opened");
    return true;
}
```

- [ ] **Step 3: Add RDPSND cleanup to gfx_cleanup**

```c
    if (ctx->rdpsnd_context) {
        RdpsndServerContext *rdpsnd = ctx->rdpsnd_context;
        if (ctx->rdpsnd_opened) {
            rdpsnd->Stop(rdpsnd);
        }
        rdpsnd_server_context_free(rdpsnd);
        ctx->rdpsnd_context = NULL;
        ctx->rdpsnd_opened = false;
        ctx->rdpsnd_ready = false;
    }
```

- [ ] **Step 4: Open RDPSND after DRDYNVC ready**

In `rdp_peer_check_vcm`, after the CLIPRDR open call (added in Task 4), add:

```c
    if (!rdpsnd_open(ctx)) {
        WLRDP_LOG_WARN("RDPSND open failed, audio will not work");
    }
```

- [ ] **Step 5: Add rdp_peer_send_audio function**

Add to `rdp_peer.c`:

```c
bool rdp_peer_send_audio(freerdp_peer *client, const int16_t *samples,
                         uint32_t n_frames)
{
    struct wlrdp_peer_context *ctx =
        (struct wlrdp_peer_context *)client->context;

    if (!ctx->rdpsnd_ready || !ctx->rdpsnd_context) return false;

    RdpsndServerContext *rdpsnd = ctx->rdpsnd_context;

    /* Handle RDPSND messages before sending */
    rdpsnd_server_handle_messages(rdpsnd);

    uint32_t timestamp = (uint32_t)(now_ms() & 0xFFFF);
    UINT rc = rdpsnd->SendSamples(rdpsnd, samples, n_frames, timestamp);
    return (rc == CHANNEL_RC_OK);
}
```

Note: `now_ms()` is defined in `main.c`. Either move it to `common.h` or use a local implementation:

```c
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
```

Add the declaration to `rdp_peer.h`:

```c
bool rdp_peer_send_audio(freerdp_peer *client, const int16_t *samples,
                         uint32_t n_frames);
```

- [ ] **Step 6: Verify the build compiles**

```bash
meson compile -C build
```

Expected: compiles with no errors.

- [ ] **Step 7: Commit**

```bash
git add src/session/rdp_peer.h src/session/rdp_peer.c \
    src/session/audio.h src/session/audio.c
git commit -m "feat: add RDPSND server channel for audio output"
```

---

### Task 8: Audio Output -- Wire Into Event Loop

**Files:**
- Modify: `src/session/main.c`

- [ ] **Step 1: Add audio to the server struct and includes**

Add `#include "audio.h"` at the top of `main.c`.

Add to `struct wlrdp_server`:

```c
    struct wlrdp_audio audio;
    bool audio_active;
```

- [ ] **Step 2: Add the audio data callback**

Add before `main()`:

```c
#ifdef WLRDP_HAVE_PIPEWIRE
static void on_audio_data(void *data, const int16_t *samples, uint32_t n_frames)
{
    struct wlrdp_server *srv = data;
    if (!srv->client_active || !srv->client) return;
    rdp_peer_send_audio(srv->client, samples, n_frames);
}
#endif
```

- [ ] **Step 3: Initialize audio after clipboard init**

After the clipboard init block in `main()`, add:

```c
#ifdef WLRDP_HAVE_PIPEWIRE
    if (!audio_init(&srv.audio, on_audio_data, &srv)) {
        WLRDP_LOG_WARN("audio init failed, continuing without audio");
    } else {
        srv.audio_active = true;
    }
#endif
```

- [ ] **Step 4: Add audio fd to epoll and handle events**

After the clipboard fd epoll setup, add:

```c
    int audio_fd = -1;
    if (srv.audio_active) {
        audio_fd = audio_get_fd(&srv.audio);
        if (audio_fd >= 0) {
            epoll_add_fd(srv.epoll_fd, audio_fd);
        }
    }
```

In the event loop, add a handler for the audio fd:

```c
            if (srv.audio_active && audio_fd >= 0 && fd == audio_fd) {
                audio_dispatch(&srv.audio);
                continue;
            }
```

- [ ] **Step 5: Set the audio pointer on the peer context**

In `on_peer_accepted` and `accept_ipc_client`, after setting `pctx->clipboard`, add:

```c
        if (srv->audio_active) {
            pctx->audio = &srv->audio;
        }
```

- [ ] **Step 6: Add audio cleanup**

In the `cleanup:` section of `main()`, before `clipboard_destroy`:

```c
    if (srv.audio_active) {
        audio_destroy(&srv.audio);
    }
```

- [ ] **Step 7: Build and test**

```bash
meson compile -C build
```

Start the session, connect with an RDP client, and play audio in the remote session (e.g., `paplay /usr/share/sounds/freedesktop/stereo/bell.oga` or `speaker-test -t sine -f 440 -l 1`). Audio should be heard on the client.

- [ ] **Step 8: Commit**

```bash
git add src/session/main.c
git commit -m "feat: wire audio output into session event loop"
```

---

### Task 9: Integration Testing and Cleanup

**Files:**
- Review all modified files

- [ ] **Step 1: Full rebuild from clean state**

```bash
meson setup --wipe build
meson compile -C build
```

Expected: clean build, no warnings related to new code.

- [ ] **Step 2: Test clipboard manually**

1. Start: `./build/src/session/wlrdp-session --port 3389 --desktop-cmd labwc`
2. Connect with RDP client
3. Copy text on remote desktop (e.g., select text in foot terminal, Ctrl+Shift+C)
4. Paste on local machine -- should contain the copied text
5. Copy text on local machine
6. Paste on remote desktop (Ctrl+Shift+V in foot) -- should contain local text

- [ ] **Step 3: Test audio manually**

1. With session running, open a terminal in the remote desktop
2. Run: `speaker-test -t sine -f 440 -l 1` (requires `alsa-utils` installed)
3. Or: `pw-play /usr/share/sounds/freedesktop/stereo/bell.oga`
4. Audio should play through the RDP client

- [ ] **Step 4: Test graceful degradation**

1. Build without PipeWire: `meson configure build -Denable-pipewire=disabled`
2. Rebuild: `meson compile -C build`
3. Run session -- should work normally, just without audio
4. Clipboard should still function independently

- [ ] **Step 5: Test with compositor lacking ext-data-control-v1**

If the compositor doesn't support the protocol, clipboard should log a warning and continue without clipboard support. The rest of the session should work normally.

- [ ] **Step 6: Commit any fixes found during testing**

```bash
git add -u
git commit -m "fix: integration test fixes for clipboard and audio"
```

---

## Notes

**What this plan does NOT cover (deferred):**
- **AUDIN (microphone input):** Requires creating a PipeWire source node and injecting audio into the session. Significantly more complex than capture.
- **RDPECAM (webcam):** Requires creating a V4L2 loopback device or PipeWire camera source. Very different architecture.
- **File clipboard:** Only text copy/paste is implemented. File drag-and-drop (`streamFileClipEnabled`) is deferred.
- **Audio codec negotiation:** Only raw PCM is supported. Adding Opus or AAC encoding would improve bandwidth but adds FFmpeg audio codec complexity.

**Runtime dependencies added:**
- PipeWire daemon must be running in the session (cage/labwc should handle this via XDG autostart if WirePlumber is installed)
- The compositor must support `ext-data-control-v1` for clipboard (labwc and cage both support it)
