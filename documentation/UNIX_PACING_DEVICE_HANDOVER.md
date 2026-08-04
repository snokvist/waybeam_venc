# `unix://` Paced Egress + Sojourn Throttle — Device Handover

<!-- version: 1.2.0 -->

Branch: `claude/waybeam-venc-bitrate-throttle-brujl3` · Version `0.65.0` ·
Contract `0.20.0`

**Base:** this branch sits on **PR #214** (`claude/unix-socket-speed-limits-az62s4`,
0.63.0), which is open and unmerged. #214 is a prerequisite, not a
coincidence — the calibrated fill denominator, `OutputSocketQueue`,
`output_socket_note_saturation()`, the congestion-classified drop counters
and the bounded flush are all load-bearing here. Do not try to apply this on
top of `master`.

Status: **device-verified on Star6E for P1–P5 (§5.10 P1/P2/P4, §5.11 P5/P3,
both 2026-08-02, 15 Mbps / 60 fps). The clamp works: overflows down 11.6x and
delivered frames 52 %→96 % at unchanged goodput, no CPU cost, invisible on a
healthy link. Two open items: the AIMD tail still reaches ~300 ms every
~4.6 s, and observe-only telemetry is broken (§5.11).** P6–P8 unrun. Star6E
only — the Maruko mirror is not written. Both knobs default off, so a deploy
with no config change must behave exactly like 0.63.0. That is check P1 and it
is the gate for everything else.

---

## 1. What this adds, in one paragraph

`unix://` output can now hold encoded frames in a producer-side queue
(`src/venc_frame_queue.c`, 2 slots x 384 KB since §5.12) and feed the socket one frame
at a time, so backlog accumulates somewhere venc can measure it instead of
in the kernel's datagram-counted queue — whose depth is latched into the
*consumer's* socket at that socket's creation and which venc therefore does
not own. On top of that, `src/venc_codel.c` clamps the encoder bitrate on
the **minimum queue sojourn time over a 200 ms interval**: CoDel's signal,
with the same AIMD multiplier `venc_shm_throttle` uses as the actuator.
The clamp never writes `video0.bitrate`. Nothing about the wire format or
the consumer's view of the socket changes.

Full rationale in `documentation/UNIX_CODEL_PACING_PLAN.md`; host soak
results in its §9.

## 2. Config

| Key | Mutability | Default | Meaning |
|---|---|---|---|
| `outgoing.unixPacing` | **restart** | **`true`** (0.65.0) | Producer-side frame queue + pacing |
| `outgoing.unixThrottle` | live | **`true`** (0.65.0) | Apply the sojourn clamp on top |
| `outgoing.unixLegacyBlocking` | **restart** | `false` | Opt back out to pre-0.63 blocking sends; disables pacing |

Constraints, all enforced in code:

- `unix://` **and** `streamMode: "rtp"` only. Anything else silently leaves
  pacing off — `paced` in the transport status is how you confirm it is
  actually on.
- Mutually exclusive with `unixLegacyBlocking` (renamed from
  `allowUnixEncoderStall` in 0.65.0; the old key is retired, not aliased —
  it loads as unknown and leaves the default). Setting both
  logs a warning and pacing loses — that flag *is* the way to turn pacing off.
- `unixThrottle` with `unixPacing` off does nothing.

`unixThrottle=false` while pacing is on is the **observe-only mode**: the
controller runs and publishes what it *would* have clamped to, but never
programs it. That is the mode most of this plan is measured in.

## 3. Telemetry

`GET /api/v1/transport/status`, UDP/Unix branch, new fields:

| Field | Meaning |
|---|---|
| `paced` | pacing actually active |
| `queueFrames` | frames currently queued (0..VENC_FRAME_QUEUE_SLOTS) |
| `queueDelayUs` | **the controller's input** — age of the oldest queued frame, 0 when empty |
| `queueSojournUs` | last completed frame's measured queue time (telemetry only) |
| `queueOverflows` | frames refused whole because the queue was full |
| `throttlePermille` | what the controller wants; 1000 = unclamped |
| `effectiveBitrateKbps` | `video0.bitrate` scaled by the above |

⚠️ **`throttlePermille` over HTTP is the *applied* clamp, not the
controller's want** — it reads 1000 whenever `unixThrottle` is off, however
hard the controller is demanding a clamp. Device-confirmed in §5.11: a
permanently full queue at 194 ms average delay published `throttlePermille`
1000 for 200/200 samples. Observe-only mode therefore publishes nothing
useful through this endpoint; the controller's want reaches only the RTP
sidecar. Treat every observe-only reading of this field as meaningless
until that is fixed.

Log lines worth grepping in `/tmp/waybeam.log`:
`unix throttle pinned at floor`, `unix throttle left the floor`,
`outgoing.unixPacing ignored`.

## 4. Setup

```sh
# build + deploy as normal
./scripts/star6e_direct_deploy.sh cycle

# target-local receiver (now has a sustained slow-drain mode)
make unix-dgram-consumer SOC_BUILD=star6e
# scp out/star6e/unix_dgram_consumer to /tmp on the target

/tmp/unix_dgram_consumer --help
#   --stall-after-ms / --stall-ms   hard wedge
#   --drain-kbps N                  sustained slow drain  <-- new, and the
#                                   case the clamp actually exists for
```

Confirm `cat /proc/sys/net/unix/max_dgram_qlen` is >= 256 before anything
else — that is #214's `S95waybeam` doing its job, and every number below
assumes it.

**Stop the external rate controller first.** On any craft running
waybeam-link, `waybeam-link tx` writes `video0.bitrate` continuously — it is
the single rate controller — so every check below would otherwise be
measuring two controllers at once. It is silent in the config file: read the
live value, not `/etc/waybeam.json`.

```sh
/etc/init.d/S96waybeam-link stop        # restart after the run
curl -s "localhost/api/v1/get?video0.bitrate"   # must now hold still
```

Leave `waybeam_hub` running — its OSD render is realistic pipeline load and
it does not write bitrate (`venc.bitrate_enabled=false`).

---

## 5. Checks

Star6E first, per the backend policy. Record actual numbers, not
pass/fail — several of these exist to *produce data*, not to confirm a
guess.

### P1 — Regression gate: pacing off changes nothing

Deploy 0.64.0 with no config change (`unixPacing=false`). Run the 0.63.0
V4 matrix: 10/15/20/25 Mbps at 60 fps and 25 Mbps at 120 fps.

**Pass:** delivered bitrate, RTP gaps, `transportDrops` and send spread all
match the 0.63.0 results in `UNIX_SOCKET_HANDOVER.md` §5. `paced` is
`false`.
**Fail:** anything moved. Stop — the paths are supposed to be untouched
when the flag is off, so a difference here is a bug in the *unpaced* path.

### P2 — ⚠️ Calibration: healthy sojourn on ARM (highest-value check)

`unixPacing=true`, `unixThrottle=false`, healthy consumer. Same bitrate
matrix as P1. Poll `/api/v1/transport/status` at ~5 Hz for 60 s per point
and record the distribution of `queueSojournUs` and `queueDelayUs`.

Host x86 reference: sojourn **avg 68 us, max 326 us**, `queueFrames` never
above 0, `throttlePermille` stays 1000.

**This sets `VENC_CODEL_TARGET_US`.** It is currently a provisional 10 ms
(`include/venc_codel.h`) chosen because host-healthy and host-congested
sojourn are three orders of magnitude apart. If ARM healthy sojourn lands
anywhere below ~1 ms, 10 ms stands. If it is within ~3x of 10 ms, the target
must move up and the reason must be written down.

**Also fail if:** `queueFrames` sits above 0 on a healthy consumer, or
`throttlePermille` ever leaves 1000. Either means pacing is throttling a
link that is fine.

### P3 — Cost of the enqueue copy

Same bitrate, pacing off then on, 120 fps (worst case). Compare CPU per
`documentation/STAR6E_CPU_PROFILE.md` method.

Predicted: ~1.9 MB/s of memcpy at 15 Mbps/60 fps, under 1 % of a core, and
the same copy `venc_frame_ring_append()` has always done on frame-shm.

**Fail:** more than ~2 points of CPU attributable to pacing. That would
make the "same cost as frame-shm" claim wrong and is worth stopping for.

### P4 — Wedged consumer, observe-only

`unixPacing=true`, `unixThrottle=false`, `--stall-after-ms 3000
--stall-ms 4000` at 15 Mbps.

**Pass:** `queueFrames` climbs to 8, `queueDelayUs` grows roughly
monotonically through the stall, `queueOverflows` increases, and — the one
that matters — **no partial frame reaches the wire**. Whole frames are
refused at admission. Everything recovers after the stall with no restart.

⚠️ **The pass criterion here originally read "zero RTP sequence gaps", and
that is wrong on device** — see §5.10 P4. The Star6E RTP packetizer stamps
sequence numbers *before* the frame is offered to the queue, so a refused
frame burns its sequence numbers and the consumer correctly reports a hole.
The host soak numbers packets after admission, which is why it reported 0.
Zero gaps is not achievable and not desirable: the receiver *should* be told
the frame is gone.

The testable form of frame-atomic admission is **quantisation** — divide
`sequence_gaps` by the run's mean packets-per-frame and the result must be
an integer equal to `queueOverflows`. Packet-granular loss does not land on
an integer. Device measurement in §5.10 uses that form.

Host reference: 233 overflows over a 4 s wedge, recovered to 950 permille.

### P5 — The actual A/B: sustained slow consumer

`--drain-kbps 8000` with `video0.bitrate` at 15000, 60 fps, 15 s. Run it
twice: `unixThrottle=false`, then `true`.

Host reference:

| | clamp off | clamp on |
|---|---|---|
| frames delivered | 493 / 901 | 891 / 901 |
| `queueOverflows` | 401 | 8 |
| sojourn avg | 209 ms | 42 ms |
| goodput | 8.56 Mbps | 8.55 Mbps |

**Pass:** clamp-on shows a large drop in `queueOverflows` and in average
sojourn at **unchanged goodput**. Goodput must not improve — the consumer's
capacity is the consumer's capacity; the clamp trades frame loss for rate,
it does not create bandwidth. If goodput jumps, something else is going on.

Expect `throttlePermille` to oscillate (1000 -> 250 -> 1000 over seconds).
That is inherent AIMD with no knowledge of consumer capacity, same as the
frame-shm clamp. Record the period and the sojourn peak — if the excursion
is unacceptable in flight, the tuning lever is `VENC_CODEL_INTERVAL_US`
(shorter reacts sooner) or `VENC_CODEL_AI_STEP` (smaller overshoots less).
Keep the interval at least 5x faster than the external actuator's ~1.5 s
settle, or the two controllers couple.

### P6 — Shared-socket audio

`outgoing.audioPort = 0` (Opus on the video socket), pacing on, 15 Mbps.

This is the one parameter chosen by arithmetic and never measured:
`OUTPUT_SOCKET_PACING_SLACK_BYTES` is 4 KiB, sized so audio in the same
queue cannot be mistaken for a video frame still in flight (~768 B of skb
truesize per Opus datagram vs ~18 KiB for the smallest video frame).

**Pass:** `queueFrames` stays at 0 on a healthy consumer with audio
flowing, and the consumer's payload-type histogram shows both 96 and the
audio PT. **Fail:** queue depth creeps up with audio on but not off — the
slack is too small and audio is holding the pacing gate shut.

### P7 — Live redirect while paced

`unix://` -> `udp://` -> `unix://` via `/api/v1/set` under load.

**Pass:** no crash, no stale frames delivered to the new destination, the
clamp releases to 1000 on the `udp://` leg (`paced` goes false), and the
queue re-arms on return. Queued frames are dropped on a redirect by design.

### P8 — Mutual exclusion

Set `unixPacing=true` and `unixLegacyBlocking=true` together, restart.

**Pass:** the `outgoing.unixPacing ignored` warning appears once and
`paced` reports `false`.

### P9 — Re-run P5 with the calibrated target

Only if P2 moved `VENC_CODEL_TARGET_US`. It is a compile-time constant in
`include/venc_codel.h`, so this is an edit + `make build SOC_BUILD=star6e`
+ redeploy. Update the constant's comment with the measured basis.

---

## 5.10 Measured — Phase A, 2026-08-02, SSC338Q / IMX335 (`192.168.2.232`)

**Partial run.** P1, P2 and P4 measured at a single 15 Mbps / 60 fps /
1920x1080 operating point. **P3, P5, P6, P7, P8 are still outstanding**, and
so is the rest of the P1 bitrate matrix. This pass was scoped to the two
checks on the §7 revert list plus the calibration the branch is blocked on.

Build: 0.64.0, `make build SOC_BUILD=star6e`, deployed via
`star6e_direct_deploy.sh cycle`. `max_dgram_qlen` 256. Consumer
`/tmp/unix_dgram_consumer`, target-local.

### Setup finding — the rate controller has to be stopped first

`waybeam-link tx` was running and actively driving `video0.bitrate`: the
config file read 2829 kbps while `GET /api/v1/get?video0.bitrate` returned
**21839**. Any measurement of a bitrate clamp taken with the external rate
controller live is measuring both controllers at once. `S96waybeam-link` was
stopped for the whole run and restarted afterwards; `waybeam_hub` was left
running, since its OSD render on VPE port 0 is realistic vehicle load and it
does not write bitrate (`venc.bitrate_enabled=false`). **Add this to §4
setup** — it is not optional, and it is invisible unless you read the live
value rather than the config file.

### P1 — regression gate, `unixPacing=false` · PASS

30 s, `unix://waybeam_venc_test`:

| | measured | #214 §5 reference |
|---|---:|---:|
| delivered | 15.361 Mbps | 15.176 Mbps |
| RTP sequence gaps | 0 | 0 |
| `transportDrops` delta | 0 | 0 |
| max marker gap | 18.727 ms | 17.309 ms |
| max frame spread | 993 us | 546 us |

1802 frames / 30 s = 60.07 fps. `paced` reported `false`. Spread is higher
than #214's row but that run did not have `waybeam_hub` OSD render on the
pipeline; it sits inside the 1.035 ms #214 recorded at 25 Mbps. Nothing in
the unpaced path moved.

### P2 — ⚠️ calibration: healthy sojourn on ARM · PASS

`unixPacing=true`, `unixThrottle=false`, healthy consumer, 60 s, status
polled at 5 Hz (300 samples):

| | ARM measured | host x86 reference |
|---|---:|---:|
| `queueSojournUs` avg | **283 us** | 68 us |
| p50 | 273 us | — |
| p95 | 392 us | — |
| max | **815 us** | 326 us |
| `queueDelayUs` | 0 across all 300 samples | 0 |
| `queueFrames` max | 0 | 0 |
| `throttlePermille` | 1000, never left | 1000 |

Delivered 15.368 Mbps, 0 sequence gaps, 3605 frames / 60 s = 60.08 fps —
i.e. **identical to the unpaced P1 run**. Pacing is invisible on a healthy
link, which is the §7 condition.

**`VENC_CODEL_TARGET_US` = 10 ms stands, and is now measured rather than
provisional.** ARM healthy sojourn is ~4x the host figure but the worst
sample seen (815 us) is still 12x below the target and ~35x below it on
average. The handover's own rule — "below ~1 ms, 10 ms stands" — is met on
the max, not merely the mean. P9 is therefore **not required**.

### P4 — wedged consumer, observe-only · PASS on substance

`--stall-after-ms 3000 --stall-ms 4000`, 15 s run. Queue trace at 5 Hz:

```
qf=0  delayUs=0        ovf=0     <- pre-stall, steady
qf=6  delayUs=83705    ovf=0     <- stall begins
qf=8  delayUs=299548   ovf=11
qf=8  delayUs=516889   ovf=24
...   (monotonic, +13 overflows per 200 ms = 65/s at 60 fps)
qf=8  delayUs=3811826  ovf=222
qf=1  delayUs=567      ovf=233   <- stall ends, one sample later
qf=0  delayUs=0        ovf=233   <- fully recovered
```

`queueFrames` reached 8 and held, `queueDelayUs` grew monotonically to
3.81 s, `queueOverflows` settled at **233 — the same figure as the host
soak**, and recovery took under one 200 ms poll interval with no venc
restart.

**Frame-atomic admission holds.** The consumer reported 5398 sequence gaps,
which is *not* a failure — see the corrected criterion in §5 P4. The
quantisation test:

| | paced | unpaced (same wedge, control run) |
|---|---:|---:|
| packets / frames delivered | 15478 / 668 = 23.170 pkt/frame | 15633 / 673 = 23.228 pkt/frame |
| sequence gaps | 5398 | 4285 |
| gaps ÷ pkt-per-frame | **232.97 frames** | **184.5 frames** |
| `queueOverflows` | 233 | 0 (`transportDrops` 4285, packet-granular) |

Paced loss lands on an integer number of whole frames and that integer is
exactly `queueOverflows`. Unpaced loss lands on a half-frame, because the
kernel drops individual datagrams and partial frames reach the wire. That
difference *is* what the producer-side queue buys, and it is the first
direct device evidence for it — the host soak's "0 gaps" was an artifact of
its harness numbering packets after admission.

### Defect found — contract version string was never bumped

`documentation/HTTP_API_CONTRACT.md` and `HISTORY.md` both declare contract
`0.19.0`, and the PR checklist claims the bump was made, but
`src/venc_api.c` still emitted `"0.18.0"` — the running device reported
`contract_version: 0.18.0` for a 0.64.0 build. Fixed in this commit. Any
consumer gating on the contract version would have silently missed the new
`transport/status` fields.

### Still outstanding after Phase A

P1 at 10/20/25 Mbps and 25 Mbps @ 120 fps · P6 (shared-socket audio) ·
P7 (live redirect) · P8 (mutual exclusion). P3 and P5 are covered by
Phase B below.

---

## 5.11 Measured — Phase B (P5 + P3), 2026-08-02, same device

**The clamp works.** `unixThrottle=true` exercised on hardware for the first
time. 15 Mbps configured / 60 fps / 1920x1080, consumer `--drain-kbps 8000`,
`waybeam-link tx` stopped, status polled at 5 Hz.

### ⚠️ Setup precondition discovered the hard way: the scene must be moving

The first attempt produced a **false negative**. On a static scene the CBR
encoder undershoots badly — measured **6.43–6.57 Mbps against a 15000 kbps
target** (~13.5 KB/frame instead of ~32 KB/frame) across five runs. Production
then sits *below* the 8000 kbps consumer cap, so there is no congestion, the
queue stays empty and the clamp correctly never engages. Everything reads
"pass" while testing nothing.

This is easy to misread as a pacing regression, because the static window
happened to coincide with the pacing-on runs and the bytes-per-frame appeared
to track the flag. It does not: an A/B/A with the scene in motion gave
**pacing on = 15.29 Mbps, 60.04 fps, 31.8 KB/frame** — full rate.

**Before any congestion test, confirm the encoder is actually producing near
`video0.bitrate` with a healthy consumer.** If it is not, you are measuring a
static scene, not a transport. (Related: the known CBR-undershoot/3DNR
interaction in `KNOWN_ISSUES.md`.)

### P5 — the A/B, moving scene

| | T1 clamp **off** (40 s) | T2 clamp **on** (60 s) |
|---|---:|---:|
| goodput | 8.000 Mbps | **7.930 Mbps** |
| frames delivered | 1246 / ~2400 = **52 %** | 3456 / ~3600 = **96 %** |
| RTP sequence gaps | 26123 | **1355** |
| `queueOverflows` | 1149 (28.7 /s) | 149 (**2.5 /s**) |
| `queueFrames` max | 8 (permanently full) | 8 (transient) |
| sojourn avg | **213.7 ms** | **27.7 ms** |
| sojourn p50 | 233.4 ms | **0.24 ms** |
| sojourn p95 | 251.2 ms | 166.4 ms |
| sojourn max | 266.9 ms | 299.1 ms |
| `queueDelayUs` avg | 194.3 ms | 22.5 ms |
| consumer max frame spread | 44.1 ms | 43.1 ms |
| waybeam CPU | 15.6 % of one core | 15.6 % |

Goodput is **unchanged** (−0.9 %), which is the required result: the clamp
trades frame loss for rate, it does not create bandwidth. Overflows fall
**11.6x**, delivered frames go 52 % → 96 %, and median queue latency collapses
**970x**, from 233 ms to 240 µs.

**The tail does not improve, and that is the real finding.** p95 is still
166 ms and the maximum is 299 ms — slightly *worse* than clamp-off. Clamp-off
is a permanently full queue at a steady ~215 ms; clamp-on is a mostly-empty
queue punctuated by AIMD excursions of the same magnitude. Half the time the
added latency is effectively zero; the excursions still reach ~300 ms.

Oscillation, measured: `throttlePermille` was below 1000 in **250 of 300
samples (83 %)**, reached the **250 floor**, and crossed the 1000 boundary
**26 times in 60 s** — a full cycle every **~4.6 s**. The log shows
`unix throttle pinned at floor` and `left the floor` **14 times each**. So the
controller repeatedly bottoms out at 25 % and walks back up. That is the
predicted AIMD behaviour (§5 P5) and it is where the latency tail comes from.
If the excursion is unacceptable in flight, the levers are
`VENC_CODEL_INTERVAL_US` (react sooner) and `VENC_CODEL_AI_STEP` (overshoot
less) — not the target constant, which Phase A settled.

### T3 — cost on a healthy link: none

Clamp armed, healthy consumer, 40 s: 14.919 Mbps, 2403 frames = 60.07 fps,
0 sequence gaps. `throttlePermille` **1000 in all 200 samples**, `queueFrames`
max **0**, `queueDelayUs` **0** throughout, no new overflows. Sojourn avg
**273 µs** / p95 343 / max 546 — statistically the same as Phase A's
pacing-only baseline (283 / 392 / 815 µs). The clamp is invisible when nothing
is wrong, which is the §7 condition.

`video0.bitrate` read **15000 before and after** the whole sequence — the D1
claim that the clamp never writes the configured bitrate holds.

### P3 — enqueue-copy CPU cost: +0.3 points

Healthy consumer, comparable bitrate, `waybeam` process CPU from
`/proc/<pid>/stat` (immune to the 5 Hz poller's own fork cost; the poll rate
was identical in every run):

| | bitrate | waybeam CPU |
|---|---:|---:|
| pacing **off** | 15.37 Mbps | 16.0 % of one core |
| pacing + clamp **on** | 14.92 Mbps | 16.3 % of one core |

**+0.3 points**, against a ~2 point fail threshold. The "same cost as
frame-shm's `venc_frame_ring_append()`" claim holds. Under congestion CPU did
not move either (15.6 % in both T1 and T2).

### Defect confirmed — observe-only mode publishes nothing

Phase A flagged this from code reading; T1 proves it empirically. Throughout
T1 the queue was **permanently full** — `queueFrames` 8, `queueDelayUs`
averaging 194 ms against a 10 ms target — and HTTP `throttlePermille` read
**1000 in all 200 samples**. The controller was certainly demanding a deep
clamp; the field never showed it.

Cause: `query_transport_status()` publishes
`star6e_controls_output_throttle()` (`src/star6e_controls.c:1487`) — the
*applied* value — and with `unix_throttle` off the apply is forced to
`VENC_CODEL_FULL_PERMILLE` (`src/star6e_runtime.c:1178-1181`). The controller's
want lives in `ps->output.throttle_permille` and reaches only the RTP sidecar
(`src/star6e_runtime.c:1511`).

So **§3's "`throttlePermille` is populated even when `unixThrottle` is off —
that is the point of observe-only" is false over HTTP**, and the observe-only
calibration mode the device plan is largely written around cannot be observed
through the documented endpoint. §5 P2's "fail if `throttlePermille` ever
leaves 1000" is likewise untestable in the mode it is specified for.

Not fixed here — surfacing the controller's want over HTTP is a field-design
call for the author (a separate key, rather than overloading `throttlePermille`
with two different meanings depending on a config flag). §3 is annotated.

### Still outstanding

P1 at 10/20/25 Mbps and 25 Mbps @ 120 fps · P6 (shared-socket audio) ·
P7 (live redirect) · P8 (mutual exclusion). Compact stream mode remains out of
scope by design.

---

## 5.12 Tuning probe — queue depth is the latency (2026-08-02, same device)

**Not part of the PR. Proposals only**, measured to answer "can the ~300 ms
excursions be reduced". Code on `experiment/pr215-tuning`; this section records
the device data so the handover stays the single device record.

Same A/B as §5.11 (15 Mbps configured, `--drain-kbps 8000`, moving scene).

| | **8 slots** (as-shipped) | **4 slots** | **4 + single-charge** |
|---|---:|---:|---:|
| sojourn avg | 27.7 ms | **9.2 ms** | 11.0 ms |
| sojourn p50 | 0.24 ms | 0.20 ms | 0.24 ms |
| sojourn p95 | 166.4 ms | **66.2 ms** | 66.8 ms |
| **sojourn max** | **299.1 ms** | **133.7 ms** | **116.8 ms** |
| `queueOverflows` /60 s | 149 | **95** | 106 |
| frames delivered | 3456 / 3600 | **3506** | 3496 |
| goodput | 7.930 Mbps | 8.000 | 8.000 |
| floor hits | **14** | 2 | **1** |
| clamp cycle period | ~4.6 s | ~20 s | ~30 s |
| waybeam CPU | 15.6 % | 15.6 % | 21.4 % ⚠️ |

**The ceiling is structural.** Clamp-*off* max sojourn went 266.9 →
**149.1 ms** on the same 8→4 change — near-exactly halved. Worst-case added
latency is `queued bytes ÷ drain rate`; nothing in `venc_codel.c` can reduce
it. That is also why clamp-off (267 ms) and clamp-on (299 ms) measured the
same at 8 slots: both are simply "queue full".

**Halving the depth improved every metric, including the one predicted to get
worse** — overflows *fell* 149 → 95 and delivered frames *rose*. Mechanism: at
4 slots the queue drains in less than `VENC_CODEL_INTERVAL_US`, so the loop
stops deciding on evidence that predates its own last decision and settles
instead of floor-slamming and re-climbing (floor hits 14 → 2). The dead-time
instability and the latency ceiling are the same fix from two directions.

**Suppressing the double-charge** (overflow ×3/5 in `venc_codel_observe()` and
sojourn ×4/5 in `venc_codel_tick()` both landing in one interval, compounding
to ×0.48) is correct in principle — same event billed twice — but marginal
once the queue is shallow, with differences inside run-to-run spread.

⚠️ **Evidence quality:** every row above is **n=1**, and the scene was
hand-moved, so encoder output varied between runs. The 8→4 result is large
enough to survive that; the single-charge column is **not** — treat it as
indicative only. The 21.4 % CPU reading is not believed (a 2-line change at
5 Hz cannot cost 6 points) and is recorded unexplained. Future sweeps should
use `tools/unix_pacing_soak` on ARM (§6) for deterministic input and n≥3.

Follow-on programme, targets and remaining ideas:
`specs/2026-08-02-unix-pacing-latency-tuning/`.

---

## 5.13 Where the refused frames come from (2026-08-04, same device)

Asked because the shipped default still refuses ~0.6 % of frames against a
sustained slow consumer, and the RTP receiver reports that as sequence gaps.

Setup: 15000 kbps configured, 1080p60, `resilience=range`,
`server=unix://waybeam_venc_test`, consumer `--drain-kbps 8000`, 45 s,
`waybeam-link` stopped, `maxIBytes`/`maxPBytes` zero. Production verified
against an **uncapped** consumer first: 15.28 Mbps, 60.05 fps, 0 gaps — so no
static-scene undershoot (§5.11). n=3 per variant, all after a restart, so the
CPU transient below is common to every row.

### The gaps are the refused frames, and nothing else

In all 14 runs:

    frames_delivered + queueOverflows = 2701..2704 = 60.05 fps x 45 s

Every frame the encoder produced was either delivered whole or refused whole.
No partial frames, no packet-granular loss, `queueOversizeDrops` 0 throughout.
The receiver still sees a hole because RTP sequence numbers are assigned before
admission (§5.10), and a refused frame burns ~8.6 of them against ~12.2 in an
average frame — the frame that gets refused is the one *after* the frame that
filled the queue, so it is typically smaller than average.

### Every clamp decrease was triggered by a lost frame

Counting clamp decreases in the 5 Hz status log and attributing each to whether
`queueOverflows` moved in the same sample:

| variant | loss-driven cuts | delay-driven cuts |
|---|---:|---:|
| as-shipped (200 ms interval) | 28 | **0** |
| 50 ms interval | 8 | **0** |
| AI_STEP 20 | 9 | **0** |

**The min-sojourn rule never fires.** It is not mistuned — at 2 slots it cannot
fire. `venc_codel` decides on the *minimum* sojourn across a control interval,
and 2 slots at 60 fps is 33 ms of queue: the window between "queue starts
standing" and "queue refuses a frame" is shorter than any usable interval, so
every interval contains a sample where the queue touched empty and the minimum
reads 0. Shortening the interval 4x does not reach it either.

So at depth 2 the controller is necessarily **loss-driven** — Reno without ECN.
Refusals are not a failure of the loop, they *are* its input, and the refusal
count is the AIMD cycle count. This is the other half of the §5.12 depth
decision: 8 slots (133 ms) could hold standing delay across a 200 ms interval,
2 slots cannot. The 13.8x latency win and the loss-driven control are one fact.

### What actually moves it

Only the cycle *rate* is left, i.e. how fast the probe walks past capacity
(sustainable here is 8000/15000 = 533 permille):

| n=3 each | as-shipped | 50 ms interval | AI_STEP 20 | AI_STEP 20 + overflow x4/5 |
|---|---:|---:|---:|---:|
| frames refused | 16.7 | 13.3 | **8.3** | 12.0 |
| (per run) | 16-16-18 | 14-14-12 | 10-7-8 | 17-11-8 |
| sequence gaps | 141 | 115 | **73** | 121 |
| goodput | 7.891 Mbps | 7.767 | 7.667 | **7.873** |
| frame loss | 0.62 % | 0.49 % | **0.31 %** | 0.44 % |

Halving the additive step halves the refusals for ~2.8 % goodput: the clamp
tracks capacity more tightly (mean permille 500-534 against a sustainable 533)
but spends longer below it after each cut. Softening the overflow charge moves
the same trade back the other way — goodput restored, refusals part-way back.
Two independent knobs landing on one frames-against-bitrate curve is what a real
trade looks like, not a mistuned constant.

The fourth column is the one worth another look: it lands *off* that curve —
baseline goodput with ~29 % fewer refusals — but its run-to-run spread (17, 11,
8) is far wider than the baseline's (16, 16, 18) and trends downward within the
set, so it is suggestive, not established. **Nothing here is adopted.** These
are single-operating-point numbers; the shipped constants were locked against
the RF-shaped profile of §5.12, where a slower climb is charged differently.
Re-run the candidate under that profile before touching a default.

### Rejected: drain before admitting

Hypothesis: `MI_VENC_GetStream` blocks for most of a frame period, so at
admission the socket may have drained since the last pass; draining first would
free a slot instead of refusing. Measured with a drain call added ahead of
`venc_frame_queue_begin`, n=3 against an n=3 post-restart control:

| | control | drain-before-admit |
|---|---:|---:|
| frames refused | 16, 16, 18 | 17, 17, 17 |
| sequence gaps | 143, 117, 163 | 136, 168, 140 |
| queue depth reached | 0-1 | 0-2 |
| sojourn max | 33.6 ms | 66.3 ms |

No effect on loss and the queue got *deeper*, because pushing earlier fills the
socket and the pacing gate then blocks the following pass. Reverted.

⚠️ **CPU is unreadable for ~2 runs after a restart** — again. The first
measurement read 15.4 % (control, long-running) against 25.8 % (modified, fresh
restart) and looked like a large regression. Restarting the *control* binary
reproduced 25.8 %, and by its third run it had decayed to 15.2 %. Same trap as
§5.12's unexplained 21.4 %. Any CPU comparison must restart both sides and
discard the first two runs.

## 6. Optional: soak harness on ARM

`tools/unix_pacing_soak.c` drives the real modules over a real socket with
synthesised frames, so it isolates queue/controller behaviour from the
encoder. It is wired as a host build (`make soak-tools`) but has no x86
dependency — only `output_socket.c`, `venc_frame_queue.c`, `venc_codel.c` —
so it cross-compiles if an ARM-side sanity check on the timing constants is
wanted before touching the real pipeline. Not required.

## 7. What would make me revert rather than tune

- P1 shows any change with the flag off.
- P3 shows the copy costs real CPU. The design assumes it is negligible
  because frame-shm already pays it.
- P4 shows partial frames reaching the wire. Frame-atomic admission is the
  central claim; if it does not hold, the queue is not buying what it
  claims to buy.
- Pacing raises steady-state latency on a *healthy* link. Everything here
  is justified by being invisible when nothing is wrong.

Tuning, not reverting, for: clamp oscillation amplitude, the target
constant, the audio slack.

## 8. Known gaps

- **Maruko mirror not written.** Star6E-only branch. Per the backend
  policy the mirror follows Star6E device confirmation; it is a direct
  port of `star6e_service_unix_codel` + the drain call after
  `MI_VENC_ReleaseStream`.
- **Compact stream mode is not covered** — pacing requires the batch path.
  Consistent with the compact gap already documented in
  `UNIX_SOCKET_HANDOVER.md` §6.
- **`udp://` deliberately excluded.** Its queue drains at qdisc speed, so
  sojourn would read ~0 regardless of what the RF link is doing and the
  controller could never engage. Do not "fix" this by enabling it.
- **The clamp cannot help a consumer taking nothing.** Under a total wedge
  the queue fills in 8 frames (133 ms at 60 fps) and everything after is
  refused. That is correct behaviour, not a tuning failure.
- `queueOverflows` is a `uint32_t` snapshot of a `uint64_t` counter for
  telemetry; it wraps after 4G overflows and nothing depends on it not
  wrapping.

## 9. Rollback

Two commits on top of #214. `git revert` the implementation commit and the
branch returns to plain 0.63.0 behaviour. Or leave it deployed and set
`unixPacing=false` — the flag is the rollback, which is why the default is
off.

---

## 5.14 Pacing + bounded blocking: the missing mode (2026-08-04, same device)

Proposed by the author: pace *and* block — let the pacer catch the common case,
and when the queue does fill, hold the encoder instead of refusing the frame.
Currently the two are mutually exclusive and `unixLegacyBlocking` turns pacing
off entirely.

**Why it should work, and why the obvious mechanism is not the one.** A refused
frame is a reference picture the decoder never receives. A *held* frame is one
the encoder never produced — the next capture is encoded against the previous
encoded frame, so the H.265 reference chain is intact and the only cost is
framerate. Holding converts a chain break into a dropped frame at the source.

My prior expectation — that holding would let the queue stand full and revive
the dead min-sojourn signal of §5.13 — is **wrong**: `queueFrames` still reads
only 0 or 2, never 1, and delay-driven cuts stayed at 0. The queue at depth 2 is
bimodal; holding does not create standing delay, it just alternates faster. The
mechanism is simpler than that — the hold buys the drain enough time to free a
slot.

Implemented as `STAR6E_OUTPUT_ADMIT_HOLD_US` (0 = off): before
`venc_frame_queue_begin`, drain in a loop while the queue is full, up to a
bound, then refuse exactly as before. **The bound is not optional.**
`begin_frame` runs between `MI_VENC_GetStream` and `MI_VENC_ReleaseStream`,
which is precisely where UNIX_SOCKET_HANDOVER.md §1.2 measured a 634 ms capture
stall from an unbounded block.

Same operating point as §5.13 (15 Mbps configured, `--drain-kbps 8000`, 45 s,
n=3):

| | default (refuse) | hold 16 ms | **hold 50 ms** |
|---|---:|---:|---:|
| frames refused (chain breaks) | 16.7 | 19.5 | **1.7** |
| sequence gaps | 141 | 184 | **3.3** |
| frames never encoded | 0 | 0 | 73 |
| delivered fps | 59.7 | 59.6 | 58.4 |
| goodput | 7.891 Mbps | 7.87 | **8.000** (drain ceiling) |

**50 ms removes 90 % of the chain breaks and 98 % of the sequence gaps, and
goodput goes *up*** — pinned at the consumer's 8.000 Mbps ceiling, because
capacity is no longer spent encoding frames that get thrown away. The price is
73 frames per 45 s that were never encoded: 58.4 fps instead of 59.7.

**16 ms buys nothing** — worse than the default, inside spread. One frame of
drain at 8 Mbps is ~16 ms, so a one-frame-period bound expires just as the slot
would have freed. The bound has to cover several frames of drain to be worth
anything.

### Hard wedge — the case the bound exists for

4 s wedge inside a 40 s run, hold 50 ms: delivery gap **4.02 s**, i.e. the wedge
and nothing more. 80 frames refused once the bound expired, HTTP responsive
throughout, CPU 19.7 %, no capture stall. Recovery on an uncapped consumer
immediately after: **60.07 fps, 14.87 Mbps, 0 sequence gaps**, max frame spread
567 us. Compare the same wedge on the other modes (§5.11): pre-0.65 default
discards 4396 packets as partial frames, `unixLegacyBlocking` discards none but
stalls capture for the full duration and unboundedly beyond.

So the three-way exclusion is wrong. The useful axis is not "pace *or* block",
it is **how long to hold before giving up**, with `unixLegacyBlocking`
(unbounded) and the pre-0.66 default (zero) as the two ends of it. Shipped as
`outgoing.unixAdmitHoldFrames`, default **3**.

### Correction — the delay signal *does* come back

The paragraph above (written from the 16 ms run) said holding does not revive
§5.13's dead min-sojourn signal. That is wrong, and only counting cut causes
across the whole matrix showed it:

| | loss-driven cuts | delay-driven cuts |
|---|---:|---:|
| no hold (§5.13) | 28 | **0** |
| hold 1 frame period | 7 | **0** |
| **hold 3 frame periods, 2 slots** | 3 | **37** |
| hold 3 frame periods, 3 slots | 1 | 44 |

One frame period is too short to produce standing occupancy, so the queue stays
bimodal and every cut is still loss-driven. Three is enough: `queueFrames`
starts reading intermediate values, the interval minimum clears `TARGET_US`, and
**the controller flips back to delay-driven** — clamping before the queue
overflows, which is what it was designed to do. That is the mechanism behind the
94 % drop in refusals, and it means the hold is not a workaround bolted beside
the controller but the thing that lets the controller work at this depth.

### Depth revisited, with the hold in place

Hold fixed at 3 frame periods, n=3 each:

| | **2 slots** | 3 slots | 4 slots |
|---|---:|---:|---:|
| frames refused | 1.0 | 0.3 | 0.3 |
| delivered fps | 58.68 | 59.22 | 59.38 |
| frames never encoded | 60 | 37 | 30 |
| goodput | **7.994 Mbps** | 7.979 | 7.953 |
| queue delay p95 | **19.8 ms** | 34.8 | 50.6 |
| queue delay max | **38.2 ms** | 65.6 | 98.9 |

Each extra slot buys ~0.35 fps (fewer holds) and costs ~15 ms of p95 queue
delay. §5.12's law — added latency is depth over drain rate — still holds, but
for a changed reason: the queue no longer *runs* full, it simply sits deeper.
Refusals are already ~1 per 45 s at 2 slots, so depth has almost nothing left to
remove. **2 slots stays.**

⚠️ Do not read `max_marker_gap_us` as queue latency. It is delivery *spacing*
and is dominated by encoder cadence — it read 35.7 / 33.8 / 36.2 ms across the
three depths while the actual queue delay doubled. Use `queueDelayUs` from the
status poll.
