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

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    (void)version;
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
    if (inp->vkbd) zwp_virtual_keyboard_v1_destroy(inp->vkbd);
    if (inp->vptr) zwlr_virtual_pointer_v1_destroy(inp->vptr);
    if (inp->vkbd_mgr) zwp_virtual_keyboard_manager_v1_destroy(inp->vkbd_mgr);
    if (inp->vptr_mgr) zwlr_virtual_pointer_manager_v1_destroy(inp->vptr_mgr);
    if (inp->seat) wl_seat_destroy(inp->seat);
    if (inp->registry) wl_registry_destroy(inp->registry);
    if (inp->display) wl_display_disconnect(inp->display);
    memset(inp, 0, sizeof(*inp));
}
