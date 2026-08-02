# `unix://` Paced Egress + Sojourn Throttle — Device Handover

<!-- version: 1.0.0 -->

Branch: `claude/waybeam-venc-bitrate-throttle-brujl3` · Version `0.64.0` ·
Contract `0.19.0`

**Base:** this branch sits on **PR #214** (`claude/unix-socket-speed-limits-az62s4`,
0.63.0), which is open and unmerged. #214 is a prerequisite, not a
coincidence — the calibrated fill denominator, `OutputSocketQueue`,
`output_socket_note_saturation()`, the congestion-classified drop counters
and the bounded flush are all load-bearing here. Do not try to apply this on
top of `master`.

Status: **host-soaked only. Never run on hardware. Star6E only — the Maruko
mirror is not written.** Both knobs default off, so a deploy with no config
change must behave exactly like 0.63.0. That is check P1 and it is the
gate for everything else.

---

## 1. What this adds, in one paragraph

`unix://` output can now hold encoded frames in a producer-side queue
(`src/venc_frame_queue.c`, 8 slots x 384 KB) and feed the socket one frame
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
| `outgoing.unixPacing` | **restart** | `false` | Producer-side frame queue + pacing |
| `outgoing.unixThrottle` | live | `false` | Apply the sojourn clamp on top |

Constraints, all enforced in code:

- `unix://` **and** `streamMode: "rtp"` only. Anything else silently leaves
  pacing off — `paced` in the transport status is how you confirm it is
  actually on.
- Mutually exclusive with `allowUnixEncoderStall`. Setting both logs a
  warning and pacing loses.
- `unixThrottle` with `unixPacing` off does nothing.

`unixThrottle=false` while pacing is on is the **observe-only mode**: the
controller runs and publishes what it *would* have clamped to, but never
programs it. That is the mode most of this plan is measured in.

## 3. Telemetry

`GET /api/v1/transport/status`, UDP/Unix branch, new fields:

| Field | Meaning |
|---|---|
| `paced` | pacing actually active |
| `queueFrames` | frames currently queued (0..8) |
| `queueDelayUs` | **the controller's input** — age of the oldest queued frame, 0 when empty |
| `queueSojournUs` | last completed frame's measured queue time (telemetry only) |
| `queueOverflows` | frames refused whole because the queue was full |
| `throttlePermille` | what the controller wants; 1000 = unclamped |
| `effectiveBitrateKbps` | `video0.bitrate` scaled by the above |

`throttlePermille` is populated even when `unixThrottle` is off — that is
the point of observe-only.

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
that matters — **the consumer reports zero RTP sequence gaps within
surviving frames**. Whole frames are refused at admission; no partial frame
should ever reach the wire. Everything recovers after the stall with no
restart.

Host reference: 233 overflows over a 4 s wedge, 0 sequence gaps, recovered
to 950 permille.

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

Set `unixPacing=true` and `allowUnixEncoderStall=true` together, restart.

**Pass:** the `outgoing.unixPacing ignored` warning appears once and
`paced` reports `false`.

### P9 — Re-run P5 with the calibrated target

Only if P2 moved `VENC_CODEL_TARGET_US`. It is a compile-time constant in
`include/venc_codel.h`, so this is an edit + `make build SOC_BUILD=star6e`
+ redeploy. Update the constant's comment with the measured basis.

---

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
