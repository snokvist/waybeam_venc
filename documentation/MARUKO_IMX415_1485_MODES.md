# Maruko IMX415 1485 Mbps Non-Binned Modes

Status: device-verified July 2026 on SSC378QE (Infinity6C) + IMX415, 4-lane
MIPI at 1485 Mbps/lane. Covers driver modes 5–7 in
`drivers/sensor_imx415_maruko.c` and the pipeline bind policy in
`src/maruko_pipeline.c`.

## Why 1485 Mbps

The stock vendor modes run 891 Mbps/lane, which caps non-binned readout at
3760x2116@30. Doubling the link to 1485 Mbps (SYS_MODE `0x3033=0x08`,
HMAX=652, line time 8.736 µs) makes 60/90 fps non-binned crops possible —
subject to the ISP ceiling below.

## The two required platform changes

1485 modes produce zero frames (frozen `vif0` IRQs) or overflow unless BOTH
of these are in place:

1. **VIF→ISP bind = FRAME_BASE** (`src/maruko_pipeline.c`,
   `bind_maruko_pipeline()`). At 1485x4 the instantaneous line burst is
   ~594 MPix/s, which exceeds the ISP core drain (384 MHz, its silicon max —
   reg `0x1F207184` mux[12:10]=7). A REALTIME bind overflows the ISP P0
   input FIFO. FRAME_BASE buffers VIF output in DRAM so only the average
   rate matters. The bind is **mode-conditional**: keyed on the `_1485`
   suffix in the sensor mode name; 891 modes keep REALTIME for minimum
   glass-to-glass latency (their ~356 MPix/s burst matches the drain).
   VIF→ISP on I6C accepts ONLY these two link types — RING / AUTOSYNC /
   LOWLATENCY all fail to bind (-1610014712).

2. **CSI-MAC clock ≥ 288 MHz** (`pCus_poweron`: `SetCSI_Clk(idx,
   CUS_CSI_CLK_288M)` for mode index ≥ 5). At the default 216 MHz a 1485
   link delivers ZERO frames. The 432M enum is not a real csi-mac parent on
   I6C (the mux tops out at 288 MHz, see `infinity6c-clks.dtsi`).

## The ISP throughput ceiling (FRAME_BASE modes)

FRAME_BASE puts the ISP in memory-to-memory operation with a measured
per-frame cost of:

```
T_frame ≈ 1.7 ms + 3.65 ns/pixel     (≈274 MPix/s marginal)
```

Five-point device fit (encoder-out fps via the `[verbose]` stream-loop
print, NOT `/api/v1/fps/live` which reports configured RC fps):

| Crop | MPix | Measured fps | Model |
|---|---|---|---|
| 2952x1224 | 3.61 | 68 | 67.2 |
| 2952x1656 | 4.89 | 50.9 | 51.2 |
| 2952x1848 | 5.46 | 45 | 46.3 |
| 3264x1848 | 6.03 | 42 | 42.2 |
| 3840x1800 | 6.91 | 37.2 | 37.1 |

The ceiling is clock-insensitive (ISP already at 384 MHz max; live mux pokes
of ISP and CSI-MAC clocks change nothing) and buffer-insensitive (VIF output
depth 4→8 no change). It is a per-frame dispatch + drain cost of the ISP m2m
path. Consequences:

- Max non-binned pixels @60 fps ≈ **4.1 MPix** → mode 6 `2952x1368@60`
- Max non-binned pixels @90 fps ≈ **2.6 MPix** → mode 7 `2112x1184@90`
- Full readout 3760x2116@60 (~8 MPix) is unreachable on this silicon.

## Mode table (driver indexes)

| Idx | Mode name | Link rate | Notes |
|---|---|---|---|
| 0–4 | vendor modes (4K30, superwide, binned 1080p60/90, 720p120) | 891 | REALTIME bind |
| 5 | 2952x1656@50fps_1485 | 1485 | 1:1 5MP crop, ~16:9; sensor paced to 50.0 (VMAX=2289) = the ceiling for 4.89 MPix — measured ~49 fps (pacing at the exact ceiling leaves ~2% dispatch shortfall, but the FRAME_BASE queue stays empty for minimum latency; the old 60-fps pacing measured 50.9 by saturating the queue) |
| 6 | 2952x1368@60fps_1485 | 1485 | **max @60** — measured 60.0 fps exact |
| 7 | 2112x1184@90fps_1485 | 1485 | **max @90** — measured 90.0 fps exact |

Modes 6/7 deliver their full nominal rate; mode 5 delivers ~49/50. The ceiling-
characterization ladder probes (2952x1848 / 3264x1848 / 3552x1848 @60,
2952x1224@90 — delivering 45/42/~39/68 fps) were pruned after measurement;
regenerate any of them with the recipe below if needed.

## Generating a new 1485 mode

All 1485 tables derive from the proven 5m skeleton (mode 5); substitute only:

- `0x3024/25` VMAX little-endian (60 fps → 1908; 90 fps → 1272; fps =
  1 / (VMAX × 8.736 µs))
- `0x3040/41` HST (horizontal crop start; 2952 uses the vendor 444,
  otherwise center: (3864 − W) / 2)
- `0x3042/43` HWIDTH = W
- `0x3044/45` VST = 2192 − H
- `0x3046/47` VWIDTH = 2 × H

Everything else (HMAX=652, SYS_MODE, INCKSEL2–5, the 1485 analog front-end
block, MIPI timing regs, and the rate-dependent PHY INCKSELs `0x400C=0x01`,
`0x401F=0x01`, `0x4074=0x00`) stays identical. Then add the enum entry, mode
list entry, and `pCus_SetVideoRes` case with `vts_30fps` = VMAX and
`Preview_line_period = 8736`.

**Trap: `0x3032` must be `0x01`.** The vendor 1485 table ships
`0x3032 = 0x00`, which produces a dark image at ~¼ brightness (AE reads
avgY≈0, encoder bitrate collapses to ~⅕ of normal) — the same
single-register bug fixed on the 891 tables on 2026-05-14. Live-poke
verified: `i2ctransfer -y 1 w3@0x1a 0x30 0x32 0x01` on a running dark
sensor restores the image instantly. The skeleton in the driver is fixed;
keep the byte if regenerating from vendor sources.

**Trap: write the readout-mode registers explicitly.** Any new 1485
(all-pixel) table must include `0x3020/0x3021/0x3022 = 0x00`
(HADD/VADD/ADDMODE), `0x30D9 = 0x06` and `0x30DA = 0x02` (DIG_CLP,
all-pixel values). The vendor all-pixel tables omit them and rely on
power-on defaults, but the sensor keeps state across teardown and warm
reboot, so a prior binned run otherwise leaves 2x2 binning latched and
every warm 1485 bring-up fails (see Known issues).

Constraints: VENC device limit 4096x2176; at 90 fps (VMAX=1272) active
height ≤ ~1230 lines.

## Known issues

- **Binned→1485 mode switch wedge — ROOT-CAUSED AND FIXED (2026-07-03).**
  The failure was never in the CSI/VIF/ISP receive path: the binned 891
  tables set the sensor's binning registers (HADD/VADD/ADDMODE
  `0x3020/21/22 = 0x01`, DIG_CLP `0x30D9=0x02`/`0x30DA=0x01`) and the 1485
  all-pixel tables did not write those registers at all, relying on
  power-on defaults. The IMX415 keeps register state across pipeline
  teardown **and warm reboot** (the chip stays powered; the RESET-pin
  toggle in `pCus_HardwareReset` demonstrably does not clear it), so any
  warm entry into a 1485 mode after a binned run left the sensor in 2x2
  binning against an all-pixel PHY/crop config — geometry mismatch, zero
  frames to the ISP, venc aborts "no encoder data received", and the
  kernel floods `invalid in early CameraOpen` (MhalCameraOpen never
  completes, `/proc/mi_modules/mi_vif/mi_vif0` is never created). Only a
  power-cycle cleared it because only a power-cycle resets the sensor.
  **Fix:** all three 1485 tables (and the non-binned 891 base table) now
  explicitly write the all-pixel values (`0x3020/21/22=0x00`,
  `0x30D9=0x06`, `0x30DA=0x02`, per datasheet readout-mode table).
  Device-verified warm on a previously poisoned sensor: binned→1485 both
  directions, 1485↔1485, all modes at nominal fps and full brightness —
  no power-cycle rule needed anymore.
- **Teardown can hang in D-state.** Two occurrences with the landed build:
  SIGTERM teardown stuck forever (uninterruptible D-state,
  `wchan = MI_SYS_IMPL_FlushRealTimeOutputBuf`, SCL output port frozen at
  `workingTask_cnt=4`). Once when a second venc instance start raced the
  teardown, once after a ~5-minute run (a fresh 30-second run with the
  same config tore down clean — the trigger is task-queue depth
  accumulating over long runs, not a specific config). The pre-unbind
  drain (`maruko_wait_output_idle`) warns and proceeds but cannot unstick
  an already-frozen queue. Recovery: `reboot -f` (the I6C kernel has
  **no sysrq**; SIGKILL makes MI zombies — never use it). Notably the warm
  reboot did NOT wedge FRAME_BASE. Operational rules: wait for the old
  process to fully exit before starting a new one (poll `ps`, teardown
  takes several seconds); if the hang keeps recurring, the planned fix is
  a teardown watchdog (bounded flush wait + `reboot -f`).
- **Teardown Oops (historical)**: an Oops in
  `_MI_SYS_IMPL_UnBindChannelPort` was seen during the bind-type
  experiments, but did NOT reproduce in 6+ SIGTERM teardown cycles with
  the landed mode-conditional build (all teardowns clean, all MI
  resources freed). If it resurfaces: static analysis of `mi_sys.ko`
  located the FRAME_BASE-only hazard sites — the assert trap at
  `+0x26` (undefined instruction; console prints the mi_sys_impl.c line),
  and the framebase pending-bufref queue drain at `+0x1c6/+0x1e2/+0x286/
  +0x2a8` (data abort; fault address ≈0x3e3 means walking a freed,
  0x1d3-poisoned bufref).
- FRAME_BASE adds up to one frame of latency vs REALTIME — this is why the
  bind stays mode-conditional.
