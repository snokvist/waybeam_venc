# VPE port1 luma tap — plan

Status: **Phase 1 (spec) for an experimental branch.** This is the capture-side
foundation for inline QR scanning, and the vehicle for the device experiments
that decide whether the tap is viable at all.

## Why this exists

The QR integration plan in `documentation/QR_INLINE_INTEGRATION_PLAN.md`
captures through the MJPEG snapshot channel. Device measurement retired that
approach on three counts:

1. **The capture is OSD-contaminated by construction.** The MJPEG channel is a
   VPE **port0** 1:N consumer (`src/star6e_pipeline.c:2143-2158`). MI_RGN
   composites per output port, so every port0 consumer sees whatever overlay is
   attached. A snapshot pulled from a bench craft shows the waybeam-hub HUD
   composited dead centre of the frame — exactly where a marker would be placed
   — with `debug.showOsd` **false**, i.e. from the external hub. A QR scanner on
   port0 cannot see a marker the OSD covers.
2. **There is no capture-resolution control.** `snapshot.width`/`height` set the
   VENC channel's *maximum* encodable resolution, not a scaler. Measured: below
   the main stream every frame fails `_MI_VENC_ValidateResolution` and
   `/snapshot.jpg` returns `504` permanently; above it, the output is still the
   main-stream size. VENC has no scaler. A VPE port does.
3. **The JPEG round trip is pure overhead.** ~184 ms per attempt at 1080p to
   encode and re-decode pixels that already exist in DRAM as NV12.

## What this branch adds

A read-only luma tap on VPE port1: its own geometry, NV12, drained
continuously by a dedicated reader thread, with the Y plane copied out only on
request. Plus a debug endpoint that returns that copy as a P5 PGM so the tap can
be inspected from a workstation.

No QR decoding. No scan-window state machine. Those follow once the tap is
proven.

## Design decisions

**Enable once per pipeline run; never cycle per request.** This is the whole
lesson of the `snapshot.pgm` retirement (#205). That code ran
`SetPortMode → EnablePort → SetDepth → GetBuf → PutBuf → SetDepth(0,0) →
DisablePort` on *every HTTP request*, and the `DisablePort` half raced an
in-flight mhal buffer: 2 hard wedges in ~560 stressed captures, kernel-side, not
fixable from userspace. The port here is programmed in `configure_graph` and
released only in `teardown_graph`. A capture request sets a flag; it never
touches port state.

**The reader drains every frame; the consumer never does.** An
enabled-but-undrained port is dangerous on this BSP — the i6e port2 probe
stalled port0 with no consumer attached. The reader thread bounces every frame
(`GetBuf` → `PutBuf`, microseconds) and performs the Y-plane copy only when a
grab is pending. A future QR decode of ~1.5 s therefore never sits between
`GetBuf` and `PutBuf`.

**Teardown order is load-bearing:** park the reader outside the GetBuf/PutBuf
window → join → `SetChnOutputPortDepth(0,0)` → `DisablePort` → release the
arbiter claim. Leaving the depth registered wedges the *next* process with a
fence that never completes. Every path that enabled the port — including
failure unwinds — exits through one teardown function.

**QR is the lowest-priority port1 claimant.** Stab and detect are unchanged.
`star6e_vpe_port1_claim("qr")` runs *after* both have had their turn, and a
refusal is logged and non-fatal. Deliberately not implemented: having QR
displace stab or detect automatically. The stab preset programs and enables its
tap before its (advisory, return-discarded) claim at
`src/star6e_pipeline.c:2117`, so making QR win would mean restructuring two
working, wedge-prone subsystems — unjustified on a branch whose purpose is to
find out whether the tap is safe. Operators turn stab/detect off to free port1.

**Geometry is configurable and independent of port0.** Proven by the existing
taps: detect runs 640×352 while port0 runs up to 2560×1440. `0` inherits the
main stream. The tap is what makes capture resolution a knob at all, and it is
the real memory lever for a future scanner — peak cascade heap scales with W×H,
so tapping at 1280×720 instead of 1920×1080 cuts it by 2.25×.

**Stride comes from the buffer descriptor, never assumed.** NV12 `u32Stride[0]`
with a width fallback. The PGM output is tightly packed and self-describing, so
a consumer reads real dimensions from the header.

**`MI_VPE_SetPortCrop` is never called.** It is sticky on i6e and a leftover
rect poisons a later detect run. The tap uses full-frame scaling only.

## Files

```
include/star6e_luma_tap.h   NEW  start/stop/grab API
src/star6e_luma_tap.c       NEW  port setup, reader thread, Y copy, PGM pack
src/star6e_pipeline.c       wire start after detect, stop in teardown_graph
src/venc_api.c              GET /api/v1/qr/tap.pgm + 3 config fields
src/venc_config.c           defaults, load_qr, to_json, render_qr
include/venc_config.h       VencConfigQr (trailing member — ABI append-only)
config/waybeam.default.json qr section
Makefile                    STAR6E_ONLY_SRC += src/star6e_luma_tap.c
```

## Config

| JSON | API | Type | Default | Mut. |
|---|---|---|---|---|
| `qr.tapEnabled` | `qr.tap_enabled` | bool | `false` | restart |
| `qr.tapWidth` | `qr.tap_width` | uint | `0` (inherit) | restart |
| `qr.tapHeight` | `qr.tap_height` | uint | `0` (inherit) | restart |

All `MUT_RESTART`: the port is programmed at graph configure time, and making
them live would mean reprogramming a live port — the thing this design exists to
avoid. Fields carry `FIELD_UI` descriptors so the WebUI builds controls from
`/api/v1/capabilities` with no `SECTIONS[]` edit and no `make webui` rebuild.

## Endpoint

`GET /api/v1/qr/tap.pgm` → `image/x-portable-graymap`, one P5 frame at tap
geometry. `503 tap_disabled` when the tap is not running (disabled, port1 held
by stab/detect, or pipeline down); `504 tap_timeout` when no frame arrives
within the grab deadline.

Debug-grade and Star6E-only by construction. It is the experiment instrument,
not a product surface.

## Verification

- `make lint`, then `make verify` (both backends; Maruko must still build with
  the tap absent).
- Device, on a bench with `framing=off` and `detect.enabled=false` so port1 is
  free:
  - **OSD-cleanliness** — with the hub OSD compositing on port0, pull
    `/api/v1/qr/tap.pgm` and `/api/v1/snapshot.jpg` back to back. The HUD must
    appear in the JPEG and be absent from the PGM. This is the experiment the
    whole approach rests on.
  - **Geometry** — set `qr.tapWidth`/`Height` below the main stream and confirm
    the PGM header reports the requested size, i.e. the resolution knob the JPEG
    path cannot offer.
  - **Stress** — ~500 grabs against the persistently-enabled tap, checking for
    `EnsureInputPortFifoEmpty` / MMU faults in `dmesg` and confirming the encoder
    never drops frames. Captures, not port cycles: the point is that the port is
    enabled once and the count exercises the *grab* path.
  - **Lifecycle** — pipeline restart and SIGHUP reinit with the tap enabled,
    confirming clean teardown and no stale-fence wedge in the successor.
