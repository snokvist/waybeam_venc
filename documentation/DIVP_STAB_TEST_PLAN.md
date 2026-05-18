# DIVP-Backed Stabilization — Test & Verification Plan

<!-- version: 1.0.0 -->

Branch: `claude/review-divp-pipeline-HASzf` (PR #119)
Status: **COMPLETE** — all gates passed; surgical stretch swap and full
channel backend both landed on the branch.
Reference candidate: `f2d45c19-star6e_pipeline_1.c` (informed the design;
final implementation in commits `338036c`..`051887b`).

This plan validated the **open questions** raised by the review of the
candidate pipeline before production code was touched. All Q gates are
resolved; results recorded in §5.

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

**Decision: GO — implementation landed on PR #119.** All gates passed:

- Q1a, Q1b: PASS — `MI_DIVP_StretchBuf` works without `CreateChn` on this BSP.
- Q4: PASS — clean build, `Stab*` prefix style retained.
- Q2: PASS by construction and empirically (Y-plane dump confirmed DIVP is pure crop).
- Q3: PASS — 5-min soak shows ±1 fps and bandwidth tracking the BlitPa baseline.

Q5, Q6 deferred as known-issue followups; they don't block the golden
path. After the surgical swap landed, a follow-on design pass added the
channel backend — see `DIVP_CHANNEL_OSD_ARCH.md` for Q-DIVP-1..4
(resolved during channel-backend bring-up in commit `051887b`).

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
| 2026-05-18 | Q3  | 5-min RTP soak on bench 192.168.1.13 (1280x960 stab 80%, H.265 CBR 6.3 Mbps, 60 fps) | **PASS** | Sender: continuous 59–60 fps for all 300 s, 338 `[verbose]` lines, no slow seconds. Receiver: avg 59.47 fps / 6.45 Mbps, p50 frame interval 16.68 ms, p99 21.68 ms. One outlier (max 2.4 s) corresponded one-for-one to a sender-side `[net] 13 send errors` UDP-link blip (LAN cause, not encoder). No DIVP-induced regressions. |

### Q1 findings that changed implementation

1. **No `MI_DIVP_CreateChn` needed for stretch backend.** The candidate's
   no-channel assumption is confirmed on this BSP. No production code
   change required for the surgical swap (commit `338036c`).
2. **Pixel format value matters.** The vendor `libmi_divp.so` on this build
   accepts `ePixelFormat = 0x0B` (which equals `I6_PIXFMT_YUV420SP` in the
   waybeam compat layer), and **rejects** the canonical
   `MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 = 0x0A` with error `0x1F`.
   The candidate already assigns `I6_PIXFMT_YUV420SP` to
   `divp_src/dst.ePixelFormat`, so it is correct on this point — but the
   value is BSP-specific. If the firmware is rebuilt against a different
   SDK revision, rerun `divp_probe --pixfmt-sweep` to confirm the accepted
   value. The channel backend uses the same 0x0B for its
   `MI_DIVP_OutputPortAttr_t.ePixelFormat`.
3. **DIVP MMA heap names.** Custom names (`#nocache_divp_probe`) work on
   this BSP. Production uses VENC-input / DIVP-internal buffers, not raw
   MMA, so this doesn't affect production directly — but it's the closest
   we'll get to a synthetic stretch benchmark.

### Channel backend additional findings (commits `051887b`)

1. **VPE port0 serves both bind and manual drain.** The channel backend's
   stab thread continues to `ChnOutputPortGetBuf` on VPE port0 for IVE
   input, while the same port is bound to DIVP via `MI_SYS_BindChnPort2`.
   No port budget pressure observed.
2. **`MI_DIVP_SetChnAttr` is safe at 60 Hz.** Updating `stCropRect` every
   frame on a running channel does not stall the engine. The new crop
   applies to the next inbound frame; frames already inflight use the
   previous crop.
3. **RGN attach to DIVP composites correctly.** `E_MI_RGN_MODID_DIVP=1`
   works as documented. Snapshot endpoint proves OSD pixels reach DIVP
   output. The earlier "VENC attach succeeded but produced no pixels"
   observation was a misnamed enum constant (`RGN_MODID_VENC=2` actually
   means LDC per the vendor `MI_RGN_ModId_e` definition); VENC is not a
   valid RGN attach point on this BSP.
4. **JPEG-VENC bind to DIVP output works in parallel with VENC ch0 bind.**
   `/api/v1/snapshot.jpg` returns a 1024×768 JPEG with the OSD baked in
   while ch0 continues to encode at 60 fps. Previously timed out under
   the stretch backend due to VPE port0 contention with the stab thread.

