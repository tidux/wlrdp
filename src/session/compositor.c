#include "compositor.h"
#include "common.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>

static bool wait_for_file(const char *path, int timeout_ms)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000 }; /* 50ms */
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        if (access(path, F_OK) == 0) {
            return true;
        }
        nanosleep(&ts, NULL);
        elapsed += 50;
    }

    return false;
}

static bool read_display_name(const char *path, char *out, size_t out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n <= 0) return false;

    /* Strip trailing newline */
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        n--;
    out[n] = '\0';

    return n > 0;
}

bool compositor_launch(struct wlrdp_compositor *comp,
                       const char *desktop_cmd,
                       int width, int height)
{
    if (!getenv("XDG_RUNTIME_DIR")) {
        char dir[64];
        snprintf(dir, sizeof(dir), "/run/user/%d", getuid());
        mkdir(dir, 0700);
        setenv("XDG_RUNTIME_DIR", dir, 1);
        WLRDP_LOG_WARN("XDG_RUNTIME_DIR not set, using %s", dir);
    }

    /* Temp file where the wrapper script will write WAYLAND_DISPLAY.
     * Cage picks its own socket name (wayland-0, wayland-1, etc.)
     * via wl_display_add_socket_auto() and sets WAYLAND_DISPLAY for
     * its child. We capture it through this file. */
    char display_file[128];
    snprintf(display_file, sizeof(display_file),
             "/tmp/wlrdp-display-%d", getpid());
    unlink(display_file);

    /* Build a wrapper shell command: write WAYLAND_DISPLAY to our file,
     * set output resolution via wlr-randr, then exec the desktop command.
     * Cage picks its own socket name and sets WAYLAND_DISPLAY for children. */
    char wrapper[1024];
    snprintf(wrapper, sizeof(wrapper),
             "echo \"$WAYLAND_DISPLAY\" > %s && "
             "wlr-randr --output HEADLESS-1 --custom-mode %dx%d 2>/dev/null; "
             "exec %s",
             display_file, width, height, desktop_cmd);

    pid_t pid = fork();
    if (pid < 0) {
        WLRDP_LOG_ERROR("fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        /* Child: exec cage */
        setenv("WLR_BACKENDS", "headless", 1);
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
        setenv("WLR_RENDERER", "pixman", 1);
        setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
        execlp("cage", "cage", "--", "/bin/sh", "-c", wrapper, NULL);
        fprintf(stderr, "[wlrdp] ERROR: exec cage failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    comp->pid = pid;
    WLRDP_LOG_INFO("launched cage (pid %d) at %dx%d", pid, width, height);

    /* Wait for the wrapper to write the display name */
    if (!wait_for_file(display_file, 5000)) {
        WLRDP_LOG_ERROR("timed out waiting for cage display name");
        unlink(display_file);
        compositor_destroy(comp);
        return false;
    }

    /* Small delay for the file to be fully written */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000 };
    nanosleep(&ts, NULL);

    if (!read_display_name(display_file, comp->display_name,
                           sizeof(comp->display_name))) {
        WLRDP_LOG_ERROR("failed to read display name from %s", display_file);
        unlink(display_file);
        compositor_destroy(comp);
        return false;
    }
    unlink(display_file);

    /* Now wait for the actual socket to appear */
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char socket_path[512];
    snprintf(socket_path, sizeof(socket_path), "%s/%s",
             xdg, comp->display_name);

    if (!wait_for_file(socket_path, 3000)) {
        WLRDP_LOG_ERROR("timed out waiting for socket %s", socket_path);
        compositor_destroy(comp);
        return false;
    }

    WLRDP_LOG_INFO("cage ready on display '%s'", comp->display_name);
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
