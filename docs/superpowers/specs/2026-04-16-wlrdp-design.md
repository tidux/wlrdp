# wlrdp Design Specification

## Context

Linux lacks a mature, compositor-agnostic RDP terminal server for Wayland. Existing solutions (gnome-remote-desktop, KRdp) are tied to specific desktop environments. xrdp serves X11 well but has no native Wayland support. wlrdp fills this gap: a standalone RDP terminal server that works with any Wayland compositor via nesting.

## Overview

wlrdp is an RDP-based remote desktop terminal server for Wayland Linux systems. It supports multiple simultaneous user sessions with persistence, hardware-accelerated rendering, H.264 encoding, and audio/video redirection.

**Language:** C  
**Build system:** Meson  
**License:** MIT  

## Process Model

Three-tier architecture:

```
                    ┌─────────────────┐
                    │  wlrdp-daemon   │  (root, single instance)
                    │  TCP :3389      │
                    │  TLS + PAM auth │
                    └───────┬─────────┘
                            │ Unix socket (SCM_RIGHTS)
              ┌─────────────┼─────────────┐
              v             v             v
      ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
      │wlrdp-session │ │wlrdp-session │ │wlrdp-session │
      │  (user A)    │ │  (user B)    │ │  (user C)    │
      └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
             v                v                v
      ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
      │    cage      │ │    cage      │ │    cage      │
      │ (compositor) │ │ (compositor) │ │ (compositor) │
      └──────────────┘ └──────────────┘ └──────────────┘
```

### wlrdp-daemon
- Runs as root (or CAP_NET_BIND_SERVICE)
- Creates `freerdp_listener`, accepts incoming RDP connections
- Performs TLS handshake (mandatory, self-signed cert by default)
- Authenticates users via PAM
- Maintains session registry: username -> session-worker PID + IPC socket
- Forks new session-workers or routes reconnections to existing ones
- Never touches frame data (pure control plane)

### wlrdp-session
- One per user session, runs as the target user (privilege drop via setuid/setgid)
- Launches cage as a child process
- Connects to cage as a Wayland client for screen capture and input injection
- Encodes frames and sends via FreeRDP peer interface
- Survives client disconnect; cage and desktop continue running
- Accepts new client connections via IPC fd passing from daemon

### cage
- Launched with `WLR_BACKENDS=headless` per session
- Hosts user's desktop environment or application
- Exposes wlr-screencopy-v1, wlr-virtual-pointer-v1, virtual-keyboard-v1 protocols
- GPU rendering (OpenGL/Vulkan) works via Mesa even in headless mode

## IPC Protocol

Daemon and session-worker communicate via a Unix domain socket pair created at fork time.

Message format:
```
[4 bytes: msg_type][4 bytes: payload_len][payload][optional ancillary fd]
```

Message types:
| Type | Direction | Description |
|------|-----------|-------------|
| `WLRDP_MSG_NEW_CLIENT` | daemon->session | Carries peer socket fd via SCM_RIGHTS |
| `WLRDP_MSG_DISCONNECT` | daemon->session | Client disconnected cleanly |
| `WLRDP_MSG_RESIZE` | daemon->session | Client requested resolution change |
| `WLRDP_MSG_STATUS` | session->daemon | Reports session state |
| `WLRDP_MSG_SHUTDOWN` | daemon->session | Terminate session |

## Data Flow

### Frame Pipeline

```
cage (wlroots headless)
  │ renders to GPU/software framebuffer
  v
wlr-screencopy-v1 protocol
  │ compositor copies output to wl_shm buffer (Phase 1)
  │ or DMA-BUF (Phase 3)
  v
capture.c: on_frame_ready()
  │ pixel data in mmap'd buffer
  v
encoder.c: encoder_encode()
  │ Phase 1: NSCodec via freerdp
  │ Phase 3: H.264 via FFmpeg (VA-API/NVENC/software)
  v
rdp_peer.c: send_frame()
  │ Phase 1: SurfaceBits command
  │ Phase 3: RDPGFX SurfaceFrameCommand (AVC420)
  v
FreeRDP -> TLS -> TCP -> RDP client
```

### Input Pipeline

```
RDP client -> FreeRDP input callbacks
  │ KeyboardEvent(flags, scancode)
  │ MouseEvent(flags, x, y)
  v
rdp_peer.c: translate RDP events
  │ evdev_key = scancode + 8
  │ RDP mouse flags -> button/motion
  v
input.c: inject via Wayland protocols
  │ zwlr_virtual_pointer_v1 (motion, button, axis)
  │ zwp_virtual_keyboard_v1 (key)
  v
cage -> delivers to focused application
```

## Session Lifecycle

### New Session
1. Client connects to daemon on TCP port
2. TLS handshake (mandatory)
3. RDP capability negotiation
4. `peer->Logon` callback: extract username + password
5. PAM: `pam_authenticate` + `pam_acct_mgmt`
6. Session manager checks registry: no existing session
7. Fork wlrdp-session:
   - Drop privileges (setgid, initgroups, setuid)
   - Call `pam_open_session`
   - Launch cage with `WLR_BACKENDS=headless`
   - Wait for Wayland display socket
   - Connect as Wayland client, bind protocols
   - Enter event loop
8. Daemon sends peer socket fd via IPC
9. Session-worker initializes RDP peer, starts frame capture
10. Session is live

### Disconnect
1. TCP connection drops or clean disconnect PDU
2. Session-worker destroys RDP peer context, stops capture
3. Cage and desktop continue running
4. Session-worker notifies daemon: status DISCONNECTED
5. Session-worker enters idle wait on IPC fd

### Reconnect
1. Client connects and authenticates
2. Session manager finds existing session for username
3. Daemon sends new peer fd to existing session-worker
4. Session-worker initializes new peer, optionally resizes output
5. Resumes frame capture and input injection
6. User sees desktop exactly as left

### Termination
1. User logs out (cage child exits)
2. Session-worker detects cage exit (SIGCHLD/waitpid)
3. `pam_close_session`, `pam_end`
4. Notifies daemon: status TERMINATED
5. Session-worker exits
6. Daemon reaps child, removes from registry

## Components

### RDP Listener (`src/daemon/listener.c`)
- FreeRDP: `freerdp_listener_new`, `listener->Open`, `PeerAccepted` callback
- Configures TLS certificates on each peer

### Authenticator (`src/daemon/auth.c`)
- PAM: `pam_start("wlrdp", ...)`, `pam_authenticate`, `pam_acct_mgmt`, `pam_open_session`
- Returns uid/gid for privilege drop

### Session Manager (`src/daemon/session_mgr.c`)
- Session registry (hash table: username -> PID + IPC fd)
- Fork logic with privilege drop
- Reconnect routing

### Session Worker (`src/session/session.c`)
- Main event loop (epoll): Wayland display fd, peer fds, IPC fd
- Orchestrates compositor, capture, encoder, peer

### Compositor Launcher (`src/session/compositor.c`)
- Fork+exec cage with environment:
  - `WLR_BACKENDS=headless`
  - `WLR_HEADLESS_OUTPUTS=1`
  - `WLR_HEADLESS_OUTPUT_MODE={w}x{h}`
- Polls for Wayland display socket readiness

### Screen Capture (`src/session/capture.c`)
- Binds `zwlr_screencopy_manager_v1` from cage (wlr-screencopy-unstable-v1)
- Note: wlr-screencopy is deprecated upstream in favor of ext-image-capture-source-v1, but cage still supports it and it's simpler for Phase 1. Migration to ext-image-capture-source can happen later.
- Allocates `wl_shm` buffer matching output resolution (XRGB8888)
- Continuous capture loop: request frame -> on_ready -> encode -> send -> request next
- Phase 3: DMA-BUF path via `linux_dmabuf`

### Input Injector (`src/session/input.c`)
- Binds `zwlr_virtual_pointer_manager_v1` and `zwp_virtual_keyboard_manager_v1`
- Creates virtual pointer and keyboard on cage's seat
- Loads XKB keymap via xkbcommon
- Translates RDP scancodes to evdev keycodes (scancode + 8 for standard keys; extended keys via FreeRDP's `GetKeycodeFromVirtualKeyCode`)

### Encoder (`src/session/encoder.c`)
- Phase 1: FreeRDP NSCodec (`nsc_context_new`, `nsc_compose_message`)
- Phase 3: FFmpeg H.264 (`avcodec_find_encoder_by_name("h264_vaapi")`, fallback to `libx264`)
- Phase 3: RemoteFX fallback for clients without H.264

### RDP Peer Handler (`src/session/rdp_peer.c`)
- Manages `freerdp_peer` lifecycle
- Registers callbacks: PostConnect, Activate, KeyboardEvent, MouseEvent
- Phase 1: `update->SurfaceBits` for frame sending
- Phase 3: `RdpgfxServerContext` for RDPGFX pipeline

## Source Tree

```
wlrdp/
├── meson.build
├── meson_options.txt
├── LICENSE
├── README.md
├── src/
│   ├── common/
│   │   ├── meson.build
│   │   ├── common.h           # Shared types, log macros
│   │   ├── ipc.h              # IPC message definitions
│   │   ├── ipc.c              # sendmsg/recvmsg with SCM_RIGHTS
│   │   ├── config.h           # Configuration structure
│   │   └── config.c           # Config file parser
│   ├── daemon/
│   │   ├── meson.build
│   │   ├── main.c             # Entry point, signal handling, event loop
│   │   ├── listener.c/.h      # freerdp_listener, TLS, peer accept
│   │   ├── auth.c/.h          # PAM authentication
│   │   └── session_mgr.c/.h   # Session table, fork, reconnect
│   ├── session/
│   │   ├── meson.build
│   │   ├── main.c             # Entry point, privilege drop, event loop
│   │   ├── session.c/.h       # Session lifecycle orchestration
│   │   ├── compositor.c/.h    # cage fork+exec
│   │   ├── capture.c/.h       # wlr-screencopy client
│   │   ├── input.c/.h         # Virtual pointer + keyboard
│   │   ├── encoder.c/.h       # Frame encoding
│   │   └── rdp_peer.c/.h      # freerdp_peer management
│   └── protocols/
│       ├── meson.build         # wayland-scanner rules
│       ├── wlr-screencopy-unstable-v1.xml
│       ├── wlr-virtual-pointer-unstable-v1.xml
│       └── virtual-keyboard-unstable-v1.xml
├── config/
│   ├── wlrdp.conf.example
│   └── wlrdp.pam
├── systemd/
│   ├── wlrdp.service
│   └── wlrdp.sysusers
└── docs/
```

## Build System

Meson with these dependencies:

**Required (all phases):**
- `freerdp3`, `freerdp-server3`, `winpr3` (FreeRDP 3.x)
- `wayland-client`, `wayland-scanner`
- `xkbcommon`
- OpenSSL (for TLS certificates)

**Phase 2+:**
- `pam`

**Phase 3+:**
- `libavcodec`, `libavutil` (FFmpeg)
- `libpipewire-0.3`

**Runtime:**
- `cage` (installed on system)

## TLS Configuration

- Mandatory TLS on all connections
- Default: auto-generated self-signed RSA 2048-bit certificate
- Stored at `/etc/wlrdp/tls/cert.pem` and `/etc/wlrdp/tls/key.pem`
- Config option to specify custom certificate paths
- NLA (Network Level Authentication) disabled in Phase 1, enabled in Phase 2 with PAM

## Configuration

`/etc/wlrdp/wlrdp.conf`:
```ini
[server]
port = 3389
cert_file = /etc/wlrdp/tls/cert.pem
key_file = /etc/wlrdp/tls/key.pem

[session]
desktop_cmd = labwc
default_width = 1920
default_height = 1080

[security]
max_sessions = 50
session_timeout = 3600  # idle timeout in seconds
```

## Phased Implementation

### Phase 1: Minimal Working Prototype
**Goal:** Single-process, single-session RDP with display + input.

**Scope:**
- Single binary `wlrdp-session` (no daemon split)
- No authentication (accept all connections)
- No session persistence (disconnect = session dies)
- NSCodec encoding via FreeRDP (no H.264)
- wlr-screencopy with wl_shm buffers (no DMA-BUF)
- Mandatory TLS with auto-generated self-signed cert
- Keyboard + mouse input via virtual pointer/keyboard protocols

**Success criteria:** Run `wlrdp-session --port 3389 --desktop-cmd foot`, connect with `xfreerdp /v:host:3389 /cert:ignore`, see a foot terminal, type and click.

**Files:** `src/session/main.c`, `compositor.c`, `capture.c`, `input.c`, `encoder.c`, `rdp_peer.c`, `src/common/common.h`, `src/protocols/*.xml`, `meson.build`

### Phase 2: Multi-Session + Auth + Persistence
**Goal:** Production session management.

**Scope:**
- Daemon/session process split
- PAM authentication via `peer->Logon` callback
- systemd-logind session tracking
- Session registry with reconnect support
- IPC with SCM_RIGHTS fd passing
- systemd service file
- Config file parser

### Phase 3: H.264 + GPU Encoding
**Goal:** High-quality, low-bandwidth video.

**Scope:**
- RDPGFX pipeline replacing SurfaceBits
- H.264 encoding via FFmpeg (VA-API, NVENC, software fallback)
- RemoteFX fallback for clients without H.264
- DMA-BUF capture path for zero-copy GPU frames
- Adaptive framerate

### Phase 4: Audio + Peripherals
**Goal:** Full multimedia support.

**Scope:**
- PulseAudio output via RDPSND channel (PipeWire backend)
- Microphone input via AUDIN channel
- Webcam via RDPECAM channel
- Clipboard via CLIPRDR channel

## Verification

### Phase 1 Testing
1. Build: `meson setup build && ninja -C build`
2. Generate cert: `openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj '/CN=wlrdp'`
3. Start: `./build/src/session/wlrdp-session --port 3389 --cert cert.pem --key key.pem --desktop-cmd foot`
4. Connect: `xfreerdp /v:localhost:3389 /cert:ignore`
5. Verify: terminal visible, keyboard input works, mouse movement and clicks work
6. Disconnect and verify server process exits cleanly

### Integration Testing (Phase 2+)
1. Start daemon: `systemctl start wlrdp`
2. Connect as user A from client 1
3. Connect as user B from client 2 simultaneously
4. Disconnect user A, reconnect, verify session persisted
5. Verify sessions are isolated (different UIDs, separate cage instances)
