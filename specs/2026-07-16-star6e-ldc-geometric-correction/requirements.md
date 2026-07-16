# Investigation: Star6E hardware Geometric Distortion Correction (VPE-embedded LDC)

**Status: INVESTIGATION — not scheduled.** Research spec capturing what is
verified from the repo, a cheap on-device liveness plan, and an explicit
"hunt list" to run against the full Star6E SDK (available under Claude Code)
before any implementation. No code has been written.

## Scope

This spec targets **lens / geometric distortion correction** (barrel /
pincushion / fisheye dewarp) on **Star6E only** — the `MI_LDC_WORKMODE_LDC`
equivalent, i.e. a static per-lens warp applied to every frame. It is **not**
gyro EIS (`DIS_GYRO`) and not the CPU/crop `stab`/`stab-fill` framing that
ships today. The 3×3 projection path is used here only as the cheapest
liveness vehicle; the deliverable is a static lens-correction warp.

Maruko's LDC path is tracked separately in
`specs/2026-07-09-maruko-ldc-gyro-dis/` (different SoC, different SDK
generation, currently harder — no vendored `libmi_ldc.so`).

## Why Star6E first

Per `AGENTS.md` ("New SigmaStar SDK features: implement on Star6E first,
validate, then port to Maruko"), and because Star6E is the lower-risk path:

- The LDC block is **embedded inside VPE (V2 API)** — every LDC entry point
  is in `libmi_vpe.so`, which the backend already `dlopen`s. No new vendored
  lib, no kernel module, no BSP packaging (unlike Maruko's standalone
  `MI_LDC` module, which needs a version-matched `libmi_ldc.so` +
  `mi_ldc.ko` that are not in `vendor-libs/maruko/`).
- The enable knob is already present in the structs the backend uses today.

## SDK inventory — verified from this repo (2026-07-16)

### The LDC structs are already in our build headers

`include/sigmastar_types.h` (mirrored in the SDK reference
`sdk/ssc338q/include/i6_vpe.h`) defines the full V2 LDC surface:

```c
/* init-time lens config (part of the VPE channel attr) */
typedef struct {                 /* i6e_vpe_ildc */
    void *configAddr;            /* opaque LDC config blob (see "blob" below) */
    unsigned int configSize;
    int mapType;                 /* selects dispInfo vs calibInfo */
    union {
        struct { void *xMapAddr, *yMapAddr;       /* X/Y displacement maps */
                 unsigned int xMapSize, yMapSize; } dispInfo;
        struct { void *calibPolyBinAddr;          /* calibration polynomial */
                 unsigned int calibPolyBinSize; } calibInfo;
    };
    char lensAdjOn;
} i6e_vpe_ildc;

/* runtime lens adjust (part of the VPE channel param) */
typedef struct {                 /* i6e_vpe_ldc */
    char bypassOn;
    char proj3x3On;
    int  proj3x3[9];             /* 3x3 projection matrix (fixed-point) */
    unsigned int focalLengthX, focalLengthY;
    void *configAddr; unsigned int configSize;
    union { /* dispInfo | calibInfo, same as above */ };
} i6e_vpe_ldc;
```

These are wired into the channel structs the Star6E backend **already uses**:

- `MI_VPE_ChannelAttr_t` = `i6e_vpe_chn` → has `i6e_vpe_ildc lensInit;` +
  `char lensAdjOn;` (`include/sigmastar_types.h:397-413`,
  `include/star6e.h:124`).
- `MI_VPE_ChannelParam_t` = `i6e_vpe_para` → has `i6e_vpe_ldc lensAdj;` +
  `char lensAdjOn;` (`include/sigmastar_types.h:434-443`,
  `include/star6e.h:125`).

### Current pipeline leaves LDC off

`src/star6e_pipeline.c:504-544` creates the VPE channel and calls
`MI_VPE_SetChannelParam` with **`param.lensAdjOn = 0`** — LDC is explicitly
disabled and `lensInit`/`lensAdj` are zero-initialized. Enabling geometric
correction is therefore a change to *existing* call sites, not new plumbing.

### Runtime LDC entry points (exported, not yet bound)

`documentation/EIS_INTEGRATION_PLAN.md` confirms these are exported by
`libmi_vpe.so` (symbol dump) but **not** bound in `src/star6e_mi.c` yet
(only Create/Destroy/Start/Stop/SetChannelAttr/SetChannelParam/
SetPortMode/Enable/DisablePort/SetPortCrop are bound today):

| Symbol | Purpose |
|---|---|
| `MI_VPE_LDCSetViewConfig` | Set/replace LDC warp config on a running channel |
| `MI_VPE_LDCBegViewConfig` / `LDCEndViewConfig` | Atomic (begin/end) config update |
| `MI_VPE_LDCSetBatchViewConfig` | Multi-view / rolling-shutter slices |

**Key consequence:** the liveness probe needs **none** of these — LDC can be
turned on entirely through the already-bound `MI_VPE_CreateChannel` /
`MI_VPE_SetChannelParam` path by setting `lensAdjOn = 1` with an identity
`lensAdj`. `MI_VPE_LDCSetViewConfig` only becomes necessary for *runtime*
re-warp (a later phase / the EIS use case), and would be a one-line addition
to the `star6e_mi.c` binding table (signature to confirm from the SDK).

## The calibration blob — what we know vs. what to find

This is the central unknown for real (non-identity) lens correction.

### Known from the repo

- The blob is referenced two ways: as the opaque `configAddr`/`configSize`
  pair, and as the typed `calibInfo.calibPolyBinAddr` (a calibration
  **polynomial**) or `dispInfo.xMapAddr`/`yMapAddr` (raw X/Y pixel
  displacement maps). `mapType` selects which union arm is live.
- `documentation/EIS_INTEGRATION_PLAN.md` §"Key Unknowns" #1 flags exactly
  this: *"`i6e_vpe_ildc.configAddr` — unclear if this is required ... may
  need a dummy config or vendor-generated calibration file."*
- Prior-art caveat (from `specs/2026-07-09-maruko-ldc-gyro-dis/`): an
  exhaustive **standalone** MI_LDC attempt on i6e returned error 513 on every
  config + a kernel NULL-deref. That was the *standalone module*, not the
  VPE-embedded path this spec uses — but it means "LDC on i6e" is unproven
  on-device and the config-passing mechanism must be treated as suspect
  until the probe passes.
- No example calibration data, blob, `.bin`, or generator tool exists
  anywhere in **this** repo (searched: `calibPoly`, `xMap`, `dispmap`,
  `ViewConfig`, `*ldc*` files — only struct defs and prose).

### To find in the full Star6E SDK (the hunt list)

Run these when the full SDK is mounted under Claude Code. Goal: recover the
exact blob layout, whether it is mandatory, and any shippable example.

1. **Real struct + enum definitions** — locate the vendor `mi_vpe.h` /
   `mi_vpe_datatype.h` (or `mi_ldc*.h`) and diff against our
   `i6e_vpe_ildc` / `i6e_vpe_ldc`. Confirm: field order/sizes, the
   `mapType` enum values (which int = DISPMAP vs SENSORCALIB), and the
   fixed-point format + scale of `proj3x3[9]` (Q-format? row- or
   column-major? identity = `[65536,0,0, 0,65536,0, 0,0,65536]` or
   `[4096,...]`?).
2. **`MI_VPE_LDCSetViewConfig` signature** — exact prototype (arg struct
   type) so the `star6e_mi.c` binding is correct for the runtime phase.
3. **Example calibration data** — grep the SDK demos/tools for shipped
   blobs and generators:
   - files: `*.bin` near `ldc`/`dewarp`/`calib`; `*ldc*`, `*dewarp*`,
     `*fisheye*`, `*calib*` under `ipc_demo*/`, `sample*/`, `tools*/`,
     `Tools/`, `doc*/`.
   - symbols/strings: `LDCSetViewConfig`, `calibPolyBin`, `SENSORCALIB`,
     `DISPMAP`, `GenLdc`, `dewarp`, `mesh`.
   - a **mesh/grid generator** (common SigmaStar deliverable: a PC tool that
     turns lens intrinsics/distortion coefficients into the displacement
     map or poly blob). Note its input format (OpenCV `k1..k4`, or a grid).
4. **Is the blob mandatory?** — read any `ldc.c` demo: does it pass a real
   `configAddr` for a plain LDC (non-DIS) channel, or is NULL/identity
   accepted? Note the exact create→setparam→(start) ordering and whether
   LDC must be set at CreateChannel or can be toggled via SetChannelParam
   on a running channel.
5. **Constraints** — capture from demos/release notes: max in/out
   resolution with LDC on, memory-bandwidth cost, whether LDC composes with
   VPE port scaling (our port does ISP→scale→VENC) and with
   `MI_VPE_SetPortCrop` (used by `stab`/`stab-fill`), and any REALTIME-link
   restriction (our VIF→VPE is `I6_SYS_LINK_REALTIME`).

Record findings in `plan.md` §"SDK findings" as they come in.

## De-risking plan (summary — detail in plan.md)

- **Phase 0 — identity liveness** (existing bench `root@192.168.1.13`,
  imx335): flip `lensAdjOn = 1` with an identity 3×3 (or NULL blob) via the
  already-bound SetChannelParam path. Gate: frames still reach VENC, no
  stall, no error-513 ghost. Kills-or-confirms the whole idea for ~free.
- **Phase 1 — visible static warp**: a deliberately non-identity `proj3x3`
  (small zoom/shear) to prove the warp is actually applied and measure
  added latency / bandwidth at 1080p and at 120fps.
- **Phase 2 — real lens correction**: feed a calibration blob (poly or
  displacement map, from the SDK generator against the fpv lens) and verify
  straight lines. Only reachable once the blob format is recovered (hunt
  list above).

## Open questions

1. Is `configAddr` required for `WORKMODE_LDC`, or is an identity
   3×3/NULL-blob channel accepted? (Phase 0)
2. Fixed-point format + majorness of `proj3x3[9]`? (SDK hunt #1)
3. Does the SDK ship a blob generator + any example calibration? (hunt #3)
4. Does LDC compose with VPE port scaling and with `SetPortCrop`
   (`stab-fill`), or is it mutually exclusive? (hunt #5 / Phase 1)
5. Latency / memory-bandwidth cost of LDC-on at 1080p120? (Phase 1)
6. Must LDC be configured at `CreateChannel` (`lensInit`) or can it be
   toggled live via `SetChannelParam` (`lensAdj`)? Teardown/reinit ordering
   implications (cf. `documentation/FULL_TEARDOWN_REINIT_PLAN.md`).

## Verified fact sources (this repo)

- `include/sigmastar_types.h:355-443` (LDC structs, VPE chn/para)
- `include/star6e.h:124-128,335-341` (typedefs + VPE prototypes)
- `src/star6e_pipeline.c:504-544` (VPE channel create, `lensAdjOn = 0`)
- `src/star6e_mi.c:138-160` (bound VPE symbols — LDC funcs absent)
- `documentation/EIS_INTEGRATION_PLAN.md` (exported LDC symbols; blob unknown)
- `documentation/Sigmastar_EIS_Research.md` (V2-vs-V3 LDC generations)
- `specs/2026-07-09-maruko-ldc-gyro-dis/requirements.md` (i6e-513 prior art)
