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

bool capture_init(struct wlrdp_capture *cap, const char *display_name,
                  capture_frame_cb cb, void *cb_data);
bool capture_request_frame(struct wlrdp_capture *cap);
int capture_get_fd(struct wlrdp_capture *cap);
int capture_dispatch(struct wlrdp_capture *cap);
void capture_destroy(struct wlrdp_capture *cap);

#endif /* WLRDP_CAPTURE_H */
