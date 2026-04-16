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
    if (cap->buffer) wl_buffer_destroy(cap->buffer);
    if (cap->pool) wl_shm_pool_destroy(cap->pool);
    if (cap->pixels) munmap(cap->pixels, cap->buf_size);
    if (cap->shm_fd >= 0) close(cap->shm_fd);
    if (cap->screencopy_mgr) zwlr_screencopy_manager_v1_destroy(cap->screencopy_mgr);
    if (cap->output) wl_output_destroy(cap->output);
    if (cap->shm) wl_shm_destroy(cap->shm);
    if (cap->registry) wl_registry_destroy(cap->registry);
    if (cap->display) wl_display_disconnect(cap->display);
    memset(cap, 0, sizeof(*cap));
    cap->shm_fd = -1;
}
