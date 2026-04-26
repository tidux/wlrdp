#include "common.h"
#include "ipc.h"
#include "compositor.h"
#include "capture.h"
#include "input.h"
#include "encoder.h"
#include "clipboard.h"
#include "audio.h"
#include "rdp_peer.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>

#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define FRAME_INTERVAL_MIN_MS  16  /* ~60 fps ceiling */
#define FRAME_INTERVAL_MAX_MS  100 /* 10 fps floor */
#define FRAME_INTERVAL_DEFAULT_MS 33 /* ~30 fps starting point */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct wlrdp_server {
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;
    int ipc_fd;

    struct wlrdp_compositor comp;
    struct wlrdp_capture capture;
    struct wlrdp_input input;
    struct wlrdp_encoder encoder;
    struct wlrdp_clipboard clipboard;
    bool clipboard_active;
    struct wlrdp_audio audio;
    bool audio_active;
    freerdp_listener *listener;
    freerdp_peer *client;

    int epoll_fd;
    int vcm_fd;            /* VCM event fd for DRDYNVC polling, -1 if inactive */
    bool client_active;
    bool encoder_initialized;
    uint64_t last_frame_ms;
    uint32_t frame_interval_ms;
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

static void init_encoder_for_client(struct wlrdp_server *srv)
{
    if (srv->encoder_initialized) {
        encoder_destroy(&srv->encoder);
        srv->encoder_initialized = false;
    }

    enum wlrdp_encoder_mode mode = WLRDP_ENCODER_RAW;
    uint8_t avc444_version = 0;

    uint32_t format = PIXEL_FORMAT_BGRX32;
    if (srv->client) {
        struct wlrdp_peer_context *pctx =
            (struct wlrdp_peer_context *)srv->client->context;
        format = pctx->pixel_format;

        rdpSettings *settings = srv->client->context->settings;

        enum wlrdp_send_mode send_mode = rdp_peer_get_send_mode(srv->client);
        switch (send_mode) {
        case WLRDP_SEND_GFX_AVC444V2:
            mode = WLRDP_ENCODER_H264_FREERDP;
            avc444_version = 2;
            break;
        case WLRDP_SEND_GFX_AVC444:
            mode = WLRDP_ENCODER_H264_FREERDP;
            avc444_version = 1;
            break;
        case WLRDP_SEND_GFX_AVC420:
            mode = WLRDP_ENCODER_H264_FREERDP;
            avc444_version = 0;
            break;
        case WLRDP_SEND_GFX_PROGRESSIVE:
            mode = WLRDP_ENCODER_PROGRESSIVE;
            avc444_version = 0;
            break;
        default:
            if (freerdp_settings_get_bool(settings, FreeRDP_NSCodec)) {
                mode = WLRDP_ENCODER_NSAC;
            } else {
                mode = WLRDP_ENCODER_RAW;
            }
            break;
        }
    }

    if (!encoder_init(&srv->encoder, mode, srv->width, srv->height,
                      avc444_version, format)) {
        WLRDP_LOG_ERROR("failed to initialize encoder");
        return;
    }

    encoder_request_keyframe(&srv->encoder); /* Ensure first frame is IDR (helps initial render/prompt) */

    srv->encoder_initialized = true;

    const char *mode_str;
    switch (srv->encoder.mode) {
    case WLRDP_ENCODER_H264_FREERDP:
        mode_str = avc444_version == 2 ? "AVC444v2" :
                   avc444_version == 1 ? "AVC444" : "H.264 (AVC420)";
        break;
    case WLRDP_ENCODER_PROGRESSIVE: mode_str = "Progressive"; break;
    case WLRDP_ENCODER_NSAC: mode_str = "NSCodec"; break;
    default:                   mode_str = "RAW"; break;
    }
    WLRDP_LOG_INFO("encoder mode: %s", mode_str);
}

static void on_frame_ready(void *data, uint8_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t stride)
{
    struct wlrdp_server *srv = data;

    if (!srv->client_active || !srv->client || !srv->encoder_initialized)
        return;

    uint64_t now = now_ms();
    if (now - srv->last_frame_ms < srv->frame_interval_ms) {
        capture_request_frame(&srv->capture);
        return;
    }

    uint64_t encode_start = now;

    if (!encoder_encode(&srv->encoder, pixels, stride)) {
        capture_request_frame(&srv->capture);
        return;
    }

    if (srv->encoder.out_len == 0 && srv->encoder.aux_len == 0) {
        /* Encoder buffering (shouldn't happen with zerolatency) */
        capture_request_frame(&srv->capture);
        return;
    }

    rdp_peer_send_frame(srv->client,
                        srv->encoder.out_buf, srv->encoder.out_len,
                        srv->encoder.aux_buf, srv->encoder.aux_len,
                        &srv->encoder.h264_meta,
                        &srv->encoder.h264_aux_meta,
                        srv->encoder.avc444_lc,
                        width, height, srv->encoder.is_keyframe);

    /* Adaptive framerate: target interval = 2x encode+send time,
     * clamped to [16ms, 100ms] */
    uint64_t encode_time = now_ms() - encode_start;
    uint32_t target = (uint32_t)(encode_time * 2);
    if (target < FRAME_INTERVAL_MIN_MS) target = FRAME_INTERVAL_MIN_MS;
    if (target > FRAME_INTERVAL_MAX_MS) target = FRAME_INTERVAL_MAX_MS;

    /* Smooth the interval to avoid jitter */
    srv->frame_interval_ms = (srv->frame_interval_ms * 3 + target) / 4;
    srv->last_frame_ms = now_ms();

    capture_request_frame(&srv->capture);
}

static void on_peer_resize(void *data, uint32_t width, uint32_t height, uint32_t scale)
{
    struct wlrdp_server *srv = data;

    /* width/height from the RDP client are already physical pixel dimensions.
     * scale is the client's DPI scale factor (e.g. 200 for Retina 2x).
     * We pass scale to wlr-randr so Wayland apps render at the right DPI. */

    /* Round to multiple of 16 to avoid odd logical sizes after scale (fixes cage crash
     * on large retina resolutions). No cap — full resolution supported with the gles2/auto renderer. */
    width = (width + 15) & ~15;
    height = (height + 15) & ~15;

    if (srv->width == (int)width && srv->height == (int)height)
        return;

    WLRDP_LOG_INFO("resizing compositor to %ux%u (scale %u%%)",
                    width, height, scale);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "WAYLAND_DISPLAY=%s wlr-randr --output HEADLESS-1 --custom-mode %dx%d --scale %g 2>/dev/null",
             srv->comp.display_name, width, height, (double)scale / 100.0);
    if (system(cmd) != 0) {
        WLRDP_LOG_WARN("failed to resize compositor output via wlr-randr");
    }

    srv->width = width;
    srv->height = height;

    if (srv->client) {
        rdp_peer_update_size(srv->client, width, height);
    }

    input_update_size(&srv->input, width, height);

    if (srv->encoder_initialized) {
        init_encoder_for_client(srv);
    }
}

#ifdef WLRDP_HAVE_PIPEWIRE
static void on_audio_data(void *data, const int16_t *samples, uint32_t n_frames)
{
    struct wlrdp_server *srv = data;
    if (!srv->client_active || !srv->client) return;
    rdp_peer_send_audio(srv->client, samples, n_frames);
}
#endif

static void on_wl_clipboard_changed(void *data, const char *text, size_t len)
{
    struct wlrdp_server *srv = data;
    (void)text; (void)len;
    if (!srv->client_active) return;
    clipboard_notify_rdp(&srv->clipboard);
}

static void disconnect_client(struct wlrdp_server *srv)
{
    if (srv->vcm_fd >= 0) {
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, srv->vcm_fd, NULL);
        srv->vcm_fd = -1;
    }

    if (srv->client) {
        freerdp_peer_context_free(srv->client);
        freerdp_peer_free(srv->client);
        srv->client = NULL;
        srv->client_active = false;
        WLRDP_LOG_INFO("client disconnected");
    }

    if (srv->encoder_initialized) {
        encoder_destroy(&srv->encoder);
        srv->encoder_initialized = false;
    }

    if (srv->ipc_fd >= 0) {
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_DISCONNECTED;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv->ipc_fd, &msg, -1);
    }
}

static bool accept_ipc_client(struct wlrdp_server *srv)
{
    struct wlrdp_ipc_msg msg;
    int peer_fd = -1;

    if (!ipc_recv_msg(srv->ipc_fd, &msg, &peer_fd)) {
        WLRDP_LOG_ERROR("IPC recv failed — daemon gone?");
        g_running = 0;
        return false;
    }

    if (msg.type == WLRDP_MSG_SHUTDOWN) {
        WLRDP_LOG_INFO("received shutdown from daemon");
        g_running = 0;
        return false;
    }

    if (msg.type != WLRDP_MSG_NEW_CLIENT || peer_fd < 0) {
        WLRDP_LOG_WARN("unexpected IPC message type %u", msg.type);
        if (peer_fd >= 0) close(peer_fd);
        return false;
    }

    disconnect_client(srv);

    WLRDP_LOG_INFO("received new client fd %d from daemon", peer_fd);

    freerdp_peer *client = freerdp_peer_new(peer_fd);
    if (!client) {
        WLRDP_LOG_ERROR("freerdp_peer_new failed");
        close(peer_fd);
        return false;
    }

    client->ContextSize = sizeof(struct wlrdp_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return false;
    }

    if (!rdp_peer_init(client, srv->cert_file, srv->key_file, &srv->input)) {
        WLRDP_LOG_ERROR("rdp_peer_init failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return false;
    }

    {
        struct wlrdp_peer_context *pctx =
            (struct wlrdp_peer_context *)client->context;
        pctx->width = srv->width;
        pctx->height = srv->height;
        pctx->on_resize = on_peer_resize;
        pctx->on_resize_data = srv;
        if (srv->clipboard_active)
            pctx->clipboard = &srv->clipboard;
        if (srv->audio_active)
            pctx->audio = &srv->audio;
    }

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return false;
    }

    srv->client = client;
    WLRDP_LOG_INFO("IPC client initialized");
    return true;
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

    {
        struct wlrdp_peer_context *pctx =
            (struct wlrdp_peer_context *)client->context;
        pctx->width = srv->width;
        pctx->height = srv->height;
        pctx->on_resize = on_peer_resize;
        pctx->on_resize_data = srv;
        if (srv->clipboard_active)
            pctx->clipboard = &srv->clipboard;
        if (srv->audio_active)
            pctx->audio = &srv->audio;
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
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        return (errno == EEXIST);
    }
    return true;
}

static void epoll_add_vcm(struct wlrdp_server *srv)
{
    if (!srv->client)
        return;

    int fd = rdp_peer_get_vcm_fd(srv->client);
    if (fd >= 0 && fd != srv->vcm_fd) {
        epoll_add_fd(srv->epoll_fd, fd);
        srv->vcm_fd = fd;
        WLRDP_LOG_INFO("VCM fd %d added to epoll", fd);
    }
}

static void epoll_update_client(struct wlrdp_server *srv)
{
    if (!srv->client)
        return;

    HANDLE events[32];
    DWORD count = srv->client->GetEventHandles(srv->client, events, 32);
    for (DWORD i = 0; i < count; i++) {
        int fd = GetEventFileDescriptor(events[i]);
        if (fd >= 0) {
            epoll_add_fd(srv->epoll_fd, fd);
        }
    }

    epoll_add_vcm(srv);
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
        "  --port PORT          RDP listen port (standalone mode, default: 3389)\n"
        "  --ipc-fd FD          IPC socket fd (daemon mode)\n"
        "  --width WIDTH        Display width (default: 800)\n"
        "  --height HEIGHT      Display height (default: 600)\n"
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
        .width = 3840,  /* Large initial size to support full retina resolutions without crashing cage on dynamic resize from 800x600 */
        .height = 2160,
        .desktop_cmd = "foot",
        .cert_file = NULL,
        .key_file = NULL,
        .ipc_fd = -1,
        .vcm_fd = -1,
        .frame_interval_ms = FRAME_INTERVAL_DEFAULT_MS,
    };

    static struct option long_opts[] = {
        { "port",        required_argument, NULL, 'p' },
        { "ipc-fd",      required_argument, NULL, 'i' },
        { "width",       required_argument, NULL, 'W' },
        { "height",      required_argument, NULL, 'H' },
        { "desktop-cmd", required_argument, NULL, 'd' },
        { "cert",        required_argument, NULL, 'c' },
        { "key",         required_argument, NULL, 'k' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:i:W:H:d:c:k:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': srv.port = atoi(optarg); break;
        case 'i': srv.ipc_fd = atoi(optarg); break;
        case 'W': srv.width = atoi(optarg); break;
        case 'H': srv.height = atoi(optarg); break;
        case 'd': srv.desktop_cmd = optarg; break;
        case 'c': srv.cert_file = optarg; break;
        case 'k': srv.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    bool standalone = (srv.ipc_fd < 0);

    if (standalone) {
        WLRDP_LOG_INFO("running in standalone mode (no daemon)");
    } else {
        WLRDP_LOG_INFO("running in IPC mode (fd=%d)", srv.ipc_fd);
    }

    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (standalone && (!srv.cert_file || !srv.key_file)) {
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

    if (!clipboard_init(&srv.clipboard, srv.comp.display_name,
                        on_wl_clipboard_changed, &srv)) {
        WLRDP_LOG_WARN("clipboard init failed, continuing without clipboard");
    } else {
        srv.clipboard_active = true;
    }

#ifdef WLRDP_HAVE_PIPEWIRE
    if (!audio_init(&srv.audio, on_audio_data, &srv)) {
        WLRDP_LOG_WARN("audio init failed, continuing without audio");
    } else {
        srv.audio_active = true;
    }
#endif

    /* Encoder is initialized per-client after capability negotiation */

    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    int capture_fd = capture_get_fd(&srv.capture);
    epoll_add_fd(srv.epoll_fd, capture_fd);

    int input_fd = input_get_fd(&srv.input);
    epoll_add_fd(srv.epoll_fd, input_fd);

    int clipboard_fd = -1;
    if (srv.clipboard_active) {
        clipboard_fd = clipboard_get_fd(&srv.clipboard);
        epoll_add_fd(srv.epoll_fd, clipboard_fd);
    }

    int audio_fd = -1;
    if (srv.audio_active) {
        audio_fd = audio_get_fd(&srv.audio);
        if (audio_fd >= 0) {
            epoll_add_fd(srv.epoll_fd, audio_fd);
        }
    }

    if (standalone) {
        srv.listener = freerdp_listener_new();
        if (!srv.listener) {
            WLRDP_LOG_ERROR("freerdp_listener_new failed");
            goto cleanup;
        }

        srv.listener->info = &srv;
        srv.listener->PeerAccepted = on_peer_accepted;

        if (!srv.listener->Open(srv.listener, "0.0.0.0", srv.port)) {
            WLRDP_LOG_ERROR("failed to listen on port %d", srv.port);
            goto cleanup;
        }

        WLRDP_LOG_INFO("listening on port %d", srv.port);

        HANDLE events[32];
        DWORD count = srv.listener->GetEventHandles(srv.listener, events, 32);
        for (DWORD i = 0; i < count; i++) {
            int fd = GetEventFileDescriptor(events[i]);
            if (fd >= 0) epoll_add_fd(srv.epoll_fd, fd);
        }
    } else {
        epoll_add_fd(srv.epoll_fd, srv.ipc_fd);

        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_READY;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv.ipc_fd, &msg, -1);
    }

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
        if (srv.clipboard_active) clipboard_flush(&srv.clipboard);

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
                    WLRDP_LOG_ERROR("Wayland capture dispatch error");
                    g_running = 0;
                }
                continue;
            }

            if (fd == input_fd) {
                if (input_dispatch(&srv.input) < 0) {
                    WLRDP_LOG_ERROR("Wayland input dispatch error");
                    g_running = 0;
                }
                continue;
            }

            if (srv.clipboard_active && fd == clipboard_fd) {
                if (clipboard_dispatch(&srv.clipboard) < 0) {
                    WLRDP_LOG_WARN("clipboard dispatch error");
                    srv.clipboard_active = false;
                }
                continue;
            }

            if (srv.audio_active && audio_fd >= 0 && fd == audio_fd) {
                audio_dispatch(&srv.audio);
                continue;
            }

            if (!standalone && fd == srv.ipc_fd) {
                accept_ipc_client(&srv);
                continue;
            }

            if (srv.vcm_fd >= 0 && fd == srv.vcm_fd) {
                if (!rdp_peer_check_vcm(srv.client)) {
                    WLRDP_LOG_WARN("VCM fatal error");
                    disconnect_client(&srv);
                }
                continue;
            }

            if (standalone && srv.listener) {
                if (!srv.listener->CheckFileDescriptor(srv.listener)) {
                    WLRDP_LOG_ERROR("listener check failed");
                    g_running = 0;
                }
            }
        }

        if (srv.client) {
            struct wlrdp_peer_context *ctx =
                (struct wlrdp_peer_context *)srv.client->context;

            if (!srv.client->CheckFileDescriptor(srv.client)) {
                disconnect_client(&srv);
                continue;
            }

            input_flush(&srv.input);

            /* Pick up new fds (socket, VCM, etc.) */
            epoll_update_client(&srv);

            /* Always pump VCM to process GFX responses */
            if (!rdp_peer_check_vcm(srv.client)) {
                WLRDP_LOG_WARN("VCM fatal error");
                disconnect_client(&srv);
                continue;
            }

            if (ctx->activated && !srv.client_active) {
                /* If GFX/VCM is active, wait for negotiation to complete
                 * before starting frames. Otherwise start immediately. */
                if (!ctx->gfx_vcm || ctx->gfx_ready) {
                    srv.client_active = true;
                    srv.frame_interval_ms = FRAME_INTERVAL_DEFAULT_MS;
                    init_encoder_for_client(&srv);
                    WLRDP_LOG_INFO("starting frame capture");
                    capture_request_frame(&srv.capture);
                }
            }

            /* GFX negotiation completed — now start frames */
            if (ctx->activated && !srv.client_active && ctx->gfx_ready) {
                srv.client_active = true;
                srv.frame_interval_ms = FRAME_INTERVAL_DEFAULT_MS;
                init_encoder_for_client(&srv);
                WLRDP_LOG_INFO("GFX ready, starting frame capture with H.264");
                capture_request_frame(&srv.capture);
            }

            /* GFX VCM failed — fall back and start with NSCodec */
            if (ctx->activated && !srv.client_active &&
                ctx->gfx_vcm == NULL && !ctx->gfx_ready) {
                srv.client_active = true;
                srv.frame_interval_ms = FRAME_INTERVAL_DEFAULT_MS;
                init_encoder_for_client(&srv);
                WLRDP_LOG_INFO("GFX failed, starting frame capture with NSCodec");
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
    if (srv.ipc_fd >= 0) {
        struct wlrdp_ipc_msg msg = {
            .type = WLRDP_MSG_STATUS,
            .payload_len = 4,
        };
        uint32_t status = WLRDP_STATUS_TERMINATED;
        memcpy(msg.payload, &status, 4);
        ipc_send_msg(srv.ipc_fd, &msg, -1);
        close(srv.ipc_fd);
    }
    if (srv.epoll_fd >= 0) {
        close(srv.epoll_fd);
    }
    if (srv.encoder_initialized) {
        encoder_destroy(&srv.encoder);
    }
    if (srv.audio_active) {
        audio_destroy(&srv.audio);
    }
    if (srv.clipboard_active) {
        clipboard_destroy(&srv.clipboard);
    }
    input_destroy(&srv.input);
    capture_destroy(&srv.capture);
    compositor_destroy(&srv.comp);

    WLRDP_LOG_INFO("bye");
    return 0;
}
