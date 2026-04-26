# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is wlrdp

wlrdp is a Linux RDP server that runs Wayland desktops over RDP. It launches a headless Cage compositor per session, captures frames via wlr-screencopy, and streams them to RDP clients using FreeRDP 3.x server APIs. Input is injected via wlr-virtual-pointer and virtual-keyboard Wayland protocols. Authentication uses PAM.

## Build Commands

```bash
# Configure (first time)
meson setup build

# Build
meson compile -C build

# Reconfigure with options
meson configure build -Denable-h264=enabled -Dprefer_hardware_h264=enabled

# Clean rebuild
meson setup --wipe build
```

The project produces two binaries: `wlrdp-daemon` and `wlrdp-session`. There are no tests yet.

## Architecture

Two-process model with Unix domain socket IPC:

- **wlrdp-daemon** (`src/daemon/`) — Listens for RDP connections, authenticates users via PAM (`auth.c`), manages per-user sessions (`session_mgr.c`). On successful login, passes the peer socket fd to the appropriate session worker via SCM_RIGHTS.

- **wlrdp-session** (`src/session/`) — Runs one Wayland desktop per user. Launches Cage compositor (`compositor.c`), captures frames via wlr-screencopy (`capture.c`), handles keyboard/mouse input (`input.c`), encodes frames (`encoder.c`), and manages the RDP peer connection (`rdp_peer.c`). Can run standalone (own listener) or in IPC mode (receives fds from daemon).

- **src/common/** — Shared IPC wire protocol (`ipc.c/h`) and common definitions (`common.h`). IPC messages use a type+payload format with optional fd passing.

- **src/protocols/** — Wayland protocol XML files, compiled by wayland-scanner at build time.

The event loop in both binaries uses Linux `epoll`. Frame capture runs at ~30fps max.

## Key Dependencies

- FreeRDP 3.x (`freerdp3`, `freerdp-server3`, `winpr3`)
- Wayland client libraries
- xkbcommon (keyboard handling)
- PAM (authentication, daemon only)
- Cage (headless Wayland compositor, runtime dependency)

## Build System

Meson with C11. Wayland protocol C bindings are generated at build time via `wayland-scanner`. The `src/protocols/meson.build` drives this generation.

## Running

`wlrdp-session --port 3389` for standalone mode (no auth, single session). The daemon mode (`wlrdp-daemon`) is for multi-user deployments with PAM auth. Config example at `config/wlrdp.conf.example`, systemd unit at `systemd/wlrdp.service`, PAM config at `config/wlrdp.pam`.

## Development Notes

- This is a Linux-only project (epoll, SCM_RIGHTS, PAM). Development requires a Linux environment or the provided devcontainer.  If the host system is MacOS and Claude is not running in VSCode, use MCP tools to start, rebuild, restart, or run commands inside the devcontainer.
- If xfreerdp is available on the host system, test the server with the following command: `xfreerdp /u:developer /p:developer /cert:ignore /v:localhost /gfx:AVC420`
- H.264 encoding uses FreeRDP's codec APIs. Use `-Dprefer_hardware_h264=disabled`
  to request software-only H.264.
- Frame data uses raw BGRX pixels (no compression yet); the vertical flip in `session/main.c:on_frame_ready` is needed because screencopy gives top-down but SurfaceBits expects bottom-up.

## H.264 Options

Prefer hardware H.264 encoding:

```bash
meson setup --wipe build -Denable-h264=enabled -Dprefer_hardware_h264=enabled
```

Request software H.264 encoding:

```bash
meson setup --wipe build -Denable-h264=enabled -Dprefer_hardware_h264=disabled
```

Manual client checks:

```bash
xfreerdp /u:developer /p:developer /cert:ignore /v:localhost /gfx:AVC420
xfreerdp /u:developer /p:developer /cert:ignore /v:localhost /gfx:AVC444
```
