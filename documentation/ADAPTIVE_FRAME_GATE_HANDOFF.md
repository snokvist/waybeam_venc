# Adaptive Frame Gate — Local Takeover Handoff

<!-- version: 7.0.0 -->

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

---

## 11. The way around it on SigmaStar: `MI_VENC_SetRcParam`

Row 3 kills the *actuator*, not the *idea*. The ring-occupancy signal the gate
computes is fine; what costs an IDR is `MI_VENC_StartRecvPic`. So the question
became: is there another way to shed rate on SigmaStar that does not route
through `MI_VENC_SetChnAttr` (implicit IDR, §1) or through intake stop/resume
(implicit IDR, §10)?

### What the SDK offers

From the OpenIPC HAL bindings in `sdk/ssc338q/hal/star/` — `i6_venc.h`
(Star6E) and `i6c_venc.h` (Maruko), both reverse-engineered against the real
`libmi_venc.so`:

- **`MI_VENC_StartRecvPicEx(chn, int *count)`** — present on both. Meters
  intake by frame count, the same semantic as CV610's
  `ss_mpi_venc_start_chn(recv_pic_num)`. Still a resume, so it was not
  pursued ahead of the option below.
- **`MI_VENC_SetInputSourceConfig(chn, src_conf*)`** — `NORMAL`, `RING_ONE`,
  `RING_HALF` (Maruko adds `HW_SYNC`, `RING_DMA`). This selects the input ring
  coupling mode for latency, not an intake throttle. Dead end.
- **`MI_VENC_SetRcParam(chn, MI_VENC_RcParam_t*)`** — the one that matters.

### `SetRcParam` does not keyframe — and this repo already knew

`src/star6e_controls.c:442` states it outright: *"Unlike apply_bitrate() this
is a real reduction — MI_VENC_SetRcParam does NOT implicitly keyframe."*
Measured 2026-08-23, ten spaced `video0.qpDelta` writes went from eleven IRAP
access units to one.

Re-confirmed on the 0.85.0 checkpoint build, `.232`, 2026-09-12: **eleven
distinct `SetRcParam` writes across five runs produced zero IDR frames**,
and `fps/live` never left 100-101 — the stream is never interrupted at all.

### It is a real lever, and it has a proportional band

`.232`, 720p100, H.265 CBR, racing/GDR, static bench scene, 8 s samples,
`video0.minQp` swept through `MI_VENC_SetRcParam`:

| `minQp` | Mbit/s | vs unclamped | IDR |
|---|---|---|---|
| 0 (driver default) | 5.74 | 1.00x | 0 |
| 20 | 2.67 | 0.47x | 0 |
| 23 | 0.84 | 0.15x | 0 |
| 26 | 0.35 | 0.06x | 0 |
| 29 | 0.17 | 0.03x | 0 |
| 30 | 0.17 | 0.03x | 0 |
| 34-45 | 0.12-0.13 | 0.02x | 0 |

Monotonic 16:1 through the 20-29 band, then saturated. The plan's §2 called
`min_qp` "a cliff, usable only as a binary panic switch" — that is half right:
it *is* steep (~6 dB per 3 QP steps, which is simply what QP does) and it
saturates above 30, but 20-29 is a usable continuous band, not a cliff.

### Why this beats the gate on SigmaStar

| | frame gate | `SetRcParam` clamp |
|---|---|---|
| IDR per actuation | 1 (Star6E) / 3 (Maruko) | **0** |
| fps while shedding | 0 (intake stopped) | **unchanged, ~100** |
| Control shape | binary open/closed | proportional |
| Failure modes | BUSY, D-state risk, six reopen paths, 500 ms escape | one call, no state |
| GDR refresh wave | broken by the resume keyframe | continuous |

Frames keep flowing, so the decoder never starves and the refresh wave is
never interrupted — the gate's whole hazard surface (§7) disappears.

**Caveat.** The absolute QP values are scene-dependent; this was a static
bench scene and a moving scene will move the band
(`feedback_moving_scene_required_for_venc_rate_tests`). That argues for a
closed loop driven by ring occupancy rather than a fixed QP table.

### Recommended shape

1. **Keep the gate for CV610.** `ss_mpi_venc_start_chn()` measured zero IDR on
   resume (§8), so the feature works as designed there. Scope `video0.frameGate`
   to the CV610 backend and report it inert elsewhere.
2. **On Star6E and Maruko, keep `frame_gate_observe()`'s occupancy signal but
   change the actuator** — drive an RC clamp through `MI_VENC_SetRcParam`
   instead of `Stop/StartRecvPic`. The policy, its thresholds and its 68 host
   assertions survive; only the two backend `service_frame_gate()` bodies
   change, and the six reopen paths and the safety escape all become
   unnecessary.
3. **Prefer a byte budget to a QP floor.** `MI_VENC_RcParam_t` carries
   `u32MaxPSize` / `u32MaxISize` on both SigmaStar backends
   (`include/star6e.h:395-440`), and `rc_commit_intent()` already does a
   read-modify-write so they survive every other RC write. A per-frame byte cap
   is scene-independent and is exactly the quantity a ring-occupancy controller
   wants, where a QP floor has to be re-found per scene. These were exposed as
   `video0.maxIBytes` / `video0.maxPBytes` in an earlier build and measured as
   a hard proportional lever (4096 B cap = 4.9 Mbps, uncapped = 13.0 Mbps,
   same scene, `.232` 2026-08-03); they are not in the 0.85.0 config surface.
   Re-exposing them is the natural next step and should be measured for IDR
   behaviour the same way.

---

## 12. The real vendor SDK — three purpose-built mechanisms

`/home/snokvist/dev/SDK_IPCAM` holds the actual SigmaStar releases, and they
answer this far better than the reverse-engineered HAL headers in §11:

- Star6E: `star6e_SDK/project-Pudding-ILC02V009/project/release/include/` —
  `mi_venc.h`, `mi_venc_datatype.h`, `mi_venc_user_rc.h`
- Maruko: `Maruko/SourceCode/project/project/release/include/` — same set
- Official docs: `SGS_IPU_SDK/.../API 说明文档/API/MI_VENC_API.pdf`
  (MI VENC API v2.12); extract with `pdftotext -layout`

The in-repo OpenIPC HAL binds ~20 VENC calls. **The real SDK exposes 80**, and
the ones that matter here were simply never bound. All layouts below are
**identical on Star6E and Maruko**, and all six symbols are **exported by the
shipped `/usr/lib/libmi_venc.so` on both `.232` and `.233`** (verified
2026-09-12), so none of this needs a firmware change.

### 12.1 `MI_VENC_SetFrameLostStrategy` — the purpose-built primitive

```c
typedef struct {
    MI_BOOL                 bFrmLostOpen;      /* switch */
    MI_U32                  u32FrmLostBpsThr;  /* threshold, bit/s */
    MI_VENC_FrameLostMode_e eFrmLostMode;      /* NORMAL | PSKIP */
    MI_U32                  u32EncFrmGaps;     /* spread evenly, not in bursts */
} MI_VENC_ParamFrameLost_t;
```

This *is* the frame gate, built into the SDK: "配置瞬时码率超出阈值时丢帧策略"
— shed load when the instantaneous bitrate exceeds a threshold. The doc says a
call during encoding "takes effect at the next frame", and `u32EncFrmGaps`
spreads the shedding evenly instead of dropping a burst.

`E_MI_VENC_FRMLOST_PSKIP` is the important mode: instead of dropping the frame,
the encoder emits a **P-skip frame** — a few bytes saying "same as reference".
So the bitstream has **no gap**, PTS stays continuous, the decoder never
starves, the GDR wave keeps advancing, and it is a P frame, so **no IDR**. That
is every property the gate wanted, natively, with a threshold in bit/s that a
ring-occupancy controller can drive directly.

### 12.2 `MI_VENC_EnableIdr` — suppress the resume keyframe outright

```c
MI_S32 MI_VENC_EnableIdr(MI_VENC_CHN VeChn, MI_BOOL bEnableIdr);
```

Doc, §1.3.19: *"若不使能 IDR 帧，则在下一帧之后都编不出 IDR 帧或 I 帧，直到再次使能为止"*
— with IDR disabled, no IDR or I frame is encoded from the next frame onward
until it is re-enabled. H.264/H.265 only.

That is a direct answer to §10: bracket the gate's reopen with
`EnableIdr(FALSE)` / `StartRecvPic` / `EnableIdr(TRUE)` and the resume keyframe
should never be emitted. It keeps the existing gate design intact. Two things
to prove on device: that it actually suppresses *this* IDR (the SDK may treat a
receive-restart keyframe as mandatory), and where to re-enable — too early and
the IDR returns, too late and a genuine recovery request is swallowed.

### 12.3 `MI_VENC_SetSuperFrameCfg` — RULED OUT (latency)

```c
typedef struct {
    MI_VENC_SuperFrmMode_e eSuperFrmMode;        /* NONE | DISCARD | REENCODE */
    MI_U32 u32SuperIFrmBitsThr;
    MI_U32 u32SuperPFrmBitsThr;
    MI_U32 u32SuperBFrmBitsThr;
} MI_VENC_SuperFrameCfg_t;
```

§1's complaint is that the response to congestion sends *the largest frame in
the stream* into an already-overflowing link. This caps that case at its
source: any frame over the per-type bit threshold is discarded or re-encoded.

### 12.4 Also unbound and worth knowing

`MI_VENC_SetRcPriority` (`BITRATE_FIRST` vs `FRAMEBITS_FIRST`),
`MI_VENC_SetRoiBgFrameRate` (background frame rate outside ROI),
`MI_VENC_AllocCustomMap`/`ApplyCustomMap` (per-CTU QP map),
`MI_VENC_SetAdvCustRcAttr` + `mi_venc_user_rc.h` (user-defined RC),
`MI_VENC_StartRecvPicEx(chn, {s32RecvPicNum})` (meter intake by frame count —
the direct analogue of CV610's `recv_pic_num`).

### 12.5 Suggested order of attack

1. **`FRMLOST_PSKIP`** — the native mechanism, no gap, no IDR, threshold in
   bit/s. Bind it, drive `u32FrmLostBpsThr` from ring occupancy, and the frame
   gate's whole state machine becomes unnecessary on SigmaStar.
2. **`EnableIdr(FALSE)` around the reopen** — smallest change if the gate as
   built is worth keeping; needs the two device proofs in §12.2.
3. **`SetRcParam`** (§11) — already measured IDR-free, already wired, good
   fallback and useful as the slow outer loop.
4. ~~`SuperFrameCfg`~~ — ruled out on latency grounds, see §12.3.

Measure each the same way §10 did: gate the bitstream through
`tools/frame_shm_consumer_test`, count IDRs against a no-change control, and
never trust `/api/v1/idr/stats` for this question.

---

## 13. All four SigmaStar mechanisms, measured — pros and cons

Everything in §12 was tried on hardware. Method throughout: gate the **wire**,
not the metadata — `tools/frame_shm_consumer_test` with a raw Annex-B dump, NAL
histogram via `scripts/hevc_analyze.py walk`, always against a no-change
control. `SuperFrameCfg` was dropped on the operator's call: `REENCODE` adds a
re-encode pass and `DISCARD` drops a whole frame, and neither belongs in a
latency-critical FPV path.

### Scoreboard

| Mechanism | Star6E `.232` | Maruko `.233` | Verdict |
|---|---|---|---|
| Frame gate (`Stop`/`StartRecvPic`) | works, **1 IDR per reopen** | works, **3 IDR per reopen** | rejected — §10 |
| `EnableIdr(FALSE)` | resume IDR **unchanged** | not retested | does not rescue the gate |
| `FrameLostStrategy` PSKIP | **`E_MI_ERR_NOT_SUPPORT`** | accepted, **no-op** | unusable |
| `FrameLostStrategy` NORMAL | accepted, **no-op** | accepted, **no-op** | unusable |
| `SetRcParam` (QP) | **0 IDR, fps unbroken, 16:1** | not retested | **the one that works** |

### 13.1 `MI_VENC_EnableIdr` — reduces parameter sets, does not stop keyframes

The headline question was whether it also suppresses the IDR a bitrate write
emits. Four spaced `video0.bitrate` writes per arm, 18 s captures, NAL
histogram:

| | `EnableIdr=TRUE` | `EnableIdr=FALSE` |
|---|---|---|
| IDR_W_RADL | 42 | 24 |
| VPS / SPS / PPS | 12 each | 4 each |

And on the path that actually matters, four gate cycles per arm:

| | `EnableIdr=TRUE` | `EnableIdr=FALSE` |
|---|---|---|
| IDR_W_RADL | 12 | **12** |
| VPS / SPS / PPS | 8 each | **2** each |

**Pros:** cuts VPS/SPS/PPS re-emission consistently (12→4, 8→2), which is real
if small bandwidth. Cheap, one call, no state.
**Cons:** does **not** suppress the `StartRecvPic` resume keyframe at all —
12 versus 12. On bitrate writes it roughly halves IRAPs but does not eliminate
them, consistent with a CBR reconfiguration forcing a mandatory resync point
the flag cannot override. And while disabled it would also swallow a genuine
recovery IDR.
**Verdict:** not the rescue. Worth keeping in mind only as a parameter-set
trimmer.

*Trap this exposed:* the consumer's `IDR frames` counter reads the producer's
`VENC_FRAME_FLAG_IDR` metadata bit, not the bitstream. In the disabled arm the
metadata still said "6 IDRs" while the parsed slice count fell from 12 to 6 —
two observation channels disagreeing. Only the NAL histogram settles it.

### 13.2 `MI_VENC_SetFrameLostStrategy` — exported, accepted, inert

The most promising candidate on paper, and the biggest disappointment.

- **Star6E:** `E_MI_VENC_FRMLOST_PSKIP` is refused outright with
  `0xA0022008` = `E_MI_ERR_NOT_SUPPORT`. `NORMAL` is accepted and does nothing.
- **Maruko:** both modes accepted, both do nothing.

Pushed hard on both boards — `u32FrmLostBpsThr = 1000` (1 kbit/s) against a
live 7 Mbit/s stream on Star6E and 19.6 Mbit/s on Maruko, with
`u32EncFrmGaps` 0, 1 and 3:

| Board | threshold | measured | fps |
|---|---|---|---|
| Star6E | 1 kbit/s | 7.03 Mbit/s | 101 |
| Maruko | 1 kbit/s | 19.58 Mbit/s | 31 |

Not a partial effect — no effect. The struct layout is right (the 16-byte
`_Static_assert` passes and the SDK validates the enum, since it rejects PSKIP
by *name* on Star6E). No demo in either vendor SDK calls this function; only
the RTOS linker maps mention it.

**Pros:** would have been ideal — threshold in bit/s, no gap in PSKIP mode, no
IDR, effective at the next frame.
**Cons:** it does not work on either board. Maruko is the worse failure of the
two: it returns **success** for both modes and silently does nothing, so a
return-code check alone would have shipped a dead actuator.
**Verdict:** unusable. Do not build on it without re-measuring on the exact
silicon, from the wire.

### 13.3 `MI_VENC_SetRcParam` — still the only thing that works

Unchanged from §11: eleven writes, zero IDRs, fps never left 100-101, and a
monotonic 16:1 range through `minQp` 20-29.

**Pros:** IDR-free; never interrupts the stream, so the decoder never starves
and the GDR wave keeps advancing; proportional; one call; already wired and
already unit-covered; no reopen paths, no escape, no BUSY, no D-state risk.
**Cons:** QP is scene-dependent, so the usable band moves with content — the
numbers above are from a static bench scene. It is steep (~6 dB per 3 QP) and
saturates above QP 30. It trades quality for rate, where the gate traded
frames for rate.
**Verdict:** the recommendation stands, and it is now the *only* surviving
candidate on SigmaStar.

### 13.4 Where this leaves the PR

- **CV610 keeps the frame gate.** `ss_mpi_venc_start_chn()` does not keyframe
  on resume (§8), so the feature works there as designed.
- **Star6E and Maruko get an RcParam clamp** driven by the same
  `frame_gate_observe()` occupancy signal. The policy and its 68 host
  assertions survive; the six reopen paths and the 500 ms escape do not.
- **Next measurement, not yet done:** re-expose `u32MaxPSize` /
  `u32MaxISize` (§11.3) and check from the wire whether a per-frame byte
  budget is also IDR-free. It is the scene-independent quantity a
  ring-occupancy controller actually wants, and unlike the two dead ends above
  it is known to bind hard.

### 13.5 Test hooks in this tree

`GET /api/v1/idr/enable?on=0|1` and
`GET /api/v1/frmlost?on=&bps=&mode=pskip|normal&gaps=` exist on both SigmaStar
backends, plus `star6e_controls_enable_idr()`,
`star6e_controls_frame_lost()` and their Maruko twins, and the
`MI_VENC_EnableIdr` / `MI_VENC_SetFrameLostStrategy` bindings.

**These are investigation scaffolding, not merge candidates.** They are kept
because they are what any re-measurement needs. Strip them, or gate them behind
a debug build, before this branch goes near main.
