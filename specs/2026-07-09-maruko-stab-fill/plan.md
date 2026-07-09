# Plan — Maruko `stab-fill` via module-bind compose

See `requirements.md` for goal, background (Phase 5a), and reuse. This plan is
gated on **Phase F0** (the one remaining unknown: does compose→module→VENC
frame-base encode in *our* pipeline, and at what CPU cost on the single A7).

## Reference points

- Star6E compose: `star6e_framing_stab.c:816-959` (`send_frame_to_venc_fill`) —
  shift the in-bounds content rect, black-fill the four border rects (Y=16,
  UV=128), via `BufBlitPa`/`BufFillPa` on the destination buffer. The *math*
  ports verbatim; only the destination (a module input buffer, not VENC) and
  the fill/blit calls (i6c symbol names) change.
- SDK inject→bind: `.../interface/uvc/module_uvc.cpp` — `MI_SYS_ChnInputPortGetBuf/
  PutBuf` into SCL input; `stBindInfo[1]` = `SCL→VENC` `E_MI_SYS_BIND_TYPE_FRAME_BASE`;
  VENC `E_MI_VENC_INPUT_MODE_NORMAL_FRMBASE`.
- SDK SCL manual input: `.../mid/maruko_impl/scl/mid_scl_impl.cpp:662,703`.
- Maruko VENC create + current RING bind: `maruko_pipeline.c:~2000` (create dev/chn,
  `SetInputSourceConfig(RING_DMA)`), RING bind `SCL→VENC` at `:~2391`.
- i6c MI_SYS input ABI (device-proven): `maruko_framing_stab.c` StabFrame_t /
  StabBufInfo_t; conf layout in `maruko_stabfill_probe.c` (PrbBufConf_t).

## Phase F0 — DE-RISKING BENCH (go/no-go, do FIRST, like 5a)

**Question:** in our pipeline, does a composed frame injected into a bridge
module and FRAME_BASE-bound to VENC actually encode, and is the single-core CPU
compose cost acceptable at 50 fps?

- Extend the existing env-gated probe harness (`maruko_stabfill_probe.c` style).
- **F0a (primary, SCL-inject):** create a 2nd SCL channel; set its input to
  manual (pushable); FRAME_BASE-bind `SCL2→VENC(chnN, NORMAL_FRMBASE)`; push
  ~30 hand-composed gray+border frames into SCL2 input; confirm VENC emits
  (`Query.curPacks>0` / `GetStream`). Measure per-frame compose+push CPU.
- **F0b (fallback, DIVP):** only if F0a's compose cost pegs the core — bench
  `SCL→DIVP→VENC` with DIVP doing the offset-blit-into-black-canvas; confirm
  DIVP supports it and that it encodes.
- **Decision gate:** pick A or B (or, if neither encodes in our pipeline,
  document stab-fill as i6c-infeasible and STOP — ship stab-only). Record
  numbers here, like the Phase 1 / 5a RESULTS blocks.

### Phase F0a RESULTS (device `.233`, SSC378QE, 2026-07-09)

Env-gated `MARUKO_STABFILL_F0A` bench (`maruko_stabfill_probe.c
:maruko_stabfill_f0a_run`, hooked post-`bind_maruko_pipeline`). Three iterations:

1. **SCL-inject topology stands up in our binary.** A 2nd SCL channel (dev 0,
   chn 1, no upstream bind) created + started + output-port(IFC 1080×720)
   configured + enabled — **all `ret=0`**. So SCL manual-input (the
   `PutStreamToSclInputPort` primitive) is available to us.
2. **VENC channel ceiling = 3.** `MI_VENC_MAX_CHN_NUM_PER_DC = 3` → valid H26x
   channels are **0,1,2 only**; CreateChn(0,6) rejected (SDK returns raw `31`,
   not an `MI_DEF_ERR`). Retest at chn 2 → `CreateChn=0`. *(Recontextualises 5a:
   its chn 4/5 were also out-of-range, so 5a's device signal was weaker than a
   clean "push doesn't encode" — the SDK-source conclusion still stands.)*
3. **FRAME_BASE can't coexist with the live RING encode on the single H26x
   device.** `BindChnPort2(SCL(0,1,0)→VENC(0,2) FRAME_BASE)` = `0xA0092012` =
   **SYS / E_MI_ERR_BUSY**. The main encode (dev 0 chn 0) is RING-fed; a
   frame-base bind on another channel of the *same* VENC device is refused.
4. **The 2nd H26x device is not backed by hardware.** Moving the bridge to
   `MI_VENC_DEV_ID_H264_H265_1` (dev 1): `MI_VENC_CreateDev(1)` **blocks
   forever** (printf after the call never fires across 28 s while other threads
   keep logging). SSC378QE has a single H26x core.

**Net:** the SDK already proves the compose→SCL-inject→`SCL→VENC` FRAME_BASE
topology encodes H.265 (UVC path; `ST_Sys_Bind` is byte-identical to our bind).
Our binary reproduces the whole chain **up to** the frame-base bind; the only
blocker is that a frame-base VENC can't run **alongside** the live RING encode on
the one H26x device. **This is not a problem for the real design** — in
`stab-fill` the composed frame *is* the main encode, so VENC dev 0 chn 0 is
frame-base from the start and there is no competing RING leg. The sibling-probe
simply can't demonstrate that without tearing the live RING down first.

**Still unproven in-binary:** (a) the frame-base bind actually succeeds once dev
0 is RING-free, and (b) the single-A7 compose+push CPU cost at 50 fps. Both fall
out naturally from the F2 rewire (build VENC frame-base from the start) and can
be measured at F5 — or via a **destructive** dev-0 ring-swap bench first
(unbind the live RING → bind frame-base → inject → measure → daemon killed).
Clean, non-disruptive probing is exhausted. **DECISION PENDING** (see
requirements "Open decision").

## If F0 passes — the port

### F1 — compose helpers (port Star6E's fill/blit to i6c)
- Add fill-rect (`BufFillPa`) + blit-rect (`BufBlitPa`) helpers for the i6c
  MI_SYS symbols (dlsym; the stab module already loads the sibling MI_SYS
  symbols). Y=16 / UV=128 borders, NV12 even-offset clamping — mirror
  `star6e_framing_stab.c` fill/blit exactly.
- Reuse `stab_crop_pct` as the max-shift / border budget (already the semantics
  under stab-fill in `venc_config.c`).

### F2 — pipeline rewire (stab-fill mode only)
- When `framing==stab-fill`: build the bridge chosen in F0 —
  - (A) a 2nd SCL channel with manual input + `SCL2→VENC` frame-base bind, VENC
    `NORMAL_FRMBASE`; the main camera→SCL keeps feeding the detector tap; OR
  - (B) insert DIVP between SCL and VENC.
- `off`/`zoom`/`stab` keep the existing camera→SCL→VENC(RING) path untouched
  (branch on the framing preset at graph-configure time).

### F3 — wire into `maruko_framing_stab.c` (fill-mode branch)
- Add `g_fill_mode` (mirror Star6E). `prepare()` already resolves the accuracy
  level + Kalman; in fill mode the detector still taps SCL port 2, but the EMIT
  path becomes: read the full SCL frame → compose (shift by Kalman `acc`,
  black-fill borders) into the bridge-module input buffer → PutBuf.
- The compose may need a dedicated thread (Star6E uses a `SCHED_FIFO` blit
  thread) so it doesn't stall the detector/drain loop — decide from F0's timing.
- Teardown: keep the R6 rule — join the compose/detector thread BEFORE
  unbinding/disabling the bridge module and the tap (MMU-storm → watchdog).

### F4 — un-gate stab-fill (schema + WebUI)
- `venc_api.c` `field_supported_for_backend`: currently no maruko gate on
  `framing` values, but `web/dashboard.html` still disables the `stab-fill`
  enum option on Maruko (`optDisabled`) with an "(N/A on Maruko)" label — flip
  those, update the framing tooltip, regen `venc_webui.c` via `make webui`.
- `venc_config.c` stab-fill preset comment → note the Maruko module-bind path.

### F5 — device verify (.233)
- framing=stab-fill applies; full-FOV floating-on-black confirmed on the RTP
  view; pan + `pause_stab` glide-home work; ≥10 stop/start cycles clean, 0 MMU
  resets; record fps + CPU vs `stab`.

## Risks

- **R-F0 (pivotal):** F0 may show the compose+bind path encodes but the
  single-core CPU cost (detector ~8–17ms + compose + copy) blows the 20ms
  budget at 50 fps → stab-fill only viable at a lower fps or with DIVP HW
  compose. F0 measures this before any port.
- **R-F1 (topology):** the 2nd-SCL-channel bridge (A) means VENC is fed by SCL2
  not SCL0; the detector tap + the camera path must coexist with SCL2 on the
  same SCL device — verify SCL supports the extra channel + manual input while
  chn0 runs (an early F0a check).
- **R-F2 (DIVP unknown):** DIVP offset-blit-into-canvas is unverified on i6c;
  (B) only viable if F0b confirms it.
- **R-F3 (teardown):** the fill path adds a bridge module + compose thread to
  the join-before-disable ordering; more MMU-storm surface than `stab`.
- **R-F4 (zero-copy loss):** stab-fill is inherently a copy (compose), unlike
  `stab`'s crop-reprogram — a real per-frame cost, acceptable by design.

## Effort

F0 ~1 focused bench session. If it passes, F1–F5 ~ multi-day (larger than
`stab` was — new bridge + compose + rewire). If F0 fails on cost/feasibility,
stop and ship stab-only with the finding recorded.
