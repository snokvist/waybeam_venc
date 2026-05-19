# EIS Gyro-Assisted LDC Stabilization — Spec

> **Status:** Phase 1 (Spec). Awaiting human approval before code changes.
> **Target:** Star6E only. Maruko deferred (different SDK vintage).
> **Branch:** `feature/mi-ldc-gyro-eis`.

## Goal

Per-frame digital image stabilization on Star6E using BMI270 gyro samples
warped through the SigmaStar VPE LDC engine. Replaces the historical
software EIS that was removed in 0.8.0 (see HISTORY).

## Approach

V2 API path: LDC embedded in VPE via `libmi_vpe.so` (the `MI_VPE_LDC*`
symbol family — already exported on device, confirmed by symbol dump).

**Why not Path A (kernel-driven gyro):** The bundled `mi_gyro.ko` is
hardcoded for an InvenSense ICG-20660 IMU at I2C-0. Our board has a BMI270
at I2C-1. No module param, no userspace knob, no register-map match.
Tested + dead. We keep `src/imu_bmi270.c` as the gyro source.

**Mode:** `MI_VPE_LDC_WORKMODE_DIS_GYRO` (=4) with `proj3x3On=TRUE` so the
SDK uses *our* userspace 3×3 matrix rather than expecting kernel gyro
data. This is the same pattern as the bundled `param_eis.ini`.

**Per-frame update:** `MI_VPE_SetChannelParam(chn, &param)` with the
runtime `i6e_vpe_para.lensAdj` filled with the new `proj3x3[9]`.
`MI_VPE_LDC{Beg,Set,End,SetBatch}ViewConfig` are added for Phase 3c
(rolling-shutter slicing).

## Files changed

### New
- `include/star6e_eis.h` — EIS module interface
- `src/star6e_eis.c` — gyro→matrix loop, channel-param push, recentering

### Modified
- `include/star6e_mi.h` — add 4 LDC fn pointers to `star6e_vpe_impl`
- `src/star6e_mi.c` — `LOAD_SYM` + null-check the 4 new symbols
- `include/star6e.h` — add `MI_VPE_LDCBegViewConfig` etc. macros
- `src/star6e_pipeline.c` —
  - flip `lensAdjOn = 1` and populate `channel_attr.lensInit` when
    `eis.enabled`
  - replace `star6e_pipeline_imu_push` stub with a push into the EIS ring
  - call `star6e_eis_init/destroy` at start/stop
- `src/star6e_runtime.c` — call `star6e_eis_update_for_next_frame()` once
  per frame after `imu_drain()`
- `include/venc_config.h` — add `EisConfig` (5 fields)
- `src/venc_config.c` — defaults + parse + `g_fields[]` + aliases
- `configs/*.json` — add `eis.*` block (default off)
- `documentation/CONFIG_SCHEMA.md` — document new fields
- `HISTORY.md` — entry under unreleased

### Untouched (vendor)
- `sdk/ssc338q/include/i6_vpe.h` — `i6e_vpe_ildc` / `i6e_vpe_ldc` /
  `i6e_vpe_para.lensAdj` already have all fields we need.

## Config schema (Phase 4)

```json
"eis": {
  "enabled": false,
  "crop_ratio_pct": 80,
  "focal_length_x": 204137,
  "focal_length_y": 204137,
  "slice_count": 1,
  "recenter_tau_ms": 500
}
```

- `enabled` — master switch. When false, `lensAdjOn = 0` (current behavior).
- `crop_ratio_pct` — output retention %. 80 = 10% margin each edge.
- `focal_length_x/y` — `f_mm / sensor_um × 100000`. Default from
  `param_eis.ini` (IMX307 reference; IMX335 needs measurement, v1 ships
  with sensor-agnostic default and accepts the resulting mild distortion).
- `slice_count` — Phase 3a/3b use 1 (whole-frame). Phase 3c bumps to 6
  (per `param_eis.ini` reference) for rolling-shutter correction.
- `recenter_tau_ms` — exponential decay time-constant pulling the crop
  origin back to center so the warp doesn't drift to the margin edge.

## Phase split

### Phase 3a — LDC bring-up smoke (kill-switch)
1. Add 4 LDC fn pointers + LOAD_SYM
2. When `eis.enabled`:
   - `channel_attr.lensInit.mode = 4` (DIS_GYRO)
   - `channel_attr.lensInit.proj3x3On = 1`
   - `channel_attr.lensInit.proj3x3 = identity_Q16`
   - `channel_attr.lensInit.userSliceNum = 1`
   - `channel_attr.lensInit.focalLengthX/Y` from config
   - `channel_attr.lensInit.mapType = SENSORCALIB`
   - `channel_attr.lensInit.calibInfo.calibPolyBinAddr = NULL, size = 0`
     (test first whether the SDK accepts no calibration; if it refuses,
     fall back to embedding a static identity calib blob)
   - `channel_attr.lensAdjOn = 1`
   - `param.lensAdjOn = 1`
3. Build, deploy, **verify the encoder still streams a normal image**.
   Failure here is the kill-switch — bail before phase 3b.

### Phase 3b — gyro→matrix loop
4. `star6e_eis_init()` — owns the producer-side ring (the `imu_push`
   callback feeds it), records the last-frame timestamp, holds the
   accumulated-rotation state (yaw, pitch, roll integrators).
5. `star6e_eis_update_for_next_frame()` — called once per encoded frame:
   - Drain ring samples newer than last update
   - Sum `ω · dt` per axis → delta-rotation since last call
   - Apply 1st-order low-pass (high-pass on motion to skip DC drift)
   - Build a small-angle rotation 3×3 (yaw=Rz, pitch=Rx; roll defer)
   - Scale by focal length → projection matrix in Q-format expected by
     the SDK (verify Q-format empirically; reference suggests Q16)
   - `MI_VPE_SetChannelParam(0, &param)` with updated `lensAdj.proj3x3`
6. Recentering spring: exponentially decay accumulated angle toward zero
   with `tau = recenter_tau_ms`. Prevents margin saturation.

### Phase 3c — rolling-shutter slicing
7. Switch from `SetChannelParam` to `MI_VPE_LDCSetBatchViewConfig` —
   build N=`slice_count` matrices spanning the inter-frame interval.
8. Each slice's matrix integrates gyro to slice-mid-time rather than
   frame-end. Compensates the IMX335 line-by-line scan.

### Phase 4 — config + docs
9. `venc_config.{c,h}` 7-touch-point compliance (per
   `feedback_venc_schema_field_checklist.md`).
10. `configs/*.json` default `eis.enabled = false`.

### Phase 5 — verify
11. `make verify` (both backends; eis code gated by `PLATFORM_STAR6E`).
12. `scripts/star6e_direct_deploy.sh cycle` on 192.168.1.13 with
    `eis.enabled = false` → confirm zero regression.
13. Cold-boot, switch to `eis.enabled = true`, confirm:
    - stream still flows
    - hand-shake produces visible counter-motion in the encoded frame
    - no crashes after 5 min runtime
    - clean shutdown (no D-state on stop)

## Open risks (call out before coding)

1. **SDK may refuse `lensAdjOn=1` without a real CalibPoly bin.** If
   Phase 3a fails with a NULL calib pointer, we'll embed a generic
   pass-through calib blob (a few hundred bytes; format documented in
   the `Pattern/...` reference files).
2. **Q-format of `proj3x3` is unverified.** Reference cpp passes `{1,0,0,
   0,1,0, 0,0,1}` as the identity, suggesting Q0 or just-set-`proj3x3On=
   FALSE` semantics. Phase 3a will probe this by trying both Q0
   (`{1,0,0,...}`) and Q16 (`{65536,0,0,...}`) identities and seeing
   which keeps the image undistorted.
3. **`MI_VPE_SetChannelParam` may not accept runtime `proj3x3` updates
   while the channel is running.** Reference cpp only sets it at create
   time. If runtime updates are rejected, we switch to
   `LDCBegViewConfig + LDCSetViewConfig + LDCEndViewConfig` which is
   the documented per-frame path.
4. **Focal length default mismatch for IMX335** will produce mild
   geometric distortion at the image edges (the warp engine
   over/under-corrects). v1 accepts this; v2 adds a per-sensor calib
   capture flow.

## Out of scope

- Maruko backend (different SDK; revisit after Star6E proves the model)
- Per-sensor lens calibration capture (manual `CalibPoly.bin` generation
  workflow is its own feature)
- Accelerometer-fused horizon leveling (requires an AHRS filter and
  adds latency; gyro-only is the documented fast path)
- OSD-overlay tracking through the warp (the existing OSD attaches at
  VENC, post-LDC; should "just work" but needs visual check)
