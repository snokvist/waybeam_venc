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

Tool: `tools/divp_probe.c` — dlopen-only ARM binary, cross-compile with
`make divp_probe`. Implements both Q1a and Q1b. Exit codes: 0 PASS,
1 missing symbol, 2 MMA/init failure, 3 StretchBuf non-zero return,
4 destination pixels mismatch.

**Test Q1a — symbol probe (no pipeline run):**

```bash
make divp_probe
scp divp_probe root@192.168.1.13:/tmp/
ssh root@192.168.1.13 /tmp/divp_probe --probe-only
```

Pass: stdout reports `PRESENT` for `MI_DIVP_StretchBuf` and the final
line is `PASS Q1a: MI_DIVP_StretchBuf is resolvable.` Exit 0.
Fail: any required symbol marked `MISSING` → DIVP path is a no-go on
this BSP. Capture stdout to §5 and escalate.

**Test Q1b — direct-buf one-shot:**

```bash
ssh root@192.168.1.13 /tmp/divp_probe
```

The probe:
- `MI_SYS_Init` (auto-detects zero-arg vs single-arg ABI).
- Allocates 1280x720 + 1024x576 NV12 buffers via `MI_SYS_MMA_Alloc`
  under the `#nocache_divp_probe` MMA name.
- Paints src Y plane with `row[x] = x & 0xff`, UV with 128.
- Calls `MI_DIVP_StretchBuf` with a centred 1024x576 crop, no channel.
- `FlushInvCache` dst, prints first 16 bytes of row 0, verifies that
  `dst[x] == ((crop_x + x) & 0xff)` for every pixel in row 0.

Pass: `MI_DIVP_StretchBuf -> 0` and `PASS Q1b: ... direct-buf path
does NOT need a channel`. Exit 0.

Fail (non-zero StretchBuf return, or row-0 mismatch):

```bash
ssh root@192.168.1.13 /tmp/divp_probe --with-chn
```

`--with-chn` calls `MI_DIVP_InitDev` + `CreateChn(0)` + `StartChn(0)`
before StretchBuf and tears them down after. If `--with-chn` passes
but the bare path failed, the production change is: add channel
create/start to `star6e_stab_start` and stop/destroy to
`star6e_stab_stop` in the candidate before merging.

If `--with-chn` also fails, DIVP is unusable on this BSP — keep the
existing `MI_SYS_BlitPa` path and record the failure log in §5.

### Q2. Does the candidate clobber 3DNR / mirror / flip?

The review flagged `star6e_pipeline_reapply_stab_vpe_port` calling
`MI_VPE_SetChannelParam(0, {level3DNR=0, mirror=0, flip=0})` after the
real values were set in `star6e_pipeline_start_vpe_dis_channel`.

**Outcome — N/A by design.** The surgical DIVP swap landed on this
branch keeps the original waybeam VPE-start order: `MI_VPE_CreateChannel`
→ `MI_VPE_SetChannelParam(level3DNR, mirror, flip)` →
`MI_SNR_SetOrien(mirror, flip)` → `MI_VPE_StartChannel`. The
`reapply_stab_vpe_port` function from the candidate is never imported,
so the clobber bug cannot be triggered.

Independent code-analysis result for the DIVP API itself: the vendor
`MI_DIVP_DirectBuf_t` struct
(`waybeam-hub/vendor/sigmastar/include/mi_divp_datatype.h:69-76`) has
**no orientation fields** — only pixel format, dims, strides, and
phyAddrs. `MI_DIVP_StretchBuf` therefore cannot apply any mirror, flip,
or rotation. Mirror / flip flags only exist on the channel-attribute
API (`MI_DIVP_ChnAttr_t.bHorMirror/bVerMirror`), which is *not* used
here. Whatever orientation the VPE port produces is what DIVP outputs.

**Pending visual confirmation (recorded as Q2-followup):** during a
deploy cycle (mirror=false → mirror=true) the user briefly observed the
flipped frame, then it appeared to revert. This is more likely the
viewer holding onto a buffer from the previous run / SetOrien register
commit latency, but it warrants a clean check. Definitive method:

1. Disable stab: `json_cli -s .video0.stabCropPct 0 -i /etc/waybeam.json -o /etc/waybeam.json`
2. Restart waybeam, take baseline snapshot:
   `curl -o /tmp/stab_off.jpg http://192.168.1.13/api/v1/snapshot.jpg`
3. Re-enable stab: `... .video0.stabCropPct 80 ...`, restart.
4. With stab active, snapshot is starved because both MJPEG-bind and
   stab thread consume VPE port0. Capture orientation via a live RTP
   viewer instead, side-by-side with the stab-off baseline.

Note: the snapshot endpoint timing out under stab is a **pre-existing
issue**, present on BlitPa too — JPEG channel is bound to VPE port0
(`star6e_pipeline.c:2535`) and starves when stab's tight-loop
`ChnOutputPortGetBuf` drains the port. Worth a separate fix later
(feed JPEG from the stab thread's VENC input handoff instead).

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
| 2026-05-18 | Q1a | bench 192.168.1.13 (ssc338q, OpenIPC 4.9.84) | **PASS** | All required symbols present; `MI_SYS_Pa2Va` absent (probe falls back to `MI_SYS_Mmap`). |
| 2026-05-18 | Q1b | same | **PASS, no channel needed** | `MI_DIVP_StretchBuf` returns 0 standalone — `MI_DIVP_CreateChn` not required. Row-0 verification: dst gradient matches src at `crop_x = 128 (0x80)`. |
| 2026-05-18 | Q1b | same, `--with-chn` | PASS (sanity) | Channel path also works; either pattern is valid. |
| 2026-05-18 | Q4  | local `make lint` (-Wall -Wextra -Werror) on `src/star6e_pipeline.c` after DIVP swap | **PASS** | Clean build, zero warnings. `Stab*` prefix style retained so no header collisions. |
| 2026-05-18 | Q2  | code analysis vs vendor `mi_divp_datatype.h` | **N/A by construction** | Surgical swap kept original VPE start order. `MI_DIVP_DirectBuf_t` has no flip flags — `MI_DIVP_StretchBuf` cannot change orientation. |
| 2026-05-18 | Q2  | Y-plane dump (`STAB_DUMP` env var) on bench 192.168.1.13 | **PASS — empirically confirmed** | With `image.mirror=false`: src and dst dumps both show fan-right / power-strip-left (sensor native). With `image.mirror=true`: BOTH src and dst dumps show fan-LEFT / power-strip-RIGHT — `MI_SNR_SetOrien` mirrored the sensor and DIVP preserved the mirror. DIVP is a pure crop. The earlier "switched back" observation was viewer buffer catch-up timing, not DIVP. |
| 2026-05-18 | Q2  | side observation | known issue (separate) | `image.mirror=true + image.flip=true` together cause the IMX335 to stop producing frames on this branch. mirror-only and flip-off both work; combined `flip=true` is the trigger. Pre-existing, unrelated to DIVP — see roadmap item `IMX335_FLIP_WEDGE` followup. |

### Q1 findings that change implementation

1. **No `MI_DIVP_CreateChn` needed.** The candidate's no-channel assumption
   is confirmed on this BSP. No production code change required.
2. **Pixel format value matters.** The vendor `libmi_divp.so` on this build
   accepts `ePixelFormat = 0x0B` (which equals `I6_PIXFMT_YUV420SP` in the
   waybeam compat layer), and **rejects** the canonical
   `MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 = 0x0A` with error `0x1F`.
   The candidate already assigns `I6_PIXFMT_YUV420SP` to
   `divp_src/dst.ePixelFormat`, so it is correct on this point — but the
   value is BSP-specific. If the firmware is rebuilt against a different
   SDK revision, rerun `divp_probe --pixfmt-sweep` to confirm the accepted
   value.
3. **DIVP MMA heap names.** Custom names (`#nocache_divp_probe`) work on
   this BSP. The candidate uses VENC-input buffers, not raw MMA, so this
   doesn't affect production directly — but it's the closest we'll get to
   a synthetic stretch benchmark.

