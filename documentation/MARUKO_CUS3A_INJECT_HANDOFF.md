# Maruko (I6C) inject-mode AE — HANDOFF

**Branch:** `feature/maruko-cus3a-apply-cost`
**Date:** 2026-07-04
**For:** whoever picks this up next (Fable 5).
**One-line state:** The CPU win is **proven** (inject-mode → 34–39% vs 70% native).
Getting a **good image** in inject-mode is the open problem: two approaches
tried, both diagnosed, both blocked on the same fork (below). Device is restored
to the good flash-native image.

---

## Goal
Close the Maruko 100 fps CPU gap (waybeam 70% vs majestic 43% @1080p100) **and**
keep a good image (correct AE exposure, correct AWB/white balance, low noise) at
full 100 fps. Ideally also retire the AE-throttle stepping compromise.

## What is PROVEN (do not re-litigate)
1. **Root cause of the gap = `MI_ISP_RegisterIspApiAgent`.** It is pulled in only
   by `MI_ISP_EnableUserspace3A`. It relocates the SigmaStar IQ/3A mid-layer into
   waybeam's process, so every per-frame `MI_ISP_CUS3A_SetAeParam` runs a full
   in-process CMDQ/IQ pass → the `IspMidThreadWq` ~13pt + heavy `isp0`. The apply
   call itself is byte-identical in all modes (one ioctl). SDK-disassembly-grounded.
2. **Inject-mode removes it.** `MI_ISP_CUS3A_Enable` + `MI_ISP_CUS3A_InjectModeEnable`
   (or `CUS3A_SetRunMode(INJECT)`), **no agent** → `IspMidThreadWq`≈0, total 34–39%.
   Device-measured on .12. This half works perfectly.
3. **Not architectural / not VPE.** See `MARUKO_VPE_PORT_INVESTIGATION.md`. I6C has
   no VPE; majestic uses the same discrete ISP+SCL path.
4. **majestic uses CUS3A inject-mode** (`MI_ISP_CUS3A_Enable` + `InjectModeEnable`
   + `Cus3A_ProcAE`), **no `EnableUserspace3A`**. It is the existence proof that a
   good image at ~43% is possible on this exact silicon.

## The three attempts

### Attempt 1 — throttle mode (SHIPPED, PR #156, not this branch)
`aeEngine=custom`: no-op AE adaptor + our P1/P4 controller drives `SetAeParam` at
15 Hz, still under `EnableUserspace3A` (agent present). Result: 70→52.8%, image
"very similar to SDK" (user-confirmed). This is the **safe partial win available
today** (17pt). Quality acceptable; CPU not fully closed.

### Attempt 2 — manual P1 in inject (`MARUKO_AE_INJECT`, our-loop variant)
Drop agent, `InjectModeEnable`, keep P1 driving `SetAeParam` at full rate.
- **CPU: 38.9%** (below majestic). Pacing/`IspMidThreadWq` proof. ✓
- **Image: BAD** — yellow/green (no AWB: the agent was pumping AWB, our path only
  does AE), heavy noise (metering bug, below), and **85 fps** (manual full-rate
  `SetAeParam` perturbs pacing). ✗

### Attempt 3 — native vendor algo in inject (current HEAD of this branch)
`CUS3A_Init` + `CUS3A_EnableUserspaceAE` + `CUS3A_EnableUserspaceAWB` +
`CUS3A_SetRunMode(INJECT)`, **no agent**, supervisory thread limits-only, plus a
new **`maruko_inject_ae_driver`** thread ticking `CUS3A_RunOnceEn(0,0,1,1,0)` at
sensor rate (since no `3A_Proc_0` auto-thread exists without the agent).
- **CPU: 34.9%, 100 fps.** All framework calls `ret=0`, driver spawns. ✓✓
- **Image: BAD — AE won't converge.** Log: `AE state=-1 (not NORMAL)`,
  `init limits: maxShutter=0 maxGain=0`; exposure frozen at cold-boot
  `300us / gain 1024`, WB frozen (green). The native sstar algo is installed and
  `RunOnceEn` ticks it, but it never reaches NORMAL state. ✗

## 2026-07-04 UPDATE — Path A run + majestic disassembly (READ THIS FIRST)

**Path A executed (one cold-start, .12): FAILED on image, exactly as branch 3
of the decision tree.** `CUS3A_SetRunMode(NORMAL)` ret=0 (env-selectable now:
`MARUKO_AE_INJECT=normal`), driver ticking, 100 fps, system busy **33.9%** —
but `AE state=-1` persists, exposure frozen at 300us/sgain=1024 for 50+ s,
snapshot near-black with green cast. Run-mode is irrelevant to convergence:
**the native algo does not run without the agent.**

**Why (disassembly-grounded, `libcus3a.so`):** `CUS3A_Init` itself spawns
`Cus3A_ProcRoutine` (so our RunOnceEn driver duplicates an existing thread —
remove it). That routine polls the ISP frame-sync fd and calls `_DoAeInit` →
`MI_ISP_CUS3A_GetAeInitStatus` to fill `ISP_AE_INIT_PARAM`; with no IQ
calibration in the ISP the init params are zeros (matches our logged
`maxShutter=0 maxGain=0` / `ISP bin limits unavailable`) and the algo never
leaves state=-1. The agent is what makes IQ/calibration loading
(`MI_ISP_ApiCmdLoadBinFile`) work — i.e. **the agent is the algo's config
feed, not just the expensive apply path.**

**majestic's actual recipe (from its binary, pulled off .12):** it statically
embeds the *same* `mi_cus3a` framework as our `libcus3a.so` (identical build
`project_commit.699b9f2 build 20240618`; `Cus3A_ProcAE` is a thread name, not
an API). Its dlsym surface: `MI_ISP_RegisterIspApiAgent` ✓,
`MI_ISP_ApiCmdLoadBinFile` ✓, `MI_ISP_CUS3A_Enable` ✓,
`MI_ISP_CUS3A_InjectModeEnable` ✓ — and **no** `MI_ISP_EnableUserspace3A`.
And `libmi_isp.so` disassembly shows `MI_ISP_EnableUserspace3A` ==
`CUS3A_Init + CUS3A_EnableUserspaceAE/AWB/AF + MI_ISP_RegisterIspApiAgent`.
So majestic = **agent registered + native framework + INJECT run-mode** — the
one combination our attempt matrix never tried. The handoff's claim
"majestic has no agent" is WRONG; the agent is present, but with run-mode
INJECT the per-frame apply bypasses the in-process IQ mid-layer
(IspMidThreadWq≈0 on majestic despite the agent).

**⇒ Path C (next experiment, replaces Path B as priority):** inject branch =
current native install **+ `MI_ISP_RegisterIspApiAgent`** (or simply
`MI_ISP_EnableUserspace3A`) **+ `CUS3A_SetRunMode(INJECT)`**, IQ bin load as
in the default path, RunOnceEn driver removed. Expect AE state NORMAL +
converged image. CPU estimate: inject floor 34.9 + native algo thread ~22 →
~55-57%; if so, the follow-on lever is pacing the algo (RunOnceEn at reduced
rate with the proc thread quiesced, or run-mode OFF + manual RunOnceEn) and
the isp0 kernel gap (ours ~16.5 vs majestic 7.0) which is NOT explained by
agent/run-mode (constant across all our configs).

## THE FORK (superseded by the update above — kept for history)

`AE state=-1` in Attempt 3 strongly implies **the native sstar AE algo needs the
API agent** (its ISP-register tunnel) to reach a running state — the exact thing
we removed for the CPU win. If true, "native algo + cheap apply" is not
simultaneously achievable via `EnableUserspaceAE`. That points to majestic
running **its own custom AE+AWB algo** (registered via `CUS3A_RegInterfaceEX`
under `E_ALGO_ADAPTOR_1`) in inject-mode — not the native one.

**Two candidate paths for the next driver to try (in priority order):**

**Path A — `SetRunMode(NORMAL)` instead of `INJECT`, keep the RunOnce driver.**
Cheap 1-line experiment. Hypothesis: the native algo runs in NORMAL mode (reaches
state NORMAL via `RunOnceEn`) and NORMAL-mode apply may still be cheap *without*
the agent (the agent, not the run-mode, was the cost). If image converges AND
`IspMidThreadWq`≈0 → **this is the whole answer.** If image converges but CPU
climbs → apply needs the agent (native path dead). If still `state=-1` → native
algo structurally needs the agent. One cold-start settles it.

**Path B — custom AE+AWB algo in inject (majestic's likely approach).** Register
our own algo via `CUS3A_RegInterfaceEX(E_ALGO_ADAPTOR_1, AE)` + `(…, AWB)` +
`SetAlgoAdaptor(ADAPTOR_1)` + `SetRunMode(INJECT)`, driven by `RunOnceEn`. This
gives the CPU win but requires our algo to actually be *good*:
  - **Fix AE metering** (see sub-bug below) so exposure converges.
  - **Implement AWB** (we currently have none in the custom path — that is the
    yellow/green cast). Non-trivial: needs an AWB control law or reuse of the
    vendor AWB interface (`Sigma3AGetAwbInterface`) if it works without the agent.
  This is the most work but is the majestic-proven architecture.

Consider disassembling majestic's `Cus3A_ProcAE` / which `ISP_*_INTERFACE` it
registers (native `Sigma3AGetAeInterface` vs its own) to settle A-vs-B before
building.

## Sub-bug: AE metering (`uAvgY≈4`)
Our P1 metering reads `uAvgY≈4` **uniformly** across the AE grid on a *lit* scene.
Confirmed:
- Struct is correct: `MI_ISP_AE_AVGS{uAvgR,uAvgG,uAvgB,uAvgY}` (`.y` @ offset 3),
  8-byte header, grid `nBlkX×nBlkY = 32×32`, **packed at stride nBlkX** (2D
  stride-128 read scores ~0, packed-linear ~real). Divisor bug fixed to use
  `ae_hw->nBlkX*nBlkY` not `ae_info.AvgBlk*` (which read 0 → /11520).
- **Even the correct packed read is ~4** → the stat scale/source itself is wrong,
  not the divisor. The image *is* exposed (so exposure reaches the sensor), the
  metric just doesn't track it. Needs: is `uAvgY` a different scale (not 0–255)?
  wrong dev/chn to `MI_ISP_AE_GetAeHwAvgStats`? stale buffer? Compare against what
  the vendor AE targets. This blocks any custom-AE path (P1/P6/Path B).

## Code map (this branch, `src/maruko_pipeline.c`)
- `maruko_enable_cus3a()` (~line 201): `MARUKO_AE_INJECT` branch = Attempt 3
  (native install + `SetRunMode(INJECT)`; resolves `CUS3A_*` from `libcus3a.so`).
  Non-inject = original `CUS3A_Enable` + `EnableUserspace3A`.
- `maruko_inject_ae_driver()` + globals `g_inj_*` (~line 184): the RunOnce driver.
  **No clean teardown yet** (process-lifetime thread) — add join on stop before
  shipping.
- Dispatch (~line 2339): `int inject`; inject → supervisory thread limits-only
  (`throttle_mode=false`), spawn driver, no no-op adaptor.
- `src/maruko_cus3a.c`: P1/P4 controller (Attempt-1/2 path); metering block
  (~line 390, `avg_y`), instrumented with `blk=%ux%u avgY(lin=%u)`.
- Star6E template (limits-only supervisory): `src/star6e_cus3a.c:204-458`,
  `star6e_pipeline.c:307-331`.

## SDK reference (`/home/snokvist/dev/Maruko`)
- `release/include/isp_cus3a_if.h`: framework — `CUS3A_Init`(:512),
  `CUS3A_RegInterfaceEX`(:518), `CUS3A_SetAlgoAdaptor`(:519),
  `CUS3A_SetRunMode`(:522), `CUS3A_RunOnce/RunOnceEn`(:523-524); run-mode enum
  `E_CUS3A_MODE_{NORMAL=0,OFF=1,INJECT=2}`(:505-508); adaptors
  `E_ALGO_ADAPTOR_{NATIVE=0,_1=1}`(:132-134); types `E_ALGO_TYPE_{AE=0,AWB=1,AF=2}`
  (:124-127); dev/ch enums `E_ISP_DEV_0=0`/`E_ISP_CH_0=0`.
- `release/include/isp/maruko/mi_isp_hw_dep_datatype.h`: `MI_ISP_AE_AVGS`(:55-61),
  `MI_ISP_AE_HW_STATISTICS_t`(:63-68), `CusInject3AEnable_t{bInject3A}`(:106-109).
- `release/include/isp/mi_isp_cus3a_api.h`: `MI_ISP_CUS3A_InjectModeEnable`(:53),
  `MI_ISP_EnableUserspace3A`(:129), `MI_ISP_RegisterIspApiAgent`(:126).
- Reference wiring (custom algo): `sdk/.../verify/mixer/.../mid_iq_impl.cpp:3020-3070`
  (RegInterfaceEX + SetAlgoAdaptor), `:3289-3327` (stats read + `SetAeParam` loop).
- Device libs: `/usr/lib/libcus3a.so` (all `CUS3A_*` exported, verified),
  `/usr/lib/libmi_isp.so`. All symbols dlsym-able on .12.

## Bench / device rules (learned the hard way)
- Device: `192.168.1.13`? NO — this bench is **192.168.2.12** (Maruko, tmpfs).
  `ssh -o ConnectTimeout=12`. Snapshot: `GET /api/v1/snapshot.jpg`. fps:
  `GET /api/v1/fps/live`. Config: `json_cli -i /etc/waybeam.json -s .path val`
  (paths `.isp.aeEngine|.isp.aeFps|.fpv.noiseLevel|.sensor.mode`).
- **`aeEngine`/enable-path/`noiseLevel` are reinit-forcing → force a full RESPAWN
  → WEDGE the SCL fence into D-state** ("inputtask's fence not finished", load→12)
  after 2–3 restart cycles. **Measure each config via `reboot -f` + a single
  cold-start**, never live `/api/v1/set` or restart-cycling. `reboot -f` recovers
  (ssh hangs on it → background + poll reachability).
- No autostart; `setsid /usr/bin/waybeam` (flash, good image) or
  `MARUKO_AE_INJECT=1 setsid /tmp/waybeam_inject` (test). Teardown = SIGTERM +
  poll `pidof` empty. Never SIGKILL (MI_SYS zombie). **Deploy order: teardown →
  `cat > /tmp/waybeam_inject` → start** (writing a running binary = "Text file
  busy").
- Flash binary `/usr/bin/waybeam` is the shipped good one — untouched; all tests
  ran from `/tmp/waybeam_inject`.
- Build: `make build SOC_BUILD=maruko` (+ `star6e`); clang LSP diagnostics are
  false positives (missing cross-toolchain includes) — trust the gcc build.

## Measured numbers (1080p100, of one core)
| config | busy | IspMidThreadWq | 3A_Proc_0 | isp0 | image |
|---|---|---|---|---|---|
| native baseline (agent) | 70.0% | 13.2 | 21.9 | 17.5 | good |
| throttle 15Hz (agent) | 52.8% | 2.0 | 15.8 | 16.9 | ~good (shipped) |
| inject + P1 (no agent) | 38.9% | ~0 | ~0 | 16.9 | yellow+noisy+85fps |
| inject + native algo (no agent) | 34.9% | ~0 | ~0 | 16.4 | frozen (state=-1) |
| majestic (ref) | 42.9% | ~0 | 24.9 | 7.0 | good |

Note majestic's `3A_Proc_0`=24.9 (full-rate algo) yet `isp0`=7 (vs our ~16.5) —
its ISP kernel path is also lighter; a second-order gap worth a look once AE works.
