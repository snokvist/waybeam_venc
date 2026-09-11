# Adaptive Frame Gate — Phase 1 Plan

<!-- version: 0.1.0 (draft, unapproved) -->

An in-daemon, frame-shm-only overload gate: pause the stream VENC channel's
frame intake when the egress ring stops draining, resume when it does.

Prior art: the `ldy_sky` FPV streamer for Hi3516CV610 (`Fxn_vi_to_venc_dropframe`
+ `vi_ctl`), which runs the same gate off its radio's queue depth.

---

## 1. Problem

`video0.bitrate` is currently the only proportional rate actuator, and on both
SigmaStar backends it costs a keyframe. From `src/star6e_controls.c:252`:

> `MI_VENC_SetChnAttr()` emits an IDR on its own. Measured on a SSC338Q
> (IMX335 1920x1080@60, H.265 CBR, GDR via the racing preset, 2026-08-23),
> ten spaced `video0.bitrate` writes produced eleven IRAP access units.
>
> `MI_VENC_RcParam_t` carries no bitrate field on either SigmaStar backend,
> so there is no rate-only actuator to switch to. **Calling SetChnAttr less
> often is the open follow-up.**

This is worst exactly when it matters: the response to a congested link is a
bitrate write, and a bitrate write injects the largest frame in the stream
into an already-overflowing link. The control action amplifies the fault it
is responding to.

No IDR-free *proportional* QP lever exists to substitute (§2). A frame gate
sidesteps the problem entirely: it changes no encoder state at all.

## 2. Why not a QP lever

**`qpDelta` is not a rate lever.** `apply_rc_qp_delta()`
(`src/star6e_controls.c:162`) writes `s32IPQPDelta` — the I-to-P QP
*relationship*. It is genuinely IDR-free (`SetRcParam` does not keyframe;
measured 2026-08-23, eleven IRAP → one), but CBR still targets the same
bitrate. It redistributes bits between I and P; it does not reduce them.

**`min_qp` is a cliff, not a dial.** It is the real IDR-free QP lever
(`u32MinQp` via `SetRcParam`, CBR-only per `rc_qp_bound_ptrs()`), but README's
bench table rules it out as proportional control:

| `min_qp` | 0 (default 12) | 20 | 24 | 26 | 28 | 30 | 40 |
|---|---|---|---|---|---|---|---|
| rate | 19.58 Mbps | 19.10 | **0.63** | 0.31 | 0.19 | 0.13 | 0.08 |

At or above the scene's natural QP, CBR is abandoned and the stream becomes
effectively fixed-QP — a 30× drop between 20 and 24. Unusable as a
continuous actuator.

**ROI.** ROI writes `MI_VENC_SetRoiCfg` with `bAbsQp = 0`
(`src/star6e_controls.c:685`) — a *relative* offset. It therefore composes
additively with whatever QP the RC picks, and the gradient's shape survives a
moving base QP. The hazard is clamping, not conflict: raising `min_qp` puts a
floor under the whole frame, and the center band's negative offset hits that
floor first — flattening center-priority precisely when the link is worst.

**The gate writes neither RC state nor ROI cfg**, so the ROI gradient and the
CBR contract are both untouched. That is the main argument for it.

## 3. Mechanism

`MI_VENC_StopRecvPic(chn)` / `MI_VENC_StartRecvPic(chn)` gate whether a VENC
channel accepts frames from its bound input. Not create/destroy, not RC.

- Documented as safe mid-stream: *"StopRecvPic is a soft pause and does not
  deadlock while still bound"* (`src/star6e_pipeline.c:2582`).
- Already exercised as the first step of teardown (stop → drain → unbind).
- Already `dlsym`'d and **mandatory** (`src/star6e_mi.c:262-265`, guard at
  `:307`). No new SDK surface on Star6E.

Sensor, ISP, 3A, VI and VPE keep running at full rate throughout — only the
VPE→VENC handoff is gated — so AE/AWB never see a frame-rate step and there is
no exposure pump on reopen. Frames die before encoding, so no bits are spent
on a frame that would only have queued.

Cost: throttling is a duty cycle, not a smooth rate; and the first frame after
reopen sits further from its reference, so it is a fatter P-frame. We trade
framerate for per-frame size — but never for an IDR.

## 4. Signal

**Instantaneous ring fill, not the published `low_water_slots`.**

`low_water_slots` is published on a `VENC_RING_LOW_WATER_WINDOW_US` = **200 ms**
window (`include/venc_frame_ring.h:320`). That is the right cadence for an
external rate controller and far too slow for an overload gate.

`star6e_service_ring_low_water()` already calls
`star6e_output_frame_ring_fill(output, &fill)` **once per encoded frame**, from
`star6e_runtime_process_stream()` (`src/star6e_runtime.c:1647`), on the thread
that owns the VENC channel. `fill.used_slots` is the per-frame signal, already
in hand. The gate attaches there.

This is strictly better than the `ldy_sky` design, which needs a separate
10 ms poller and therefore quantizes to ~1.2 frames at 120 fps. Per-frame
evaluation has no quantization and adds no thread.

The 200 ms published value is left exactly as-is — waybeam-link keeps owning
policy. The gate is overload protection, not policy (§7, D6).

## 5. Design decisions

**D1 — Instantaneous `fill.used_slots`, not `low_water_slots`.** §4.
The 200 ms window cannot protect against a burst.

**D2 — Policy in a pure, backend-agnostic module; actuator behind `BackendOps`.**
`src/frame_gate.c` holds thresholds, hysteresis and counters as a pure state
machine over `(used_slots, slot_count, now_us)` → `OPEN | CLOSE | HOLD`. No SDK
types, so it unit-tests on the host like `src/intra_refresh.c`. Each backend
supplies `gate_set(int open)`.

**D3 — Reopen needs a timed wakeup.** While the gate is closed no frames are
produced, so the per-frame loop stops iterating and cannot reopen itself. The
stream loop already blocks in `MI_VENC_GetStream(chn, strm, ms)` with a
timeout; on timeout while closed, run the gate evaluation anyway. This is the
one genuinely fiddly part of the change and the first thing to get right —
a gate that cannot reopen is a hang.

**D4 — Asymmetric hysteresis, closed-biased.** Close at
`used_slots >= close_slots` (default 3), reopen at `used_slots <= open_slots`
(default 1) AND a minimum closed dwell has elapsed (default 20 ms). Rationale:
the ring's healthy idle occupancy is one frame, not zero
(`include/venc_frame_ring.h:111`), so `<= 1` is the documented healthy band and
`>= 2` is standing backlog; closing at 3 leaves one slot of burst tolerance,
matching the header's own note that 2–3 slot spikes are normal at 100 fps with
a healthy consumer. `ldy_sky` uses the same shape (single threshold at 2, or a
9/15 Schmitt trigger in its other mode).

**D5 — Stream channel only, never the record channel.** Gating the recorder
would punch holes in the SD-card file for a *radio* problem. In dual/dual-stream
the gate binds to the stream channel only; in mirror mode, where ch0 feeds both
stream and recorder, the gate is refused at config-validation time rather than
silently corrupting recordings.

**D6 — Off by default, and it does not move policy back in-daemon.** 0.69.0
deliberately removed `throttle_permille` so waybeam-link owns the rate model.
This does not walk that back: the gate is a local overload reflex, not a rate
target, and it acts on a signal (per-frame fill) and an actuator (VENC intake)
that are both inside venc and cannot be reached from outside at this cadence.
The two compose — waybeam-link still sets bitrate, the gate absorbs the
transients between its decisions. Default `video0.frameGate = "off"`.

**D7 — Count gate drops as `other_drops`, not `full_drops`.** A gated frame is
not a full-ring congestion drop, and the header is explicit that the two
*"demand different responses from a rate controller"*
(`include/venc_frame_ring.h:122-133`). Miscounting would make waybeam-link's
model react twice to one event. Frames never enter the ring, so this needs an
explicit `venc_frame_ring_note_other_drop()` call.

## 6. Files

| File | Change |
|---|---|
| `include/frame_gate.h` | **new** — pure state machine API |
| `src/frame_gate.c` | **new** — policy, hysteresis, counters |
| `tests/test_frame_gate.c` | **new** — host unit tests |
| `include/venc_config.h` | `frame_gate` preset + resolved knobs on `VencConfigVideo` |
| `src/venc_config.c` | parse, defaults, preset expansion, mirror-mode refusal (D5) |
| `src/venc_api.c` | `FIELD(video0, frame_gate, …, MUT_LIVE)`; gate state in `/api/v1/status` |
| `include/venc_api.h` | `gate_set` in `BackendOps` |
| `src/star6e_runtime.c` | evaluate per frame at `:1647`; timed reopen (D3) |
| `src/star6e_controls.c` | `star6e_gate_set()` → `MI_VENC_Start/StopRecvPic` |
| `src/maruko_pipeline.c`, `src/maruko_controls.c` | same wiring |
| `src/cv610_runtime.c` | same wiring, pending R5 |
| `Makefile` | `frame_gate.c` into all three `*_SRC`; register the new test |
| `README.md`, `HISTORY.md`, `VERSION` | document + changelog + minor bump |

## 7. Risks

**R1 — `StopRecvPic` D-state, under the exact trigger condition. (blocking)**
`src/star6e_pipeline.c:2501`: *"any gap in consumption causes VPE backpressure
→ kernel D-state → StopRecvPic hangs."* Our trigger is a backed-up egress
ring, which is adjacent to that condition.

Static analysis says we are safe: `venc_frame_ring_begin_write()` increments
`full_drops` and returns `-1` on a full ring (`include/venc_frame_ring.h:424`)
— the producer **drops, it does not block**. So the output thread keeps calling
`GetStream`/`ReleaseStream` regardless of consumer health, VENC output never
backs up, and VPE never backpressures. This must still be proven on the bench
before the change is trusted; it is the single most important gate on the work.

**R2 — GDR / SVC-T state across a gate cycle.** Hierarchical attrs must be set
*before* `StartRecvPic` — *"Star6E silently no-ops the call if invoked after
StartRecvPic, producing a flat single-layer stream"* (`:1447`, `:1665`). The
gate toggles intake only, not attrs, so in principle nothing is re-armed and
nothing is lost. Must be verified: cycle the gate under `resilience=racing`
(GDR) and `resilience=range` (SVC-T) and confirm the stripe cadence and layer
structure survive.

**R3 — Recorder starvation.** Covered by D5 for dual/mirror, but the refusal
path needs a test.

**R4 — Interaction with `idr_rate_limit` and the scene detector.** A reopen
produces a large P-frame after a temporal gap; the scene detector may read that
as a cut and request an IDR — re-introducing the keyframe the gate exists to
avoid. May need a short suppression window after reopen.

**R5 — CV610 parity.** `ldy_sky` proves `ss_mpi_venc_stop_chn`/`start_chn`
exist and work on Hi3516CV610, but waybeam's CV610 backend builds against
external public headers, so the symbol may not be exposed. Needs a check
against `CV610_SDK_INC` before committing to three-backend parity; CV610 may
ship in a follow-up.

**R6 — Reopen livelock.** If `open_slots` is never reached because the consumer
is permanently dead, the gate stays closed forever and the stream silently
stops. Needs a max-closed-dwell escape that reopens and lets `full_drops` do
the talking.

## 8. Open questions

1. **Bench R1 first, before any other work.** If `StopRecvPic` can hang under
   a full egress ring, the whole approach dies and the fallback is D2's policy
   module driving `min_qp` as an on/off panic switch (§2 — a cliff, but a
   usable one for a binary emergency).
2. Should the gate expose a duty-cycle metric (gated frames / total) in
   `/api/v1/status`? Argues yes: waybeam-link's rate model needs to know the
   delivered fps is not the configured fps. Cheap to add.
3. Is per-frame evaluation too twitchy without a minimum *open* dwell as well
   as a minimum closed dwell? D4 only bounds the closed side.
