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
    pam-devel \
    cage \
    xorg-x11-server-Xwayland \
    pixman-devel \
    wlr-randr \
    foot \
    xkeyboard-config \
    git \
    gdb \
    ffmpeg-free-devel \
    libva-devel \
    mesa-va-drivers \
    && dnf clean all

# Create test users for multi-session testing
RUN useradd -m developer && \
    echo "developer:developer" | chpasswd && \
    useradd -m testuser && \
    echo "testuser:testuser" | chpasswd && \
    mkdir -p /run/user/1000 /run/user/1001 && \
    chown developer:developer /run/user/1000 && \
    chown testuser:testuser /run/user/1001

USER developer
ENV XDG_RUNTIME_DIR=/run/user/1000
WORKDIR /workspace
