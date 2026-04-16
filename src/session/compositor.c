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
