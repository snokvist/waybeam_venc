# EIS Gyro-Assisted LDC — Handoff for Next Session

**Branch:** `feature/mi-ldc-gyro-eis` (worktree: `waybeam_venc-gyro/`)
**Date:** 2026-05-19
**Status:** Phase 3a partial — daemon survives EIS=on, kernel does not produce frames.
**Not for merge:** infrastructure-only branch; encoder pipeline does not flow video yet.

## Where we got

Working / shipped:

- **Schema** (`eis.*` block) wired through all 7 venc touch points
  (`venc_config.h` struct, defaults, `load_eis`/`render_eis`, cJSON writer,
  dispatch, `g_fields[]`, `waybeam.default.json`).
- **Vendoring** (`vendor/sigmastar-eis/`):
  - `lib/libeptz.a` (173 KB, ARM, libc-only deps)
  - `include/mi_eptz.h`
  - No SDK source — policy preserved.
- **dlopen shim** (`star6e_mi.{h,c}`) gained four nullable
  `MI_VPE_LDC{Beg,Set,End,SetBatch}ViewConfig` slots; absent symbols don't
  break the no-EIS path.
- **EIS module** (`src/star6e_eis.c`, `src/star6e_eis_eptz.c`):
  - `star6e_eis_attach` populates `lensInit` (mode `DIS_GYRO`,
    `mapType=SENSORCALIB`, `configAddr`, `calibInfo`, inner `lensAdjOn=1`)
    plus outer channel-attr `lensAdjOn=1`.
  - `star6e_eis_push_view_config` calls
    `LDCBeg → LDCSet → LDCEnd ViewConfig` after channel start.
  - Eptz path: writes a tiny `.cfg`, `mi_eptz_config_parse`, override
    in/out sizes, `calib_type=1`, `mi_eptz_get_buffer_info` (~7 MB),
    `mi_eptz_runtime_init`, `mi_eptz_runtime_map_gen` → returns
    `(bin, bin_size)` in one shot.
- **Build:** both backends clean. Star6E binary 279 KB, Maruko 193 KB.
  `eis.enabled=false` produces zero regression vs master.

## Where we stopped

`feature/mi-ldc-gyro-eis @ HEAD` on device 192.168.1.13 with `eis.enabled=true`
runs cleanly through:

1. calib load (15140 B `CalibPoly_new.bin`)
2. `mi_eptz_runtime_init` → handle returned
3. `mi_eptz_runtime_map_gen` → bin returned (~52 KB, exact size varies)
4. `MI_VPE_LDCBeg/Set/End ViewConfig(chn=0)` → all return 0
5. `MI_VPE_StartChannel`, encoder armed

…and then HTTP `/cmd/start` answers, but downstream the encoder loops
`waiting for encoder data...` indefinitely. VPE→SCL→VENC is not producing
frames. dmesg tail showed only `[HAL_SCL] Clock Idx ... Val ...` and
`[check_clk_store] Set clock [384000000]` — no kernel error from MI_VPE.

So Phase 3a smoke test result is: **boots clean, blob is accepted by kernel
syscalls, but the resulting LDC view does not produce output**.

## Likely culprits to investigate next session

In rough order of suspicion:

1. **Bypass mode chosen** — we ask eptz for `LDC_MODE_1O` with identity
   m33 and zoom=1. That mode may always require a real DIS pipeline. Try
   `LDC_MODE_1O` with `bypassOn=1` on `lensInit`, or compile with one of
   the modes the bundled `eptz_demo` actually used (the bundle has
   examples that did produce stream — pull args from there).
2. **focalLengthX/Y units** — we default to 204137 (the bundle's value);
   may need recomputation for our 1920×1080 capture vs. their original
   resolution. Bundle `ldc_config.json` lists matched values, but they're
   in unknown units (kernel comment says they're sensor-cooked).
3. **Slice count** — we set 1; some kernel paths reject anything except
   `userSliceNum >= 4` for HEVC.
4. **`calibInfo.calibPolyBinAddr/Size`** — we point at the raw calib file
   bytes. The kernel may want a different envelope (the libeptz blob
   already embeds the calib; calibInfo may be redundant or conflicting).
   Try clearing `calibInfo` and rely only on `configAddr`.
5. **Channel ordering** — `LDCSetViewConfig` runs *after* CreateChannel +
   SetChannelParam but *before* StartChannel. Bundle docs suggest it
   should be between Set and Start. Verify ordering in `star6e_pipeline.c`.
6. **`mi_eptz_runtime_map_gen` vs `runtime_bin_gen`** — we switched
   because `bin_gen` SEGVd. `map_gen` may produce a "view map" blob (used
   for projection lookup) rather than a "view config" blob (used by
   LDCSetViewConfig). The libeptz header distinguishes them but our
   header doesn't make ownership/lifetime clear. Worth a second pass on
   `mi_eptz.h` and a try of `bin_gen` with proper para init (the SEGV
   may have been an uninitialized field, not a fundamental
   incompatibility).

## Device state at end of session

- `192.168.1.13:/etc/waybeam.json` — `eis.enabled=true`, `sensor.mode=3`,
  pointing at `/etc/waybeam/eis/calib.bin`.
- `/etc/waybeam/eis/` on device:
  - `calib.bin` (15140 B, copy of bundle's `CalibPoly_new.bin`)
  - `ldc_config.json` (5618 B, currently unused by our path)
  - `Cali_LDCpoly.bin` → symlink → `calib.bin`
  - `CalibPoly_new.bin` → symlink → `calib.bin` (libeptz opens this when
    `calib_type=1`)

## To resume

```bash
cd /home/snokvist/dev/waybeam-coordination/waybeam_venc-gyro
git status   # branch feature/mi-ldc-gyro-eis, all changes uncommitted
make build
scripts/star6e_direct_deploy.sh 192.168.1.13
# Then: enable on device, restart waybeam, watch dmesg + journalctl.
```

For PR: title `eis: gyro-assisted LDC stabilization scaffolding (WIP)`;
explicitly DO NOT merge — leave `eis.enabled=false` default, mark as
"Phase 3a smoke" and reference this file.

## Files touched

Modified:
- `Makefile` (vendor link + new src files)
- `config/waybeam.default.json` (eis block)
- `include/star6e.h` (LDC view-config macros)
- `include/star6e_mi.h` (vtable slots)
- `include/venc_config.h` (`VencConfigEis`)
- `src/star6e_mi.c` (LOAD_SYM)
- `src/star6e_pipeline.c` (attach + push hookpoint, eis param)
- `src/venc_api.c` (`g_fields[]`)
- `src/venc_config.c` (defaults/load/render/cJSON)

New:
- `documentation/EIS_GYRO_LDC_SPEC.md` (Phase 1 spec)
- `documentation/EIS_HANDOFF.md` (this file)
- `include/star6e_eis.h`, `include/star6e_eis_eptz.h`
- `src/star6e_eis.c`, `src/star6e_eis_eptz.c`
- `vendor/sigmastar-eis/include/mi_eptz.h`
- `vendor/sigmastar-eis/lib/libeptz.a`

Dead-end paths already eliminated (do not reopen unless you have new evidence):

- **Path A — kernel `mi_gyro.ko`**: hardcoded to I2C-0 ICG-20660. Our hardware
  is I2C-1 BMI270 (WHO_AM_I lives at different register). No module param to
  override. Dead.
- **`mi_eptz_config_set`**: returned `0x20c` (PARSE_LINE_BUFFER); it's
  post-parse enrichment, not a parse-replacement. Skipped.
- **`calib_type=0`**: expects three poly bins (rd/ru/+one). We only have a
  single poly — must use `calib_type=1`.
- **Identity m33 alone**: doesn't make `bin_gen` SEGV go away.

## Safety / policy notes

- We vendor binary `libeptz.a` and header `mi_eptz.h` only. No SDK source.
- Device-side `calib.bin` is from the bundle and must NOT be checked in;
  it's deployment payload (staged at `/etc/waybeam/eis/`).
- BMI270 IMU at I2C-1 0x68 is still untouched on this branch — Phase 3b
  picks that up only after the LDC frame flow is proven.
