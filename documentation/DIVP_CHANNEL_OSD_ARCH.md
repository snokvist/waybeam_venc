# DIVP-Channel Architecture for Static OSD + Hardware Stab Crop

<!-- version: 1.0.0 -->

Companion to `DIVP_STAB_TEST_PLAN.md` and `IMAGE_STAB_OSD_STATUS.md`.

Status: **IMPLEMENTED.** Live in PR #119 as of commit `051887b`. The four
waves described in §5 are merged and verified on bench `192.168.1.13`.
The original surgical `MI_DIVP_StretchBuf` swap (commit `338036c`) is
kept as the default backend; the channel pipeline is opt-in via
`video0.stabBackend = "channel"`.

---

## 1. Problem recap

Pre-PR #119 the stab path was:

```
VIF → VPE port0 (manual drain) → stab thread:
    1. ChnOutputPortGetBuf VPE
    2. IVE shift detector
    3. ChnInputPortGetBuf VENC
    4. MI_SYS_BufBlitPa Y + UV (later: MI_DIVP_StretchBuf, PR #119)
    5. ChnInputPortPutBuf VENC
    6. ChnOutputPortPutBuf VPE
```

Two unresolved feature gaps came along for the ride:

- **OSD lag (`IMAGE_STAB_OSD_STATUS.md` §2).** RGN painted onto the VPE
  port output canvas; the stab thread then cropped that canvas. With the
  crop offset moving frame to frame, the user-perceived OSD position
  lagged the crop by one frame.
- **Snapshot/JPEG starvation.** JPEG-VENC's input was bound to VPE port0;
  the stab thread monopolised that port via `ChnOutputPortGetBuf`, so the
  JPEG bind saw no frames.

The previous `OSD-on-VENC` attempt (`d069ad2`) used `RGN_MODID_VENC=2`
and failed silently. **That was a misnamed constant** — the vendor enum
`MI_RGN_ModId_e` is:

```
E_MI_RGN_MODID_VPE  = 0
E_MI_RGN_MODID_DIVP = 1
E_MI_RGN_MODID_LDC  = 2
```

(`waybeam-hub/vendor/sigmastar/include/mi_rgn_datatype.h:21-27`).
Value 2 is LDC, not VENC. **RGN attach to VENC is not supported on
this BSP.** The only HW compositor attach points are VPE, DIVP, LDC.
`src/debug_osd.c` was corrected to match.

## 2. Solution — route stab through a DIVP channel

DIVP exposes a full channel API (`MI_DIVP_CreateChn`, `SetChnAttr`,
`StartChn`, …) with input + output ports that can be bound like any
other MI module, and **RGN attaches to a DIVP channel** via
`E_MI_RGN_MODID_DIVP`. Layout when `video0.stabBackend = "channel"`:

```
VIF → VPE port0 ─bind→ DIVP chn0 (crop + RGN OSD composite) ─bind→ VENC ch0
                  │                                            │
                  │                                            └─bind→ JPEG-VENC chn7
                  │
                  └── stab thread: drain VPE port0 for IVE only,
                      run IVE shift detector,
                      MI_DIVP_SetChnAttr(chn0, stCropRect = new offset)
                      per frame.
```

Properties (measured):

| Property | Default (`stretch`) | `channel` backend |
|---|---|---|
| NV12 crop primitive | `MI_DIVP_StretchBuf` from stab thread | `MI_DIVP_ChnAttr_t.stCropRect`, applied by DIVP hardware on its own |
| OSD compositor | RGN on VPE (pre-crop) | RGN on DIVP (post-crop) |
| OSD position relative to encoded frame | drifts with crop offset, +1 frame lag | **static** — RGN canvas is in DIVP output coords |
| VPE→VENC bind | absent (manual CPU handoff) | absent (replaced by VPE→DIVP→VENC bind chain) |
| Stab-thread CPU work | GetBuf×2 + StretchBuf + PutBuf×2 | drain VPE for IVE, IVE shift, one `SetChnAttr`, release |
| JPEG-VENC snapshot under stab | starves (VPE port0 contention) | **works** (bound to DIVP output) |
| Latency added | ~0 (same buffer in/out) | +1 DIVP frame (queue) |
| Failure mode | DIVP returns error → fall back to manual BlitPa | DIVP channel start fails → fall back to `stretch` |

## 3. Concrete API sequence (as implemented)

Setup (in `star6e_stab_channel_create`, replacing the stretch-backend
init path when the backend selector returns `channel`):

```c
MI_DIVP_InitParam_t init = { .u32DevId = 0 };
g_MI_DIVP_InitDev(&init);

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
g_MI_DIVP_CreateChn(STAB_DIVP_CHN, &attr);

MI_DIVP_OutputPortAttr_t out = {
    .u32Width     = enc_w,
    .u32Height    = enc_h,
    .ePixelFormat = 0x0B,                    /* I6_PIXFMT_YUV420SP — Q1 finding */
    .eCompMode    = E_MI_SYS_COMPRESS_MODE_NONE,
};
g_MI_DIVP_SetOutputPortAttr(STAB_DIVP_CHN, &out);
g_MI_DIVP_StartChn(STAB_DIVP_CHN);

/* RGN OSD attaches AFTER DIVP starts.  Canvas size matches DIVP output. */
debug_osd_create_for_divp(enc_w, enc_h, STAB_DIVP_CHN);

/* Bind chain — frames flow without CPU help. */
MI_SYS_BindChnPort2(&vpe_out_port,  &divp_in_port,  fps, fps, REALTIME, 0);
MI_SYS_BindChnPort2(&divp_out_port, &venc_in_port,  fps, fps, FRAMEBASE, 0);

/* JPEG-VENC is initialised later in jpeg_init.  Its input port is now
 * DIVP output instead of VPE port0 when channel backend is active. */
```

Per frame (`star6e_stab_channel_thread_main`, simplified):

```c
while (running) {
    GetBuf(vpe_port_for_ive, &frame);          /* IVE-only drain */
    int dx, dy = ive_shift(prev, frame);
    PutBuf(handle);

    acc_x = clamp(acc_x + dx_to_offset(dx));
    acc_y = clamp(acc_y + dy_to_offset(dy));

    /* Hot-update DIVP crop — next frame DIVP receives is cropped at
     * the new offset.  Frames already inflight use the previous crop. */
    MI_DIVP_ChnAttr_t a;
    g_MI_DIVP_GetChnAttr(STAB_DIVP_CHN, &a);
    a.stCropRect.u16X = center_x + acc_x;
    a.stCropRect.u16Y = center_y + acc_y;
    g_MI_DIVP_SetChnAttr(STAB_DIVP_CHN, &a);
}
```

VENC ch0 input arrives via the bind — all `ChnInputPortGetBuf` /
`PutBuf` plumbing from `star6e_stab_send_frame_to_venc` is bypassed
in this backend.

## 4. Open questions — resolved

| Q | Status | Evidence |
|---|---|---|
| **Q-DIVP-1**: Is `MI_DIVP_SetChnAttr` cheap enough per-frame? | **PASS** | 60 fps sustained on bench. No visible stalls when `SetChnAttr` fires every frame with a moving `stCropRect`. The engine accepts the update mid-stream and applies it to the next inbound frame. |
| **Q-DIVP-2**: Does RGN attach to DIVP composite correctly? | **PASS** | `[debug_osd] overlay 1024x768 ... attached to rgn_mod=1 dev=0 chn=0` log confirms `E_MI_RGN_MODID_DIVP=1` attach. JPEG snapshot via `/api/v1/snapshot.jpg` renders OSD pixel-perfect onto the DIVP output. |
| **Q-DIVP-3**: Does IVE want VPE or DIVP frames? | **PASS — VPE works fine** | The stab thread keeps draining VPE port0 for IVE input. VPE port0 happily serves both the IVE-only drain AND the DIVP bind on this BSP — no port budget pressure, no need for VPE port1. |
| **Q-DIVP-4**: VENC bitrate accounting under bind chain? | **PASS** | 6.3 Mbps CBR holds tight under the channel backend, identical to the stretch backend. SDK assigns PTS via the bind; no manual `star6e_stab_pts_us()` needed in this path. |

## 5. Implementation — what landed

Each step is a separate commit on PR #119:

1. **`338036c` — Surgical `StretchBuf` swap.** Default `stretch` backend.
2. **`0c055e2`, `77ef6df` — divp_probe** for Q1 (no-channel direct-buf
   verification + pixfmt sweep).
3. **`a798e8b` — Q2 PASS proof** (DIVP preserves orientation).
4. **`fc50730` — Q3 PASS + this design proposal.**
5. **`051887b` — Four-wave channel implementation:**
   - Wave 1: `video0.stabBackend` config knob (`stretch` | `channel`).
   - Wave 2: DIVP-channel pipeline (`star6e_stab_channel_create`,
     `..._thread_main`, `..._start`, `..._stop`; VPE→DIVP→VENC bind chain;
     per-frame `SetChnAttr` crop update).
   - Wave 3: OSD attaches to DIVP via `debug_osd_create_for_divp`.
     `star6e_pipeline_stab_panel_anchor()` returns 0 when channel backend
     is active — no per-frame offset compensation needed.
   - Wave 4: JPEG snapshot bound to DIVP output port instead of VPE port0.
     Works while stab is active.

The `RGN_MODID_VENC=2` mis-comment in `debug_osd.c` was corrected as part
of Wave 3. `debug_osd_create_for_venc` is kept as an ABI shim with a
deprecation warning; on Star6E it now falls back to VPE attach.

## 6. Side benefits delivered

- **Static OSD** without per-frame anchor compensation — the cosmetic
  defect described in `IMAGE_STAB_OSD_STATUS.md` §2 is gone in the
  channel backend. OSD canvas lives in DIVP output coordinates; pan
  tests (zoomX/Y 0.5/0.5 vs 0.15/0.85) show OSD pixel-aligned at the
  top-left while scene content shifts.
- **Snapshot endpoint works while stab is on** — JPEG no longer fights
  for VPE port0. Bench reproduction:
  `curl -o /tmp/s.jpg http://192.168.1.13/api/v1/snapshot.jpg` returns
  a valid 1024×768 JPEG with the OSD baked in, under any `stabCropPct`.
- **CPU savings in the stab hot loop** — no `GetBuf/PutBuf` pair for
  VENC, no manual `StretchBuf` call (DIVP runs autonomously off the
  bind). Only the IVE drain and the `SetChnAttr` remain on the stab thread.
- **Cleaner dual-VENC story** — DIVP output can feed both VENC channels
  via bind, no second crop path. (Not exercised yet — dual VENC config
  still falls back to `stretch` for backwards compatibility.)
- **Maruko parity opportunity** — DIVP exists on Maruko (Infinity6C)
  too; the channel-backend code is straightforward to port into
  `src/maruko_pipeline.c`. Followup, not in this PR.

## 7. What this proposal does NOT change

- IVE shift detector logic — same dx/dy math.
- Crop percent / recenter UX — same `stabCropPct` / `stabRecenterSpeed`
  config keys.
- `stretch` backend — kept as default and as fallback when DIVP channel
  creation fails. Both backends ship in the same binary.
- Maruko pipeline — channel-backend is Star6E-only for now.

## 8. Operator usage

Switch backends via `json_cli`:

```bash
# Enable channel backend (static OSD, working snapshot under stab)
json_cli -s .video0.stabBackend '"channel"' -i /etc/waybeam.json -o /etc/waybeam.json
killall waybeam   # restart-only field

# Revert to the surgical stretch swap (PR #119 default)
json_cli -s .video0.stabBackend '"stretch"' -i /etc/waybeam.json -o /etc/waybeam.json
killall waybeam
```

The dashboard exposes the same field as `stabBackend` (camelCase alias).
Unknown values are rejected by the schema validator.
