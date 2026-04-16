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
    foot \
    xkeyboard-config \
    git \
    gdb \
    && dnf clean all

RUN useradd -m developer
USER developer
WORKDIR /workspace
