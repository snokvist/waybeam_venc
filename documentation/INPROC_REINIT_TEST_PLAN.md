# In-Process Reinit vs. Fork+Exec Respawn — Test Plan

> **Status: Phase 1 (Spec).**  This document defines an *experiment*, not a
> shipped behaviour change.  The proven fork+exec respawn remains the
> default; the in-process path is gated behind the `VENC_INPROC_REINIT`
> environment variable so it can be A/B-tested on the bench and reverted
> instantly without a rebuild.

## Goal

Re-test the "old truth" that drove the fork+exec respawn design: that an
in-process pipeline rebuild on a `MUT_RESTART` config change wedges the
SigmaStar SoC.  Since that design landed, the tree gained a set of fd /
teardown-ordering fixes (PR #122 VENC teardown reorder, PR #126
`debug_osd_destroy` detach→destroy settle, the size-change cold-vif fd
scrub).  If those fixes closed the failure modes that forced the respawn,
we can keep a **static PID across all API calls** and rebuild only the
pipeline in place.

This is a *measurement*, not a migration.  The deliverable is evidence:
either the in-process path survives the validation matrix below (and we
plan a follow-up to make it the default), or it wedges and we keep
fork+exec with this document recording exactly where it failed.

## Background — the three historical failure modes

The respawn exists because in-process reinit hit three distinct walls.
Each has a different status going into this test:

| # | Failure mode | Evidence | Status |
|---|---|---|---|
| 1 | `MI_SYS_Exit` + `MI_SYS_Init` in the same PID → driver keeps "already_inited" PID-tied flags → 2nd `MI_SYS_Init` hangs `MI_DEVICE_Open`, VPE "no wakeup event for 5 s" | `SIGHUP_REINIT.md`, `FULL_TEARDOWN_REINIT_PLAN.md` | **Not addressed.**  The test path therefore **never cycles `MI_SYS`** — `MI_SYS_Init`/`MI_SYS_Exit` stay in `runner_init`/`runner_teardown` only. |
| 2 | Partial reinit without `MI_SYS_Exit` → VIF degrades after ~4 cycles (`layout type 2 bindmode 4 not sync err`); cycling `MI_AI_Disable`/CUS3A-enable deadlocks `CamOsMutexLock` | `SIGHUP_REINIT.md`, `CRASH_LOG.md` 2026-04-26 | Mitigated by the `g_ai_persist` / `g_isp_initialized` / `g_last_isp_bin_path` persist hacks — **but those assume a fresh PID follows.**  Reusing them in a static PID is exactly what this test stresses. |
| 3 | MMU read-fault storm (client `0x15`, `IsWrite=0`) on the ~2nd consecutive VENC channel destroy+rebind → HW watchdog reset | `CRASH_LOG.md` 2026-05-21, PR #123 | **Contradiction in the tree.**  `star6e_runtime.c` claims it is "now fixed at its root" via the OSD settle; `CRASH_LOG.md` 2026-05-21 says PR #123 proved it is "intrinsic … independent of process model — an in-process rebuild storms too."  **Resolving this empirically is the primary objective.** |

Failure mode #3 is the gate.  If the MMU storm is genuinely
process-model-independent, the static-PID path storms on the 2nd
consecutive same-mode reinit exactly as the 2nd respawn does, and the
experiment ends there.

## Design of the experiment

### Switch

`VENC_INPROC_REINIT=1` in the daemon's environment selects the in-process
path.  Unset (default) keeps fork+exec respawn.  The env var is read once
and cached.  Star6E only — Maruko keeps respawn unconditionally until
Star6E validates (backend-split policy).

### In-process reinit path (`star6e_runtime_inproc_reinit`)

Triggered from `star6e_runtime_handle_reinit()` when the env switch is on.
Runs entirely inside the run loop — `g_running` stays 1, the process never
exits, the PID never changes:

1. Fork a **deadline watchdog** child (`waybeam-rwd`): sleeps the reinit
   deadline, then `sysrq-b` if still alive.  The parent SIGKILLs+reaps it
   on success.  This is the D-state safety net (a plain `alarm()`+`_exit`
   cannot escape an uninterruptible kernel lock — the same reason the
   shutdown path forks a watchdog).
2. `venc_httpd_pause()` — drain in-flight handlers; new requests get 503
   during the rebuild window.
3. Pipeline-level teardown via the shared `star6e_runner_pipeline_teardown()`
   helper: cus3a stop → iq cleanup → controls reset → `star6e_pipeline_stop`
   → cus3a join → recorder/ts-recorder stop → audio ring destroy.
   **`MI_SYS_Exit`, `mi_deinit`, and `venc_httpd_stop` are deliberately
   NOT called** — those stay process-lifetime (failure mode #1).
4. Reload config: `venc_config_defaults()` + `venc_config_load(/etc/waybeam.json)`
   so the `MUT_RESTART` field that triggered the reinit takes effect.
5. `star6e_pipeline_start()` → snapshot `started_base_{w,h}` →
   `star6e_runtime_apply_startup_controls()` (mirrors the cold-start
   ordering in `star6e_runner_init`).
6. SIGKILL+reap the watchdog, `venc_httpd_resume()`.

On any non-zero return from step 5, the function returns the error; the
run loop propagates it and the process exits non-zero.  **Note:** unlike
the respawn path, S95waybeam does *not* auto-restart on exit, so an
in-process reinit failure leaves the daemon **down** — the test harness
must detect "HTTP unreachable" and restart via `S95waybeam start`.

### Shared teardown helper

`star6e_runner_teardown()` is refactored to call the new
`star6e_runner_pipeline_teardown()` for the pipeline subset, then layer on
the process-exit-only steps (watchdog fork, `venc_httpd_stop`,
`MI_SYS_Exit`, `mi_deinit`).  This keeps the two teardown paths from
drifting.

### What stays on the respawn path

The `video0.size` cold-vif fd scrub (`venc_respawn_set_cold_vif`) is
respawn-only — it closes inherited `/dev/mi_vif` + `/dev/mi_vpe` fds
*before execv*, which has no analogue inside a live process.  In the
in-process path, `star6e_pipeline_stop` tears VIF/VPE down and
`star6e_pipeline_start` rebuilds them in the same PID; whether that
survives a sensor-mode (size) change is one of the things under test
(cross-mode matrix below).

### Known minor limitations of the test path (not blockers)

- The one-shot legacy-AE cold-boot fps re-kick (`legacy_fps_kick_done`, a
  local in `star6e_runner_run`) does not re-fire after an in-process
  reinit.  The CUS3A thread does its own frame-15 kick, so this only
  affects `isp.aeEngine="sdk"` (legacy) runs.

## Validation matrix

Bench: Star6E SSC338Q + IMX335 @ `root@192.168.1.13`.  Deploy with
`VENC_INPROC_REINIT=1` in the environment, e.g.:

```bash
ssh root@192.168.1.13 "killall waybeam; sleep 2; \
  VENC_INPROC_REINIT=1 nohup waybeam > /tmp/waybeam.log 2>&1 &"
```

Run each step against the in-process build, then re-run the same step on
the default (respawn) build as the control.

| # | Test | Pass criteria |
|---|------|---------------|
| 0 | **PID-static assertion** | `pidof waybeam` is identical before and after every API call in steps 1–4.  This is the whole point — if the PID changes, the in-process path did not engage. |
| 1 | **Same-mode storm (the gate)** | 5+ rapid `MUT_RESTART` SETs in one sensor mode (e.g. `video0.rc_mode`, `audio.enabled`, resilience/zoom canaries).  Watch for the MMU `0x15 IsWrite=0` storm on the **2nd** cycle.  Pass = stream alive after each, `dmesg` clean, no watchdog reset.  **This step falsifies/confirms failure mode #3.** |
| 2 | **Same-mode soak** | 50+ same-mode reinits.  Pass = no VIF `not sync err`, no `CamOsMutexLock` D-state (failure mode #2 over many cycles). |
| 3 | **Cross-mode rotation** | `video0.size` rotation across imx335 modes 0→1→2→3→0 via `json_cli`+restart, 3 rounds.  Pass = stream alive after each hop, no `vpe0_P0_MAIN` wedge.  Expected-risk step (no cold-vif scrub in-process). |
| 4 | **fd / RSS soak (new failure mode)** | 200+ same-mode reinits, sampling `ls /proc/$(pidof waybeam)/fd \| wc -l` and RSS every 10 cycles.  A static PID never launders leaked `/dev/mi_*` fds via process exit, so the documented "~1-fd-per-respawn leak" becomes unbounded vs. `RLIMIT_NOFILE` (1024).  Pass = fd count and RSS bounded. |
| 5 | **Cycle time** | Time SET → first frame on output.  Compare against the respawn baseline (~13 s cold init).  In-process skips `MI_SYS_Init`; expect faster. |

Drive hang detection with
`make remote-test ARGS='--json-summary --host root@192.168.1.13 -- …'`
(`device_alive`, `dmesg_hits`, exit codes 0/1/124/2).  `scripts/api_test_suite.sh`
exercises the restart path and every live field.

### Recovery during testing

- In-process reinit hang that does *not* trip the deadline watchdog:
  `echo b > /proc/sysrq-trigger` from a fresh SSH session
  (`CRASH_LOG.md` 2026-04-26).
- In-process reinit *failure* (clean exit): daemon is down — restart with
  `/etc/init.d/S95waybeam start`.
- On a wedge, log mode transition + `dmesg` + the `remote_test.sh` JSON
  summary in `CRASH_LOG.md`, then re-test the control (respawn) build to
  confirm the wedge is specific to the in-process path.

## Decision criteria

- **Steps 1–4 all pass** → write up the evidence, then plan a follow-up PR
  to promote in-process reinit to default (and retire the respawn for the
  same-mode case).  Keep respawn for cross-mode/size changes if step 3
  fails but 1/2/4 pass.
- **Step 1 storms** → failure mode #3 is confirmed process-model-independent.
  Keep fork+exec respawn.  Record the exact transition + `dmesg` here and
  in `CRASH_LOG.md`, and correct the contradicting comment in
  `star6e_runtime.c`.
