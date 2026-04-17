FROM registry.fedoraproject.org/fedora:43

RUN dnf install -y \
    gcc \
    meson \
    ninja-build \
    pkgconf-pkg-config \
    freerdp-devel \
    freerdp-server \
    libwinpr-devel \
    wayland-devel \
    wayland-protocols-devel \
    libxkbcommon-devel \
    openssl-devel \
    openssl \
    cage \
    xorg-x11-server-Xwayland \
    pixman-devel \
    wlr-randr \
    foot \
    xkeyboard-config \
    git \
    gdb \
    && dnf clean all

RUN useradd -m developer && \
    mkdir -p /run/user/1000 && \
    chown developer:developer /run/user/1000

USER developer
ENV XDG_RUNTIME_DIR=/run/user/1000
WORKDIR /workspace
