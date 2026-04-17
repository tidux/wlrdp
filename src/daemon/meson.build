daemon_sources = files(
    'main.c',
    'auth.c',
    'session_mgr.c',
)

executable('wlrdp-daemon',
    daemon_sources,
    dependencies: [
        freerdp_dep,
        freerdp_server_dep,
        winpr_dep,
        pam_dep,
        common_dep,
    ],
    install: true,
)
