# DIVP-Channel Architecture for Static OSD + Hardware Stab Crop

<!-- version: 0.1.0 -->

Companion to `DIVP_STAB_TEST_PLAN.md` and `IMAGE_STAB_OSD_STATUS.md`.
Captures the architecture that becomes possible now that Q1 has confirmed
DIVP is usable on the Star6E BSP.

Status: design proposal — not yet implemented. PR #119 is the surgical
`MI_DIVP_StretchBuf` swap only.

---

## 1. Problem recap

Today (PR #119) the stab path is:

```
VIF → VPE port0 (manual drain) → stab thread:
    1. ChnOutputPortGetBuf VPE
    2. IVE shift detector
    3. ChnInputPortGetBuf VENC
    4. MI_DIVP_StretchBuf (Y+UV crop, ATOMIC)         ← was 2× BlitPa
    5. ChnInputPortPutBuf VENC
    6. ChnOutputPortPutBuf VPE
```

Two unresolved feature gaps come along for the ride:

- **OSD lag (`IMAGE_STAB_OSD_STATUS.md` §2).** RGN paints onto the VPE
  port output canvas; the stab thread then crops that canvas. With the
  crop offset moving frame to frame, the user-perceived OSD position
  lags the crop by one frame. The runtime tries to compensate via
  `debug_osd_set_panel_offset`, but always one frame stale.
- **Snapshot/JPEG starvation.** `star6e_jpeg.c:109` binds JPEG-VENC's
  input to VPE port0; the stab thread monopolises that port via
  `ChnOutputPortGetBuf`, so the JPEG bind sees no frames. Pre-existing.

The previous `OSD-on-VENC` attempt (`d069ad2`) used `RGN_MODID_VENC=2`
and failed silently. **That was actually a misnamed constant** — the
vendor enum `MI_RGN_ModId_e` is:

```
E_MI_RGN_MODID_VPE  = 0
E_MI_RGN_MODID_DIVP = 1
E_MI_RGN_MODID_LDC  = 2
```

(`waybeam-hub/vendor/sigmastar/include/mi_rgn_datatype.h:21-27`).
Value 2 is LDC, not VENC. **RGN attach to VENC is not supported on
this BSP.** The only HW compositor attach points are VPE, DIVP, LDC.

`src/debug_osd.c:228-235` should be corrected to match this, and the
`debug_osd_create_for_venc` path either removed or repurposed.

## 2. Proposal — route stab through a DIVP channel

DIVP exposes a full channel API (`MI_DIVP_CreateChn`, `SetChnAttr`,
`StartChn`, …) with input + output ports that can be bound like any
other MI module, and **RGN can attach to a DIVP channel** via
`E_MI_RGN_MODID_DIVP`. New layout:

```
VIF → VPE port0 ─bind→ DIVP chn0 (crop + RGN OSD composite) ─bind→ VENC ch0
                  │
                  └── stab thread: drain VPE port0 (or read DIVP
                      input PTS), run IVE shift detector,
                      MI_DIVP_SetChnAttr(chn0, stCropRect = new offset)
                      per frame.
```

Properties:

| Property | Today (PR #119) | Proposed |
|---|---|---|
| NV12 crop primitive | `MI_DIVP_StretchBuf` from stab thread | `MI_DIVP_ChnAttr_t.stCropRect`, applied by DIVP hardware on its own |
| OSD compositor | RGN on VPE (pre-crop) | RGN on DIVP (post-crop) |
| OSD position relative to encoded frame | drifts with crop offset, +1 frame lag | static — RGN canvas is in DIVP output coords |
| VPE→VENC bind | absent (manual CPU handoff) | absent (replaced by VPE→DIVP→VENC bind chain) |
| Stab-thread CPU work | GetBuf×2 + StretchBuf + PutBuf×2 | drain VPE, IVE shift, one `SetChnAttr`, release |
| JPEG-VENC snapshot | starves on VPE port0 contention | can attach to DIVP output (or a second DIVP chn) — no contention |
| Latency added | ~0 (same buffer in/out) | +1 DIVP frame (queue) |
| Failure mode | DIVP returns error → fall back to `BlitPa` | DIVP channel start fails → fall back to current PR #119 path |

## 3. Concrete API sequence

Setup (in `bind_and_finalize_pipeline` after VIF→VPE bind, replacing
the current `star6e_stab_start`):

```c
MI_DIVP_InitParam_t init = { .u32DevId = 0 };
MI_DIVP_InitDev(&init);

MI_DIVP_ChnAttr_t attr = {
    .u32MaxWidth  = src_w,
    .u32MaxHeight = src_h,
    .eTnrLevel    = E_MI_DIVP_TNR_LEVEL_OFF,
    .eDiType      = E_MI_DIVP_DI_TYPE_OFF,
    .eRotateType  = E_MI_SYS_ROTATE_NONE,
    .stCropRect   = { (src_w - enc_w)/2, (src_h - enc_h)/2, enc_w, enc_h },
    .bHorMirror   = MI_FALSE,
    .bVerMirror   = MI_FALSE,
};
MI_DIVP_CreateChn(0, &attr);

MI_DIVP_OutputPortAttr_t out = {
    .u32Width     = enc_w,
    .u32Height    = enc_h,
    .ePixelFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420,
    .eCompMode    = E_MI_SYS_COMPRESS_MODE_NONE,
};
MI_DIVP_SetOutputPortAttr(0, &out);
MI_DIVP_StartChn(0);

/* Attach OSD AFTER DIVP starts, so canvas size matches output dim. */
MI_RGN_ChnPort_t divp_port = {
    .eModId          = E_MI_RGN_MODID_DIVP,
    .s32DevId        = 0,
    .s32ChnId        = 0,
    .s32OutputPortId = 0,
};
MI_RGN_AttachToChn(osd_handle, &divp_port, &chn_attr);

/* Bind chain — frames flow without CPU help. */
MI_SYS_BindChnPort2(&vpe_port,  &divp_in_port,  fps, fps, REALTIME, 0);
MI_SYS_BindChnPort2(&divp_out_port, &venc_port, fps, fps, FRAMEBASE, 0);
```

Per frame (stab thread, simplified):

```c
while (running) {
    /* Pull VPE frame via the dup'd output port for IVE only. */
    GetBuf(vpe_port_dup, &frame);
    int dx, dy = ive_shift(prev, frame);
    PutBuf(handle);

    acc_x = clamp(acc_x + dx_to_offset(dx));
    acc_y = clamp(acc_y + dy_to_offset(dy));

    /* Hot-update DIVP crop — frames already inflight use old crop;
     * the next frame DIVP receives is cropped at the new offset. */
    MI_DIVP_ChnAttr_t a;
    MI_DIVP_GetChnAttr(0, &a);
    a.stCropRect.u16X = center_x + acc_x;
    a.stCropRect.u16Y = center_y + acc_y;
    MI_DIVP_SetChnAttr(0, &a);
}
```

VENC ch0 input now arrives via the bind, so all the
`ChnInputPortGetBuf` / `PutBuf` plumbing in `star6e_stab_send_frame_to_venc`
goes away.

## 4. Open questions before implementation

These need on-device verification before committing to the rewrite.

### Q-DIVP-1: Is `MI_DIVP_SetChnAttr` cheap enough per-frame?
Vendor docs don't specify latency. If it stalls the engine, we have a
problem at 60 fps. Test: drive `SetChnAttr` 60×/s with varying
`stCropRect` and watch DIVP output fps.

### Q-DIVP-2: Does RGN attach to DIVP composite correctly on this BSP?
The enum value exists; whether `libmi_rgn.so` actually paints onto DIVP
output on the ssc338q firmware is a separate fact. Test: minimal
sample binary that creates a DIVP chn, attaches a known palette
canvas, encodes 10 frames, dumps for visual check.

### Q-DIVP-3: Does IVE want VPE-port frames or DIVP-input frames?
IVE wants the un-cropped frame so the search window has motion to
detect. The current stab thread reads VPE port0 directly; we need to
keep that path even with the bind in place. Either:
- VPE port0 supports 1-to-N output (we'd test on Star6E),
- or we open VPE port1 for IVE (eats a port budget — may block dual VENC).

### Q-DIVP-4: VENC bitrate accounting under bind chain
Today the stab thread sets PTS explicitly via `star6e_stab_pts_us()`.
With bind chains the SDK assigns PTS. Confirm AVBR/CBR rate control
still tracks 60 fps correctly under the new timing source.

## 5. Implementation order (when greenlit)

1. **Land PR #119** (surgical `StretchBuf` swap) — done modulo Q3.
2. **Probe Q-DIVP-2** with a standalone tool (extend `tools/divp_probe.c`)
   to verify RGN-on-DIVP renders.
3. **Probe Q-DIVP-1, Q-DIVP-3** in same expanded probe binary.
4. **Fix the RGN constant bug**: correct `RGN_MODID_VENC=2` comment
   and dead `debug_osd_create_for_venc` path in `src/debug_osd.c`.
5. **Build the DIVP-channel stab path** behind a config flag
   (`video0.stabBackend = "blit" | "stretch" | "channel"`) so both
   shipping paths coexist while the new one bakes.
6. **Make snapshot work with stab** — bind JPEG-VENC to DIVP output
   port instead of VPE port0. Independent of the stab backend choice.

Each step is mergeable independently.

## 6. Side benefits if this lands

- **Static OSD** without per-frame anchor compensation — the cosmetic
  defect described in `IMAGE_STAB_OSD_STATUS.md` §2 goes away because
  the OSD canvas is in DIVP output coordinates, not source.
- **Snapshot endpoint works while stab is on** — JPEG no longer fights
  for VPE port0.
- **CPU savings in the stab hot loop** — no `GetBuf/PutBuf` pair for
  VENC, no manual `StretchBuf` call (DIVP runs autonomously off the
  bind).
- **Cleaner dual-VENC story** — DIVP output can feed both VENC channels
  via bind, no second crop path.
- **Maruko parity opportunity** — DIVP exists on Maruko (Infinity6C)
  too; if this works on Star6E we can fold the equivalent into
  `src/maruko_pipeline.c`.

## 7. What this proposal does NOT change

- IVE shift detector logic — same dx/dy math.
- Crop percent / recenter UX — same `stabCropPct` / `stabRecenterSpeed`
  config keys.
- BlitPa fallback path — kept indefinitely as a safety net for BSPs
  without DIVP, and as the current fallback in PR #119.
