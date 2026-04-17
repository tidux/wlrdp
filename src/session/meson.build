session_sources = files(
    'main.c',
    'compositor.c',
    'capture.c',
    'input.c',
    'encoder.c',
    'rdp_peer.c',
)

session_deps = [
    freerdp_dep,
    freerdp_server_dep,
    winpr_dep,
    wayland_client_dep,
    xkbcommon_dep,
    protocols_dep,
    common_dep,
]

if have_h264
    session_deps += [libavcodec_dep, libavutil_dep, libswscale_dep]
endif

executable('wlrdp-session',
    session_sources,
    dependencies: session_deps,
    install: true,
)
