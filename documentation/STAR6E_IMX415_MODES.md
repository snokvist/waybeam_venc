# Star6E IMX415 Mode Lineup

Status: **device-verified 2026-07-05** on SSC338Q (Infinity6E, kernel 4.9.84)
+ IMX415, 4-lane MIPI (bench box 192.168.1.13). Covers the in-tree driver
`drivers/sensor_imx415_star6e.c`, built via `make drivers-star6e` and staged
to `sensors/star6e/sensor_imx415_star6e.ko`.

This is the Star6E counterpart to `drivers/sensor_imx415_maruko.c`. Unlike the
IMX335 Star6E lineup (which adds window-crop tiers, see
`STAR6E_IMX335_MODES.md`), the IMX415 driver ships the **stock OpenIPC
infinity6e linear lineup verbatim** — four fps-ordered modes — with HDR/DOL
removed. No window-crop tiers were added.

## Why an in-tree Star6E driver

Star6E previously had only a prebuilt stock `.ko` in `sensors/star6e/`. This
adds the same buildable, customizable driver that Maruko owns
(`make drivers-star6e KSRC_STAR6E=<i6e-kernel>`), so the Star6E IMX415 lineup
is owned here and can be extended with crop tiers later if wanted.

## Mode table (fps-ordered)

All four modes are the stock infinity6e linear tables, unchanged. HDR/DOL is
not exposed (the two HDR handles are `NULL`; the compiler dead-code-eliminates
the HDR init/AE subtree).

| Idx | Resolution | fps | Aspect | Init function | Origin |
|---|---|---|---|---|---|
| 0 | 3840×2160 | 30  | 16:9 | `pCus_init_8m_30fps_mipi4lane_linear`  | stock (4K full) |
| 1 | 2560×1440 | 60  | 16:9 | `pCus_init_5m_60fps_mipi4lane_linear`  | stock |
| 2 | 1920×1080 | 90  | 16:9 | `pCus_init_2m_90fps_mipi4lane_linear`  | stock |
| 3 | 1472×816  | 120 | 16:9 | `pCus_init_1m_120fps_mipi4lane_linear` | stock |

## Device verification (2026-07-05, .13)

Test method: pin `video0.fps=120` + `video0.size=auto`, then switch only
`sensor.mode`. True sensor+ISP capture rate = the `Src/Dst`-ratio row's FPS
field in `/proc/mi_modules/mi_vif/mi_vif0` (not AE `sensor_fps`). `dmesg`
watched for sustained FIFO-FULL / Skip-IQ / FrmLost.

| Idx | Target fps | Measured VIF FPS | Sustained drops |
|---|---|---|---|
| 0 | 30  | 32.16  | 0 |
| 1 | 60  | 59.46  | 0 |
| 2 | 90  | 90.09  | 0 |
| 3 | 120 | 118.64 | 0 |

All four modes came up clean. (Mode 0 reads slightly above 30 — stock 4K table
behavior, unchanged from the OpenIPC blueprint, 0 drops.)

## HDR removal

Identical to the IMX335 Star6E driver: the two HDR handles are passed `NULL`
in `SENSOR_DRV_ENTRY_IMPL_END_EX`, and `cus_camsensor_init_handle_hdr_dol_sef`
is made `static`, so the compiler dead-code-eliminates the whole HDR init/AE
subtree (verified: no `*_HDR` mode strings in the `.ko`). The dead HDR source
lines remain in-file (unreferenced); a later cleanup can strip them for source
parity with the Maruko driver.

## Deploy gotcha — shrinking the mode count

The venc persists `sensor.mode` into `/etc/waybeam.json`. The stock IMX415
driver exposed 11 modes (4 linear + 7 HDR); this driver exposes 4. If the
persisted `sensor.mode` is ≥ 4 (e.g. a stock HDR index), venc fails
mode-select on boot (`--sensor-mode N is not available on selected pad(s)` →
`Failed to select sensor mode on any pad`) and **exits** — no video. Fix:
patch `/etc/waybeam.json` sensor block `"mode"` to a valid index (0–3) before
first boot of the new driver, then restart venc.

## Deploying a driver change

Same mechanics as IMX335 — the `.ko` is selected at boot by
`S94sensor-detect` (`lsmod | grep -o 'sensor_imx[0-9]*'`) and loaded by the MI
framework, which binds pads, so a swap needs file-replace + reboot. The .13
rootfs is squashfs + overlay, so a single write copies-up and persists:

```
# cross-built .ko from sensors/star6e/sensor_imx415_star6e.ko, then:
arm-linux-strip --strip-debug sensor_imx415_star6e.ko           # ~14.8 KB
cat sensor_imx415_star6e.ko | \
  ssh root@<star6e> 'cat > /lib/modules/4.9.84/sigmastar/sensor_imx415_mipi.ko'
# then reboot (prefer graceful `reboot`; power-cycle if enumeration wedges)
```

Build: `make drivers-star6e KSRC_STAR6E=<i6e-4.9.84-kernel-src>` — builds both
IMX335 and IMX415 Star6E objects and stages `sensors/star6e/sensor_imx*_star6e.ko`.
Use the OpenIPC builder output (`builder/openipc/output/build/linux-custom`) so
module **vermagic matches** the target
(`4.9.84 SMP preempt mod_unload ARMv7 thumb2 p2v8`). The modpost `undefined!`
warnings for `DrvSensor*`/`DrvVif_*`/`intlog10` are expected — they resolve
from the proprietary `mhal`/`mi` modules at load time.

For rollback, the repo already tracks a stock IMX415 driver at
`sensors/star6e/drv_ms_cus_imx415_MIPI.ko`. The exact `.ko` pulled from .13
before this deploy (24992 B, md5 `3fee27ec7f9b4252db01d59505e28404`) was kept
in the deploy scratchpad, not committed.
