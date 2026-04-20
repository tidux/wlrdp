FROM registry.fedoraproject.org/fedora:43

# Enable RPMFusion for full FFmpeg with patented codecs (H.264/x264)
RUN dnf install -y \
    https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-43.noarch.rpm \
    https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-43.noarch.rpm \
    && dnf clean all

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
    ffmpeg-devel \
    libva-devel \
    mesa-va-drivers \
    labwc \
    pipewire-devel \
    wireplumber \
    wl-clipboard \
    mesa-libEGL \
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
