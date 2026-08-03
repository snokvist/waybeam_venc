# Validation — host soak rig, 2026-08-03

Session 1 of the programme, run **without hardware**: `tools/unix_pacing_soak`
drives the production `venc_frame_queue` + `venc_codel` + `output_socket` over
a real AF_UNIX socket against `tools/unix_dgram_consumer`, with **synthetic
frames**. That removes the scene variable that confounded every device A/B
(requirements.md gap 2) and makes n≥3 cheap (gap 1).

Conditions: 15000 kbps configured, 60 fps, 1400 B payload, consumer
`--drain-kbps 8000`, 40 s per run. Sweeps use the new compile-time knobs
`-DVENC_FRAME_QUEUE_SLOTS` and `-DVENC_CODEL_AI_EVERY`, both defaulting to
as-shipped behaviour.

## Headline

**n=4, mean ± sample stdev:**

| slots | ai_every | sojourn avg | sojourn max | overflows | delivered | goodput |
|---:|---:|---:|---:|---:|---:|---:|
| **8** | **1** *(as shipped)* | 50 ±4 ms | 300 ±59 ms | 30 ±4 | 99 % | 7.53 Mbps |
| 4 | 5 | 10 ±2 ms | 121 ±8 ms | 18 ±3 | 99 % | 7.73 |
| 3 | 5 | 6 ±1 ms | 96 ±8 ms | 16 ±1 | 99 % | 7.82 |
| **2** | **5** | **2 ±1 ms** | **67 ±14 ms** | **13 ±2** | 99 % | **8.03** |
| 2 | 10 | 1 ±0 ms | 75 ±29 ms | 9 ±3 | 100 % | 8.02 |

Against the shipped configuration, **2 slots + `AI_EVERY=5`** gives average
sojourn **25x lower** (50 → 2 ms), max **4.5x lower** (300 → 67 ms), overflows
**down 57 %** (30 → 13) — and goodput **improves** 7.53 → 8.03 Mbps, i.e. it
reaches the consumer's 8 Mbps cap that the shipped config does not.

Against `requirements.md` targets: max < 70 ms **met** (67); goodput unchanged
**exceeded** (improved); delivered > 99.5 % **not quite** at `AI_EVERY=5`
(99 %), met at `AI_EVERY=10` (100 %) at the cost of a slightly worse tail.

## The two knobs interact — this overturns the depth-only conclusion

An earlier depth sweep with the **shipped** controller (`AI_EVERY=1`, n=3)
showed a knee at 4 slots: going shallower bought no tail improvement and cost
overflow.

| slots (ai_every=1) | sojourn avg | sojourn max | overflows |
|---:|---:|---:|---:|
| 8 | 46 ±3 ms | 272 ±38 ms | 31 ±8 |
| 4 | 21 ±0 | 133 ±29 | 46 ±6 |
| 3 | 16 ±1 | 128 ±35 | 53 ±6 |
| 2 | 10 ±1 | 128 ±10 | 77 ±3 |

Max sojourn **plateaus at ~128 ms below 4 slots** and overflow climbs steeply
(46 → 77). The plateau has a mechanical explanation: the *head* frame's
sojourn is set by how long the socket takes to accept it under congestion —
`frame_bytes / drain_rate` — which is independent of how many frames are
queued behind it. Depth only affects the followers, so the average keeps
falling while the max stops.

With asymmetric probing, that penalty disappears: 2 slots drops from 77
overflows to 13, and the max falls to 67 ms. **Fixing the overshoot removes
the cost of running shallow**, so the two must be tuned jointly rather than
one at a time — the single-variable conclusion was wrong.

## Slower probing is not free — except when the queue is shallow

At 4 slots, `AI_EVERY` 1 → 5 → 10 traded goodput away (8.04 → 7.65 → 7.21
Mbps in the n=3 sweep) because the clamp spends longer below the sustainable
rate. At 2 slots that cost vanishes (8.03 Mbps at the cap): the shallow queue
keeps the sojourn signal small, so the controller clamps less and still lands
at capacity. Another instance of the same interaction.

## Confidence and what does not transfer

- **Host x86, `max_dgram_qlen` 512** — the device is ARM with 256. Absolute
  values will not transfer; the ordering and the interaction should.
- **Synthetic frames.** The harness models an IDR every 2 s at 5x P size; the
  real encoder's size distribution differs, and frame-size variance is exactly
  what sets the head-frame sojourn floor.
- **The drain runs on the pipeline thread with a 4 ms budget on device**
  (`STAR6E_OUTPUT_DRAIN_BUDGET_US`); the harness has no equivalent
  constraint. This is the most likely source of device/host divergence.
- `sojourn_max` is one worst sample per run, hence the wide spread (±8 to
  ±59 ms). Averages are tight.
- **2 slots is the structural minimum** — one frame draining, one filling.
  There is no headroom below it, so if the device disagrees the answer is the
  time-based cap (plan T3), not a smaller count.

## Next

T1 (layer-aware admission), and a decision on the clamp floor — see below.

---

# Device confirmation — 2026-08-03, SSC338Q `.2.232`

Real encoder, ARM, `max_dgram_qlen` 256, `waybeam-link` stopped. Two star6e
builds differing only in the two constants. 60 s per run, consumer capped at
**3000 kbps** (see "operating point" below), production measured immediately
before each run.

| | **A** = 8 slots / `AI_EVERY`=1 *(shipped)* | **B** = 2 slots / `AI_EVERY`=5 |
|---|---:|---:|
| production before run | 7.52 Mbps | 8.10 Mbps |
| sojourn avg | 129.3 ms | **17.0 ms** (7.6x) |
| sojourn p50 | 149.1 ms | **16.8 ms** |
| sojourn p95 | 150.5 ms | **34.4 ms** (4.4x) |
| sojourn max | 233.8 ms | **66.9 ms** (3.5x) |
| `queueDelayUs` avg | 113.8 ms | **6.0 ms** |
| `queueOverflows` | 630 | 679 |
| frames delivered | 2967 | 2923 |
| goodput | 3.000 Mbps | 3.000 Mbps |
| waybeam CPU | 14.9 % | 15.1 % |
| floor hits | 1 | 56 |

**The latency result reproduces on hardware** — average 7.6x, p95 4.4x, max
3.5x, at identical goodput and CPU. Host ordering transferred.

**The overflow difference is not real.** B shows +7.8 % overflows, but B also
ran at +7.7 % production (8.10 vs 7.52 Mbps — the scene drifted upward during
the session). Same magnitude, so the two are indistinguishable; this A/B
cannot separate them.

**Healthy link, variant B (30 s):** 14.93 Mbps, 1803 frames = 60.1 fps, 0
sequence gaps, `queueFrames` max **0**, `throttlePermille` 1000 throughout,
sojourn 145 µs, no new overflows, CPU 16.4 %. So **2 slots — the structural
minimum — is still invisible when nothing is wrong**, which was the main risk
of going that shallow.

## The 250-permille floor blocked convergence

Not planned, but this run answered a question filed for session 3.

`video0.bitrate` was 15000, so the floor at 250 permille commands 3750 kbps —
**above the consumer's 3000 kbps capacity**. The clamp therefore bottomed out
and could not reach the rate it needed, which is why overflows stayed at
630–679 for *both* variants and delivered frames sat at ~82 %. B hit the floor
56 times to A's 1: B recovers off the floor and re-probes, A simply stayed
pinned.

So: **when the sustainable rate is below `VENC_CODEL_FLOOR_PERMILLE`, the
controller cannot converge and overflow persists regardless of tuning.** The
floor is a fixed fraction of a configured bitrate that may itself be far above
what the scene produces, which makes it doubly awkward. Worth deciding whether
the floor should be lower, or expressed against measured production rather
than `video0.bitrate`.

Consequence for reading the table: this A/B ran in a **floor-limited regime**
the host sweep never entered (host was 15 Mbps configured vs an 8 Mbps
consumer, comfortably above floor). Latency comparisons hold; overflow
comparisons do not transfer between the two.

## ⚠️ Test-methodology trap: stale frame-size caps

Production initially measured **4.9 Mbps** against a 15000 kbps target, which
I first misread as a static scene. It was not — setting `video0.maxPBytes=0`
jumped production to **13.0 Mbps instantly with no scene change**.

Cause: the craft config carries `maxIBytes=21761` / `maxPBytes=4096`, sized
for the ~2829 kbps that `waybeam-link` normally commands. In normal operation
link drives these upward as it drives bitrate (observed live value 17885 with
link running). **Stopping link and forcing `bitrate=15000` leaves the stale
small caps in place, strangling the encoder to roughly a third of target.**

**Any test that stops `waybeam-link` and raises the bitrate must also zero
`maxIBytes`/`maxPBytes`.** Added to the session-start checklist. This also
partly revises the earlier "static scene" diagnosis — content and the caps are
separate ceilings and both must be cleared before a congestion result means
anything.

Note this contradicts a prior finding that `maxPBytes` was a no-op; that
conclusion came from static-scene evidence, and it is wrong here.

---

# Device round 2 — clean A/B above the floor, 2026-08-03

Round 1 was accidentally run **below** the clamp floor (3000 kbps consumer vs a
3750 kbps floor), which pinned both variants and made the overflow numbers
meaningless. Repeated at `--drain-kbps 8000` with `maxIBytes`/`maxPBytes`
zeroed, production **15.3–15.6 Mbps**, floor 3750 well below the consumer's
capacity. `resilience: "range"` (the craft's deployed setting) on both sides.

| | **A** = 8 slots / `AI_EVERY`=1 *(shipped)* | **B** = 2 slots / `AI_EVERY`=5 |
|---|---:|---:|
| sojourn avg | 20.4 ms | **0.96 ms** (21x) |
| sojourn p50 | 213 µs | 184 µs |
| sojourn p95 | 134.2 ms | **0.30 ms** (443x) |
| sojourn max | 249.8 ms | **50.1 ms** (5x) |
| `queueDelayUs` avg | 17.3 ms | **0.28 ms** |
| `queueOverflows` | 118 | **20** (5.9x fewer) |
| RTP sequence gaps | 1308 | **190** (6.9x fewer) |
| frames delivered | 3486 / 3600 = 96.8 % | **3584 / 3600 = 99.6 %** |
| goodput | 7.978 Mbps | 7.896 Mbps |
| `queueFrames` max | 8 | 2 |
| waybeam CPU | 20.7 % | 15.4 % |

**All three `requirements.md` targets are met, with margin:**

| target | result |
|---|---|
| p95 < 20 ms | **0.30 ms** |
| max < 70 ms | **50.1 ms** |
| delivered > 99.5 % | **99.6 %** |

Goodput is −1 % (inside run-to-run spread) and CPU did not regress. The
headline is p95: the shipped configuration spends a large fraction of its time
deeply queued (p95 134 ms), whereas the candidate's queue is essentially empty
except during brief excursions (p50 184 µs, p95 303 µs, max 50 ms).

## Intra-refresh vs periodic IDR — no difference

Tested on variant B at the same operating point: `resilience: "range"`
(intra-refresh `balanced` + refPred, ref_base 1 / ref_enhance 4) against
`resilience: "off"` with `gopSize` 2.0 (no intra refresh, no refPred, periodic
IDR).

| | `range` | `off` (normal GOP + periodic I) |
|---|---:|---:|
| sojourn avg | 963 µs | 684 µs |
| sojourn p50 | 184 µs | 185 µs |
| sojourn p95 | 303 µs | 309 µs |
| sojourn max | 50.1 ms | 48.7 ms |
| `queueOverflows` | 20 | 21 |
| sequence gaps | 190 | 187 |
| frames delivered | 3584 | 3583 |
| goodput | 7.896 Mbps | 7.895 Mbps |

**Statistically identical on every metric.** Frame-size variance between
rolling intra-refresh and periodic IDR does **not** drive queue latency at this
depth — which is consistent with the mechanism: a 2-slot queue cannot hold
enough frames for the size *distribution* to matter, only the individual head
frame's size, and that is similar either way.

Practical consequence: **choose `resilience` on its own merits** (error
resilience, OSD-safety per the preset table's `OSD-safe?` column) — it is not a
latency lever. Plan item **T4 (flatten frame-size variance) is closed as
not-useful** at 2 slots.

CPU read 15.4 % (`range`) vs 22.6 % (`off`) vs 20.7 % (A). These swings do not
track the changes and match the unexplained outliers seen twice before; CPU
differences under ~5 points on this rig should not be believed without repeats.

## T1 withdrawn

Temporal-layer-aware admission assumed non-reference SVC-T frames exist to drop
preferentially. Per the craft owner, **ref-enhance is not run**, so no frame
ever carries `ENHANCE_P_NOTFORREF` and the policy would have nothing to select
on. `svct_active` is merely `ref_base > 0`
(`src/star6e_pipeline.c:2679`), which is true under `range` without implying
any droppable frames exist. **T1 is withdrawn**, not deferred.

## Remaining

- **The clamp floor.** `VENC_CODEL_FLOOR_PERMILLE` 250 x `video0.bitrate` is
  the lowest commandable rate; a consumer below it cannot be tracked (round 1
  demonstrated this). The floor is a fraction of a *configured* bitrate the
  scene may never reach, which makes it doubly awkward. Decide whether to lower
  it or express it against measured production.
- n=1 per cell here. The effect sizes (21x, 443x) are far outside plausible
  noise, but the small differences (goodput −1 %, CPU) are not.
- P6/P7/P8 from the handover, the bitrate matrix, and the long soak.
