# Maruko stab — implementation plan

Read `requirements.md` first. Phase 1 is a **go/no-go gate**: do not write the
framing module until 1b passes.

## Architecture: what transfers and what doesn't

`FramingModule` (`include/star6e_framing.h:14-34`) is **genuinely SoC-agnostic** —
nothing in the vtable names VPE/SCL/`MI_*`:

```
preset_name; enabled(vcfg); prepare(vcfg,src_w,src_h,*enc_w,*enc_h);
setup_ports(Star6ePipelineState*, src_fps, dst_fps); start(); stop();
apply_ae_crop(); set_pan(x,y); active(); set_live(key,val);
```

Registry lives in `src/star6e_pipeline.c:60-88` (not a separate framing.c):
`g_framing_registry[FRAMING_MAX=4]`, `star6e_framing_register()` `:65-69`,
`star6e_framing_select()` `:71-78` (first `enabled()` wins),
`star6e_framing_register_builtins()` `:80-88` under `#if HAVE_FRAMING_STAB`.

Pipeline drive sites (all `src/star6e_pipeline.c`):

| Site | Line | Call |
|---|---|---|
| init registry | 2360 | `star6e_framing_register_builtins()` |
| select + geometry | 2402-2409 | `select()` → `prepare()` overwrites `pconf.image_width/height` before VENC create |
| select + bind | 1954-1987 | `setup_ports()` → `start()` → on fail `stop()` → `apply_ae_crop()` |
| live pause | 1002-1003 | `set_live("pause", …)` |
| zoom override | 1018-1020 | if `active()`, force `set_pan(0.5,0.5)`, swallow zoom |
| dual-record gate | 2491 | `active()` ⇒ downgrade dual→single channel |
| teardown | 2276-2278 | `stop()`, `g_framing=NULL` |

`Star6ePipelineState` leaks into exactly one place — `setup_ports`'s first arg
(`star6e_framing.h:25`) — and the module touches only 4 fields
(`src/star6e_framing_stab.c:1473-1478`, `1543-1551`, `1610-1620`):
`state->venc_port`, `state->vpe_port`, `state->bound_vpe_venc`,
`state->active_precrop.{w,h}`. Plus two host callbacks
(`include/star6e_framing_host.h:21,25`).

Maruko already has the analogues: `MarukoAeCropRect` + `maruko_apply_ae_crop()`
(`src/maruko_pipeline.c:105-108`, `1440-1447`).

### Compile-out

`Makefile:40-46` (`STAB ?= 1`, `STAR6E_ONLY_SRC += src/star6e_framing_stab.c`) and
`Makefile:72-75` (`SOC_DEFS += -DHAVE_FRAMING_STAB=1`). `MARUKO_ONLY_SRC`
(`Makefile:57`) has no stab entry; maruko `SOC_DEFS` (`:60`) never sets the define.
Note maruko `SOC_CFLAGS` is **empty** (`Makefile:63`) while star6e gets
`-mfpu=neon-vfpv4 -mfloat-abi=hard -ftree-vectorize` (`:71`). Irrelevant for the
detector (NEON lives inside `libmi_ive.so`), relevant for any pure-C fallback.

## The three risks that shape everything

**R4 — pivotal unknown: `MI_SCL_SetOutputPortParam` at sensor fps.**
Star6E emits with a lightweight input-domain rect poke,
`MI_VPE_SetPortCrop(0,0,&rect)` (`star6e_framing_stab.c:572`). **Maruko has no
`MI_SCL_SetPortCrop`** — `fnSetPortConfig` (dlsym'd `src/maruko_mi.c:389` from
`MI_SCL_SetOutputPortParam`) rewrites the *whole* `i6c_scl_port` struct
(`include/maruko_bindings.h:37-43`). Which is exactly why the pan ramp is capped:

```
maruko_pipeline.c:113-120
 * MI_SCL_SetPortConfig is heavier than Star6E's MI_VPE_SetPortCrop
 * (writes a full port struct, not just a rect), so we drive the ramp
 * at 30 Hz instead of 60 Hz to keep per-tick cost bounded.
#define MARUKO_PAN_RAMP_TICK_MS  33   /* ~30 Hz */
```

Stab needs an emit **every frame** (`STAB_DETECT_EVERY == 1`;
`star6e_framing_stab.c:210-218` explicitly warns that decimating aliases real
jitter (Nyquist) and produces visible fighting). Sensor fps on Maruko IMX415 is
60/90/100/144.

**R2 — the SCL source-crop is the ONLY crop stage, and it's already occupied.**
`maruko_pipeline.c:786-798` enumerates the three blocked alternatives (VIF
sub-window unsupported on I6C; ISP output-port crop stalls at high res;
`MI_SCL_SetInputPortCrop` refuses the REALTIME ISP→SCL edge with `-1610014712`).
The one rect already carries AR precrop + zoom window (`:824-837`, `:1473-1487`).
Stab must fold `acc_x/acc_y` into that same rect. Mitigation: stab forces pan to
centre anyway (`star6e_pipeline.c:1018-1020`), so **stop the pan-ramp thread while
stab is active** (`maruko_pan_ramp_stop()`) and make stab the sole writer.

**R1 — port0 output is IFC-compressed** (`maruko_pipeline.c:859`
`scl_port.compress = 6`), required for the `I6_SYS_LINK_RING` bind to VENC
(`:848-849`, `:2273`). `stab-fill` manually drains port0 and reads raw NV12 →
**not portable as written**. Plain `stab` never CPU-reads port0, so it's unaffected.

## Maruko emit + tap surface

| Function | Line | Role |
|---|---|---|
| `maruko_pan_apply_locked(ctx,pct,x,y)` | 1453-1503 | the emit path; `fnSetPortConfig(0,0,0,&scl_port)` `:1488` |
| `maruko_pan_ramp_thread` | 1505-1578 | 30 Hz tween |
| `maruko_pipeline_apply_zoom(ctx,pct,x,y)` | 1649-1700 | public entry (`include/maruko_pipeline.h:129`) |

`maruko_pan_apply_locked` is `static`, takes normalized `x,y`, and always re-derives
the rect via `maruko_compute_zoom_rect()` — which **cannot express a pixel-offset
rect**. Add a sibling `maruko_scl_apply_crop_rect(ctx, x, y, w, h)` (~20 lines,
calls `fnSetPortConfig` directly). Do **not** reuse the ramp thread; the detector
thread is already the 1:1-per-frame clock.

SCL ports (`maruko_setup_scl`, `:744-942`; device created with `scl_bind = 0xF`, all
4 HW ports enabled, `:752-754`):

| Port | Bound to | Format |
|---|---|---|
| 0 | VENC ch0, `BindChnPort2(..., I6_SYS_LINK_RING, 0)` `:2273` | YUV420SP, **compress=6 (IFC)** `:859` |
| 1 | MJPG snapshot VENC, FRAMEBASE @5fps (`src/maruko_jpeg.c:144`) | YUV420SP, **compress=0 (raw)** `:905` |
| 2 | **free** | — |
| 3 | **free** | — |

**Detector tap plan:** SCL port **2**, 384×384, `compress=0`,
`MI_SCL_EnableOutputPort(0,0,2)`, drain via `MI_SYS_ChnOutputPortGetBuf` on
`{I6_SYS_MOD_SCL,0,0,2}` — the structural analogue of Star6E's VPE port1
(`star6e_framing_stab.c:1571-1605`).

## Detector I/O contract (from Star6E)

- Y patch is **zero-copy**: `star6e_stab_make_center_y_crop()` `:585-620` offsets the
  drained buffer's own `pVirAddr[0]`/`phyAddr[0]` by `crop_y*stride + crop_x`
  (`:614-617`). Crop 16-aligned in x, 2-aligned in y (`:603-608`).
- dx/dy dests: `star6e_stab_alloc_ive_image()` `:622-647`, 1×1 S8C1,
  `posix_memalign(64)` + `MI_SYS_Va2Pa`, with the comment: *"IVE needs a valid
  physical address even for 1×1 S8C1 result images; leaving aphyPhyAddr[0]=0 makes
  Shift_Detector return zero dx/dy"* (`:640-645`). On i6c our probe used
  `MI_SYS_MMA_Alloc` + `MI_SYS_Mmap`; **confirm whether posix_memalign+Va2Pa also
  works** (the detector is software/NEON, so VA is what it reads — but the phy!=0
  guard may still apply).
- Results read after `MI_SYS_FlushInvCache` on each (`:1132-1135`), signs
  `STAB_SHIFT_SIGN_{X,Y} = -1`.
- Thread: `star6e_stab_thread_main()` `:1139-1455`. `select()` on `MI_SYS_GetFd`
  (50 ms) → `GetBuf` `:1248` → centre crop `:1263` → decimation `:1312-1313` →
  `estimate_shift()` `:1319` → Kalman `:1322-1379` → emit `:1401-1415` → rotate
  prev/curr `:1416-1422`.

`libmi_sys.so` symbols dlsym'd at `star6e_stab_load_sys_extra_symbols()` `:340-380`:

| Symbol | plain `stab`? | on i6c? |
|---|---|---|
| `MI_SYS_ChnOutputPortGetBuf` / `PutBuf` | **required** | ✅ |
| `MI_SYS_FlushInvCache` | **required** | ✅ |
| `MI_SYS_Va2Pa` | **required** | ✅ |
| `MI_SYS_GetFd` / `CloseFd` | optional (falls back to `usleep(1000)` poll `:1252`) | ✅ |
| `MI_SYS_ChnInputPortGetBuf` / `PutBuf` | **stab-fill only** | ✅ |
| `MI_SYS_BufBlitPa` / `MI_SYS_BufFillPa` | **stab-fill only** | ✅ |

**All 10 exist on i6c** — symbol availability is not a blocker for either preset.
The blocker for stab-fill is R1 (IFC), not the API surface.

**Plain `stab` needs no Blit/Fill.** It is `GetBuf → IVE → SetPortCrop → PutBuf`.
Dropping stab-fill drops the entire `star6e_framing_stab.c:649-1051` block plus
`setup_ports_fill` `:1464-1509`.

## Phases

### Phase 1 — Bench the unknowns (GATE). Half a day. No module code.
- **1a.** Enable free SCL port 2 at 384×384 `compress=0`; drain-and-discard.
  *Gate:* GetBuf cadence == sensor fps; CPU cost acceptable.
- **1b.** Hammer `fnSetPortConfig` at sensor fps with dedup'd rect-only deltas.
  *Gate:* no ISP P0 FIFO stall, no scaler renegotiation, no frame drops.
  **If this fails, stab in this shape is dead on Maruko. Stop and reconsider.**
- **1c.** Determine SCL `crop.x/y` granularity (2-px vs 16-px). `i6c_scl_port.crop`
  is `i6_common_rect` of `unsigned short`; existing code assumes 2-px even rounding
  (`maruko_pipeline.c:809`). Star6E's `SetPortCrop` precedent does not transfer.
- **1d.** Measure achievable fps with the detector thread running, full vs cheapened
  config. Feeds the product decision on default fps.

Mitigation ladder if 1b is marginal: (a) dedup — skip the write when the rect is
unchanged; (b) raise `STAB_DETECT_EVERY` (against the author's explicit warning);
(c) `stab-fill` (no SCL poke) — blocked by R1.

#### Phase 1 RESULTS — GATE PASSED (2026-07-08, device .233, IMX415 1080×720@~50fps)

Benched via an in-venc, env-gated probe (`src/maruko_stab_bench.c`, built with
`STAB_BENCH=1`, run with `WAYBEAM_STAB_BENCH=1a|1b|1c|1d`). It lives in-process
because a second process cannot attach to MI_SCL's per-process channel state.
Note: config fps is 144 but the sensor actually delivers ~50fps.

- **1a — PASS.** Port-2 tap (384×384 raw, `compress=0`) delivered **49.4 fps**
  (149 frames / 3.01 s), intervals mean/min/max **20.1 / 17.7 / 21.2 ms** — tracks
  the real source rate, no gaps. Drain-loop CPU ≈ 20% of one core.
  **Gotcha found:** an *unbound* SCL output port produces **0 frames** for GetBuf
  until `MI_SYS_SetChnOutputPortDepth(0, &port, 2, 4)` gives it a user-frame queue
  (i6c ABI has a leading `u16 soc_id`; mirrors `star6e_framing_stab.c:1624`). The
  Phase-3 `maruko_framing_stab.c` MUST call this after `EnableOutputPort(2)`.
- **1b — PASS (the pivotal gate).** Hammered port-0 `SetOutputPortParam` (full
  struct, ±1px crop.x) at 30/60/90/144 Hz for 4 s each. Every rung: **errs=0**,
  per-call latency **~0.020 ms mean** (max 0.035–0.049 ms; a lone 0.773 ms blip at
  144 Hz), and the SCL processed a clean **~50 fps THROUGHOUT** (199–200 frames per
  4 s window, read from `/proc/mi_modules/mi_scl/mi_scl0` `TotalKickoff`). Host RTP
  on :5600 held steady ~1200 pkts/s across all rungs — no dropout. **No** FIFO
  stall / scaler renegotiation / MMU storm / reinit. **The 30 Hz pan-ramp cap
  (`MARUKO_PAN_RAMP_TICK_MS`) was conservative, not a hardware limit** — stab can
  emit every frame. R4 is retired.
- **1c — partial.** `SetOutputPortParam` on port 2 with **odd** `crop.x`
  (base+1, base+3) returned **0** — no API-level 2-px rejection. Whether the
  scaler honours a 1-px offset or silently rounds is NOT yet verified (needs a
  drained-frame content correlation); keep the 2-px-even assumption until proven.
- **1d — PASS.** Full loop (tap → `MI_IVE_Shift_Detector` → port-0 emit) ran
  **44.2 fps sustained** (266 frames / 6.0 s), **265/265 emits** applied with
  ret=0, detector **17.9 ms CPU / 21.8 ms wall** per call. No MMU/watchdog/FIFO
  warnings; clean teardown across 5 SIGTERM cycles. At the source ~50 fps,
  detect-every-frame nets ~44 fps stabilized (matches the cost model; the 144 fps
  config needs the documented fps downgrade).

**Verdict: proceed to Phase 2.** stab-in-this-shape is viable on Maruko; no
mitigation-ladder fallback needed. Carry two facts into Phase 3: the port-depth
call (1a) and the still-open 1-px-vs-2-px content granularity (1c).

### Phase 2 — Extract the Kalman
`src/star6e_framing_stab.c`: constants `:230-237`, globals `:311-320`,
seed/validate `:458-481`, filter+pause-glide+clamp `:1322-1379` (core predict/update
math is only `:1364-1373`, 10 lines). Pure math — no `MI_*` calls. Non-pure deps:
`<math.h>` (`fabs`, `lround`, `isfinite`, already `-lm`), module globals
(`g_stab_paused`, `g_stab_recenter_period`, `g_stab_fill_mode`, `g_stab_src_*`,
`g_stab_enc_*`, `g_stab_crop_percent`), `star6e_stab_max_off_x/y()` `:501-513`, and a
debug `fprintf` `:1381-1390`.

Extract to `framing_kalman.{c,h}` in `HELPER_SRC` (~70 lines), signature ≈
`kalman_step(KalmanState*, double meas_dx, double meas_dy, int paused, uint32_t tau,
int max_x, int max_y, int *acc_x, int *acc_y)`. Refactor Star6E onto it first.
*Gate:* Star6E device test, no behaviour change.

> Repo convention favours parallel `star6e_*`/`maruko_*` files, but the Kalman is
> the one genuinely SDK-agnostic block and already carries the comment "the SINGLE
> control law for both" (`:220-229`). Its 4 tuning constants must not drift. This is
> the justified exception.

### Phase 3 — `maruko_framing_stab.c` (`stab` only)
- `include/maruko_framing.h` (same vtable, `setup_ports(MarukoBackendContext*, …)`).
- `include/maruko_framing_host.h` — needs a **rect-taking** `maruko_emit_ae_crop()`;
  today `maruko_apply_ae_crop(ctx,pct,x,y)` is `static`, derives the rect internally,
  has a dedup short-circuit on `crop_w` (`:1440-1447`) and a
  `g_maruko_ae_crop_disabled` latch (`:1432`). (R8)
- Registry (~25 lines) into `src/maruko_pipeline.c`, mirroring `star6e_pipeline.c:60-88`.
- 7 drive sites mirroring the table above. `maruko_pipeline_apply_zoom()` `:1649-1700`
  needs the `active()` → `set_pan(0.5,0.5)` short-circuit.
- `maruko_scl_apply_crop_rect()` as the emit.
- `ctx` stashed in module globals (borrowed, cleared on stop) — same pattern as
  `g_maruko_pan_ramp.ctx` (`:139` "borrowed; cleared on ramp_stop"). (R9)
- `Makefile`: `MARUKO_ONLY_SRC += src/maruko_framing_stab.c` under `STAB`, and
  `-DHAVE_FRAMING_STAB=1` in the maruko `SOC_DEFS` branch.
- **R6 — teardown ordering is load-bearing.** `star6e_stab_stop()` `:1771-1825`,
  comment `:1774-1787`: **`pthread_join` the detector BEFORE disabling the tap
  port**, else `_MI_SYS_MMU_Callback Status=0x2` storms → hardware watchdog reset.
  Maruko is *more* reinit-fragile than Star6E. Replicate exactly. Never `killall -9`.
- Stop the pan-ramp thread while stab is active (R2).

### Phase 4 — Un-gate schema + WebUI (8 touches)

Maruko currently rejects the knobs in **two** places, both must be reversed:

1. `src/venc_api.c:599-605` — `venc_api_field_supported_for_backend()` returns 0 for
   `maruko` on `video0.{stab_crop_pct,stab_recenter_speed,stab_kalman_q,stab_kalman_r,pause_stab}`.
   This is PR#165's `supported:false`. It feeds `/api/v1/fields` `:2236-2238`,
   single-set 501 `:1932-1936`, multi-set `:1667-1672`.
2. `web/dashboard.html:1049-1053` — disables the `stab`/`stab-fill` enum *options*
   when `backendName==='maruko'` (`backendName` from `/api/v1/version`, `:565`);
   N/A label `:1056`. Tooltips `:619`, `:621` also assert Star6E-only.

Note `video0.framing` itself is **not** backend-gated (`venc_api.c:792-796` accepts
both presets on both backends today; it just expands into a `stab_crop_pct` nobody
consumes on Maruko).

The 7-touch schema surface:

| # | Touch | File:line |
|---|---|---|
| 1 | struct fields | `include/venc_config.h:156-181` |
| 2 | defaults | `src/venc_config.c:185-187` |
| 3 | preset table | `src/venc_config.c:446-511` (`:488` stab, `:489` stab-fill). **Comment `:465` "Star6E only; no-op on Maruko" must be updated.** |
| 4 | JSON load | `src/venc_config.c:584-620` |
| 5 | JSON render/pp | `src/venc_config.c:1188`, `:1386` |
| 6 | `g_fields[]` + FieldUi | `src/venc_api.c:476`, `483-486`, `490`; UI descriptors `:338,346,354,362,374` — **all five carry "Maruko/I6C: unavailable — stabilization needs the IVE block, absent on…" at `:343,351,359,367,379`; strip them.** |
| 7 | aliases | `src/venc_api.c:544-548` |

Validators `src/venc_api.c:792-822`: framing enum `:792-797`;
`stab_crop_pct ∈ {0} ∪ [60,100]` `:799-805`; `stab_recenter_speed ∈ [0,3600]` `:806-810`;
`stab_kalman_q ∈ [0.001,1.0]` `:811-816`; `stab_kalman_r ∈ [0.1,50.0]` `:817-822`.
Live-only list `:898-912` includes `video0.framing`. `pause_stab` not-persisted skip `:1207`.

**8th touch — live-apply plumbing.** `venc_api.h:83` `apply_pause_stab`,
`venc_api.c:1399` capability probe, `:1591` dispatch, `:1473` copy-live.
`src/maruko_controls.c:1120-1143` vtable has **no** `.apply_pause_stab` — add it,
wired to a new `maruko_pipeline_set_pause_stab()` mirroring
`star6e_pipeline.c:1000-1005` / `star6e_controls.c:277-280`.

### Phase 5 — `stab-fill`: DEFER
Strictly larger on Maruko than on Star6E. Requires dropping port0 to `compress=0`
and re-binding FRAMEBASE (as `maruko_jpeg` does for port1), losing the RING
zero-copy path. Only attempt if Phase 1b fails *and* stabilization is still wanted.

stab-fill-only surface (for reference): helpers `star6e_framing_stab.c:649-1051`
(`uv_pa/uv_va` `:655-669`, `make_i8_plane` `:672-682`, `fill_i8_rect` `:687-731`,
`blit_i8_rect` `:734-790`, `copy_y_to_sw` `:796-833`, `send_frame_to_venc_fill`
`:840-983`, `blit_thread_main` `:987-1027`, `fill_queue_blit` `:1030-1051`),
`setup_ports_fill` `:1464-1509`, second module struct `:2015-2026`, blit thread at
`SCHED_FIFO VENC_RT_PRIO` `:1718-1735`.

## Corrections to prior notes

- **NOT three blobs.** Only `libmi_ive.so` is swapped. The BSP's `libmi_sys` /
  `libmi_common` must NOT be imported (segfault on musl). `vendor-libs/maruko/` does
  not need `libmi_ive.so` — venc dlopens it from `/usr/lib`, which the image now ships.
- **There is no `rwlock` in the venc source** (`grep -rn rwlock src/ include/` → 0 hits).
  The "maruko pipeline rwlock serialises HTTP under reinit" note is void as stated.
  Only `g_maruko_pan_ramp.lock` (`:128`) and `g_zoom_status_mutex` (`:1277`) exist.
  The HTTP-hang-under-reinit behaviour may still exist via another mechanism —
  re-derive before relying on it.
