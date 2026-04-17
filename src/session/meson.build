session_sources = files(
    'main.c',
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
    'rdp_peer.c',
)

executable('wlrdp-session',
    session_sources,
    dependencies: [
        freerdp_dep,
        freerdp_server_dep,
        winpr_dep,
        wayland_client_dep,
        xkbcommon_dep,
        protocols_dep,
        common_dep,
    ],
    install: true,
)
