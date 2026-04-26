project('wlrdp', 'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ],
)

add_project_arguments('-D_GNU_SOURCE', '-Wno-deprecated-declarations', language: 'c')

freerdp_dep = dependency('freerdp3', version: '>= 3.0')
freerdp_server_dep = dependency('freerdp-server3', version: '>= 3.0')
winpr_dep = dependency('winpr3', version: '>= 3.0')
wayland_client_dep = dependency('wayland-client')
wayland_scanner_dep = dependency('wayland-scanner', native: true)
xkbcommon_dep = dependency('xkbcommon')
pam_dep = dependency('pam')

h264_opt = get_option('enable-h264')
have_h264 = not h264_opt.disabled()
if have_h264
    add_project_arguments('-DWLRDP_HAVE_H264=1', language: 'c')
endif

prefer_hw_h264 = get_option('prefer_hardware_h264')
if prefer_hw_h264 == 'disabled'
    add_project_arguments('-DPREFER_HW_H264=0', language: 'c')
else
    add_project_arguments('-DPREFER_HW_H264=1', language: 'c')
endif

pw_opt = get_option('enable-pipewire')
pipewire_dep = dependency('libpipewire-0.3', required: pw_opt)
have_pipewire = pipewire_dep.found()
if have_pipewire
    add_project_arguments('-DWLRDP_HAVE_PIPEWIRE=1', language: 'c')
endif

wayland_scanner = find_program(
    wayland_scanner_dep.get_variable('wayland_scanner'),
)

subdir('src/protocols')
subdir('src/common')
subdir('src/session')
subdir('src/daemon')
