# Star6E IMX415 Mode Lineup

Status: **device-verified 2026-07-05** on SSC338Q (Infinity6E, kernel 4.9.84)
+ IMX415, 4-lane MIPI (bench box 192.168.1.13). Covers the in-tree driver
`drivers/sensor_imx415_star6e.c`, built via `make drivers-star6e` and staged
to `sensors/star6e/sensor_imx415_star6e.ko`.

Star6E counterpart to `drivers/sensor_imx415_maruko.c`. Ships the stock
OpenIPC infinity6e linear tables plus **one new non-binned window-crop tier at
100fps**, fps-ordered, with HDR/DOL removed.

## Mode table (fps-ordered)

| Idx | Resolution | fps | Readout | Link | Init function |
|---|---|---|---|---|---|
| 0 | 3840×2160 | 30  | all-pixel, **non-binned** | 891  | `pCus_init_8m_30fps_mipi4lane_linear` |
| 1 | 2560×1440 | 60  | crop, **non-binned**      | 1485 | `pCus_init_5m_60fps_mipi4lane_linear` |
| 2 | 1920×1080 | 90  | **2×2 binned** (full FOV) | 891  | `pCus_init_2m_90fps_mipi4lane_linear` |
| 3 | **2304×1296** | **100** | crop, **non-binned** | 1485 | `pCus_init_window_2304x1296_100` **(NEW)** |
| 4 | 1472×816  | 120 | **2×2 binned** (crop)     | 891  | `pCus_init_1m_120fps_mipi4lane_linear` |

HDR/DOL is not exposed (the two HDR handles are `NULL`; the compiler dead-code-
eliminates the HDR subtree — no `_HDR` mode strings in the `.ko`).

## Device verification (2026-07-05, .13)

Method: pin `video0.fps=120` + `video0.size=auto`, switch only `sensor.mode`;
true capture rate = the `Src/Dst`-ratio row FPS in `/proc/mi_modules/mi_vif/
mi_vif0`. `dmesg` watched for sustained FIFO-FULL / Skip-IQ / FrmLost.

| Idx | Target fps | Measured VIF FPS | Sustained drops |
|---|---|---|---|
| 0 | 30  | 32.16  | 0 |
| 1 | 60  | 59.51  | 0 |
| 2 | 90  | 89.92  | 0 |
| 3 | 100 | 99.00 (99.10 over a 30 s soak) | 0 |
| 4 | 120 | 118.64 | 0 |

## The non-binned 100fps mode (idx3)

**Goal:** a 100fps mode with the widest possible FOV that is *not* binned
(native-resolution readout, sharp — unlike the 2×2-binned 90/120 modes which
keep full FOV but at half resolution). Three facts made it possible on I6E,
each device-established here:

1. **The 1485 Mbps link removes the MIPI bottleneck.** The stock 891 Mbps modes
   cap non-binned readout low; idx1 already proves a non-binned crop on the
   doubled `SYS_MODE=0x08` (1485) link. The new mode reuses idx1's analog/PLL
   base verbatim.
2. **A reduced HMAX beats the vertical-timing wall.** At the 1485 line time
   (HMAX=652, ~8.7 µs) a 100fps frame (VMAX≈1144) only fits ~1080 active lines —
   so a fixed-HMAX 16:9@100 pins to 1920×1080. Cutting HMAX to **548** (a
   narrower crop needs less line time) shortens the line to ~7.34 µs, letting
   **1296 active lines** fit a 100fps frame.
3. **The I6E ISP sustains ~300 MPix/s at 100fps** — higher than the I6C model.
   Device-mapped wall (all HMAX-reduced, non-binned, @100fps):

   | crop | MPix | MPix/s | VIF | verdict |
   |---|---|---|---|---|
   | 1920×1080 | 2.07 | 207 | 99.5 | clean |
   | 2176×1224 | 2.66 | 266 | 99.6 | clean |
   | **2304×1296** | **2.99** | **299** | **99.0** | **chosen — widest clean 16:9** |
   | 2432×1368 | 3.33 | 333 | 74.4 | over (drops) |
   | 2560×1440 | 3.69 | 369 | 49.8 | halved |

**Result:** 2304×1296@100 non-binned = **35% FOV area** at native resolution,
0 drops (incl. a 30 s soak) — vs the ~25% of Maruko's I6C 1920×1080@100, and
sharp where the binned 90/120 modes are soft.

Geometry (`imx415_geo_2304x1296_100`, centered 16:9): HMAX 548, VMAX 1360,
HST 780, HWIDTH 2304, VST 896, VWIDTH 2592 (half-line units). The crop helper
`imx415_init_window_crop_1485()` replays the idx1 base minus its standby-exit,
injects the geometry in standby, then re-issues the exit (PR#156 discipline).

No venc changes were needed: Star6E's REALTIME VIF→VPE bind and default CSI
clock already carry the 1485 link (idx1 proves it), so a new 1485 mode is
driver-only — unlike Maruko/I6C, which needed a FRAMEBASE bind + 288 MHz CSI
clock keyed on a `link_mbps` field.

## Warm-switch safety (readout-mode register latch)

The SigmaStar SDK **keeps sensor registers across a mode switch** (no reset).
The stock non-binned tables (idx0/idx1) did **not** write the binning registers
— they relied on power-on defaults — so a warm switch **binned→non-binned**
(e.g. 90→30) left 2×2 binning + binned DIG_CLP latched and corrupted the
readout. (The earlier ascending-only sweep never hit this direction.)

Fix: every non-binned path now writes the readout-mode registers **explicitly**
in standby — `0x3020/21/22 = 0x00` (HADD/VADD/ADDMODE off) and `0x30D9=0x06 /
0x30DA=0x02` (all-pixel DIG_CLP): retrofit into the stock idx0/idx1 tables, and
carried in the idx3 crop geometry. The binned tables (idx2/idx4) already set
their own binning + binned DIG_CLP, so the reverse direction was already safe.

Device-verified both directions, 0 drops: 120→30, 90→100, 120→60, 90→30.

## Deploy gotcha — shrinking the mode count

venc persists `sensor.mode` in `/etc/waybeam.json`. The stock IMX415 driver
exposed 11 modes; this one exposes 5. If the persisted `sensor.mode` is ≥ 5
(e.g. a stock HDR index, or a leftover probe index), venc fails mode-select on
boot (`--sensor-mode N is not available on selected pad(s)` → `Failed to select
sensor mode on any pad`) and **exits** — no video. Fix: patch
`/etc/waybeam.json` sensor-block `"mode"` to a valid index (0–4) before first
boot of the new driver, then restart venc.

## Deploying a driver change

The `.ko` is selected at boot by `S94sensor-detect`
(`lsmod | grep -o 'sensor_imx[0-9]*'`) and loaded by the MI framework, which
binds pads — a swap needs file-replace + reboot. The .13 rootfs is squashfs +
overlay, so a single write copies-up and persists:

```
# cross-built .ko from sensors/star6e/sensor_imx415_star6e.ko, then:
arm-linux-strip --strip-debug sensor_imx415_star6e.ko           # ~15 KB
cat sensor_imx415_star6e.ko | \
  ssh root@<star6e> 'cat > /lib/modules/4.9.84/sigmastar/sensor_imx415_mipi.ko'
# then reboot (prefer graceful `reboot`; power-cycle if enumeration wedges)
```

Build: `make drivers-star6e KSRC_STAR6E=<i6e-4.9.84-kernel-src>` — builds both
IMX335 and IMX415 Star6E objects. Use the OpenIPC builder output
(`builder/openipc/output/build/linux-custom`) so module **vermagic matches**
(`4.9.84 SMP preempt mod_unload ARMv7 thumb2 p2v8`). The modpost `undefined!`
warnings for `DrvSensor*`/`DrvVif_*`/`intlog10` are expected.

For rollback, the repo tracks a stock IMX415 driver at
`sensors/star6e/drv_ms_cus_imx415_MIPI.ko`; the exact `.ko` pulled from .13
before this work (md5 `3fee27ec7f9b4252db01d59505e28404`) is in the deploy
scratchpad.
