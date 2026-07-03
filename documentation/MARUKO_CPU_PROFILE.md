# Maruko IMX415 — CPU profile at high fps (90 / 100 fps)

Investigation of the sustained ~50–60% CPU observed at the 90 and 100 fps
modes. Device: SSC378QE Maruko, **single-core ARMv7** (so all percentages are
of one core). Method: `/proc/<pid>/task/*/stat` utime+stime jiffie deltas over
a 10 s window + `/proc/stat` for whole-system busy
(`scripts/maruko_cpu_profile.sh`, run on-device: `sh maruko_cpu_profile.sh
[interval_s]`), while streaming H.265 at ~16 Mbps.

This is orthogonal to the mode-lineup change on this branch — it characterises
the pre-existing per-frame cost of the pipeline. No code behaviour is changed
here; the recommendations below are proposals.

## Measured

| Config | System busy | `3A_Proc_0` | main thread | other |
|---|---|---|---|---|
| mode 4, 100 fps, native AE | **62.3%** | 16.7% | 12.3% | 0.7% |
| mode 3, 90 fps, native AE | **60.0%** | 15.3% | 12.5% | 0.6% |
| mode 3, 90 fps, **throttle AE** | **49.9%** | 11.3% | 12.2% | 1.4% |

90→100 fps only moves ~2 points: at these rates the pipeline is near a high
fixed baseline, not scaling linearly in that narrow range.

> **Load-average red herring:** `top` shows load ~14 on this 1-core SoC. That
> is the SigmaStar SDK's kernel pipeline threads (`venc0_P0_MAIN`,
> `IspDriverThread`, `SensorIfThreadW`, `scl0_P0_MAIN`, `vif3_P0_MAIN`, …)
> sitting in uninterruptible **D-state** waiting on hardware. D-state counts
> toward load average but consumes ~no CPU. Ignore load average here; use the
> jiffie-delta busy% above.

## Where the ~60% goes

Three buckets. At 90 fps native AE (60.0% busy):

1. **Kernel / SDK hardware pipeline — ~32% (sys time, not in any userspace
   thread row).** The SDK's D-state kernel workers plus ISP 3A-stats DMA,
   VENC hardware servicing, SCL, and MIPI/interrupt handling. This is the
   intrinsic cost of clocking sensor→ISP→SCL→VENC at 90–100 Hz and is largely
   **irreducible at a given fps/resolution** — the only levers are lower fps
   or fewer pixels.
2. **`3A_Proc_0` — ~15%.** The SDK's userspace 3A thread, running the AE+AWB
   algorithm **once per sensor frame** (so 90–100 Hz at the high modes). This
   is the largest *reducible* userspace cost. `src/maruko_pipeline.c:184-231`
   enables it (`MI_ISP_EnableUserspace3A`); it is mandatory (without it the
   ISP FIFO stalls at ≥60 fps).
3. **main encode/output thread — ~12%.** `MI_VENC_GetStream` → HEVC/RTP
   packetize → `sendmmsg`, plus per-frame scene-size accounting and the IMU
   FIFO drain. **Already well-optimised** (see below); ~1.2 ms/frame at
   100 fps is reasonable and there is no high-value cut here.

## What is already efficient (verified by code audit)

- **RTP output path** (`maruko_output.c`, `rtp_packetizer.c`, `hevc_rtp.c`):
  zero-copy — the payload stays an iovec pointer into the encoder buffer, only
  a 15-byte RTP+FU header is copied per packet, and **all packets of a frame
  go out in one `sendmmsg`** (~100 send syscalls/s at 100 fps, not per-packet).
  No per-frame heap allocation, no spin, no per-byte CRC. Nothing to gain.
- **Encode fetch loop** (`maruko_pipeline.c:3311`): blocks on `poll()` of the
  VENC fd — wakes at fps, does not spin. The 1000 ms timeout is only a
  shutdown-latency cap.
- **IMU** (`imu_bmi270.c`): default is FIFO mode with **no polling thread** —
  drained once per frame (2 I2C transactions). Avoids the known 1600 Hz
  I2C death-spiral. Only the *fallback* path (FIFO init failed) spins at ODR.
- **Scene detector** (`scene_detector.c`): integer EMA over encoded byte
  counts, no pixel/histogram work. Negligible.
- **Verbose log + CPU sample**: gated to ~1 Hz / 500 ms. Negligible.

## Recommendations (prioritised)

### 1. Enable the AE throttle at high fps — **measured 60.0% → 49.9%**
The throttle (`isp.aeEngine = custom`, default `sdk`) swaps the SDK's
per-frame AE algorithm for a stub and runs the AE control law in a fixed
~15 Hz supervisory thread instead (`src/maruko_cus3a.c`, dispatch
`maruko_pipeline.c:2333-2362`). Measured **~10 points off the whole system**
at 90 fps (bigger than the 4-pt `3A_Proc_0` drop, because it also sheds the
associated kernel ISP-stats/i2c work). AWB stays per-frame native by design
(no colour cast); only AE responsiveness relaxes to 15 Hz, which is fine for
FPV.

- Config key: `isp.aeEngine` = `sdk` | `custom`; companion `isp.aeFps`
  (default 15) sets the supervisory rate.
- **Proposal:** auto-select `custom` when the active sensor mode fps ≥ ~60,
  keep `sdk` for low-fps modes (per-frame AE is cheap there and native
  quality is free). This makes 90/100 fps ship at ~50% instead of ~60%
  without a config change, and leaves 30/50 fps untouched.

### 2. Gate the debug OSD redraw to ~5–10 Hz (only when OSD is enabled)
When `debug_osd` is on, the panel is re-rasterised and
`MI_RGN_UpdateCanvas`'d **every frame** (`maruko_pipeline.c:3538-3607`,
`debug_osd.c:856`) even though the displayed values change at ≤1 Hz. Gate the
whole block behind a ~100–200 ms timer (or a value-changed check). Zero cost
when the debug OSD is off (the default), so this only helps OSD users.

### 3. Guard the IMU polling-fallback rate
Default FIFO mode is fine. But `imu.odr` allows up to 1600 Hz, and if FIFO
init ever fails the fallback `imu_reader_thread` does one I2C transaction per
sample at that ODR — an I2C-bound CPU sink. Clamp the *fallback* effective
rate (e.g. ≤ 200–400 Hz) or warn when falling back above that.

### 4. Irreducible floor
The ~32% kernel/SDK bucket is the hardware pipeline cost at 90–100 Hz. If a
mode must run cooler than throttle can achieve, the only remaining levers are
a lower fps or a smaller encode/sensor size.

## Bottom line
The ~60% is real and mostly structural: ~32% kernel SDK pipeline (fixed for
the fps/res), ~15% per-frame AE, ~12% encode+send. The output/encode paths are
already optimal. The one high-value, low-risk lever is the **AE throttle**,
which is measured to bring 90 fps from 60% to 50%; recommend auto-enabling it
at fps ≥ 60. Debug-OSD gating and the IMU fallback guard are secondary.
