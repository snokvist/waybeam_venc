# Adaptive Frame Gate — Bench Handoff

<!-- version: 1.0.0 -->

For the Claude Code CLI session that finishes this. All three backends are
wired, Star6E and Maruko build and link, CV610 compiles, and the host suite
passes — **but nothing has run on silicon.** This session had no device
access.

Branch: `claude/dazzling-goldberg-o49554`
Design: `documentation/ADAPTIVE_FRAME_GATE_PLAN.md` (read §7 and §9 first)

---

## What you are inheriting

`video0.frameGate` (default `off`, restart-required) pauses the stream VENC
channel's frame intake via `MI_VENC_StopRecvPic` while the frame-shm egress
ring is not draining, and resumes it on `MI_VENC_StartRecvPic`. It exists
because a bitrate write — the only proportional rate actuator on SigmaStar —
implicitly emits an IDR, so the normal response to congestion makes
congestion worse. The gate changes no encoder state at all.

| | |
|---|---|
| Policy | `src/frame_gate.c` — pure state machine, no SDK types, 68 host assertions |
| Star6E wiring | `src/star6e_runtime.c` — `star6e_service_frame_gate()` |
| Maruko wiring | `src/maruko_pipeline.c` — `maruko_service_frame_gate()` |
| CV610 wiring | `src/cv610_runtime.c` — `cv610_service_frame_gate()`; compile-verified, not link-verified (see §5) |

Defaults: close at `>= 3` slots, reopen at `<= 1` after a 20 ms debounce,
safety escape at 500 ms.

## 1. R1 is a blocking gate. Do this before anything else

**Claim to disprove:** `MI_VENC_StopRecvPic` can hang when the egress ring is
full, because `src/star6e_pipeline.c:2501` warns that a gap in consumption
causes VPE backpressure → kernel D-state → `StopRecvPic` hangs. The gate's
trigger condition is adjacent to that.

**Why it is believed safe:** `venc_frame_ring_begin_write()` increments
`full_drops` and returns `-1` on a full ring
(`include/venc_frame_ring.h:424`) — the producer **drops, it does not
block** — so the output thread keeps calling `GetStream`/`ReleaseStream`,
VENC output never backs up, and VPE never backpressures. That is static
reasoning only. Prove it.

```sh
scripts/star6e_direct_deploy.sh cycle          # root@192.168.1.13, imx335
curl "http://192.168.1.13/api/v1/set?video0.frameGate=on"   # restart-required
```

Then stall the frame-shm consumer (SIGSTOP waybeam-link, or attach and stop
reading) so the ring fills, and confirm:

- the daemon logs the gate closing and does **not** wedge;
- `/tmp/waybeam.log` shows no `MMU_Callback` storm;
- `SIGCONT` the consumer → the gate reopens and video resumes.

**If `StopRecvPic` hangs, stop.** The approach is dead and the fallback is
`min_qp` as a binary panic switch (a cliff, but usable for a pure
emergency). Do not try to work around a D-state.

**Never `killall -9`** either bench device — `feedback_no_sigkill_sigmastar.md`.
Use SIGTERM, or sysrq-b for D-state recovery (`sysrq_b_zombie_recovery.md`).

## 2. The rest of the bench matrix

| # | Test | Pass condition |
|---|---|---|
| 2 | Gate cycles under real congestion | close/reopen logged, no oscillation, video recovers |
| 3 | **No IDR on a gate cycle** | `/api/v1/idr/stats` unchanged across many cycles. This is the entire point of the feature — if the cycle keyframes, the feature has no reason to exist |
| 4 | GDR survives a cycle (`resilience=racing`) | stripe cadence intact after reopen; see R2, `star6e_pipeline.c:1447` |
| 5 | SVC-T survives a cycle (`resilience=range`) | layer structure intact, not flattened to single-layer |
| 6 | ROI gradient unaffected | center-priority QP delta unchanged across a cycle (it should be — the gate never touches `SetRoiCfg`) |
| 7 | Safety escape | stop the consumer permanently → reopens after ~500 ms, `full_drops` climbs, stream does not stop |
| 8 | Mirror-mode recording | record in mirror mode + congest → no gate closes, file is clean |
| 9 | Recording started while gated | gate reopens, resulting file has no holes (R3) |
| 10 | SIGHUP reinit while gated | clean fork+exec respawn, no D-state |
| 11 | Non-frame-shm transport | `frameGate=on` on udp:// warns at bring-up and stays inert |
| 12 | Maruko parity | repeat 1–3 and 7 on `root@192.168.2.12` (imx415). Use `-o ConnectTimeout=10` — `feedback_maruko_ssh_timeout.md` |
| 13 | Maruko idle-abort guard | gate closed > 20 s (set `frameGateMaxClosedMs=30000`) does **not** abort the stream loop. This guard is new and untested |
| 14 | CV610 link + repeat 1–3, 7, 8 | first `make build SOC_BUILD=cv610` against real vendor libs (§5), then the same gates as Star6E |

## 3. Tuning to bring back

`frameGateCloseSlots` default 3 and `frameGateMaxClosedMs` default 500 were
reasoned from the ring header's own notes, not measured. Sweep them at 60
and 120 fps and record what the craft actually wants. If the defaults are
wrong, change them in `include/frame_gate.h` and say so in `HISTORY.md`.

Worth measuring while you are there: the duty cycle under sustained
congestion (`g->close_events`, `g->closed_total_us` are tracked but not yet
exposed over HTTP — plan §8 open question 2 asks whether they should be).

## 4. Known-weak spots — look here first if something is off

- **Reopen paths.** Every path reachable with the gate closed must evaluate
  it, or the stream hangs. There are six: Star6E `curPacks == 0` and
  `Query`-failure; Maruko `!POLLIN` and `Query`-failure; CV610 the `select()`
  return and its `cur_packs == 0` / query-failure `continue`. If video stops
  and never returns, one of these is the reason.
- **Wait-timeout shortening.** Maruko's fd poll and CV610's `select()` both
  drop from 1 s to 2 ms while gated, because that timeout would otherwise BE
  the reopen latency. If reopens feel sluggish on either, check that.
- **`star6e_record_wants_frame()` polarity.** Closes are suppressed while a
  mirror recording runs. If the gate never fires with recording off, the
  predicate is inverted.

## 5. CV610: wired, compile-verified, never linked

CV610 is now wired. It uses `ss_mpi_venc_stop_chn` /
`ss_mpi_venc_start_chn(recv_pic_num = -1)` — the same primitive the SigmaStar
backends reach as `MI_VENC_Stop/StartRecvPic`, and exactly what `ldy_sky`, the
vendor FPV streamer for this silicon, does. Its `select()` wait drops from
1 s to 2 ms while gated, for the same reason Maruko's poll does.

**What was verified here:** every CV610 object compiles against the real
OpenHisilicon headers (`OpenIPC/openhisilicon`, cloned to
`/home/user/openipc/openhisilicon`), `src/frame_gate.c` included.

**What was NOT verified:** the link. The vendor `.so` set (`libss_mpi`,
`libacs`, `libbnr`, `libldci`, `libsecurec`, `libot_osal`, …) is a firmware
build output that was not available, so `make build SOC_BUILD=cv610` stops at
`ld: cannot find -lacs`. Every `.o` is produced first. Link it yourself:

```sh
make build SOC_BUILD=cv610 \
  CV610_SDK_INC=/path/to/openhisilicon \
  CV610_SDK_LIB=/path/to/firmware/output/target/usr/lib
```

**CV610-specific bench notes.** It records in mirror mode only (the stream
loop refuses every other `record.mode`), so the recorder always shares the
gated channel and the close-suppression always applies — test 8 matters more
here than on the SigmaStar backends. Its reopen paths are the `select()`
timeout and the `cur_packs == 0` / query-failure `continue`; if video stops
and never returns on CV610, look there first.

## 6. State of the tree

- `make verify` passes: Star6E and Maruko both build and link.
- CV610 compiles (all 90 objects) but was not linked — see §5.
- `make test-ci`: **3087 passed, 3 failed.** The three failures are
  `rotfail no file open` / `rotfail recording cleared` /
  `rotfail reason recorded` — recorder-rotation tests that were **already
  failing at this branch point**, unrelated to the gate. Baseline before any
  of this work was 3019/3. Do not attribute them to this change, and do not
  let them mask a new one.
- Version bumped 0.82.0 → 0.83.0; `contract_version` unchanged at 0.29.0
  (three new fields, no endpoint or payload change).

## 7. When it passes

Update `HISTORY.md` — the 0.83.0 entry currently says **"Not yet
bench-verified"** in bold. Replace that with what was actually measured
(device, sensor, resolution, fps, and the IDR count across gate cycles, as
the surrounding entries do). Then the PR is ready to merge.

If R1 fails, say so in the PR and close it rather than reworking around a
kernel D-state.
