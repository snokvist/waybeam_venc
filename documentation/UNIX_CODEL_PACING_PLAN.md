# `unix://` Producer-Side Frame Queue + CoDel Sojourn Throttle — Plan

<!-- version: 1.0.0 -->

Branch: `claude/waybeam-venc-bitrate-throttle-brujl3` · Base: PR #214
(`claude/unix-socket-speed-limits-az62s4`, 0.63.0) · Status: **Phase 1 (spec),
awaiting approval**

---

## 1. Goal

Give `unix://` the same class of protection `frame-shm://` already has: an
occupancy-driven bitrate clamp that reduces the encoder's rate *before* the
transport is forced to discard anything, so a slow consumer never breaks the
H.26x reference chain.

The mechanism is invisible to the consumer — the wire format, the socket, and
the datagram stream are unchanged. Only the pacing of writes into the socket
and the encoder's programmed bitrate change.

## 2. Why the existing mechanism does not port directly

`venc_shm_throttle` observes `used_slots` in an 8-slot frame ring. Two
properties make that a good control signal, and `unix://` has neither:

1. **A slot is one frame regardless of size.** Intra-GOP frame size varies
   10–50× (a 15 Mbps IDR is ~90 RTP packets, a P-frame ~15 — §1.1 of
   `UNIX_SOCKET_HANDOVER.md`). On the frame ring that variance is divided out
   by construction. On a packet queue it is not: pushing one IDR moves fill by
   ~56 percentage points against a 256-deep queue in a few hundred µs. The
   encoder would be injecting a large periodic disturbance into the very
   signal it controls on, at GOP rate. This is the same failure the
   high-water rule showed in v0.9.2 (`venc_shm_throttle.h`), except the bursts
   would be guaranteed rather than incidental.

2. **venc owns the ring.** The AF_UNIX queue depth is latched into the
   *receiver's* socket from `net.unix.max_dgram_qlen` at its creation
   (§2.1 of `UNIX_SOCKET_HANDOVER.md`). venc cannot set it, cannot read it
   directly, and cannot change it for a consumer that is already running.

Note what is *not* the problem: queue depth in **time** is `bytes / drain
rate`, which is frame-size independent. 256 × 1400 B is ~190 ms at 15 Mbps
whether those bytes are two IDRs or a hundred P-frames. The defect is in the
occupancy *signal* and in admission granularity, not in the byte accounting.

## 3. Design

### 3.1 Producer-side frame queue

A queue of packetized frames in venc's own memory, drained into the socket one
frame at a time. Before pushing frame N+1, the drain loop checks that the
socket has taken frame N; if it has not, N+1 waits in the venc queue.

This restores every property the frame ring has:

- occupancy is frame-granular, so frame-size variance is divided out;
- admission is frame-atomic — a frame is enqueued whole or refused whole;
- the depth is a number venc chooses;
- the kernel queue never holds more than ~one frame, so its packet-unit
  behaviour stops being the binding constraint.

**Pacing edge.** `SIOCOUTQ` returning to (near) zero is the signal that the
consumer has taken the previous frame. This is only meaningful because #214
established that a `unix://` socket's queue is the *consumer's* backlog.

### 3.2 CoDel sojourn signal

Each queued frame carries its enqueue timestamp. When the frame has been fully
handed to the socket, its sojourn is `dequeue_us - enqueue_us` — measured, not
modelled. No link rate, no drain-rate estimate, no cooperation from the
consumer.

Current queue delay is `max(last_completed_sojourn, now - oldest_enqueue_us)`.
The second term matters: a fully wedged consumer completes no frames, so
completion-only sojourn would freeze exactly when the signal is needed.

The controller tracks the **minimum** of that across a 200 ms interval, which
is CoDel's rule and, not coincidentally, the same trick
`venc_shm_throttle_tick()` already uses with low-water slots: a burst that
drains is not congestion, standing delay is.

### 3.3 Control law

CoDel's *signal*, the existing AIMD *actuator*:

```
min_sojourn >= TARGET over interval  -> permille = max(FLOOR, permille * 4/5)
frame refused (queue full)           -> permille = max(FLOOR, permille * 3/5),
                                        at most once per interval
min_sojourn <= TARGET/2 over interval-> permille = min(1000, permille + 50)
```

Floor 250, ceiling 1000, `AI_STEP` 50 — identical to `venc_shm_throttle.h`,
including the floor-edge log-once behaviour. The clamp is a multiplier applied
at the existing apply path; it never writes `video0.bitrate`.

## 4. Design decisions

| # | Decision | Rationale | Rejected alternative |
|---|---|---|---|
| D1 | Producer-side frame queue | §2 — frame granularity and ownership | Deeper `max_dgram_qlen`: does not fix the signal, and venc cannot set it for an already-running consumer |
| D2 | Sojourn time as the control variable | Directly the quantity we budget; needs no rate estimate; invariant to frame size, bitrate and fps | Fill percentage: no stable meaning across frame sizes (§2) |
| D3 | No new thread — drain on the pipeline thread after `MI_VENC_ReleaseStream` | Keeps the threading model; #214 showed the danger is blocking *inside* the VENC critical section, not doing transport work on that thread | Dedicated sender thread: new lock, new teardown ordering. Held as the Phase-B escalation if the drain budget proves too tight |
| D4 | Keep AIMD, not CoDel's `1/sqrt(count)` drop schedule | Our actuator is a rate clamp; dropping *is* the damage we are avoiding. The AIMD law is device-proven and shares the apply path | Literal CoDel: its response is to drop, which breaks the reference chain |
| D5 | `TARGET_US` provisional 10 000, `INTERVAL_US` 200 000 | Healthy sends measure 0.888 ms max spread on Star6E, 3.567 ms on Maruko (#214 device data), so 10 ms is well clear of healthy and under a 60 fps frame period. 200 ms preserves the ≥5× cascade separation from the ~1.5 s external actuator documented in `venc_shm_throttle.h` | Asserting a final target now — **the target is calibrated from Phase A data, not from this table** |
| D6 | Queue = 8 frames × 384 KB, allocated only when pacing is on | Mirrors `venc_frame_ring_create(..., 8, 384*1024)` exactly, so both egress paths have one story. 8 frames at 60 fps is 133 ms — deliberately deeper than TARGET, so the controller has room to act before overflow | Byte-pool with variable descriptors: less memory, more code, no proven precedent in-tree |
| D7 | New config fields, not overloading `outgoing.shm_throttle` | That field is shipped and documented as frame-shm-scoped; changing its meaning would break the HTTP contract's description | Generic `outgoing.egress_throttle`: silently changes an existing field's semantics |
| D8 | `unix://` + RTP mode only | `udp://` drains at qdisc speed, so sojourn measures nothing about the real link and the clamp would never engage. Compact mode is already a documented gap in #214 §6 | Covering all transports: adds surface with no signal behind it |

## 5. Files

**New**

| File | Purpose |
|---|---|
| `include/venc_frame_queue.h`, `src/venc_frame_queue.c` | Producer-side packetized-frame queue. Pure, no SDK, no syscalls, host-testable — same discipline as `venc_frame_ring.h` |
| `include/venc_codel.h`, `src/venc_codel.c` | Sojourn controller. Same API shape as `venc_shm_throttle.h` (`reset` / `set_enabled` / `observe` / `tick` / `permille` / `floor_edge`) so it plugs into the same apply path |
| `tests/test_venc_frame_queue.c`, `tests/test_venc_codel.c` | Host unit tests, mirroring `tests/test_venc_shm_throttle.c` |
| `documentation/UNIX_CODEL_PACING_PLAN.md` | This file |

**Modified**

| File | Change |
|---|---|
| `src/star6e_output.c`, `include/star6e_output.h` | Copy `payload2` into the queue instead of holding a zero-copy pointer; enqueue on `end_frame`; drain loop; sojourn reporting |
| `src/star6e_runtime.c` | `star6e_service_unix_codel()` alongside `star6e_service_shm_throttle()`; call the drain after `MI_VENC_ReleaseStream` |
| `src/star6e_controls.c` | Publish `throttlePermille` / `effectiveBitrateKbps` / queue stats on the UDP/Unix status branch |
| `src/venc_config.c`, `include/venc_config.h`, `src/venc_api.c` | `outgoing.unix_pacing` (bool, `MUT_RESTART`, default false), `outgoing.unix_throttle` (bool, `MUT_LIVE`, default false) + camelCase aliases + UI descriptor |
| `src/star6e_video.c` | Populate `throttle_permille` in the sidecar trailer for `unix://` (field already exists) |
| `tools/unix_dgram_consumer.c` | Add `--drain-kbps` for sustained slow drain. `--stall-*` covers hard wedges; standing sojourn without a wedge is the case CoDel is actually for, and nothing exercises it today |
| `Makefile`, `tests/test_runner.c` | Register the two new test units |
| `documentation/HTTP_API_CONTRACT.md`, `HISTORY.md`, `VERSION`, `README.md` | Contract bump, changelog, version, `unix://` notes |
| `src/maruko_*.c`, `include/maruko_*.h` | Phase C mirror |

## 6. Phasing

**Phase A — measure, do not act.** Queue, pacing, sojourn instrumentation and
telemetry only; the controller runs in observe-only mode and never clamps.
Device-measure the healthy sojourn distribution and the slow-consumer curve,
then set `TARGET_US` from that data. This mirrors how #214 established its
constants and avoids shipping a control loop tuned on a guess.

**Phase B — enable the clamp** with the calibrated target. Default stays
off; flip to on after device verification, following the `allowUnixEncoderStall`
precedent from #214.

**Phase C — Maruko mirror**, per the Star6E-first policy.

## 7. Risks and open questions

1. **The extra copy.** Today `payload2` is a zero-copy iovec into the encoder
   NAL buffer, valid only until `MI_VENC_ReleaseStream`
   (`include/star6e_output.h`). Deferring the send *requires* copying the frame
   body — ~31 KB/frame at 15 Mbps/60 fps, ~1.9 MB/s of memcpy on a
   CPU-constrained SoC. Must be measured against `STAR6E_CPU_PROFILE.md`
   before Phase B. This is the single largest cost of the design.

2. **Drain must clear backlog faster than production.** One frame drained per
   pipeline iteration while one frame is produced per iteration means backlog
   never shrinks. The drain loop must push *while* the socket accepts and
   budget remains — not one frame per tick. Worth stating explicitly because
   getting it wrong yields a queue that only ever grows.

3. **Shared-socket audio perturbs the pacing edge.** With
   `outgoing.audio_port == 0`, Opus shares the video socket
   (`resolve_shared_audio_target`, verified in #214). `SIOCOUTQ == 0` may then
   never be exactly true. Pace on a small epsilon derived from the learned
   `unix_capacity` rather than on exact zero. Needs a decision before Phase A
   coding.

4. **Mutual exclusion with `allowUnixEncoderStall`.** Stall mode means an
   unbounded block; pacing means never blocking. Enabling both is incoherent —
   refuse the combination at config validation and warn.

5. **Memory.** 3 MB when pacing is enabled, matching frame-shm's footprint.
   Confirm headroom on both targets.

6. **`TARGET_US` is provisional.** Stated as 10 ms in D5 for review; the
   binding value comes from Phase A measurement.

7. **Queue overflow is still frame loss.** The clamp exists so overflow never
   happens, exactly as on frame-shm. Overflow must be counted separately from
   `socket_drops` so the two causes stay distinguishable in telemetry.

8. **#214 must land first.** This builds on `OutputSocketQueue`,
   `output_socket_note_saturation`, the congestion-classified drop counters and
   the bounded flush. The branch is currently based on #214's head and will be
   rebased onto master once it merges.

## 8. What this does not change

- The wire format and the consumer's view of the socket.
- `video0.bitrate` in `/api/v1/config` — the clamp is a multiplier, so
  external rate controllers keep write-on-change coherence, as documented in
  `venc_shm_throttle.h`.
- `frame-shm://`, `shm://` and `udp://` behaviour.
- #214's flush budget and `SO_SNDTIMEO`, which stay as the backstop. With
  pacing working they should never fire; if they do, that is a bug or a wedged
  consumer.
