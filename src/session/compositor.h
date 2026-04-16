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
