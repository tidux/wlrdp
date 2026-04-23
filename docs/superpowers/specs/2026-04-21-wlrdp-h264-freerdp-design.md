# Design Plan: wlrdp H.264 Refactor to FreeRDP Built-in Codec

## Context
wlrdp currently implements H.264/AVC encoding in `src/session/encoder.c:144` using direct calls to FFmpeg's `libavcodec` (AVCodecContext, avcodec_send_frame, libx264, nvenc, vaapi) and FreeRDP primitives for AVC444 YUV splitting (`RGBToAVC444YUV*`). This leads to maintenance burden, profile/SPS compatibility issues (e.g. Mac clients), and duplication of logic already handled in FreeRDP's codec layer (`FreeRDP/libfreerdp/codec/h264.c`, `video.c`).

The refactor uses FreeRDP's built-in H.264 functions from `/usr/include/freerdp/codec/h264.h` (`H264_CONTEXT`, `h264_context_new`, `h264_compress`, `h264_context_set_option`, `h264_get_yuv_buffer`) and `progressive.h` to reduce wlrdp's hand-encoding and eliminate most direct `libx264` calls. Hardware acceleration defaults to enabled (via FreeRDP backend selection); opt-out supported. Progressive codec added for complete RDPGFX support.

This aligns with existing GFX pipeline in `src/session/rdp_peer.c:40` (caps negotiation, `RdpgfxServerContext`, `SurfaceCommand` for AVC420/444/444v2).

## Objectives
- Default to hardware H.264 (VAAPI, NVENC, or FreeRDP's preferred HW path) when available.
- Allow deliberate opt-out via build flag/config (software-only fallback).
- Client codec negotiation remains runtime (GFX caps).
- Add Progressive codec support (`PROGRESSIVE_CONTEXT`, `progressive_compress`) as required for full rdpgfx (bitmap progressive encoding in SurfaceCommands).
- Minimize code in `encoder.c`; delegate encoding, profile handling, keyframe/SPS to FreeRDP.
- Preserve AVC444 dual-stream logic using existing primitives.
- Update docs, meson options, CLAUDE.md.

## Proposed Architecture
### Encoder Refactor (`src/session/encoder.h:7`, `encoder.c:577`)
- Extend `enum wlrdp_encoder_mode` with `WLRDP_ENCODER_H264_FREERDP` (default for H.264 requests) and `WLRDP_ENCODER_PROGRESSIVE`.
- Replace `struct h264_ctx` (FFmpeg pointers) with:
  ```c
  H264_CONTEXT *h264_ctx[2];      /* main + aux for AVC444 */
  PROGRESSIVE_CONTEXT *prog_ctx;
  ```
- `encoder_init()`:
  - Read meson option `prefer_hardware_h264` (default: 'auto') and config flag.
  - `H264_CONTEXT *ctx = h264_context_new(TRUE);` (encoder mode).
  - `h264_context_set_option(ctx, H264_CONTEXT_OPTION_HW_ACCEL, prefer_hw);`
  - Set `H264_CONTEXT_OPTION_BITRATE`, `FRAMERATE`, `QP`, `USAGETYPE` (CQP or VBR; mirror `FreeRDP/libfreerdp/codec/video.c:213`).
  - For Progressive: `progressive_context_new(FALSE)` + params for layers/quality.
  - AVC444 retains YUV primitive split + two H264 contexts.
  - Fallback chain: HW H.264 → SW H.264 → Progressive → NSC → RAW.
- `encoder_encode()`: Prepare YUV via primitives or `h264_get_yuv_buffer`, call `h264_compress(ctx, yuv, &encoded, &size)` or `progressive_compress`. Set `is_keyframe`, `out_buf`/`aux_buf`.
- `encoder_destroy()`: `h264_context_free`, `progressive_context_free`.
- `encoder_request_keyframe()`: Use `h264_context_reset` or force IDR flag.

Remove most `#ifdef WLRDP_HAVE_H264` FFmpeg includes (`avcodec.h`, `swscale.h`); retain only if needed elsewhere. Update `struct wlrdp_encoder` to hold FreeRDP contexts.

### RDPGFX Integration (`src/session/rdp_peer.c:32`)
- `gfx_caps_advertise()`: Advertise Progressive alongside AVC variants based on encoder mode/config.
- Update `send_gfx_avc*()` dispatch to new encoder modes; support `RDPGFX_CODECID_PROGRESSIVE`.
- `SurfaceCommand` handling extended for progressive PDUs (refer to FreeRDP rdpgfx headers).
- Maintain `gfx_ready`, surface ID, frame ID logic.

### Build & Config
- `meson_options.txt`: Add `option('prefer_hardware_h264', type: 'combo', choices: ['auto', 'enabled', 'disabled'], value: 'auto')`.
- `meson.build`: Wire to `WLRDP_PREFER_HW_H264`.
- `config/wlrdp.conf.example`: Add `[encoder] hardware_h264 = true`.
- Parser in session/ or common/ reads it.
- Update `CLAUDE.md` build commands and test note (`xfreerdp /gfx:AVC420` + progressive variants).
- Conditional compile for progressive/h264 remains but now pulls from system FreeRDP.

### Progressive Codec Details
- Necessary for rdpgfx completion (some clients use it for efficient progressive bitmap updates in GFX surface commands instead of/full with AVC).
- New mode initializes `PROGRESSIVE_CONTEXT`, compresses frames in layers (quality levels), integrates into GFX `SurfaceCommand` with appropriate codec ID.
- Fallback/ negotiation ensures compatibility.

## Data Flow (Updated)
```
capture.c (BGRX from wlr-screencopy)
  ↓ (aligned copy for primitives on AVC444)
primitives (RGBToAVC444YUV* or direct YUV)
  ↓
encoder.c: H264_CONTEXT.compress() or progressive_compress()
  ↓ (NALs or progressive data)
rdp_peer.c: GFX SurfaceCommand (AVC420/444/Progressive)
  ↓ FreeRDP rdpgfx_server_context
→ Client (negotiated codec)
```

## Error Handling & Edge Cases
- Codec init failure: log, fallback, prefer software if HW opted out.
- HW not available: FreeRDP auto-fallbacks; wlrdp detects via `h264_context_get_option`.
- Keyframes: Force on startup (first 5 frames) via context reset.
- Dimensions: Even sizes for YUV420 (already handled).
- ARM/NEON alignment: Retain existing aligned_buf fixes.
- No breaking changes to RAW/NSC paths.

## Tradeoffs & Rationale
- **Pros**: Leverages FreeRDP's tested backends (HW accel, profile fixes for mstsc/VideoToolbox), reduces ~300 lines of hand-rolled FFmpeg/libx264 code, easier maintenance, consistent with shadow/rdpgfx.
- **Cons**: Slight API shift (YUV buffer management via FreeRDP helpers); may need tuning for latency (set low QP, no B-frames).
- **Why this over keeping FFmpeg?** Query requires reducing direct libx264/hand-encoding; FreeRDP abstracts the encoders (including x264 backend if needed) while adding Progressive for rdpgfx.
- YAGNI: No new deps; no full shadow encoder integration.

## Verification Before Completion
- Rebuild (`meson setup --wipe build && meson compile -C build -Dprefer-hardware-h264=enabled`).
- Test: xfreerdp with `/gfx:AVC420`, `/gfx:AVC444`, progressive flags; verify no black screens, hardware used (`WLRDP_LOG_INFO`).
- Opt-out test: disabled flag falls to software/progressive/NSC.
- LSP/typecheck clean (fix includes).
- Update existing specs/plans if impacted; no unrelated changes.

Spec self-reviewed: no TODOs, consistent with current `rdp_peer.c:867` GFX flow, scoped to encoder + peer, explicit fallbacks.

**Next**: Invoke writing-plans skill for detailed implementation checklist (models, routes, tests, docs updates).
