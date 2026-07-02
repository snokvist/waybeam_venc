# Maruko IMX415 1485 Mbps Non-Binned Modes

Status: device-verified July 2026 on SSC378QE (Infinity6C) + IMX415, 4-lane
MIPI at 1485 Mbps/lane. Covers driver modes 5–11 in
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

- Max non-binned pixels @60 fps ≈ **4.1 MPix** → mode 10 `2952x1368@60`
- Max non-binned pixels @90 fps ≈ **2.6 MPix** → mode 11 `2112x1184@90`
- Full readout 3760x2116@60 (~8 MPix) is unreachable on this silicon.

## Mode table (driver indexes)

| Idx | Mode name | Link rate | Notes |
|---|---|---|---|
| 0–4 | vendor modes (4K30, superwide, binned 1080p60/90, 720p120) | 891 | REALTIME bind |
| 5 | 2952x1656@60fps_1485 | 1485 | 1:1 5MP crop; delivers ~51 fps (ceiling) |
| 6 | 2952x1848@60fps_1485 | 1485 | ~45 fps |
| 7 | 3264x1848@60fps_1485 | 1485 | ~42 fps |
| 8 | 3552x1848@60fps_1485 | 1485 | ladder probe |
| 9 | 2952x1224@90fps_1485 | 1485 | ~68 fps |
| 10 | 2952x1368@60fps_1485 | 1485 | **max @60** — measured 60.0 fps |
| 11 | 2112x1184@90fps_1485 | 1485 | **max @90** — measured 90.0 fps |

Modes 6–9 are ceiling-characterization ladder entries; they stream cleanly
but cannot reach their nominal fps. Keep or prune at packaging time.

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

Constraints: VENC device limit 4096x2176; at 90 fps (VMAX=1272) active
height ≤ ~1230 lines.

## Known issues

- **Teardown Oops**: stopping a 1485/FRAME_BASE run Oopses in
  `_MI_SYS_IMPL_UnBindChannelPort`; a reboot is required between mode
  switches. Under investigation (unbind order).
- FRAME_BASE adds up to one frame of latency vs REALTIME — this is why the
  bind stays mode-conditional.
