# Adaptive Frame Gate — Local Takeover Handoff

<!-- version: 4.0.0 -->

Written for a local agent taking over PR #287. Updated at the 2026-09-12
checkpoint after the CV610 bench pass and the SigmaStar BUSY diagnosis. No
device was modified during this final checkpoint. See §8 and §9 for exact
state and next commands.

| | |
|---|---|
| Local branch/worktree | `verify/pr-287` in `.claude/worktrees/pr-287-verify` |
| Remote PR branch | `origin/claude/dazzling-goldberg-o49554` (not pushed) |
| PR | snokvist/waybeam_venc#287 |
| Design | `documentation/ADAPTIVE_FRAME_GATE_PLAN.md` — read §7 (risks) and §9 (Phase 2 findings) |

---

## 0. Start here

```sh
cd /home/snokvist/dev/waybeam-coordination/waybeam_venc/.claude/worktrees/pr-287-verify
git status --short --branch
make test-ci          # expect 3115 passed / 0 failed
make verify           # Star6E + Maruko; toolchains auto-download on first run
```

CV610 linking and its primary R1/no-IDR trial are complete. Continue with the
unchecked recording/config transitions and the secondary SigmaStar matrix.

### The four things only this machine can do

1. Link CV610 — needs `~/dev/hisilicon` and the firmware lib output (§5).
2. Reach the current benches — Star6E `root@192.168.2.232`, Maruko
   `root@192.168.2.233`, CV610 `root@192.168.2.181`.
3. Prove R1 (`StopRecvPic` under a full egress ring) — §3.
4. Measure the defaults, which were reasoned rather than benched — §6.

## 1. What you are inheriting

`video0.frameGate` (default `off`, restart-required) pauses the stream VENC
channel's frame intake while the frame-shm egress ring is not draining, and
resumes it when the consumer catches up.

It exists because a bitrate write — the only proportional rate actuator on
SigmaStar — implicitly emits an IDR (`MI_VENC_SetChnAttr`; measured on
SSC338Q, ten spaced writes → eleven IRAP access units,
`src/star6e_controls.c:252`). So the normal response to a congested link
sends the largest frame in the stream into the link that is already
overflowing. The gate changes **no encoder state at all**, so ROI keeps its
relative QP gradient and CBR keeps its contract.

| | |
|---|---|
| Policy | `src/frame_gate.c` — pure state machine, no SDK types, 68 host assertions |
| Star6E | `src/star6e_runtime.c` — `star6e_service_frame_gate()` |
| Maruko | `src/maruko_pipeline.c` — `maruko_service_frame_gate()` |
| CV610 | `src/cv610_runtime.c` — `cv610_service_frame_gate()` |

Actuator per backend: `MI_VENC_Stop/StartRecvPic` on SigmaStar,
`ss_mpi_venc_stop_chn` / `ss_mpi_venc_start_chn(recv_pic_num = -1)` on CV610.

Defaults: close at `>= 3` slots, reopen at `<= 1` after a 20 ms minimum closed
dwell, safety escape at 500 ms. Normal reopening is **not** tied to 500 ms:
Star6E reads occupancy every ~1 ms while closed; Maruko and CV610 every ~2 ms.
Expected drain-to-video latency is that poll plus SDK start latency and about
one frame interval. The 500 ms path only admits a safety-pulse frame while a
dead/stalled consumer leaves occupancy above the reopen threshold.

## 2. What the cloud session did and did not verify

Trust the first column; re-establish the second yourself.

| Verified | NOT verified |
|---|---|
| `make verify` — Star6E + Maruko build **and link** | Any runtime behaviour, on any device |
| CV610: all 90 objects compile against real OpenHisilicon headers | CV610 **link** — vendor `.so` set absent |
| Host suite 3087/3 (68 new frame-gate assertions) | That the gate ever actually closes or reopens on hardware |
| `make lint` clean, all three backends | That `StopRecvPic` does not D-state (R1) |
| Policy logic exhaustively unit-tested | That defaults are the right numbers |

The cloud session built CV610 against `OpenIPC/openhisilicon` @ `916e767`
(2026-09-06), the tree the Makefile's `CV610_SDK_INC ?= ../openhisilicon`
default points at. If `~/dev/hisilicon` is a different revision, re-run
`make lint SOC_BUILD=cv610` against it before assuming the compile still
holds.

## 3. R1 — blocking. Do this before anything else

**Claim to disprove:** `MI_VENC_StopRecvPic` can hang when the egress ring is
full. `src/star6e_pipeline.c:2501` warns that a gap in consumption causes VPE
backpressure → kernel D-state → `StopRecvPic` hangs, and the gate's trigger
condition is adjacent to that.

**Why it is believed safe:** `venc_frame_ring_begin_write()` increments
`full_drops` and returns `-1` on a full ring
(`include/venc_frame_ring.h:424`) — the producer **drops, it does not
block** — so the output thread keeps calling `GetStream`/`ReleaseStream`,
VENC output never backs up, and VPE never backpressures. That is static
reasoning only. Prove it on hardware.

```sh
scripts/star6e_direct_deploy.sh cycle                        # root@192.168.1.13
curl "http://192.168.1.13/api/v1/set?video0.frameGate=on"    # restart-required
```

Stall the frame-shm consumer (SIGSTOP waybeam-link, or attach and stop
reading) so the ring fills, then confirm:

- the daemon logs the gate closing and does **not** wedge;
- `/tmp/waybeam.log` shows no `MMU_Callback` storm;
- `SIGCONT` → the gate reopens and video resumes.

**If it hangs, stop.** The approach is dead; say so in the PR and close it
rather than reworking around a kernel D-state. The fallback worth proposing
is `min_qp` as a binary panic switch — a cliff (§2 of the plan), but usable
for a pure emergency.

**Never `killall -9`** either bench device — `feedback_no_sigkill_sigmastar.md`.
Use SIGTERM, or sysrq-b for D-state recovery (`sysrq_b_zombie_recovery.md`).
Maruko especially wants `ssh -o ConnectTimeout=10` —
`feedback_maruko_ssh_timeout.md`.

## 4. Bench matrix

| # | Test | Pass condition |
|---|---|---|
| 1 | **R1** (§3) | `StopRecvPic` returns, no D-state, reopens on SIGCONT |
| 2 | Gate cycles under real congestion | close/reopen logged, no oscillation, video recovers |
| 3 | **No IDR on a gate cycle** | `/api/v1/idr/stats` unchanged across many cycles. This is the entire justification for the feature — if a cycle keyframes, it has no reason to exist |
| 4 | GDR survives a cycle (`resilience=racing`) | stripe cadence intact after reopen; `star6e_pipeline.c:1447` silently no-ops hierarchical attrs set after `StartRecvPic` |
| 5 | SVC-T survives a cycle (`resilience=range`) | layer structure intact, not flattened to single-layer |
| 6 | ROI gradient unaffected | center-priority QP delta unchanged across a cycle (it should be — the gate never touches `SetRoiCfg`) |
| 7 | Safety escape | kill the consumer → reopens after ~500 ms, `full_drops` climbs, stream does not stop |
| 8 | Mirror-mode recording | record in mirror mode + congest → no gate closes, file is clean |
| 9 | Recording started while gated | gate reopens, resulting file has no holes |
| 10 | SIGHUP reinit while gated | clean fork+exec respawn, no D-state |
| 11 | Non-frame-shm transport | `frameGate=on` on `udp://` warns at bring-up and stays inert |
| 12 | Maruko parity | repeat 1–3 and 7 on `root@192.168.2.12` |
| 13 | Maruko idle-abort guard | gate closed > 20 s (`frameGateMaxClosedMs=30000`) does **not** abort the stream loop. New guard, never exercised |
| 14 | CV610 | link (§5), then repeat 1–3, 7, 8 |

## 5. CV610 — link it first

CV610 is wired but has never been linked. Every `.o` builds; the final link
stops at `ld: cannot find -lacs` because the vendor `.so` set (`libss_mpi`,
`libacs`, `libbnr`, `libldci`, `libsecurec`, `libot_osal`, …) is a firmware
build output, not part of the headers tree.

```sh
make build SOC_BUILD=cv610 \
  CV610_SDK_INC=$HOME/dev/hisilicon \
  CV610_SDK_LIB=/path/to/firmware/output/target/usr/lib
```

The Makefile defaults are `CV610_SDK_INC ?= ../openhisilicon` and
`CV610_SDK_LIB ?= ../firmware/output/target/usr/lib`, so if your checkouts
sit beside the repo in that shape, a bare `make build SOC_BUILD=cv610` works
and you can drop both flags.

Two things to expect if the link surfaces anything: `src/frame_gate.c` was
missing from `CV610_SRC` and had to be added (Makefile line 83) — a missing
symbol there means that edit was lost. Otherwise the only new code is
`cv610_service_frame_gate()` and `cv610_report_frame_gate_setup()`.

**CV610-specific behaviour.** It records in mirror mode only (the stream loop
refuses every other `record.mode`), so the recorder always shares the gated
channel and the close-suppression always applies — test 8 matters more here
than on SigmaStar. Its `select()` wait drops from 1 s to 2 ms while gated.

## 6. Tuning to bring back

`frameGateCloseSlots` default 3 and `frameGateMaxClosedMs` default 500 were
reasoned from the ring header's own notes, **not measured**. Sweep both at 60
and 120 fps and record what the craft actually wants. If the defaults are
wrong, change them in `include/frame_gate.h` and say so in `HISTORY.md`.

Also worth measuring: duty cycle under sustained congestion.
`g->close_events` and `g->closed_total_us` are tracked but not exposed over
HTTP — plan §8 open question 2 asks whether they should be. If the answer is
yes, that is a small `/api/v1/status` addition and a `contract_version` bump.

## 7. Known-weak spots — look here first

- **Reopen paths.** Every path reachable with the gate closed must evaluate
  it, or the stream hangs. There are six: Star6E `curPacks == 0` and
  `Query`-failure; Maruko `!POLLIN` and `Query`-failure; CV610 the `select()`
  return and its `cur_packs == 0` / query-failure `continue`. **If video stops
  and never returns, one of these is the reason.**
- **Wait-timeout shortening.** Maruko's fd poll and CV610's `select()` both
  drop from 1 s to 2 ms while gated, because that timeout would otherwise BE
  the reopen latency. If reopens feel sluggish on either, check that.
- **`star6e_record_wants_frame()` polarity.** Closes are suppressed while a
  mirror recording runs. If the gate never fires with recording *off*, the
  predicate is inverted.
- **Mirror is the default record mode.** Easy to forget when a gate that
  "should" close does not.

## 8. State of the tree and bench results

- `make verify` passes: Star6E and Maruko build and link.
- CV610 compiles and links against the firmware vendor libraries. The linked
  0.85.0 binary is confirmed on `192.168.2.181`.
- `make test-ci`: **3115 passed, 0 failed** at the current checkpoint.
- **CV610 primary gate path confirmed on device:** 1280x720 at 100 fps,
  `resilience=racing`, frame-shm ring with eight slots. A stopped consumer
  closed intake at three slots; the 500 ms safety escape admitted one frame per
  pulse; resuming drained the ring and restored ~100 fps without a D-state or
  kernel fault.
- **CV610 no-IDR and continuity confirmed:** a direct AU consumer observed 542
  frames across a three-second gate interval, including seven safety-pulse
  frames. All 542 were GDR, zero were IDR/IRAP, Annex-B framing was clean, and
  PTS stayed monotonic. On the x86 ground hub the received rate fell from
  ~100 fps to 1.6 fps, then returned to ~100 fps; `incomplete_frames` stayed
  zero, `shm_waiting_for_idr` stayed false, and neither recovery requests nor
  source switches increased.
- **CV610 restart while gated confirmed:** graceful stop completed in one
  second with the ring held at the close threshold; the replacement process
  started with the consumer still stopped, exercised the safety escape, and
  recovered normally when consumption resumed. No D-state or new kernel fault.
- **CV610 mirror-recording rows are blocked by bench storage, not code:** the
  device has no mounted recording medium and `/tmp` has only 28 MiB free, so
  `/api/v1/record/start` correctly refused with `stop_reason=disk_full` even
  after temporarily lowering `record.maxMB`. Rows 8 and 9 remain unverified.
- Hardware verification found and fixed CV610 gate setup running before
  `cv610_output_start()` created the ring. Setup now follows successful output
  creation, and startup reports `frame gate on ... ring=8 slots` rather than
  declaring the gate inert.
- **Star6E preliminary:** the consumer-stall test paused and recovered without
  D-state, but `MI_VENC_StopRecvPic` returned `0xA0022012`. This decodes as
  `MI_ERR_VENC_BUSY`: the normal-path call was made before `ReleaseStream`.
  The local checkpoint moves it after release, but that fix is not yet tested
  on device.
- **Maruko:** code-level only. The same pre-release ordering existed and is
  moved after release locally; `.233` is now available but untested.
- Rebased on upstream 0.84.0; version bumped to 0.85.0. The
  `contract_version` remains at upstream's 0.31.0 because the frame gate adds
  three restart-required fields without changing an endpoint or payload.
- The local verification branch contains the rebased PR plus the CV610
  initialization-order fix, bench results, post-release ordering, and reopen
  retry hardening. The remote PR branch has not been rewritten. A future push
  needs explicit authorization and `--force-with-lease` because of the rebase.

## 9. When it passes

### Exact next-agent sequence

1. The checkpoint binaries are current: `make verify`, the full CV610 vendor
   link, strict lint for all three backends, and the host suite all passed.
   Re-run them after the next edit, not before deploying this checkpoint.
2. Deploy only to `.232`, `.233`, and `.181`; stop the vehicle-local
   `S97waybeam-hub` consumer during the stall interval. The x86 receiver is
   deliberately paused and must not be part of the evidence.
3. On each vehicle, run `tools/frame_shm_consumer_test` locally, SIGSTOP it long
   enough to close the gate, then SIGCONT it. Confirm no `BUSY`, D-state, fault,
   unexpected IDR/IRAP, bad Annex-B framing, or PTS regression. Measure
   drain-to-first-new-frame latency; it should be milliseconds, not 500 ms.
4. Repeat with the consumer left stalled for >500 ms to distinguish the safety
   pulse from normal drain-triggered reopening. Restore the vehicle-local hub
   after every trial.
5. Only after all three pass, make frame gating automatic for frame-shm and
   remove the public `video0.frameGate` switch. Keep the two tuning fields
   (`frameGateCloseSlots`, `frameGateMaxClosedMs`) unless evidence says they
   should also become constants. Removing the switch touches all config layers:
   `include/venc_config.h`, defaults/parser/printer/cJSON in `src/venc_config.c`,
   field/alias/UI in `src/venc_api.c`, all three default JSON files, backend
   setup calls, tests, README/HISTORY, and the HTTP contract.
6. Re-run `make test-ci`, `make verify`, the CV610 link, a config-layout grep,
   and the three vehicle trials with configs that do not contain `frameGate`.
7. Tick the PR runtime checks and report each result as **confirmed on device**
   or **code-level only**.

### Local code changes at this checkpoint

- All normal-path gate evaluations now occur after the acquired stream is
  released. Idle-path calls remain in place; they are the fast reopen path.
- `frame_gate_restore_closed()` rolls policy back after a refused OPEN so the
  actuator retries after 20 ms. Previously all three backends could believe
  they were open while hardware stayed stopped. A host test covers the retry.
- No protocol bytes changed. Do not add a reverse futex in this PR: frame-shm
  v2 only wakes consumer-from-producer; a drain wake would require coordinated
  changes to every consumer for little gain over the current 1–2 ms polling.
- Coordination `protocols/frame-shm.md` is stale versus upstream ring v2
  (`low_water_slots`/`other_drops`). That pre-existing drift is out of this PR's
  implementation scope, but should be corrected separately.

If the post-release call still returns BUSY on either SigmaStar board, capture
the exact code and logs and stop default-on work. Do not treat BUSY as success.

If R1 fails, close the PR with the finding rather than reworking it — and
keep the plan and this document, because the measurement is the valuable part.

---

## 10. SigmaStar bench results — 2026-09-12, local takeover

Run on `.232` (Star6E SSC338Q, 1280x720 @ 100 fps) and `.233` (Maruko
ssc378qe, 1080p @ 30 fps), both `resilience=racing` (GDR), frame-shm ring
of 8 slots, vehicle-local `S97waybeam-hub` stopped so
`tools/frame_shm_consumer_test` was the only consumer. Stall = SIGSTOP the
consumer; resume = SIGCONT.

### R1 is answered: PASS

`MI_VENC_StopRecvPic` does **not** hang and no longer returns BUSY.

- `StopRecvPic` returned 0 on every close. The `0xA0022012`
  (`MI_ERR_VENC_BUSY`) seen at the previous checkpoint is **fixed** by the
  post-`ReleaseStream` ordering in `e483e37` — confirmed on device.
- The daemon stayed `S`/`R` throughout; never `D`. No `MMU_Callback` storm,
  no kernel fault, no wedge.
- Gate closed at exactly `usedSlots == 3` and reopened on drain. `fps/live`
  returned to 100, ring drained to 0, **all drop counters stayed 0**.

Drain-to-reopen was prompt — recovery appeared in the consumer's very next
1 s print, not after the 500 ms escape.

### Row 3 FAILS on both SigmaStar boards — the feature's premise does not hold

Row 3 is the one the plan calls decisive: *"if a cycle keyframes, it has no
reason to exist."* Six identical stall cycles (2 s stalled / 3 s running)
inside one 40 s consumer window, gate on versus gate off, nothing else
changed:

| Board | 6 cycles, `frameGate=on` | 6 cycles, `frameGate=off` (control) |
|---|---|---|
| `.232` Star6E | **7 IDR** / 39.5 s | **1 IDR** / 39.9 s |
| `.233` Maruko | **22 IDR** / 39.2 s | **0 IDR** / 39.8 s |

On Star6E every reopen produced exactly one IDR; on Maruko every reopen
produced three. The controls show the natural rate in racing/GDR is ~1 IDR
per 40 s or less, so the excess is entirely attributable to the gate.

The reopen path calls **only** `MI_VENC_StartRecvPic()`
(`src/star6e_runtime.c:1625`) — no IDR request, no attribute write. The
keyframe is emitted by the SigmaStar SDK actuator itself, so no amount of
policy tuning avoids it.

**Consequence.** The gate was built because a bitrate write implicitly emits
an IDR, so pausing intake was supposed to shed load without one. On
SigmaStar, pausing intake emits an IDR too — while simply letting the ring
drop on full (the `frameGate=off` behaviour that already ships) sheds the
same load for **zero** IDRs, with `bad_meta=0`, `bad_startcode=0` and
`pts_regress=0` across the whole control run. On these two backends the gate
is therefore strictly worse than the status quo it was meant to improve.

This does not touch the CV610 result in §8, which measured zero IDR across a
gate interval — `ss_mpi_venc_start_chn()` evidently does not keyframe on
resume. The feature is SDK-dependent, not universally dead.

### Secondary findings

- **The 500 ms safety escape is defeated on Star6E.** `frame_gate_observe()`
  debounces only closed->open (`min_closed_us`, 20 ms); the open->closed
  direction has no dwell at all (`src/frame_gate.c:92`). The escape opens the
  gate, the idle path re-polls 1 ms later with occupancy still at the close
  threshold, and re-closes before the encoder can deliver a frame (10 ms at
  100 fps). Measured: `framesSent` advanced by exactly 300 per 5 s cycle —
  3 s of frames — so **zero** frames were admitted across each 2 s closed
  interval, and no `full_drops` were recorded either. A dead consumer
  therefore stops the stream *silently*, which is the outcome the escape's
  own comment says it exists to prevent. Maruko does pulse (~2-3 frames/s
  leaking as `transportDrops`), so this is a Star6E-specific race.
- **Maruko does not hold the ring at the close threshold.** Occupancy reached
  7-8 slots during every stall rather than stopping at 3, so the close lands
  late enough that the ring overflows anyway.
- **`/api/v1/idr/stats` cannot answer row 3.** It reports `channels: []` on a
  live stream because it counts only API-*requested* IDRs, not SDK-emitted
  ones. The check as written in row 3 would have returned a false PASS. The
  bitstream is the only truth; `tools/frame_shm_consumer_test` reads it.
- **`tools/frame_shm_consumer_test` reports `VERDICT: FAIL` on any healthy
  GDR stream.** Its `vcl_ok` requires `min_vcl == max_vcl`, but racing/GDR
  varies slice count (6 on GDR frames, 12 on an IDR). Integrity was clean in
  every run. Read the counters, not the verdict.

### What was NOT done

- Rows 4-6, 8-11, 13 were not reached: row 3 is the gate on the whole
  feature, and it failed on both SigmaStar backends.
- `.181` CV610 was not re-touched this session; its §8 results stand as the
  previous session recorded them.
- Both benches were restored: `.233` to its original 0.81.0 binary and
  config, `.232` left on the 0.85.0 checkpoint build with `frameGate=off`
  (its default). Hubs restarted, rings draining, `usedSlots` 0 on both.
