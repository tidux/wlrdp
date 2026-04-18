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

    /* CLIPRDR context -- set by rdp_peer after channel open */
    void *cliprdr_context;
};

bool clipboard_init(struct wlrdp_clipboard *cb, const char *display_name,
                    void (*on_change)(void *data, const char *text, size_t len),
                    void *cb_data);
void clipboard_destroy(struct wlrdp_clipboard *cb);

/* Set the Wayland clipboard to the given text (called when RDP client pastes) */
void clipboard_set_text(struct wlrdp_clipboard *cb, const char *text, size_t len);

/* Open/close the CLIPRDR dynamic virtual channel */
bool clipboard_open_cliprdr(struct wlrdp_clipboard *cb, void *vcm);
void clipboard_close_cliprdr(struct wlrdp_clipboard *cb);

/* Notify the RDP client that the Wayland clipboard changed */
void clipboard_notify_rdp(struct wlrdp_clipboard *cb);

int clipboard_get_fd(struct wlrdp_clipboard *cb);
int clipboard_dispatch(struct wlrdp_clipboard *cb);
void clipboard_flush(struct wlrdp_clipboard *cb);

#endif /* WLRDP_CLIPBOARD_H */
