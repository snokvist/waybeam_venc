# DIVP-Backed Stabilization — Test & Verification Plan

<!-- version: 0.1.0 -->

Branch: `claude/review-divp-pipeline-HASzf`
Status: pre-implementation (on-device validation phase)
Reference candidate: `f2d45c19-star6e_pipeline_1.c` (uploaded, not yet merged)

This plan validates the **open questions** raised by the review of the
candidate pipeline before any production code is touched. We only commit
implementation changes after every Q here resolves to PASS or has a clear
mitigation.

---

## 0. Scope

The candidate replaces the dual `MI_SYS_BlitPa` Y+UV crop with a single
`MI_DIVP_StretchBuf` call (NV12 direct-buffer ABI) and adds a "gimbal"
auto-recenter knob. Everything else in the DIS feed path is unchanged.

Out of scope here: the recenter-divider UX work and any non-DIS changes.
Those land after the DIVP swap is proven.

## 1. Test Bench

- Star6E target: `root@192.168.1.13` (per `REMOTE_TEST_WORKFLOW.md`).
- Sensor: whatever the bench currently has (record in result table).
- Toolchain: `make build SOC_BUILD=star6e`.
- Deploy helper: `./scripts/star6e_direct_deploy.sh cycle`.
- Always reboot between cold-state runs (majestic taint rule).
- Capture per run:
  - `/tmp/venc.log` (full)
  - `dmesg -c` delta
  - Receiver-side stream (`ffplay` / `ffmpeg -i` against the bench output)
  - First-frame `[stab]` log line (carries DIVP src/dst geometry)

## 2. Open Questions

### Q1. Does `MI_DIVP_StretchBuf` work without `MI_DIVP_CreateChn`?

Risk: most SigmaStar SDK revisions require an explicit DIVP channel before
any buffer op. The candidate never creates one.

**Test Q1a — symbol probe (no pipeline run):**

1. Build a tiny `test_divp_probe` binary that:
   - `dlopen("libmi_divp.so", RTLD_LAZY | RTLD_GLOBAL)`
   - resolves `MI_DIVP_StretchBuf`, `MI_DIVP_CreateChn`, `MI_DIVP_StartChn`,
     `MI_DIVP_InitDev`
   - prints which symbols are present
2. Run on bench.

Pass: `MI_DIVP_StretchBuf` symbol resolves.
Fail: link error / missing symbol → DIVP path is a no-go on this BSP,
escalate before continuing.

**Test Q1b — direct-buf one-shot:**

1. Same probe binary: after `MI_SYS_Init`, allocate two NV12 framebufs via
   `MI_SYS_MMA_Alloc` (1280x720 src, 1024x576 dst).
2. Fill src Y plane with a known gradient.
3. Call `MI_DIVP_StretchBuf` with a centred crop window, no
   `MI_DIVP_CreateChn`.
4. `MI_SYS_FlushInvCache` dst, dump first scanline to stdout.

Pass: return code 0 and dst scanline matches the expected gradient slice.
Fail (any non-zero return, garbage output, or kernel oops):
- Retry after adding `MI_DIVP_InitDev` + `MI_DIVP_CreateChn(0, attr)` +
  `MI_DIVP_StartChn(0)` before the stretch call.
- If that path passes, the production change is: add channel
  create/destroy to `star6e_stab_start` / `star6e_stab_stop`.

### Q2. Does the candidate clobber 3DNR / mirror / flip?

The review flagged `star6e_pipeline_reapply_stab_vpe_port` calling
`MI_VPE_SetChannelParam(0, {level3DNR=0, mirror=0, flip=0})` after the
real values were set in `star6e_pipeline_start_vpe_dis_channel`.

**Test Q2 — config-driven smoke:**

1. Set `/etc/venc.json`: `fpv.noise_level=4`, `image.mirror=true`,
   `image.flip=true`.
2. Run candidate build, capture stream.
3. Compare image orientation and noise floor at low light against the
   current `BlitPa` build with the same config.

Pass: stream is mirrored + flipped and shows the same noise reduction
characteristic as the current build.
Fail: orientation reverts to identity and/or noise floor visibly rises →
remove the second `SetChannelParam` from `reapply_stab_vpe_port`, OR pass
the real `level3DNR/mirror/flip` through. Retest.

### Q3. Memory bandwidth / frame-pacing under DIVP load

DIVP runs while VENC also wants the bus. Confirm no new drops.

**Test Q3:**

1. Configure 1080p60 H.265 CBR 8 Mbps, 80% crop, recenter div=25.
2. Run for 5 minutes.
3. On receiver, measure: actual fps, dropped-frame count from RTP
   sequence gaps.
4. Compare against the current `BlitPa` build, same config.

Pass: receiver fps within ±1 of source fps, dropped frames within 1% of
the `BlitPa` baseline.
Fail: investigate `MI_VPE_QueryPortFlowCtrl` and VPE port depth; consider
raising `MI_SYS_SetChnOutputPortDepth` second arg.

### Q4. Header / ABI collision (canonical `MI_SYS_*` typedefs)

The candidate dropped the `Stab*` prefix style. The original prefix was
specifically to avoid colliding with the Star6E compatibility layer.

**Test Q4:**

1. `make build SOC_BUILD=star6e` against the candidate translation unit
   verbatim.
2. Watch for redefinition warnings/errors in the preprocessor output:
   `make build V=1 2>&1 | grep -E "redefin|conflict"`.

Pass: no redefinition warnings.
Fail: restore the `Stab*` prefix wrapper style.

### Q5. dlopen leak on SIGHUP reinit

`libmi_divp.so` / `libmi_ive.so` handles are not dlclosed. The early-return
guard in `star6e_stab_load_sys_extra_symbols` should prevent re-opens, but
verify.

**Test Q5:**

1. Run candidate build, send SIGHUP every 10s for 60s (6 reinits).
2. `cat /proc/$(pidof venc)/maps | grep -cE "libmi_(divp|ive|sys)\.so"`
   before and after.

Pass: count stays constant across reinits.
Fail: cap with a `dlclose` in `star6e_stab_stop` for paths that should
fully tear down.

### Q6. Argv parsing of out-of-range crop_pct

`venc 120 25` currently silently keeps both defaults because `saw_crop`
never flips.

**Test Q6 (offline):**

1. Run with `venc 120 25`, `venc 80 9999`, `venc 80`, `venc 80 0`.
2. Check first-frame `[stab]` log line for resolved crop / return_div.

Pass: each invocation logs the clamped or default values matching the
documented contract.
Fail: log clamping and either accept-with-warning or hard-reject.

## 3. Execution Order

1. Q1a (symbol probe) — gates everything else.
2. Q1b (one-shot stretch) — gates the production swap.
3. Q4 (build clean) — local, do before any device push.
4. Q2, Q5, Q6 — quick on-bench smoke after a clean candidate build.
5. Q3 (bandwidth / drops) — longest, run last.

Each step records: timestamp, sensor, config, log excerpt, PASS/FAIL,
follow-up action. Append results below in §5.

## 4. Go / No-Go for Implementation

Implementation begins only when:

- Q1a, Q1b: PASS (or a documented `CreateChn` mitigation is in place).
- Q4: PASS or downgraded to the `Stab*` prefix style.
- Q2: PASS or `reapply_stab_vpe_port` patched to preserve params.

Q3, Q5, Q6 may be downgraded to known-issue follow-ups if they don't
break the golden path.

## 5. Results Log

| Date | Q | Sensor / Config | Result | Notes |
|------|---|-----------------|--------|-------|
|      |   |                 |        |       |
