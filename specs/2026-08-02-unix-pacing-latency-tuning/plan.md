# Plan — `unix://` pacing latency tuning

Ordered by expected value per unit of work, not by session. Sessions are at
the bottom; several items are independent and can reorder freely.

## T1 — Temporal-layer-aware admission

**Highest value, and the detection already exists.**

SVC-T is active on the test craft (`refPred: chn=0 base=1 enhance=4 pred=1`),
and `src/star6e_output.c:888` already identifies non-reference enhancement
frames:

```c
if (output->svct_active &&
    stream->h265Info.refType == STAR6E_REFTYPE_ENHANCE_P_NOTFORREF)
        meta.flags |= VENC_FRAME_FLAG_ENHANCE;
```

That is used **only on the frame-shm path**. The unix queue cannot see it —
`VencFrameQueueFrame` (`include/venc_frame_queue.h:75`) carries only
`enqueue_us`, `packet_count`, `byte_len`. So a queue-full condition refuses
whichever frame arrives next, which may be a reference frame, breaking the
reference chain. That is precisely the damage #215 exists to prevent, and
§5.12 measured 2.6 % of frames taking it.

**Change:** carry one flag through `venc_frame_queue_begin()` into
`VencFrameQueueFrame`, and under pressure refuse enhancement frames first.

**Why it is the right shape:** loss becomes graceful framerate reduction —
exactly what temporal scalability is for, and what the decoder already
tolerates. It composes with the clamp (clamp lowers bitrate, layer-drop lowers
framerate). It attacks the *cost* of residual loss rather than its count, so
it stacks with T2 rather than competing.

Satisfies R1.

## T2 — Overshoot control

Three independent changes to `src/venc_codel.c`, cheapest first. Each should
be measured separately (R4) — they are not obviously additive.

**T2a — Asymmetric probing.** Congestion response must be fast; capacity
*discovery* need not be, because links do not improve on 200 ms timescales.
Rate-limit the additive increase to roughly one step per 2 s while leaving the
decrease at every interval. Expected ~10x fewer excursions for a few lines.

**T2b — Remember the last good rate.** Nothing records that ~530 permille was
sustainable, so every cycle re-climbs to 1000 and re-crashes. Use a reduced AI
step above the last-known-healthy permille (CUBIC's plateau idea).

**T2c — Feed-forward.** The drain rate is *directly measurable* at a unix
socket, so `permille ≈ drain_rate / cfg_bitrate` plus AIMD for the residual
removes most of the search. #215's "no link-rate estimate" rationale is sound
for RF and does not bind on a local socket.

Also carry forward the already-measured **single-charge** fix (overflow ×3/5
and sojourn ×4/5 compounding to ×0.48 in one interval): correct in principle,
marginal in effect, keep as insurance.

Satisfies R2.

## T3 — Depth: find the knee, then stop counting slots

- Sweep `VENC_FRAME_QUEUE_SLOTS` ∈ {8, 4, 3, 2}. Ceilings should extrapolate
  to ~256 / 128 / 96 / 64 ms. 8→4 improved everything, so the knee is not yet
  located.
- Expect **frame-size variance, not average rate**, to set the floor: 2 slots
  is one frame draining plus one waiting, so a large IDR holds the only spare
  slot for its full drain time and P-frames arriving meanwhile are refused.
- **Then replace the slot count with a time cap** — refuse admission above
  ~40 ms of queued bytes at the measured drain rate. A slot count is a proxy
  for a latency budget that drifts with bitrate and frame size; a time cap
  *is* the budget and self-adjusts.

Satisfies R3, and closes evidence gap 4 structurally rather than by
re-tuning per bitrate.

## T4 — Flatten frame-size variance

The ceiling is set by bytes, and I-frames are the spike. Knobs already exist:
intra-refresh (already configured — `intraRefresh: mode=balanced lines/P=2`)
versus periodic IDR at `gopSize=2.0`, and whether `maxIBytes` meaningfully
bounds worst-case queue occupancy. Flatter frames mean a shallower queue
works, feeding back into T3. Verify before relying on either — `maxPBytes` was
previously found to be a no-op.

## T5 — Throughput ceiling

`STAR6E_OUTPUT_DRAIN_BUDGET_US` is 4 ms per pipeline iteration against a
16.7 ms frame period at 60 fps, and 8.3 ms at 120 fps. Instrument budget
exhaustion; if it binds, raise it or take #215's own stated escalation and move
the drain to a dedicated sender thread. That would also decouple drain from
pipeline, removing the coupling that made the *unpaced* path collapse 60→31 fps
under a slow consumer (§5.11 R0).

## Method fixes (preconditions — do these first)

- **Deterministic input.** Add an ARM target for `tools/unix_pacing_soak`; its
  three sources (`venc_frame_queue.c`, `venc_codel.c`, `output_socket.c`) have
  no x86 dependency and handover §6 already anticipates this. Synthetic frames
  remove the scene variable entirely. Use the real pipeline only for
  confirmation runs.
- **n ≥ 3 per condition**, report spread rather than single values.
- **Sidecar telemetry** instead of 5 Hz HTTP polling: it carries the
  controller's *want* (`src/star6e_runtime.c:1511`), which HTTP does not, and
  costs no fork per sample. Alternatively fix the observe-only field first.
- **Precondition check every congestion run** with a healthy consumer to
  confirm production is near `video0.bitrate` — a static scene silently
  removes the congestion being tested.
- Verify `HZ` empirically for the CPU maths (`getconf CLK_TCK` is unavailable
  on this busybox target; 100 was assumed).

Satisfies R4.

## Sessions

1. **Rig + depth sweep (~45 min).** ARM soak target, then slots {8,4,3,2},
   n=3, then confirm the winner on the real pipeline. Deliverable: the knee
   with error bars, and a go/no-go on the T3 time cap.
2. **Overshoot (~45 min).** T2a → T2b → T2c, measured separately; then T1.
   Gate: p95 < 20 ms and delivered > 99.5 %.
3. **Generalisation (~60 min).** Bitrate matrix 10/15/20/25 @ 60 fps plus
   25 @ 120 fps; gradual ramp 15→6→15 Mbps over 60 s for *tracking* rather
   than step response; a consumer slow enough to need below the 250-permille
   floor, to answer whether the floor blocks convergence; plus P6/P7/P8 from
   the handover, still unrun.
4. **Long soak (hours, unattended).** Real `waybeam-link` consumer over RF
   rather than a synthetic drain limiter. Watch for drift, counter wrap
   (`queueOverflows` is a `uint32_t` snapshot of a `uint64_t`), memory growth,
   and any interaction between the CoDel loop and waybeam-link's own rate
   controller — **two controllers on one encoder is a live deployment risk
   never yet tested**.

## Session-start checklist

- Restore/verify device state: flash may still hold the experimental 4-slot
  binary and a `unix://` config. Baseline is `/usr/bin/waybeam` 0.63.0
  (md5 `0dd913670e7993636dd3d7fd26bb3b3d`) and the `frame-shm://venc_frame`
  config.
- `/etc/init.d/S96waybeam-link stop` — it writes `video0.bitrate` continuously
  and is invisible in the config file.
- Confirm `max_dgram_qlen` ≥ 256.
- **Zero `video0.maxIBytes` / `maxPBytes`.** They are sized for the ~2829 kbps
  waybeam-link normally commands and are driven upward by link at runtime;
  with link stopped and bitrate forced to 15000 the stale 4096 B P-cap
  throttles production to ~4.9 Mbps (measured 2026-08-03). Setting both to 0
  restored 13.0 Mbps with no scene change.
- Confirm production ≈ `video0.bitrate` against a healthy consumer — *after*
  the above, or the check measures the cap rather than the scene.
- Discard the first ~2 runs (~2 min) after any restart before believing a CPU
  number — a post-restart transient runs ~10 points high and is otherwise
  perfectly confounded with whatever config you just deployed.
- Check the drain target against the clamp floor: `VENC_CODEL_FLOOR_PERMILLE`
  (250) × `video0.bitrate` is the lowest rate the controller can command. A
  consumer slower than that puts the run in a floor-limited regime where
  overflow cannot converge and overflow comparisons are meaningless.

## Verification

Every result reports goodput and CPU alongside latency, so a win cannot hide a
regression (R5), and re-checks the frame-atomic admission property by
quantisation (gaps ÷ mean packets-per-frame must equal `queueOverflows` as an
integer — handover §5.10). Findings land in handover §5.13+, this spec's
`validation.md`, and a PR comment.
