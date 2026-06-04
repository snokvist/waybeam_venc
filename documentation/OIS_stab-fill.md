# stab-fill — net functional changes since b57c4d0

This document summarizes the net difference in stabilization functionality
between commit `b57c4d06ce0d50abf0c3845cb42e5d19edd69682` and the current
working tree on the `stab_varzoom` branch. It is a "what is different" view,
not a commit-by-commit log — several intermediate experiments (stab-crop,
stab-var, SetPortShowPosition HW-fill, DIVP StretchBuf) were tried and
dropped, and are not described here.

## At b57c4d0 (baseline)

- Only `framing="stab"` existed (the crop+shrink preset: source clamped to
  ≤1920×1080, encoded resolution shrinks by `stabCropPct`).
- EMA smoothing + gated recenter logic.
- Single-thread detector: `MI_IVE_Shift_Detector` followed by a per-frame
  `MI_SYS_BufBlitPa` of the shifted crop into the VENC input buffer.
- All Kalman/feel tuning was compile-time only.
- No way to disable stabilization at runtime without restarting the pipeline.

## At HEAD

### New preset: `framing="stab-fill"`

"Floating image" mode. The source is the **full sensor** frame; VPE port0
SCL-downscales it to the configured encode resolution (e.g. 2560×1920 →
1920×1080). The detector measures motion in image domain and the per-frame
CPU compose shifts a window inside the encoder frame, black-filling the
exposed edge proportional to the shift. The output stays at the configured
encode resolution — no source clamp, no aspect surprise. `stabCropPct`
bounds the max shift / black-border budget (`max_off = src*(100−pct)/200`;
default 80 → ≤10 % border).

### Inside stab-fill

| Subsystem | Implementation |
|---|---|
| **CPU compose** | Strip-only: four `MI_SYS_BufFillPa` calls paint the exposed Y/UV strips (Y=16, UV=128, byte-replicated 32-bit constants `0x10101010` / `0x80808080`), then two `MI_SYS_BufBlitPa` calls place the in-bounds source content (Y plane + UV plane) at the shifted sub-rect of a fresh VENC input buffer. No full-buffer memset, no cache flush. |
| **Trajectory smoothing** | Kalman filter ported from `ejo_wfb_stabilizer.py` — Q=0.03 (processVar), R=2.0 (measVar), steady-state gain K≈0.11. Per axis: `P_predict = P + Q`, `K = P_predict/(P_predict + R)`, `X_estimate += K*(facc − X_estimate)`, `acc = round(facc − X_estimate)`. Slow camera pans pass through (X_estimate tracks → compensation ≈ 0), fast shake is held still (X_estimate lags → compensation ≈ facc). Replaces the EMA + recenter logic; `stab` preset is unchanged and still uses EMA. |
| **Threaded blit + sw_detect ring** | Detector copies the Y centre crop into one of two `sw_detect[2]` posix-aligned 384×384 IVE-image slots, re-points `curr_img` at the sw copy, then queues `(buf, handle, acc)` for a dedicated blit thread via a single-slot queue (mutex + 2 condvars). Handle ownership transfers through the slot: the blit thread calls `put_buf` after the blit completes, so port0 buffers release as soon as their bytes are consumed. IVE and `BufBlitPa` overlap; on dual-core SoCs cycle drops from `IVE + blit` to `max(IVE, blit)`. Falls back to inline blit if the thread spawn fails. |
| **MMU-safe teardown** | Stop sequence sets `g_stab_running=0`, broadcasts both blit condvars, joins the detector thread first (so no further `get_buf` calls), then joins the blit thread. Same barrier discipline as the existing `stab` mode. |

### New live-tunable HTTP fields

| Field | Mutability | Persisted | Purpose |
|---|---|---|---|
| `video0.stabKalmanQ` | live | yes | Kalman process noise. Default 0.03. Higher = trust measurement more = output follows camera quickly = less stabilization, lower lag on intentional pans. Lower = stronger stabilization, more lag. |
| `video0.stabKalmanR` | live | yes | Kalman measurement noise. Default 2.0. Higher = distrust measurement = X_estimate updates slowly = more stabilization. Lower = less stabilization. |
| `video0.pauseStab` | live | **no** | true = stops the detector + blit threads and hardware-binds port0 directly to VENC (zero-copy, full sensor rate, no shake compensation). false = unbind, respawn detector + blit threads (returns to stab-fill compose). Each transition costs ~20-30 ms (one dropped frame). |

`stabKalmanQ` / `stabKalmanR` are read once per detector tick under a small
mutex; `star6e_pipeline_set_kalman()` writes under the same mutex. No
pipeline reconfigure on change.

`pauseStab` orchestrates a real path switch:

- **pause=true**: stop detector + blit threads → drain leftover port0
  buffers → `MI_VENC_StopRecvPic` → `MI_SYS_BindChnPort2(port0 → VENC,
  FRAMEBASE)` → `MI_VENC_StartRecvPic` → done.
- **pause=false**: `MI_VENC_StopRecvPic` → `MI_SYS_UnBindChnPort` →
  `MI_VENC_StartRecvPic` → respawn detector + blit threads → done.

Every SDK call return code and microsecond timing is logged under
`[waybeam] stab-fill:` for inspection. If any SDK call mid-transition
fails the function restores the detector + blit threads so the stream stays
up and returns -1.

### Per-tick timing instrumentation

`[waybeam] stab time: ive=Xus send=Yus (mode=fill)` logged once every 30
detector ticks, gated on `system.verbose`. Used to spot whether IVE or
compose is the cycle's bottleneck on a given hardware target.

## Old `framing="stab"` preset

Unchanged. Same crop+shrink path, same EMA, same `stabCropPct=80
stabRecenterSpeed=180` default, same advanced feel knobs (`stabSmoothPct`,
`stabStillFrames`, `stabEdgePct`, `stabMotionThresh`). The framing preset
table is byte-identical except for the added `stab-fill` row.

## Bench-validated results

Star6E `ssc30kq`, sensor mode 2560×1920@60, framing=stab-fill:

| State | Steady-state fps |
|---|---|
| Active stab-fill compose | ~43 fps (capped by 2×`BufBlitPa` ≈ 20 ms DRAM bandwidth) |
| Paused (`pauseStab=true`, HW-bind) | ~58-59 fps (sensor ceiling) |

All bind/unbind transitions return `ret=0x0`. Two complete pause/resume
cycles tested, waybeam never restarted. Idempotent across cycles.

## API examples

```bash
# Tune the Kalman feel
curl 'http://<HOST>/api/v1/set?video0.stabKalmanQ=0.05'
curl 'http://<HOST>/api/v1/set?video0.stabKalmanR=4.0'

# Free 60 fps, no shake compensation
curl 'http://<HOST>/api/v1/set?video0.pauseStab=true'

# Back to stab-fill compose
curl 'http://<HOST>/api/v1/set?video0.pauseStab=false'
```

## Code surface

- `src/star6e_pipeline.c`: +1196 lines (helpers, Kalman state, threading,
  pauseStab orchestration, MMU-safe lifecycle).
- All other source files: small additions only — FIELD entries, alias rows,
  `copy_live_group_fields` cases, callback registrations.
- Docs: HISTORY 0.13.0 entry, EIS_INTEGRATION_PLAN note,
  HTTP_API_CONTRACT, STABILIZATION_TEST_PLAN T7 added.
- VERSION 0.12.0 → 0.13.0.
- WebUI tooltips + SECTIONS entries for the new fields; gzip blob
  regenerated with `make webui`.
