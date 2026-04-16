project('wlrdp', 'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

freerdp_dep = dependency('freerdp3', version: '>= 3.0')
freerdp_server_dep = dependency('freerdp-server3', version: '>= 3.0')
winpr_dep = dependency('winpr3', version: '>= 3.0')
wayland_client_dep = dependency('wayland-client')
wayland_scanner_dep = dependency('wayland-scanner', native: true)
xkbcommon_dep = dependency('xkbcommon')

wayland_scanner = find_program(
    wayland_scanner_dep.get_variable('wayland_scanner'),
)

subdir('src/protocols')
subdir('src/common')
subdir('src/session')
