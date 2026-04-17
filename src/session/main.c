#include "common.h"
#include "compositor.h"
#include "capture.h"
#include "input.h"
#include "encoder.h"
#include "rdp_peer.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>

#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define FRAME_MIN_INTERVAL_MS 33 /* ~30 fps max */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct wlrdp_server {
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;

    struct wlrdp_compositor comp;
    struct wlrdp_capture capture;
    struct wlrdp_input input;
    struct wlrdp_encoder encoder;
    freerdp_listener *listener;
    freerdp_peer *client;

    int epoll_fd;
    bool client_active;
    uint64_t last_frame_ms;
};

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGCHLD) {
        g_child_exited = 1;
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_frame_ready(void *data, uint8_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t stride)
{
    struct wlrdp_server *srv = data;

    if (!srv->client_active || !srv->client) return;

    /* Throttle: skip frame if too soon */
    uint64_t now = now_ms();
    if (now - srv->last_frame_ms < FRAME_MIN_INTERVAL_MS) {
        capture_request_frame(&srv->capture);
        return;
    }
    srv->last_frame_ms = now;

    /* Flip image vertically — screencopy gives top-down, but
     * SurfaceBits with codecID=0 expects bottom-up row order. */
    uint32_t row_bytes = width * 4;
    for (uint32_t top = 0, bot = height - 1; top < bot; top++, bot--) {
        uint8_t *a = pixels + top * stride;
        uint8_t *b = pixels + bot * stride;
        for (uint32_t i = 0; i < row_bytes; i++) {
            uint8_t tmp = a[i];
            a[i] = b[i];
            b[i] = tmp;
        }
    }

    rdp_peer_send_frame(srv->client, pixels, width * height * 4,
                        width, height);

    capture_request_frame(&srv->capture);
}

static BOOL on_peer_accepted(freerdp_listener *listener,
                             freerdp_peer *client)
{
    struct wlrdp_server *srv = listener->info;

    if (srv->client) {
        WLRDP_LOG_WARN("rejecting second client (single-session mode)");
        freerdp_peer_free(client);
        return TRUE;
    }

    client->ContextSize = sizeof(struct wlrdp_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return FALSE;
    }

    if (!rdp_peer_init(client, srv->cert_file, srv->key_file, &srv->input)) {
        WLRDP_LOG_ERROR("rdp_peer_init failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    srv->client = client;
    WLRDP_LOG_INFO("RDP client accepted");

    return TRUE;
}

static bool epoll_add_fd(int epoll_fd, int fd)
{
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static void generate_self_signed_cert(const char *cert_file,
                                      const char *key_file)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 "
             "-keyout '%s' -out '%s' "
             "-days 365 -nodes -subj '/CN=wlrdp' 2>/dev/null",
             key_file, cert_file);

    WLRDP_LOG_INFO("generating self-signed TLS certificate...");
    if (system(cmd) != 0) {
        WLRDP_LOG_ERROR("failed to generate certificate");
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT          RDP listen port (default: 3389)\n"
        "  --width WIDTH        Display width (default: 1920)\n"
        "  --height HEIGHT      Display height (default: 1080)\n"
        "  --desktop-cmd CMD    Command to run inside cage (default: foot)\n"
        "  --cert FILE          TLS certificate file\n"
        "  --key FILE           TLS private key file\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    struct wlrdp_server srv = {
        .port = WLRDP_DEFAULT_PORT,
        .width = WLRDP_DEFAULT_WIDTH,
        .height = WLRDP_DEFAULT_HEIGHT,
        .desktop_cmd = "foot",
        .cert_file = NULL,
        .key_file = NULL,
    };

    static struct option long_opts[] = {
        { "port",        required_argument, NULL, 'p' },
        { "width",       required_argument, NULL, 'W' },
        { "height",      required_argument, NULL, 'H' },
        { "desktop-cmd", required_argument, NULL, 'd' },
        { "cert",        required_argument, NULL, 'c' },
        { "key",         required_argument, NULL, 'k' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:W:H:d:c:k:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': srv.port = atoi(optarg); break;
        case 'W': srv.width = atoi(optarg); break;
        case 'H': srv.height = atoi(optarg); break;
        case 'd': srv.desktop_cmd = optarg; break;
        case 'c': srv.cert_file = optarg; break;
        case 'k': srv.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (!srv.cert_file || !srv.key_file) {
        generate_self_signed_cert(auto_cert, auto_key);
        if (!srv.cert_file) srv.cert_file = auto_cert;
        if (!srv.key_file) srv.key_file = auto_key;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    WLRDP_LOG_INFO("launching compositor: cage -- %s (%dx%d)",
                    srv.desktop_cmd, srv.width, srv.height);
    if (!compositor_launch(&srv.comp, srv.desktop_cmd,
                           srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to launch compositor");
        return 1;
    }

    if (!capture_init(&srv.capture, srv.comp.display_name,
                      on_frame_ready, &srv)) {
        WLRDP_LOG_ERROR("failed to initialize capture");
        compositor_destroy(&srv.comp);
        return 1;
    }

    if (!input_init(&srv.input, srv.comp.display_name,
                    srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize input");
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    if (!encoder_init(&srv.encoder, srv.width, srv.height)) {
        WLRDP_LOG_ERROR("failed to initialize encoder");
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    srv.listener = freerdp_listener_new();
    if (!srv.listener) {
        WLRDP_LOG_ERROR("freerdp_listener_new failed");
        encoder_destroy(&srv.encoder);
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    srv.listener->info = &srv;
    srv.listener->PeerAccepted = on_peer_accepted;

    if (!srv.listener->Open(srv.listener, "0.0.0.0", srv.port)) {
        WLRDP_LOG_ERROR("failed to listen on port %d", srv.port);
        freerdp_listener_free(srv.listener);
        encoder_destroy(&srv.encoder);
        input_destroy(&srv.input);
        capture_destroy(&srv.capture);
        compositor_destroy(&srv.comp);
        return 1;
    }

    WLRDP_LOG_INFO("listening on port %d", srv.port);

    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    HANDLE events[32];
    DWORD count = srv.listener->GetEventHandles(srv.listener, events, 32);
    for (DWORD i = 0; i < count; i++) {
        int fd = GetEventFileDescriptor(events[i]);
        if (fd >= 0) {
            epoll_add_fd(srv.epoll_fd, fd);
        }
    }

    int capture_fd = capture_get_fd(&srv.capture);
    epoll_add_fd(srv.epoll_fd, capture_fd);

    WLRDP_LOG_INFO("entering main event loop");

    while (g_running) {
        if (g_child_exited) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid == srv.comp.pid) {
                WLRDP_LOG_INFO("cage exited, shutting down");
                break;
            }
            g_child_exited = 0;
        }

        wl_display_flush(srv.capture.display);
        input_flush(&srv.input);

        struct epoll_event events[16];
        int nfds = epoll_wait(srv.epoll_fd, events, 16, 16);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            WLRDP_LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == capture_fd) {
                if (capture_dispatch(&srv.capture) < 0) {
                    WLRDP_LOG_ERROR("Wayland dispatch error");
                    g_running = 0;
                }
                continue;
            }

            if (!srv.listener->CheckFileDescriptor(srv.listener)) {
                WLRDP_LOG_ERROR("listener check failed");
                g_running = 0;
            }
        }

        if (srv.client) {
            struct wlrdp_peer_context *ctx =
                (struct wlrdp_peer_context *)srv.client->context;

            if (!srv.client->CheckFileDescriptor(srv.client)) {
                WLRDP_LOG_INFO("client disconnected");
                freerdp_peer_context_free(srv.client);
                freerdp_peer_free(srv.client);
                srv.client = NULL;
                srv.client_active = false;
                continue;
            }

            if (ctx->activated && !srv.client_active) {
                srv.client_active = true;
                WLRDP_LOG_INFO("starting frame capture");
                capture_request_frame(&srv.capture);
            }
        }
    }

cleanup:
    WLRDP_LOG_INFO("shutting down");

    if (srv.client) {
        freerdp_peer_context_free(srv.client);
        freerdp_peer_free(srv.client);
    }
    if (srv.listener) {
        srv.listener->Close(srv.listener);
        freerdp_listener_free(srv.listener);
    }
    if (srv.epoll_fd >= 0) {
        close(srv.epoll_fd);
    }
    encoder_destroy(&srv.encoder);
    input_destroy(&srv.input);
    capture_destroy(&srv.capture);
    compositor_destroy(&srv.comp);

    WLRDP_LOG_INFO("bye");
    return 0;
}
