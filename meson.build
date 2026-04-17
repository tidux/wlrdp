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
libavcodec_dep = dependency('libavcodec', required: h264_opt)
libavutil_dep = dependency('libavutil', required: h264_opt)
libswscale_dep = dependency('libswscale', required: h264_opt)
have_h264 = libavcodec_dep.found() and libavutil_dep.found() and libswscale_dep.found()
if have_h264
    add_project_arguments('-DWLRDP_HAVE_H264=1', language: 'c')
endif

wayland_scanner = find_program(
    wayland_scanner_dep.get_variable('wayland_scanner'),
)

subdir('src/protocols')
subdir('src/common')
subdir('src/session')
subdir('src/daemon')
