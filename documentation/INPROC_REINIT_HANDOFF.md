# On-Device Handoff — In-Process Reinit Test

> **For the local Claude Code CLI session that has bench access.**  The
> branch work was prepared in a remote (no-device) session; this file is
> the briefing to run the on-device validation matrix and report back.

## TL;DR

A gated, experimental **static-PID in-process reinit** path was added to
Star6E behind `VENC_INPROC_REINIT`.  Your job: run it on the bench and find
out whether it survives — i.e. whether the fork+exec respawn is still
needed, or whether we can keep one PID for the whole process lifetime.

- Branch: `claude/sigmastar-respawn-testing-i9xGk` (already pushed).
- Spec / rationale / matrix: `documentation/INPROC_REINIT_TEST_PLAN.md`.
- Driver: `scripts/inproc_reinit_test.sh`.
- Bench: Star6E SSC338Q + IMX335 @ `root@192.168.1.13` (HTTP port 80).

**Default behaviour is unchanged.**  Only a daemon started with
`VENC_INPROC_REINIT=1` takes the new path; everything else still
forks+execs a fresh PID.

## What changed in code (so you can reason about failures)

- `src/star6e_runtime.c`
  - `star6e_runtime_inproc_reinit()` — on a `MUT_RESTART` reinit, rebuilds
    the pipeline in place: `pipeline_stop` → reload `/etc/waybeam.json` →
    `pipeline_start` → re-apply startup controls, **without exiting or
    cycling `MI_SYS`**.  PID stays constant.
  - `star6e_inproc_reinit_watchdog_fork()` — a `waybeam-rwd` child that
    `sysrq-b`'s the box if the rebuild wedges in D-state for 30 s.
  - `star6e_runner_pipeline_teardown()` — shared helper so the in-process
    and full-shutdown teardown orderings stay identical.
  - `star6e_runtime_handle_reinit()` — branches on `VENC_INPROC_REINIT`.
- No change to `venc_respawn.c`, `main.c`, or Maruko.

## Pre-flight

```bash
git checkout claude/sigmastar-respawn-testing-i9xGk
make verify                       # both backends must be green
ssh root@192.168.1.13 true        # confirm bench reachable
```

If the bench host/IP differs, export `HOST=root@<ip>` (and `PORT=<port>`
if not 80) before every script call.

## Run the matrix

The driver auto-deploys, asserts the PID is static, drives reinits via
`/api/v1/restart` (a pure reload+rebuild — the cleanest same-mode trigger),
checks HTTP recovery after each, and scans `dmesg` for fault keywords.

```bash
# 1) Deploy the experiment build and confirm it's up
scripts/inproc_reinit_test.sh deploy
scripts/inproc_reinit_test.sh status        # note the pid

# 2) THE GATE — same-mode storm (failure mode #3, the MMU rebuild storm).
#    If this storms, the experiment is over; record and stop.
scripts/inproc_reinit_test.sh storm 10

# 3) Same-mode soak (failure mode #2 over many cycles)
scripts/inproc_reinit_test.sh storm 50

# 4) fd / RSS leak check (the NEW failure mode a static PID exposes —
#    process exit no longer launders leaked /dev/mi_* fds)
scripts/inproc_reinit_test.sh soak 200

# 5) Cross-mode (size) rotation — expected-risk; no cold-vif scrub in-proc.
#    Confirm valid imx335 sizes first via --list-sensor-modes, then:
scripts/inproc_reinit_test.sh crossmode 3 1920x1080 1280x720
```

Then run the **control** to prove any failure is specific to the
in-process path (and that the bench itself is healthy):

```bash
scripts/inproc_reinit_test.sh --respawn deploy
scripts/inproc_reinit_test.sh --respawn storm 10
```

For deeper hang classification on a specific transition, fall back to the
standard harness:
`make remote-test ARGS='--json-summary --host root@192.168.1.13 -- ...'`
(reads `device_alive`, `dmesg_hits`, exit codes 0/1/124/2).

## Pass / fail criteria

| Step | PASS | FAIL → action |
|------|------|---------------|
| storm 10 (gate) | pid static every cycle, HTTP recovers, dmesg clean | MMU/`not sync`/`vpe0_P0_MAIN` storm or HTTP never returns → failure mode #3 confirmed process-model-independent. **Keep respawn.** Log + stop. |
| storm 50 | as above over 50 | `CamOsMutexLock` D-state or VIF degradation → persist-hack assumptions don't hold in a static PID. |
| soak 200 | fd & RSS deltas bounded | steady ~1-fd/reinit growth toward 1024 → the static-PID fd leak is real; quantify the rate. |
| crossmode | pid static, no `vpe0_P0_MAIN` wedge | wedge expected-possible; document — size changes may have to stay on respawn. |

## If something wedges

Follow the AGENTS device-recovery rules:

1. **Stop immediately** — do not keep issuing commands to a wedged board.
   The driver already aborts on SSH-unreachable and on HTTP-never-returns.
2. Try `ssh root@192.168.1.13 'echo b > /proc/sysrq-trigger'` from a fresh
   session (works even when waybeam is D-state — see `CRASH_LOG.md`
   2026-04-26). Otherwise request a physical power cycle.
3. After recovery, an in-process reinit *failure* leaves the daemon down
   (S95waybeam does not auto-restart): `ssh ... /etc/init.d/S95waybeam start`.
4. **Record** the exact transition, `dmesg`, and `remote_test.sh` JSON
   summary in `documentation/CRASH_LOG.md`.

## Report back / wrap up

- Append a dated results block to `documentation/INPROC_REINIT_TEST_PLAN.md`
  under a new "## Results" section (mode transitions, pid behaviour, fd/RSS
  deltas, dmesg, pass/fail per step).
- If step 1 storms: also correct the contradicting comment in
  `star6e_runtime.c` (it currently claims the MMU storm is "fixed at its
  root") and note the evidence in `CRASH_LOG.md`.
- If steps 1–4 all pass: that's the green light for a follow-up PR to
  promote in-process reinit toward default — leave that as a recommendation,
  don't flip the default in this branch.
- Commit results to `claude/sigmastar-respawn-testing-i9xGk` and push.
