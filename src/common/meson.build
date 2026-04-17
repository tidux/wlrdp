common_sources = files('ipc.c')

common_lib = static_library('wlrdp-common',
    common_sources,
    include_directories: ['.'],
)

common_inc = include_directories('.')
common_dep = declare_dependency(
    link_with: common_lib,
    include_directories: common_inc,
)
