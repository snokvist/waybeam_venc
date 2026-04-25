# PiP Backend — Pause Checkpoint

Status as of branch tip `8622c26` on `claude/zoomed-pip-vpe-r16PG`.
Captures everything validated, the current bug, and the recommended
next move so the work can be resumed cleanly.

## What's validated (probes 1–4)

| Layer | Confirmed |
|---|---|
| `MI_DIVP_StretchBuf` between MMA buffers | ✓ HW path, ~1.4 ms standalone, ~0.4 ms in steady-state |
| YUV420SP → ARGB8888 hardware colour conversion | ✓ same cost as YUV→YUV |
| `MI_SYS_ChnOutputPortGetBuf` on VPE port-1 (non-bound) | ✓ ~67 µs once `userFrameDepth ≥ 1` |
| DIVP src = live VPE port-1 buf phys addr | ✓ |
| `MI_SYS_ChnOutputPortGetBuf` on VPE port-0 (REALTIME-bound) | **✗** rejected with `0xa009200d` — true in-place compositing impossible |
| ARGB888 RGN region creation | **✗** kernel rejects — `Check osd attr error` |
| ARGB4444 RGN region creation | ✓ |
| **I8 RGN region + DIVP YUV→Y direct write to canvas phys (one-shot)** | ✓ probe 4 — `divp_ret=0`, `update_ret=0`, palette-mismatched garble visible on stream |
| Per-frame steady-state DIVP cost | ~0.4 ms HW + ~7 % CPU at 90 fps |

See `memory/star6e_pip_apis.md` (orchestrator memory) for the full
inventory and probe JSON.

## Backend code shape (committed)

- `include/pip_compositor.h` — public API (create/destroy, start/stop,
  apply_zoom, apply_position, apply_enabled).
- `src/pip_compositor.c` — compositor with its own dlopen of
  libmi_sys/libmi_divp/libmi_rgn.  Sets up VPE port-1 to output the
  full encode frame (1920×1080), creates an I8 RGN region at
  `pip.position.{x,y,w,h}`, runs a thread that drains port-1, calls
  `MI_DIVP_StretchBuf` with `src_crop = pip.zoom rect`, target =
  RGN canvas phys (Y plane direct write).
- `src/star6e_runtime.c` — start at `apply_startup_controls` when
  `pip.enabled`, destroy before `pipeline_stop`.
- `src/debug_osd.c` — `palette_init` now installs a 256-entry
  grayscale ramp (overrides debug_osd's named colours; outline rects
  render as gray, acceptable for engineering use).
- `Makefile` — `pip_compositor.c` in `STAR6E_ONLY_SRC`.

## The active bug

Direct `DIVP → RGN canvas phys` writes succeed in the **probe**
(one-shot, `update_ret=0`, garbled-but-visible content) but not in
the **steady-state compositor**.  The kernel logs:

```
[MI_RGN_IMPL_UpdateCanvas]      Canvas didn't get.
[mi_rgn_drv_window_blitosd_front_buffer] Front buf state error!!! error state is 0.
```

The "front buffer state 0" suggests the SDK's userspace dirty-page
tracking that normally gates `UpdateCanvas` doesn't see DMA writes
from DIVP as "userspace touched the canvas".  The `debug_osd` module
gets around this implicitly because it writes pixels via
`canvas.virtAddr` (CPU writes through the SDK-provided mmap), which
naturally marks the pages dirty.

What we tried (commit `8622c26`, dead end):

1. `volatile uint8_t *p = ...; p[0] = p[0]` — RMW touch one byte to
   mark the page dirty.
2. `MI_SYS_FlushInvCache` on `canvas.virtAddr` for `stride*height`
   bytes after DIVP.

Result: same kernel errors, all-black/transparent output, CPU spikes
to load avg 16+.

## The recommended next move

Switch to the **DIVP-into-MMA-scratch + CPU memcpy** path:

```
loop:
  GetBuf port-1 → src buf
  GetCanvasInfo → canvas
  DIVP src=port-1, dst.phyAddr[0]=Y_SCRATCH, dst.phyAddr[1]=UV_SCRATCH
  // CPU memcpy keeps SDK happy (writes via canvas.virtAddr trigger
  // the dirty-page tracking that gates UpdateCanvas)
  for (y = 0; y < height; y++)
    memcpy(canvas.virtAddr + y*canvas.stride, Y_SCRATCH_VA + y*scratch.stride, width)
  UpdateCanvas
  PutBuf
```

Costs:
- DIVP: same ~0.4 ms HW (unchanged)
- Memcpy: 480×272 = 130 K bytes × 90 fps = 11.7 MB/s.  Memory-bandwidth
  bound on Cortex-A7; <2 % CPU with NEON-accelerated memcpy.
- Total: ~few % CPU plus baseline 22 % = ~25 % at 90 fps.  Well below
  the broken 30+ % from the failed direct path.

Implementation skeleton is partly sketched in commit `8622c26` notes —
add `y_scratch_phy/size/va` fields to `PipCompositor`, allocate +
mmap once at create, and rewrite the steady-state loop.  The
already-allocated `uv_scratch` stays the same.

## Acceptance criteria for the next session

- [ ] Clean grayscale PiP renders at `pip.position` showing the
      zoom-rect content (no green/blue garble, no black/transparent).
- [ ] Movement of the camera updates the PiP frame.
- [ ] Aspect ratio matches the zoom rect (no stretching).  If still
      stretched, check whether DIVP crop x-coord interpretation
      differs from u16Width units (chroma sample vs pixel).
- [ ] venc CPU at 90 fps stays under ~30 % (baseline ~22 % +
      compositor overhead).
- [ ] No `Front buf state error` or `Canvas didn't get` in dmesg.
- [ ] `debug.showOsd=true` and `pip.enabled=true` coexist (palette
      ramp already merged via `debug_osd.c` — outlines render as
      gray, that's fine).

## Open questions for later

- **Aspect "stretched"** — user reported even with port-1 = full
  1920×1080.  Likely DIVP src/crop coord interpretation.  Might be
  resolved by the scratch + memcpy path, or might need a separate
  fix (try halving zoom width and observe to characterise).
- **Pre-warm 2 s first-call latency** — currently masked inside the
  thread before the steady-state loop.  Fine.
- **`pip.refreshEvery`** — already wired to skip-frame logic.  Should
  reduce CPU proportionally if user picks 2 or 3.
