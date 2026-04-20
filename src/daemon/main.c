#include "common.h"
#include "ipc.h"
#include "auth.h"
#include "session_mgr.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>

#include <getopt.h>
#include <signal.h>
#include <sys/epoll.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_exited = 0;

struct daemon_state {
    int port;
    int width;
    int height;
    const char *desktop_cmd;
    const char *cert_file;
    const char *key_file;

    freerdp_listener *listener;
    struct wlrdp_session_mgr session_mgr;
    int epoll_fd;
};

/* Custom peer context to hold auth info during connection */
struct daemon_peer_context {
    rdpContext base;
    struct daemon_state *daemon;
};

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGCHLD) {
        g_child_exited = 1;
    }
}

static BOOL on_peer_logon(freerdp_peer *client,
                          const SEC_WINNT_AUTH_IDENTITY *identity,
                          BOOL automatic)
{
    (void)automatic;
    struct daemon_peer_context *ctx =
        (struct daemon_peer_context *)client->context;

    const char *username = (const char *)identity->User;
    const char *password = (const char *)identity->Password;

    if (!username || !password) {
        WLRDP_LOG_WARN("logon: missing username or password");
        return FALSE;
    }

    WLRDP_LOG_INFO("logon attempt for user '%s'", username);

    struct wlrdp_auth_result auth;
    if (!auth_check_credentials(username, password, &auth)) {
        return FALSE;
    }

    /* Find or create session */
    struct wlrdp_session_entry *entry =
        session_mgr_get_session(&ctx->daemon->session_mgr, &auth);
    if (!entry) {
        WLRDP_LOG_ERROR("failed to get session for '%s'", username);
        return FALSE;
    }

    return TRUE;
}

static BOOL on_post_connect(freerdp_peer *client)
{
    (void)client;
    WLRDP_LOG_INFO("daemon: peer post-connect");
    return TRUE;
}

static BOOL on_activate(freerdp_peer *client)
{
    (void)client;
    WLRDP_LOG_INFO("daemon: peer activated (should not happen — "
                    "fd should be passed to session before this)");
    return TRUE;
}

static BOOL on_peer_accepted(freerdp_listener *listener,
                             freerdp_peer *client)
{
    struct daemon_state *state = listener->info;

    client->ContextSize = sizeof(struct daemon_peer_context);
    if (!freerdp_peer_context_new(client)) {
        WLRDP_LOG_ERROR("freerdp_peer_context_new failed");
        freerdp_peer_free(client);
        return FALSE;
    }

    struct daemon_peer_context *ctx =
        (struct daemon_peer_context *)client->context;
    ctx->daemon = state;

    rdpSettings *settings = client->context->settings;

    /* Load TLS certificate and key */
    rdpCertificate *cert = freerdp_certificate_new_from_file(state->cert_file);
    if (!cert) {
        WLRDP_LOG_ERROR("failed to load certificate");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1);

    rdpPrivateKey *key = freerdp_key_new_from_file(state->key_file);
    if (!key) {
        WLRDP_LOG_ERROR("failed to load private key");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);

    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceCommandsEnabled, TRUE);

    client->PostConnect = on_post_connect;
    client->Activate = on_activate;
    client->Logon = on_peer_logon;

    if (!client->Initialize(client)) {
        WLRDP_LOG_ERROR("peer Initialize failed");
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
        return FALSE;
    }

    WLRDP_LOG_INFO("RDP peer accepted, waiting for logon");

    /* The peer will go through TLS handshake and capability exchange.
     * When the user provides credentials, on_peer_logon fires.
     * We need to pump the peer in the event loop until that happens. */

    /* TODO: add peer to tracked list for event loop pumping.
     * For now, pump inline. */
    while (g_running) {
        if (!client->CheckFileDescriptor(client)) {
            WLRDP_LOG_INFO("peer disconnected during handshake");
            break;
        }

        struct daemon_peer_context *pctx =
            (struct daemon_peer_context *)client->context;
        (void)pctx;
        usleep(10000); /* 10ms */
    }

    freerdp_peer_context_free(client);
    freerdp_peer_free(client);
    return TRUE;
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

static bool epoll_add_fd(int epoll_fd, int fd)
{
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = fd,
    };
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port PORT          RDP listen port (default: 3389)\n"
        "  --width WIDTH        Default display width (default: 800)\n"
        "  --height HEIGHT      Default display height (default: 600)\n"
        "  --desktop-cmd CMD    Command for sessions (default: foot)\n"
        "  --cert FILE          TLS certificate file\n"
        "  --key FILE           TLS private key file\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    struct daemon_state state = {
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
        case 'p': state.port = atoi(optarg); break;
        case 'W': state.width = atoi(optarg); break;
        case 'H': state.height = atoi(optarg); break;
        case 'd': state.desktop_cmd = optarg; break;
        case 'c': state.cert_file = optarg; break;
        case 'k': state.key_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    static char auto_cert[] = "/tmp/wlrdp-cert.pem";
    static char auto_key[] = "/tmp/wlrdp-key.pem";
    if (!state.cert_file || !state.key_file) {
        generate_self_signed_cert(auto_cert, auto_key);
        if (!state.cert_file) state.cert_file = auto_cert;
        if (!state.key_file) state.key_file = auto_key;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    session_mgr_init(&state.session_mgr, state.desktop_cmd,
                     state.cert_file, state.key_file,
                     state.width, state.height);

    state.listener = freerdp_listener_new();
    if (!state.listener) {
        WLRDP_LOG_ERROR("freerdp_listener_new failed");
        return 1;
    }

    state.listener->info = &state;
    state.listener->PeerAccepted = on_peer_accepted;

    if (!state.listener->Open(state.listener, "0.0.0.0", state.port)) {
        WLRDP_LOG_ERROR("failed to listen on port %d", state.port);
        freerdp_listener_free(state.listener);
        return 1;
    }

    WLRDP_LOG_INFO("wlrdp-daemon listening on port %d", state.port);

    state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (state.epoll_fd < 0) {
        WLRDP_LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        goto cleanup;
    }

    HANDLE events[32];
    DWORD count = state.listener->GetEventHandles(state.listener, events, 32);
    for (DWORD i = 0; i < count; i++) {
        int fd = GetEventFileDescriptor(events[i]);
        if (fd >= 0) {
            epoll_add_fd(state.epoll_fd, fd);
        }
    }

    while (g_running) {
        if (g_child_exited) {
            session_mgr_reap(&state.session_mgr);
            g_child_exited = 0;
        }

        struct epoll_event ep_events[16];
        int nfds = epoll_wait(state.epoll_fd, ep_events, 16, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            WLRDP_LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (!state.listener->CheckFileDescriptor(state.listener)) {
                WLRDP_LOG_ERROR("listener check failed");
                g_running = 0;
            }
        }
    }

cleanup:
    WLRDP_LOG_INFO("wlrdp-daemon shutting down");
    session_mgr_destroy(&state.session_mgr);
    if (state.listener) {
        state.listener->Close(state.listener);
        freerdp_listener_free(state.listener);
    }
    if (state.epoll_fd >= 0) {
        close(state.epoll_fd);
    }
    return 0;
}
