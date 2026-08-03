# `unix://` paced egress — latency & performance tuning

Status: **active**, device-measured baseline established 2026-08-02.
Depends on PR #215 (`claude/waybeam-venc-bitrate-throttle-brujl3`, v0.64.0),
which is itself stacked on #214. Experiment code:
`experiment/pr215-tuning`. Sibling: the coordination repo's
`specs/cross/2026-07-26-shm-egress-backpressure/` — the frame-shm clamp is the
same controller on a different signal, so findings should propagate.

## Why

PR #215's CoDel clamp was device-verified working (overflows −11.6x, delivered
frames 52 %→96 %, unchanged goodput, no CPU cost — handover §5.11). What it
does *not* fix is the latency tail: ~300 ms queue excursions every ~4.6 s at
the shipped 8-slot depth.

A tuning probe (§5.12) then established the governing relation:

> **Worst-case added latency = queued bytes ÷ drain rate.**
> It is a property of the queue, not of the controller. The clamp only
> determines how *often* you sit at the ceiling.

Halving the depth (8→4 slots) halved the ceiling and improved every other
metric too — including overflows, which were predicted to worsen. So the knee
has not been found and there is headroom left.

## Goal

Latency down and delivered frames up, at unchanged goodput and CPU.

| Metric | Baseline (4 slots) | Target |
|---|---:|---:|
| sojourn p95 | 66 ms | **< 20 ms** |
| sojourn max | 134 ms | **< 70 ms** |
| frames delivered under congestion | 97.4 % | **> 99.5 %** |
| goodput | 8.00 Mbps | unchanged |
| waybeam CPU | 15.6 % of one core | unchanged |
| healthy-link added latency | 273 µs avg | unchanged (must stay invisible) |

The last row is a constraint, not a target: everything here is justified by
being invisible when nothing is wrong (handover §7).

## Where the remaining problem is

At 4 slots the steady state is already excellent — p50 sojourn 0.2 ms. Nearly
all remaining p95/max **and essentially all overflow** come from one
behaviour: additive increase walks past the sustainable rate until the queue
fills, gets cut, and repeats. Reducing overshoot is therefore worth more than
further shrinking the queue, which only caps how bad each excursion is.

## Requirements

- **R1 — Loss must be graceful before it is rare.** When a frame must be
  refused, refuse one whose loss does not break the H.265 reference chain.
- **R2 — The controller must not overshoot the sustainable rate** every cycle
  as though it had never found it.
- **R3 — The latency budget must be expressed in time**, not in a slot count
  that silently means different milliseconds at different bitrates.
- **R4 — Results must be reproducible.** No conclusion from n=1 on a
  hand-varied scene.
- **R5 — No regression** in goodput, CPU, healthy-link latency, or the
  frame-atomic admission property proven in §5.10.

## Non-goals

- Maruko mirror (follows Star6E confirmation, per backend policy).
- Compact stream mode (out of scope by design in #215).
- `udp://` (deliberately excluded — qdisc drains at line rate, sojourn reads
  ~0, the controller could never engage).
- Changing the wire format or the consumer's view of the socket.

## Known evidence gaps carried in from the baseline

1. All A/B rows are **n=1**; run-to-run variance is real (an unexplained
   21.4 % vs 15.6 % CPU outlier).
2. **Scene content was uncontrolled.** It already produced a false negative (a
   static scene undershoots CBR to 6.4 Mbps — below the consumer cap, so there
   was no congestion at all) and nearly a false positive (a phantom 2.3x
   "pacing regression" that was really the static window lining up with the
   pacing-on runs).
3. **Observe-only telemetry is blind** — HTTP `throttlePermille` publishes the
   *applied* clamp, so clamp-off runs report 1000 regardless of what the
   controller wants. Controller intent has only ever been inferred from
   behaviour.
4. **One operating point** (15 Mbps / 60 fps / 8 Mbps drain).
5. **Step response only** — never measured tracking of a gradual degradation.

Gaps 1–3 are preconditions: until they are closed, further tuning produces
noise rather than knowledge.
