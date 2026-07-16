# Plan: Star6E VPE-embedded LDC (geometric distortion correction)

Companion to `requirements.md`. Phased, each gate cheap. No code is written
until Phase 0 is greenlit and the SDK hunt (requirements §"SDK reference
hunt") has resolved the `proj3x3` fixed-point format and the blob-required
question.

## Design decisions (locked to prevent mid-implementation drift)

1. **Use the VPE-embedded path, not standalone MI_LDC.** All entry points are
   in `libmi_vpe.so` (already `dlopen`ed). Avoids the Maruko blocker and the
   standalone-module i6e-513 prior art.
2. **Phase 0 uses only already-bound symbols.** Enable via
   `MI_VPE_SetChannelParam` (`lensAdj` + `lensAdjOn=1`). Do **not** bind
   `MI_VPE_LDCSetViewConfig` until Phase 1+ needs runtime re-warp.
3. **Static correction, not EIS.** One warp for the frame lifetime. Per-frame
   updates are out of scope here (that is the EIS/`DIS_GYRO` track).
4. **Correction is opt-in and compile-safe.** Gate behind config
   (`video0.ldc` off by default) so default builds/behaviour are unchanged
   and the identity path can never regress a shipping device.
5. **Prefer `SetChannelParam` (`lensAdj`) over `lensInit`** for the enable
   point if the SDK allows toggling on a running channel — smaller teardown
   surface. Confirm via SDK hunt #4; fall back to `lensInit` at
   `CreateChannel` if LDC must be set before Start.

## Phase 0 — identity liveness probe (~½ bench day)

**Bench:** `root@192.168.1.13` / imx335 (the documented Star6E bench).
Prefer `scripts/star6e_direct_deploy.sh cycle` per `AGENTS.md`.

**Change (throwaway, behind a temporary `#if`):** in
`star6e_pipeline_start_vpe()` (`src/star6e_pipeline.c:534`), replace the
zeroed param with an identity LDC:

```c
/* PROBE ONLY — identity 3x3, LDC on. Scale/format of proj3x3 is the
 * first thing to confirm from the SDK (Q16 shown; may be Q12). */
param.lensAdjOn        = 1;
param.lensAdj.proj3x3On = 1;
static const int IDENT_Q16[9] = {
    65536,0,0,  0,65536,0,  0,0,65536,   /* row-major identity, TBC */
};
memcpy(param.lensAdj.proj3x3, IDENT_Q16, sizeof IDENT_Q16);
param.lensAdj.configAddr = NULL;   /* tests "is the blob mandatory?" */
param.lensAdj.configSize = 0;
```

**Pass/fail gates:**
- `MI_VPE_SetChannelParam` returns 0 (watch specifically for **513** — the
  i6e prior-art failure code).
- `MI_VPE_StartChannel` succeeds; VENC produces frames; stream is visually
  unchanged (identity) via the normal UDP path.
- No `dmesg` NULL-deref / warp-engine faults (the i6e crash signature).
- Latency vs. `lensAdjOn=0` baseline within noise.
- **VPE mode:** confirm whether LDC-on is accepted with the existing
  `I6_VPE_MODE_REALTIME` + `I6_SYS_LINK_REALTIME` VIF→VPE link, or whether
  it forces an offline/framebase mode. If REALTIME is rejected with LDC,
  retry with `I6_VPE_MODE_DVR`/`CAM` and record the added latency — this is
  open question #1 and the feature's biggest risk. Record path is unaffected
  either way (it taps VENC, downstream of VPE).

**If it fails at SetChannelParam with a blob error:** the blob is mandatory
even for identity → jump to the SDK hunt for a minimal/dummy blob before
retrying. **If it NULL-derefs the kernel:** the VPE-embedded path shares the
i6e defect → document and stop (fall back to no HW LDC on Star6E).

**Teardown discipline:** exercise a full stop/reinit cycle (SIGHUP path, cf.
`documentation/SIGHUP_REINIT.md` / `FULL_TEARDOWN_REINIT_PLAN.md`) with LDC
on — attach/detach ordering is open question #6.

## Phase 1 — visible static warp + cost (~½ bench day)

- Non-identity `proj3x3` (e.g. 0.9× zoom, or a small shear) → confirm the
  warp is *actually applied* (not silently ignored) and correctly oriented
  (validates majorness/scale from SDK hunt #1).
- Measure added latency and memory bandwidth at **1080p**, then at the
  **120fps** mode (Star6E imx335 high-fps; cf.
  `documentation/STAR6E_IMX335_MODES.md`) — LDC is a DRAM-bandwidth consumer.
- Confirm LDC composes with the VPE **port scaler** (our port does
  ISP→scale→VENC) and note interaction with `MI_VPE_SetPortCrop`
  (`stab`/`stab-fill`): compose, exclusive, or ordering-sensitive?
- If runtime re-warp is wanted, bind `MI_VPE_LDCSetViewConfig` in
  `src/star6e_mi.c` (signature from SDK hunt #2) and prefer the
  Beg/End atomic variant for tear-free updates.

## Phase 2 — real lens correction (blocked on blob format)

- Generate a calibration blob for the fpv lens (poly or displacement map)
  with the SDK's generator (SDK hunt #3). Feed via `calibInfo`/`dispInfo`
  with the correct `mapType`.
- Validate on a grid/straight-edge target: lines straighten, corners not
  clipped unexpectedly, FOV cost characterized.
- Productize: `video0.ldc = { enable, blob path, mapType }` in
  `venc_config`, load blob from `/etc/…`, wire into `lensInit`/`lensAdj`.
  Add HTTP-API surface only if the config path proves stable.

## Integration surface (for the eventual implementation, not Phase 0)

- **Config:** new `video0.ldc` block in `include/venc_config.h` +
  `src/venc_config.c` (default disabled). Mirror existing optional-feature
  blocks; add a `test_venc_config.c` case.
- **Symbols:** add `MI_VPE_LDCSetViewConfig` (+ Beg/End if used) to the
  `star6e_mi.c` binding table only when Phase 1 needs runtime warp.
- **Backend split:** Star6E only for now. Maruko is the separate standalone-
  `MI_LDC` track (`specs/2026-07-09-maruko-ldc-gyro-dis/`); a Maruko stub may
  land per `AGENTS.md` dual-backend policy once Star6E validates.
- **No VERSION bump / HISTORY entry** for this spec — it is investigation
  docs only. The bump lands with Phase 2 productization.

## SDK findings (fill in when the full Star6E SDK is mounted)

> Populate from the requirements §"SDK reference hunt" checklist. Capture
> file paths and exact struct/enum text so the implementation can be
> one-shot.

- [ ] Vendor `mi_vpe.h` LDC struct — matches / differs from ours: …
- [ ] `mapType` enum values (DISPMAP=?, SENSORCALIB=?): …
- [ ] `proj3x3` fixed-point format + majorness + identity vector: …
- [ ] `MI_VPE_LDCSetViewConfig` prototype: …
- [ ] Is `configAddr` required for `WORKMODE_LDC`? demo evidence: …
- [ ] Example blob(s) / generator tool found (path + input format): …
- [ ] Resolution / bandwidth / scaler-compose constraints: …
