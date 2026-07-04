# Maruko (I6C) VPE-port investigation — RESULT: not viable, premise falsified

**Branch:** `feature/maruko-vpe-pipeline`
**Date:** 2026-07-04
**Verdict:** **Do NOT port Maruko to MI_VPE.** The hypothesis that drove this
branch — "majestic's lower CPU comes from a fused VPE pipeline; waybeam's higher
CPU comes from discrete MI_ISP+MI_SCL; port to VPE to close the gap" — is
**false on both halves.** Left as a documented dead end so it isn't re-chased.

## The hypothesis (going in)

waybeam 68% vs majestic 43% at 1920x1080@100 was believed to be *architectural*:
majestic uses `MI_VPE` (fused HW ISP+3DNR+scale, like our Star6E backend), while
waybeam uses discrete `MI_ISP` + `MI_SCL`, paying an extra per-frame kernel
control-plane thread (`IspMidThreadWq`). Proposed fix: port the Maruko pipeline
to the Star6E `MI_VPE` pattern.

## What the device actually shows (192.168.2.12, I6C / SSC378QE)

**VPE is not present in this firmware at all:**

| Probe | Result |
|---|---|
| `/usr/lib/libmi_vpe.so` | **absent** — `NO libmi_vpe.so on device` |
| `mi_vpe*.ko` anywhere on fs | **none found** |
| `lsmod \| grep vpe` | **empty** |
| `/proc/mi_modules/` | lists `mi_isp mi_scl mi_vif mi_venc` … **no `mi_vpe`** |

Since the VPE userspace lib and kernel module are not deployed, **no process on
this device can use MI_VPE** — including majestic. A port to `MI_VPE_*` would
fail to link/load against the shipped firmware.

**majestic runs the same discrete path and the same mode:**

- `/etc/majestic.yaml`: `video0: 1920x1080 h265 @100fps` — **identical** sensor
  mode/fps to the waybeam config in the head-to-head. The 43%-vs-68% comparison
  was valid (not a resolution/fps confound).
- majestic additionally runs `video1: h264 704x576 @15` **and** opus audio, so
  majestic is doing *more* encode/DSP work than waybeam and still lands at 43%.
- majestic's only ISP config is `isp: { sensorConfig: /etc/sensors/imx415.bin }`
  — no VPE section; the same `imx415.bin` waybeam loads.

> Note on a red herring: the majestic **binary** contains the strings
> `mi_vpe_init` / `mi_vpe_deinit` / `PutVpeRawDataThread`. These are majestic's
> **own lowercase internal function names** (majestic is cross-SoC; on I6E-class
> parts that path can call the SDK's `MI_VPE_*`). They are **not** the SDK's
> CamelCase `MI_VPE_CreateChannel`/`SetChannelParam` API, and with no
> `libmi_vpe.so` on the device they cannot resolve to it. On I6C, majestic's
> "vpe" stage runs over `MI_ISP`+`MI_SCL` like everything else.

## Code-side corroboration

- **Maruko topology** (`src/maruko_pipeline.c`): rigid discrete chain
  `VIF(0,0,0) → ISP(0,0,0) → SCL(0,0,0/1) → VENC`. `MI_ISP_SetChnParam` carries
  `level3DNR` (the grain knob); `MI_SCL` does the scale/crop. The Makefile
  Maruko target links `SOC_LIBS := -lm` only — **no `-lmi_vpe`**, no `MI_VPE_*`
  calls. There is even an in-code acknowledgement at `maruko_pipeline.c:113`
  ("MI_SCL_SetPortConfig is heavier than Star6E's MI_VPE_SetPortCrop"). The
  discrete `VIF→ISP→SCL→VENC` path is the only path I6C exposes in this build.
- **Star6E VPE template** (`src/star6e_pipeline.c`): the fused
  `VIF→VPE→VENC` pattern exists and is clean (extracted for reference), but it
  targets a chip whose `libmi_vpe`/`mi_vpe.ko` are actually deployed. Not
  applicable to the I6C firmware as shipped.

**The Maruko SDK *does* ship `libmi_vpe`/`mi_vpe.h` for i6c — but on I6C VPE is
an API illusion, not a hardware block.** Confirmed from the SDK tree:

- There is **no `mi_vpe.ko`** anywhere in the I6C kernel module set (which is
  `mi_isp.ko mi_scl.ko mi_vif.ko mi_venc.ko mi_ldc.ko …`). ISP and SCL are the
  real hardware/kernel drivers; VPE has none.
- `libmi_vpe.so` is a **47 KB userspace shim** (vs `libmi_isp.so` at 486 KB)
  that **imports `MI_ISP_*` symbols** (`MI_ISP_EnableUserspace3A`,
  `MI_ISP_DisableUserspace3A`, …). It is a veneer that orchestrates the same
  `mi_isp.ko`+`mi_scl.ko` drivers.
- **Zero** `MI_VPE_*` calls exist in any I6C sample/demo. The vendor-blessed
  reference IPC pipeline (`ipc_demo/.../maruko/common/st_common_isp.c`) is
  explicitly discrete `VIF → MI_ISP → MI_SCL → VENC`.

On I6E, VPE *is* the fused hardware/kernel block. On I6C that fusion **does not
exist in silicon** — the SDK's VPE API is an inherited compatibility layer over
discrete ISP+SCL. (Confidence: high — SDK-grounded.)

## Why this kills the port, not just delays it

majestic proves the target CPU (43%, while doing **more** than waybeam) is
**achievable on the discrete `MI_ISP`+`MI_SCL` path** — the exact architecture
waybeam already has. So the gap is **not** architectural. It is a
**configuration / per-frame-usage difference within the shared pipeline.**

And even setting that aside, **a VPE port could not remove the cost anyway**:
the `IspMidThreadWq` ~13pt lives inside **`mi_isp.ko`**'s per-frame
3A/statistics processing. Because I6C's `libmi_vpe.so` is just a userspace shim
that calls `MI_ISP_*`, driving the pipeline via `MI_VPE_*` runs the *same*
`mi_isp.ko` code and pays the *same* `IspMidThreadWq`. At best a VPE shim removes
some userspace bind/copy overhead of the separate SCL stage — the dominant ISP
cost is untouched. Porting would be a large, high-risk rewrite onto an
untested, sample-less, kernel-driver-less code path for **no CPU benefit.**

## Where the gap really is (from the head-to-head thread scan)

Same mode, 1920x1080@100:

| Thread | majestic | waybeam | Δ |
|---|---|---|---|
| `3A_Proc_0` (userspace AE+AWB) | 24.9% | 19.2% | majestic **higher** |
| `isp0_P0_MAIN` (kernel ISP 3A stats/math) | 7.0% | 16.6% | **+9.6** |
| `IspMidThreadWq` (ISP mid-stage control-plane) | ~0 | 13.0% | **+13.0** |
| main loop (GetStream→RTP + scene + IMU) | 4.5% | 13.1% | **+8.6** |
| **total busy** | **42.9%** | **68.5%** | **+25.6** |

The gap is concentrated in three within-architecture places:

1. **`IspMidThreadWq` +13pt** — the biggest single item, and majestic barely
   touches it. This kernel workqueue does per-frame ISP control-plane work
   (3DNR config compute, CMDQ bank-load, IQ shadow-reg push, i2c exposure).
   waybeam is triggering per-frame ISP reconfiguration that majestic is not.
   **Prime suspects:** how `level3DNR`/3DNR is (re)configured; the CUS3A IQ
   shadow-register cadence; anything that re-pushes ISP channel params per frame.
2. **`isp0_P0_MAIN` +9.6pt** — waybeam's kernel ISP thread does ~2.4× majestic's
   work for the same stats. Likely coupled to (1).
3. **main loop +8.6pt** — app-side and independently tunable: debug-OSD redraw
   cadence, scene-size detector, IMU drain, RTP path. Not ISP at all.

Notably `3A_Proc_0` is *higher* on majestic, so userspace 3A itself is **not**
the gap — ruling out "throttle the AE" as the lever for this delta (that's a
separate ~10pt win, already implemented as `isp.aeEngine=custom`).

## Recommendation — replace this branch's goal

**Abandon the VPE port.** Re-scope the CPU work to an empirical **majestic-vs-
waybeam ISP/SCL config diff on the same mode**, targeting `IspMidThreadWq`:

1. Instrument/trace what re-triggers `IspMidThreadWq` per frame under waybeam
   (3DNR reconfig? per-frame `MI_ISP_SetChnParam`? CUS3A shadow push?), and
   confirm majestic does it once at init, not per frame.
2. A/B the specific ISP setup calls: does waybeam re-issue channel/3DNR params
   more often than needed? Is `level3DNR` forcing a per-frame 3DNR bank compute
   that majestic avoids (static 3DNR)?
3. Separately trim the +8.6pt main loop (OSD redraw gate to ~5–10Hz, scene-size
   cadence) — cheap, independent, already noted in `MARUKO_CPU_PROFILE.md`.

This is diagnosis-then-surgical-fix within the existing architecture, not a
rewrite. See `MARUKO_CPU_PROFILE.md` (thread breakdown + SDK ground-truth on
`IspMidThreadWq`) and `MARUKO_AE_IMPROVEMENT_BRAINSTORM.md` (the AE-throttle
lever, orthogonal to this gap).

## One-line summary

**I6C has no VPE deployed; majestic hits 43% on the same discrete ISP+SCL path
while doing more than waybeam — so the CPU gap is config, not architecture. VPE
port cancelled; chase `IspMidThreadWq` instead.**

---

## Measured follow-up — `IspMidThreadWq` is per-frame AE-apply (device ablation)

Ran a controlled ablation on .12 (I6C), 1920x1080@100, sampling per-thread CPU
(incl. kernel threads) with a jiffie sampler. **Config changes to `aeEngine` /
`noiseLevel` force a pipeline reinit on Maruko, which is fragile (SCL-fence
D-state wedge on repeated respawn) — so each config was measured via a clean
cold-start after reboot, not a live switch.**

| Config (mode 4, 1080p100) | busy | `IspMidThreadWq` | `3A_Proc_0` | `isp0_P0_MAIN` | main |
|---|---|---|---|---|---|
| **B** baseline (sdk AE, nl=1) | 70.0% | **13.2%** | 21.9% | 17.5% | 10.6% |
| **A** 3DNR off (sdk AE, nl=0) | 64.5% | 12.6% | 18.9% | 16.4% | 9.9% |
| **D** throttle (custom AE 15Hz, nl=1) | **52.8%** | **2.0%** | 15.8% | 16.9% | 10.4% |
| majestic (ref, sdk-equiv, +h264+audio) | 42.9% | ~0 | 24.9% | 7.0% | 4.5% |

### Two findings

1. **3DNR is NOT the driver** (A vs B): turning channel-level 3DNR off dropped
   total 5.5pt but `IspMidThreadWq` barely moved (13.2→12.6). The 3DNR cost is
   spread across 3A/isp0/main, not the mid workqueue.

2. **`IspMidThreadWq` IS per-frame AE-apply** (D vs B): throttling AE from 100Hz
   to 15Hz (`aeEngine=custom`) **collapses `IspMidThreadWq` 13.2→2.0pt** and cuts
   total **70.0→52.8% (−17.2pt)**. So the +13pt majestic lacks is the cost of
   applying userspace-3A results to the ISP (CMDQ bank-load / IQ shadow / i2c
   exposure) *every* sensor frame. majestic avoids it despite running 3A at
   *full* rate — see below.

### What this means for the gap

- **The dominant lever already exists and is already built: `aeEngine=custom`.**
  It takes waybeam from 70% to 52.8% at 100fps — within ~10pt of majestic — and
  its quality was just improved this session (P1 IIR + P4 gain-pin, PR #156). The
  practical CPU close is: **ship `aeEngine=custom` at high fps.** (Confirms +
  quantifies the older ~10pt AE-throttle note, now with the mechanism.)

- **Residual ~10pt after throttle is `isp0_P0_MAIN` + main loop.** Note majestic
  runs 3A at **full rate** (`3A_Proc_0`=24.9%, *higher* than our throttled 15.8%)
  yet has `isp0_P0_MAIN`=7.0 (vs our 16.9) and `IspMidThreadWq`≈0. **majestic
  proves full-rate, smooth AE at low ISP-apply cost is achievable on this exact
  hardware** — so waybeam's expensive per-frame apply is a *usage* inefficiency
  in the `MI_ISP_EnableUserspace3A` / CUS3A path (likely re-pushing a heavy 3A/IQ
  result struct every tick where majestic pushes minimal exposure/gain deltas),
  not a hardware limit. That is the next investigation for *full-rate-quality at
  majestic CPU* — a bigger prize than the throttle, but harder.

- **main loop +5.9pt** is independent and app-side (OSD redraw cadence, scene
  detector) — cheap to trim regardless.

### Bench note
`aeEngine`/`noiseLevel` changes are **reinit-forcing and respawn-fragile** on
Maruko — do NOT A/B them by live `/api/v1/set` or restart-cycling (wedges the SCL
fence into D-state, needs `reboot -f`). Measure each via cold-start after reboot.
