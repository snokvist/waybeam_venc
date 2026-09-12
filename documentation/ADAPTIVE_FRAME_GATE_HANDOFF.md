# Adaptive Frame Gate — Local Takeover Handoff

<!-- version: 2.1.0 -->

Written for a **Claude Code CLI session on the local machine** taking over
from the cloud session that wrote this feature. Updated after the 2026-09-12
bench pass: CV610 is linked and hardware-confirmed; Star6E has preliminary
hardware results; Maruko remains code-level only. See §8 for exact status.

| | |
|---|---|
| Branch | `claude/dazzling-goldberg-o49554` |
| PR | snokvist/waybeam_venc#287 |
| Design | `documentation/ADAPTIVE_FRAME_GATE_PLAN.md` — read §7 (risks) and §9 (Phase 2 findings) |

---

## 0. Start here

```sh
git fetch origin claude/dazzling-goldberg-o49554
git checkout claude/dazzling-goldberg-o49554
make test-ci          # expect 3112 passed / 0 failed
make verify           # Star6E + Maruko; toolchains auto-download on first run
```

CV610 linking and its primary R1/no-IDR trial are complete. Continue with the
unchecked recording/config transitions and the secondary SigmaStar matrix.

### The four things only this machine can do

1. Link CV610 — needs `~/dev/hisilicon` and the firmware lib output (§5).
2. Reach the bench devices — Star6E `root@192.168.1.13` (imx335), Maruko
   `root@192.168.2.12` (imx415).
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

Defaults: close at `>= 3` slots, reopen at `<= 1` after a 20 ms debounce,
safety escape at 500 ms.

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
- `make test-ci`: **3112 passed, 0 failed** after rebasing onto upstream.
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
  D-state, but `MI_VENC_StopRecvPic` returned `0xA0022012`. Treat Star6E support
  as unresolved until that return-code/state mismatch is understood.
- **Maruko:** code-level only; no device trial yet.
- Rebased on upstream 0.84.0; version bumped to 0.85.0. The
  `contract_version` remains at upstream's 0.31.0 because the frame gate adds
  three restart-required fields without changing an endpoint or payload.
- The local verification branch contains the rebased PR plus the CV610
  initialization-order fix and these bench results; update the PR branch only
  after reviewing that diff.

## 9. When it passes

1. Tick the runtime smoke-check boxes in the PR and note which backends were
   exercised.
2. Fold any default changes from §6 into `include/frame_gate.h`.
3. Complete the two mirror-recording rows on a CV610 with writable media, then
   decide whether CV610 confirmation is sufficient to merge with SigmaStar
   still experimental.

If R1 fails, close the PR with the finding rather than reworking it — and
keep the plan and this document, because the measurement is the valuable part.
