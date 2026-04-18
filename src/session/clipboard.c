#include "clipboard.h"
#include "common.h"
#include "ext-data-control-v1-client-protocol.h"

#include <errno.h>
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

    wl_display_flush(cb->display);

    /* Switch to blocking mode for the read */
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
    (void)cb;

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
    (void)device;

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
    (void)data; (void)device;
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

void clipboard_notify_rdp(struct wlrdp_clipboard *cb)
{
    /* Stub -- real implementation added when CLIPRDR channel is wired (Task 4) */
    if (!cb->cliprdr_context) return;
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
