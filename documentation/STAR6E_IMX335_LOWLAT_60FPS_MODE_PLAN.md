# Star6E IMX335 low-latency 60 fps sensor mode — plan and direction

Status: **PLAN ONLY — no implementation yet.** Draft PR opened to park the
direction; pick up when scheduled.

## Motivation

Bench measurements on .232 (SSC338Q, IMX335 mode 1 = 2560x1920@60, encode
1280x720@60 H265 CBR, 2026-08-01) put glass-to-encoded latency at ~30 ms:

- Sidecar `frame_ready_us - capture_us` ≈ 30 ms (shown as "encode" in the
  hub Pipeline tab — a label misnomer; it is capture -> encoded-frame-dequeued).
- VENC driver's own pipeline-delay counters
  (`/proc/mi_modules/mi_venc/mi_venc0`, chn0 pass0): frame arrives at VENC
  input at avg **16.7 ms** after its PTS reference; encode itself takes
  **~3.7 ms** (input 16.7 -> dequeue 20.4 ms avg).
- Everything downstream of encode (packetise + hand-off to waybeam-link's
  frame-SHM ring) is sub-ms.

So the latency budget is entirely sensor/ISP-side: exposure + readout +
frame-boundary hand-offs dominate; encode is a small tail. The encoder path
cannot be tightened further — slice/ring low-delay encode is fused off in
the i6e MHE core (`SupportRing=0`, root-caused in
`REALTIME_PIPELINE_INVESTIGATION.md`; every VPE->VENC leg is FRAMEBASE and
must wait for a complete frame).

The remaining lever is **sensor scan timing**: readout time is set by the
mode's line clock (HMAX) and active row count, not by the encode fps. A mode
that scans fast and idles in vertical blanking delivers the finished frame
to the ISP earlier within each 16.7 ms period.

## Direction

Add a driver mode (or modes) to `drivers/sensor_imx335_star6e.c` that runs
the sensor at the **fastest line rate its geometry/MIPI budget allows** and
stretches **VMAX** so the frame rate lands at 60 fps:

```
frame period (16.7 ms) = VMAX x line_time        (fixed, 60 fps)
readout time           = active_rows x line_time (what we minimise)
```

The existing fast modes already prove the scan rates: mode 3 (2176x1224)
sustains 100 fps and mode 4 (1920x1080) sustains 120 fps on this ISP. A
"low-latency 60" variant of either keeps that scan speed and doubles the
blanking. Instantaneous line rate (what the ISP FIFO sees) is identical to
the proven high-fps mode; the average pixel load *halves*, so the I6E ISP
throughput ceiling (~2.66 MPix @100 fps, see `STAR6E_IMX335_MODES.md`) is
comfortably respected.

Candidate lineup additions (names illustrative):

| candidate | geometry | scan timing from | est. readout | note |
|---|---|---|---|---|
| `1080p60_fastscan` | 1920x1080 | mode 4 (120 fps) | ~8.3 ms or less | biggest win, biggest crop |
| `2176x1224_60_fastscan` | 2176x1224 | mode 3 (100 fps) | ~10 ms or less | milder crop, still large win |

Full-FOV 2560x1920 fast-scan is likely **not** available: the 5M@60 mode
already runs near the sensor's MIPI/line-rate budget, so shortening readout
requires reading fewer rows (crop). Confirm in step 0 rather than assume.

## Step 0 — measure before designing (MANDATORY)

The 16.7 ms figure above is the VENC-input arrival time, **not** proven to
be pure readout. The bring-up notes record `HMAX=275` / `line_period
3694 ns` for the crop modes, which implies active readout of 1944 rows could
be as low as ~7.2 ms in the *current* 5M60 mode — in which case the observed
16.7 ms is dominated by exposure overlap + frame-boundary hand-off, and a
fast-scan mode would win less than the naive estimate. Before writing any
register table:

1. Compute actual per-mode `line_time = HMAX / INCK` and
   `readout = active_rows x line_time` from the existing tables in
   `sensor_imx335_star6e.c` (VMAX regs 0x3030-32, HMAX regs 0x3034-35).
2. Pin down what the SigmaStar PTS is stamped at (exposure start vs VIF
   frame-done) — it defines what `capture_us` and the VENC proc deltas
   actually measure. Empirical probe: sweep `isp.maxShutter` (e.g. 10 ms ->
   2 ms) and watch whether VENC `OnPreProcessInputTask` avg moves 1:1.
3. From (1)+(2), predict the gain per candidate mode. Only proceed if the
   predicted glass-to-encoded win is >= ~4 ms; otherwise close this plan
   with the measurement as the documented negative result.

## Validation plan (when implemented)

- `/proc/mi_modules/mi_vif/mi_vif0` FPS column = 60 sustained, 0 drops
  (extractor recipe in `STAR6E_IMX335_MODES.md`).
- VENC proc `OnPreProcessInputTask`/`DequeueInputTask` averages drop by the
  predicted readout delta.
- Sidecar `capture -> ready` (hub Pipeline tab "encode" row) drops to match.
- CBR sanity at the new mode: bitrate holds target at `fpv.noiseLevel=0`
  (guard against mode-dependent RC surprises).
- Reinit/SIGHUP cycling x3 healthy; AE converges (longer blanking raises the
  max integration time, so shutter caps still fit — verify, don't assume).

## Known traps (from the bring-up, all bit us before)

- **Mode-count shrink/growth gotcha:** venc persists `sensor.mode` by index
  in `/etc/waybeam.json`; a persisted index outside the new table makes venc
  exit at boot with no video. Append new modes at the END of the lineup, and
  check the fps-ordered convention still holds (catalog is fps-sorted).
- **Enum wedge:** a bad mode/geometry can wedge the sensor enumeration and
  needs a device power-cycle, not a reboot. Bench on .13 or .201, never on
  the flying craft first.
- Window-crop geometry must follow the byte-exact formula recorded in
  `STAR6E_IMX335_MODES.md` (HTRIM/HNUM/AREA3/HMAX relations) — deviations
  produced unstable VIF rates (2048x1152@100 case).
- Setting `sensor.mode` triggers venc's own reinit — do not also call
  restart (collides `paused`).

## Non-goals

- No change to the VPE->VENC bind (FRAMEBASE is the only mode this silicon
  supports — settled, do not re-litigate).
- No IMX415 variant in this pass (same technique applies later if the
  IMX335 result is good).
- No attempt at full-FOV fast-scan unless step 0 shows the 5M60 line rate
  has headroom.

## Related

- `documentation/STAR6E_IMX335_MODES.md` — mode lineup, ISP ceiling, crop
  geometry formulas, measurement recipes.
- `documentation/REALTIME_PIPELINE_INVESTIGATION.md` — why slice-level low
  delay is closed on i6e.
- 2026-08-01 latency budget session: exposure (<=10 ms shutter cap) +
  readout + encode 3.7 ms ≈ the observed 30 ms; no queueing found anywhere
  (VENC input arrives one frame period after PTS; the two `venc_rec_*`
  buffers are DPB reference frames, not input caching).
