#ifndef WLRDP_INPUT_H
#define WLRDP_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_v1;

struct xkb_state;

struct wlrdp_input {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;

    struct zwlr_virtual_pointer_manager_v1 *vptr_mgr;
    struct zwlr_virtual_pointer_v1 *vptr;

    struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
    struct zwp_virtual_keyboard_v1 *vkbd;

    struct xkb_state *xkb_state;

    uint32_t width;
    uint32_t height;
};

bool input_init(struct wlrdp_input *inp, const char *display_name,
                uint32_t width, uint32_t height);
void input_pointer_motion(struct wlrdp_input *inp, uint32_t x, uint32_t y);
void input_pointer_button(struct wlrdp_input *inp, uint32_t button, bool pressed);
void input_pointer_axis(struct wlrdp_input *inp, int32_t value);
void input_keyboard_key(struct wlrdp_input *inp, uint32_t key, bool pressed);
void input_flush(struct wlrdp_input *inp);
int input_get_fd(struct wlrdp_input *inp);
int input_dispatch(struct wlrdp_input *inp);
void input_destroy(struct wlrdp_input *inp);

#endif /* WLRDP_INPUT_H */
