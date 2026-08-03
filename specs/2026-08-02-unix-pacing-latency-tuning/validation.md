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

---

# FPV-profile sweep — host rig, 2026-08-03

An FPV link does not fail as one step to a fixed rate. `tools/unix_dgram_consumer`
gained `--profile "kbps:ms,..."` (looped), so drops, recoveries, brief stalls
(`kbps` 0) and partial throttles all come from one mechanism.

Profile used (20.07 s loop, mean capacity ~9.3 Mbps):

```
12000:4000, 4000:3000, 12000:2000, 0:250, 9000:2500,
0:120, 7000:3000, 15000:3000, 2000:1200, 12000:1000
```

It deliberately includes a 2000 kbps segment — **below** the 250-permille floor
against `video0.bitrate` 15000 (= 3750 kbps) — because a deep fade really does
go there.

n=3, 45 s, producer 15000 kbps / 60 fps, clamp on. **Consumer-side** columns are
what actually arrived:

| slots | ai_every | floor | sojourn avg | overflows | delivered | frames | gaps |
|---:|---:|---:|---:|---:|---:|---:|---:|
| **8** | **1** | **250** *(shipped)* | 103.0 ±3.2 ms | 111 | 3.73 Mbps | 2584 | 0 |
| 4 | 1 | 250 | 41.2 ±1.7 | 157 | 4.58 | 2541 | 0 |
| 2 | 1 | 250 | 14.4 ±0.9 | 239 | **6.76** | 2461 | 0 |
| 2 | 3 | 250 | 13.4 ±0.7 | 168 | 5.41 | 2532 | 0 |
| 2 | 5 | 250 | 13.1 ±0.7 | 156 | 4.79 | 2544 | 0 |
| 2 | 1 | 100 | 12.5 ±1.6 | 200 | 6.72 | 2500 | 0 |
| 2 | 3 | 100 | 10.2 ±1.0 | 104 | 4.33 | 2596 | 0 |
| **2** | **5** | **100** | **7.7 ±0.5** | **78** | 3.42 | **2623** | 0 |

## Findings

**Depth still dominates latency.** 8 → 4 → 2 slots at fixed `ai`/floor gives
103 → 41 → 14 ms average sojourn, a 7.2x improvement. Same conclusion as the
static sweep, and the strongest single effect here.

**The static-drain recommendation reverses under a dynamic link.** With a
constant bottleneck, `AI_EVERY=5` looked like a clear win. Under the FPV
profile it costs **29 % of delivered bitrate** (6.76 → 4.79 Mbps at 2 slots)
while barely changing latency (14.4 → 13.1 ms). A real link keeps *recovering*,
and a deliberately slow prober cannot reclaim the capacity in time. What slower
probing actually buys here is **frame continuity**, not latency: overflows
239 → 156, frames 2461 → 2544.

**So `AI_EVERY` and the floor are both bitrate-vs-frame-continuity levers**, not
latency levers, once the queue is shallow. Two defensible operating points:

- **Throughput-first: `2 / 1 / 100`** — 12.5 ms sojourn, **6.72 Mbps** (+80 %
  over shipped), but 200 overflows and 2500 frames.
- **Continuity-first: `2 / 5 / 100`** — **7.7 ms** sojourn (13x better than
  shipped), **78 overflows** (−30 %), **2623 frames** (+1.5 %), at 3.42 Mbps
  (−8 % vs shipped).

For FPV the second is the better trade: a dropped frame is a visible glitch,
whereas 8 % less bitrate is a small quality change. Recommendation is therefore
**2 slots / `AI_EVERY`=5 / floor 100**, with `2/1/100` as the option if raw
bitrate matters more.

**A lower floor is free improvement at fixed `ai`.** 2/3/250 → 2/3/100 moves
latency 13.4 → 10.2 ms, overflows 168 → 104 and frames 2532 → 2596. It lets the
clamp actually follow a deep fade instead of bottoming out — the round-1 device
failure mode.

**Frame-atomic admission holds under dynamic conditions.** Consumer-side
sequence gaps are **0 in every configuration**, including through hard stalls.
All loss is whole frames refused at admission.

**No parameter can beat the stall.** `max_frame_spread` measured 253–262 ms in
every configuration — the profile's 250 ms stall. Under a hard link stall,
end-to-end frame latency equals the stall duration and no queue or controller
setting changes that.

## ⚠️ Instrument defect found and fixed mid-sweep

The first two profile sweeps were **invalid** and are discarded. The token
bucket gated reads on `credit > 0`, so a single bit of credit bought a whole
1400 B datagram — for any rate below ~11 Mbps the consumer degenerated to one
datagram per poll and the throttle levels never happened. Stalls (`rate 0`)
still worked, which is why those runs showed a plausible-looking uniform
233 ms max.

Fixed by letting credit go into **debt** (`int64_t`, read then subtract, block
while negative), which enforces the mean rate exactly. Verified: a constant
5000 kbps profile now delivers ~4 Mbps rather than pinning at 11–14.

Lesson worth keeping: a rate limiter that never goes into debt does not limit
rate — it limits *poll frequency*.

---

# Device confirmation of continuity-first, FPV profile — 2026-08-03

`.2.232`, real encoder, `waybeam-link` stopped for the whole session and left
stopped. `resilience: "range"`, `maxIBytes`/`maxPBytes` zeroed, production
12.3 Mbps, same 20 s FPV profile as the host sweep, 60 s runs.

| | **A** = 8/1/250 *(shipped)* | **C** = 2 slots / `AI_EVERY`=5 / floor 100 |
|---|---:|---:|
| sojourn avg | 68.8 ms | **4.15 ms** (16.6x) |
| sojourn p50 | 50.1 ms | **0.14 ms** (366x) |
| sojourn p95 | 199.1 ms | **17.5 ms** (11.4x) |
| sojourn max | **931.3 ms** | **250.2 ms** (3.7x) |
| `queueDelayUs` max | 981 ms | 133 ms |
| `queueOverflows` | 209 | **175** (−16 %) |
| RTP sequence gaps | 1461 | **1074** (−26 %) |
| frames delivered | 3395 | **3429** (+1 %) |
| consumer max frame spread | 272.7 ms | **257.0 ms** |
| delivered bitrate | 5.43 Mbps | 4.15 Mbps (−24 %) |
| `queueFrames` max | 8 | 2 |

**The host prediction holds on hardware**, and the effect is larger: latency
better on every percentile, continuity better on every measure (fewer
overflows, fewer gaps, more frames), at a bitrate cost — −24 % here versus −8 %
predicted on the host rig.

**The floor change is doing real work.** A's worst sojourn is **931 ms**. The
profile's deep segment is 2000 kbps; A's floor of 250 permille against
`video0.bitrate` 15000 commands 3750 kbps, so A *cannot* clamp low enough and
the queue runs away. C's floor of 100 permille commands 1500 kbps, below the
2000 kbps consumer, so it tracks the fade and its worst case falls back to
**250.2 ms — the profile's 250 ms stall**, i.e. the structural limit identified
in the host sweep. Nothing can go below that; C is already there.

Caveats: n=1 per arm. CPU read 15.5 % (A) vs 25.5 % (C) — the third
unexplained swing of this size on this rig, not tracking the change and not
believed; worth a dedicated repeat rather than attribution. Sequence-gap
quantisation is not clean here (1074 / 6.96 pkt-per-frame ≈ 154 vs 175
overflows) because frame size varies widely as the clamp moves, so the
§5.10 integer test does not apply under a dynamic profile.

**Selected configuration:** `VENC_FRAME_QUEUE_SLOTS` 2, `VENC_CODEL_AI_EVERY`
5, `VENC_CODEL_FLOOR_PERMILLE` 100.

---

# The floor change is unnecessary once the outer loop follows — 2026-08-03

**Craft-owner correction:** in production `waybeam-link` lowers `video0.bitrate`
during link duress; it does not hold 15000 while the link falters. Every run
above had `waybeam-link` stopped and the bitrate **pinned** at 15000, so the
250-permille floor was 250 permille of a target that never moved. That is not
the deployed condition.

Re-tested with an emulated outer loop: a device-side script drives
`video0.bitrate` to track the profile's capacity with a ~1 s settle lag,
ignoring sub-second transients the real ~1.5 s loop could not follow either.
Same FPV profile, 60 s, `waybeam-link` still stopped (the emulator replaces it).

| | **A** = 8/1/250 | **D** = 2 / ai5 / **floor 250 (stock)** | **C** = 2 / ai5 / floor 100 |
|---|---:|---:|---:|
| sojourn avg | 40.0 ms | **4.58 ms** | 2.79 ms |
| sojourn p50 | 204 µs | 124 µs | 117 µs |
| sojourn p95 | 216.6 ms | **16.9 ms** | 16.7 ms |
| sojourn max | 932.6 ms | **233.5 ms** | 100.4 ms |
| `queueOverflows` | 230 | **147** | 135 |
| sequence gaps | 1080 | 775 | 670 |
| frames delivered | 3374 | **3456** | 3468 |
| delivered bitrate | 6.01 Mbps | **3.44 Mbps** | 2.90 Mbps |

**Confirmed: with the bitrate target following, the stock floor is no longer
the binding constraint.** D matches C on average (4.58 vs 2.79 ms) and p95
(16.9 vs 16.7 ms), and beats it on delivered bitrate (3.44 vs 2.90 Mbps). The
floor's dramatic effect in the pinned-bitrate runs — A's 931 ms worst case —
was an artifact of holding the target at 15000 while the link collapsed to
2000 kbps. The outer loop is what rescues that case, exactly as designed; the
inner clamp is scaling a target that is itself already coming down.

C retains an edge on worst-case sojourn (100 vs 233 ms), but that is one sample
per arm and costs 16 % of delivered bitrate. Not worth diverging from an
upstream constant for.

**Revised selection: `VENC_FRAME_QUEUE_SLOTS` 2, `VENC_CODEL_AI_EVERY` 5,
`VENC_CODEL_FLOOR_PERMILLE` unchanged at 250.** Two changed constants become
one plus one, the composition with waybeam-link stays as designed, and
throughput is better.

Against the shipped 8/1/250 under the same emulated-outer-loop conditions, D
gives sojourn avg 40.0 → 4.58 ms (8.7x), p95 216.6 → 16.9 ms (12.8x), max
932.6 → 233.5 ms (4.0x), overflows 230 → 147 (−36 %), gaps 1080 → 775 (−28 %)
and frames 3374 → 3456 (+2.4 %) — at 6.01 → 3.44 Mbps delivered.

Note A's 932 ms worst case persists even with the outer loop following: an
8-slot queue is the problem there regardless of floor or bitrate target.

---

# n=3 device repeats — and a correction on CPU, 2026-08-03

Selected config vs shipped, FPV profile + emulated waybeam-link bitrate
following, 60 s runs, three repeats each, `waybeam-link` stopped throughout.

| | **A** = 8/1/250 *(shipped)* | **D** = 2 / `AI_EVERY`5 / 250 |
|---|---:|---:|
| sojourn avg | 40.95 ±2.14 ms | **2.97 ±0.14 ms** (13.8x) |
| sojourn p50 | 0.17 ±0.01 ms | 0.13 ±0.00 ms |
| sojourn p95 | 277.46 ±24.96 ms | **0.33 ±0.04 ms** (841x) |
| sojourn max | 904.06 ±50.10 ms | **238.15 ±9.24 ms** (3.8x) |
| frames delivered | 3373.7 ±16.0 | **3476.3 ±3.2** (+3.0 %) |
| RTP sequence gaps | 1185.7 ±96.8 | **639.7 ±45.2** (−46 %) |
| delivered bitrate | 5.88 Mbps | 3.77 Mbps (−36 %) |
| consumer max frame spread | 262.9 ms | 258.6 ms |
| `queueFrames` max | 8 | 2 |
| **waybeam CPU** | **15.53 ±0.12 %** | **24.73 ±2.37 %** |
| system busy (2 cores) | 19.77 ±1.14 % | 21.13 ±0.25 % |

Error bars are tight on everything that matters. The p95 result is the headline:
**277 ms → 0.33 ms**. D's queue is essentially always empty and only fills
during the profile's hard stall, where its worst case (238 ms) is the stall
itself.

## ⚠️ Correction: the CPU difference is real, not noise

I twice dismissed a ~6–10 point CPU swing on this rig as unexplained noise. With
n=3 it is unambiguous and reproducible: **A 15.53 ±0.12 %, D 24.73 ±2.37 %** —
roughly **+9 points of one core**, a ~60 % increase in the daemon's own CPU. A's
spread of ±0.12 % across three runs leaves no room to call this variance.

This matters against the original P3 criterion, which was "fail if more than ~2
points of CPU attributable to pacing". The selected configuration is well over
that. It does not invalidate the latency result, but it is a real cost that was
being reported as absent.

Two things are still unexplained and should not be glossed:

- **Process CPU rises ~9 points of one core while system busy rises only ~1.4
  points of two.** Those do not reconcile (+9 of one core should be ~+4.5 of
  two). Either the per-process accounting (utime+stime over wall time, assumed
  `HZ`=100 — `getconf CLK_TCK` is unavailable on this busybox) or the
  idle-derived system figure is misleading.
- **Mechanism unknown.** The plausible candidate is the drain gate: with a
  2-slot queue the pipeline thread attempts the drain far more often and finds
  the socket unready, spinning inside `STAR6E_OUTPUT_DRAIN_BUDGET_US`. That
  ties directly to plan item T5, and it may be fixable rather than inherent.

Until that is understood, the honest statement is: **the selected configuration
buys a 13.8x lower average and 841x lower p95 queue latency, plus 3 % more
frames and 46 % fewer sequence gaps, at the cost of ~9 points of one core and
36 % of delivered bitrate.**

## Note on the overflow counter

`queueOverflows` is cumulative and venc was not restarted between repeats, so
per-repeat overflow means are not meaningful in this table. Consumer-side
sequence gaps are the reliable per-run loss metric and are reported instead.

---

# CPU investigation — the penalty was a restart transient, 2026-08-03

The previous section reported a real, reproducible ~+9 point CPU cost for the
selected config. **That was wrong, and this section supersedes it.**

## Attribution attempt: a 2x2

A and D differ in *two* variables, so the CPU could not be attributed. Filling
in the missing cells (n=3 each, same profile + follower):

| | `AI_EVERY`=1 | `AI_EVERY`=5 |
|---|---|---|
| **8 slots** | A: 15.53 ±0.12 % | E: 15.47 ±0.21 % |
| **2 slots** | F: **19.73 ±5.55 %** | D: 24.73 ±2.37 % |

F's ±5.55 was the tell — its three runs read 26.0, 18.0, 15.2 %, a monotone
decay rather than scatter about a mean.

## The transient

D run six times consecutively **without a restart in between**:

| rep | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| waybeam CPU | 25.9 % | 21.3 % | **15.1 %** | 15.1 % | 15.1 % | 15.1 % |

It settles at **15.1 %** — indistinguishable from A (15.53 %) and E (15.47 %).

**There is no sustained CPU cost.** What I measured was a post-restart warm-up
lasting roughly two 60 s runs, and because every variant was measured
immediately after its own deploy-and-restart, the transient was *perfectly
confounded* with the configuration change. A and E happened not to exhibit it
in their first run, which made the pattern look like a config effect.

P3 therefore stands as originally recorded: pacing costs well under the ~2
point threshold. The earlier "+9 points, over the P3 criterion" statement is
withdrawn.

## The latency result is not affected

The same six consecutive runs, latency:

| rep | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| sojourn avg | 2.04 | 1.97 | 2.30 | 3.35 | 2.86 | 2.48 ms |
| `queueFrames` max | 2 | 2 | 2 | 2 | 2 | 2 |

Stable around 2–3 ms with no trend, so the headline comparison (A ~41 ms vs
D ~2.5 ms average) is unaffected by the transient. p95 alternates between
~0.5 ms and ~17 ms across runs — that is the profile's phase relative to the
run boundary under 5 Hz sampling, not instability; the 20.07 s loop does not
divide evenly into a 60 s run.

## Methodology fix

**When comparing configurations that each require a restart, discard the first
~2 runs (~2 minutes) after each restart.** Otherwise the restart transient is
perfectly aligned with the variable under test. This applies to CPU
specifically; queue latency showed no warm-up.

Added to the session-start checklist.

---

# Does the depth finding transfer to frame-shm? No. 2026-08-03

Asked because `frame-shm://` — not `unix://` — is the path waybeam-link
actually streams through (`io/src/config.cpp:78,89` accepts `frame-shm` and
`udp` only; there is **no `unix://` ingest**). So the tuning above is not in the
production video path on this craft.

The ring looks like the same problem — `venc_frame_ring_create(..., 8,
384*1024)`, an AIMD clamp with an identical 200 ms window, ×4/5 decrease, +50
increase, 250 floor and once-per-window drop charge. **But the depth result must
not be ported**, for two reasons.

**1. Depth is not the operating point.** `venc_shm_throttle` engages on the
window's *low-water* mark at `ENGAGE_SLOTS` = 2 and recovers at
`RECOVER_SLOTS` = 1 (`include/venc_shm_throttle.h:82-83`). The controller
already holds occupancy at ~0–1 slots; the other 7 are headroom it never
intends to use. Device transport status on the frame-shm path reads
`usedSlots: 0`, `fillPct: 0` in steady state. On `unix://` the pacing gate let
the queue fill toward `SLOTS` and depth *was* the operating point — which is
why halving it halved the tail. Not the same situation.

**2. The headroom is absorbing consumer scheduling jitter, and that is
measured.** From the module's own header:

> Measured on a Star6E at 100 fps into an 8-slot ring with a perfectly healthy
> consumer, the ring routinely spikes to 2-3 slots inside a 200 ms window and
> drains again — the consumer reads one frame per event-loop iteration, so
> short bursts are normal.

A 2-slot ring turns those routine 2–3 slot bursts into overflow drops. The
`unix://` queue could afford to be shallow because the kernel's socket queue
sat *behind* it as a second buffer; the frame-shm ring is the only buffer
between two processes and has nothing to fall back on.

**What does transfer is the probing asymmetry.** The AIMD is structurally
identical, so the same overshoot dynamic — climb past the sustainable rate,
refill, get cut — should be present, and that is what `VENC_CODEL_AI_EVERY`
addressed. It is a bitrate-vs-continuity lever there and would be here too.

⚠️ **One constraint the unix path did not carry.** `venc_shm_throttle.h:26-29`
requires the 200 ms window stay **≥5x faster than the slowest external actuator
(~1500 ms settle)** so the cascaded controllers do not couple. Rate-limiting
the *increase* to one step per 5 windows makes the probe cadence 1 s, which is
only 1.5x that settle. The decrease stays at every window, so the
fast-reaction path is preserved and only the recovery direction slows — but the
guidance is explicit and the same tension applies to the `unix://` change
already locked. Evidence against coupling so far: the device A/B ran *with* an
emulated ~1 s-lag outer loop and the selected config showed a single clamp
transition over 60 s, i.e. stable. Worth watching rather than assuming.

## Proposed next step

1. Port asymmetric probing to `venc_shm_throttle` behind the same style of
   compile-time knob. **Do not change `venc_frame_ring` depth.**
2. Build the rig: `tools/frame_shm_consumer_test.c` exists but has no rate
   shaping. It needs the `--profile` treatment the unix consumer just got, so
   the same RF-shaped drain can be applied to the ring.
3. A/B on host, then device, then real RF with waybeam-link — which *is*
   meaningful on this path, unlike a unix:// RF test.
