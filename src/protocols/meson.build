protocols = [
    'wlr-screencopy-unstable-v1',
    'wlr-virtual-pointer-unstable-v1',
    'virtual-keyboard-unstable-v1',
]

protocol_sources = []
protocol_headers = []

foreach p : protocols
    xml = files(p + '.xml')
    protocol_sources += custom_target(
        p + '-protocol-code',
        input: xml,
        output: p + '-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
    protocol_headers += custom_target(
        p + '-client-header',
        input: xml,
        output: p + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
endforeach

protocols_lib = static_library('wlrdp-protocols',
    protocol_sources + protocol_headers,
    dependencies: [wayland_client_dep],
)

protocols_dep = declare_dependency(
    link_with: protocols_lib,
    sources: protocol_headers,
)
