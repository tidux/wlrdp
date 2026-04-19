Name:           wlrdp
Version:        0.1.0
Release:        1%{?dist}
Summary:        Multi-user, multi-session Wayland RDP Terminal Server

License:        MIT
URL:            https://github.com/example/wlrdp
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  freerdp-devel
BuildRequires:  freerdp-server
BuildRequires:  libwinpr-devel
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  libxkbcommon-devel
BuildRequires:  openssl-devel
BuildRequires:  pam-devel
BuildRequires:  pixman-devel
BuildRequires:  ffmpeg-devel
BuildRequires:  libva-devel
BuildRequires:  pipewire-devel

Requires:       cage
Requires:       foot
Requires:       labwc
Requires:       wireplumber
Requires:       wl-clipboard
Requires:       xorg-x11-server-Xwayland
Requires:       wlr-randr
Requires:       xkeyboard-config
Requires:       openssl
Requires:       mesa-va-drivers
Requires:       freerdp-server

%description
WLRDP is a multiuser, multisession, compositor-agnostic Wayland RDP Terminal Server.
It uses FreeRDP 3.x server libraries to provide RDP access to headless Wayland
compositors like Cage or labwc.

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    -Denable-h264=enabled \
    -Denable-pipewire=enabled
%meson_build

%install
%meson_install

# Install configuration files
install -d -m 0755 %{buildroot}%{_sysconfdir}/wlrdp
install -m 0644 config/wlrdp.conf.example %{buildroot}%{_sysconfdir}/wlrdp/wlrdp.conf.example

# Install PAM configuration
install -d -m 0755 %{buildroot}%{_sysconfdir}/pam.d
install -m 0644 config/wlrdp.pam %{buildroot}%{_sysconfdir}/pam.d/wlrdp

# Install and update systemd service (fix path from /usr/local)
install -d -m 0755 %{buildroot}%{_unitdir}
install -m 0644 systemd/wlrdp.service %{buildroot}%{_unitdir}/wlrdp.service
sed -i 's|/usr/local/bin/wlrdp-daemon|%{?_bindir}/wlrdp-daemon|g' %{buildroot}%{_unitdir}/wlrdp.service

%post
%systemd_post wlrdp.service

%preun
%systemd_preun wlrdp.service

%postun
%systemd_postun wlrdp.service

%files
%license COPYING
%doc README.md
%{_bindir}/wlrdp-daemon
%{_bindir}/wlrdp-session
%{_unitdir}/wlrdp.service
%config(noreplace) %{_sysconfdir}/pam.d/wlrdp
%dir %{_sysconfdir}/wlrdp
%{_sysconfdir}/wlrdp/wlrdp.conf.example

%changelog
* Fri Apr 18 2025 John Doe <john@example.com> - 0.1.0-1
- Initial RPM packaging based on .devcontainer/Dockerfile dependencies.
