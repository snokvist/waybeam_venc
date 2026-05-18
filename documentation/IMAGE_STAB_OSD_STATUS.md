# Image Stabilization + Debug OSD — Current State

Branches:
- v0.11.0 stab feature: `claude/add-image-stabilization-vpe-jG0ii` (PR #118)
- DIVP rework + static OSD: `claude/review-divp-pipeline-HASzf` (PR #119)

Bench device: `root@192.168.1.13` (ssc338q + imx335 @ 60 fps).

This note captures where image stabilization stands after PR #119's
four-wave DIVP-channel implementation. The one-frame OSD lag described
in earlier revisions of this document is **resolved** in the channel
backend; the stretch backend still ships and behaves as previously
documented.

---

## 1. What ships today

### Config surface
Three keys under `video0`. All require restart:

| Key                    | Type   | Range          | Default     | Effect |
|------------------------|--------|----------------|-------------|--------|
| `stabCropPct`          | int    | 0, or 50..100  | 0           | 0 = stabilization off.  Otherwise the encoded view is a centered crop of `stabCropPct`% of the VPE port output. |
| `stabRecenterSpeed`    | int    | 0..1000 frames | 0           | Time-constant τ for the exponential return-to-center.  0 = no recenter (crop window stays wherever the patch ends up).  Lower = snappier, higher = smoother/slower.  Typical at 60 fps: 30–120. |
| `stabBackend`          | enum   | `stretch` \| `channel` | `stretch` | Crop primitive selector.  See §1.1 for behavior differences. |

### 1.1 Backend choice

Two complete pipelines coexist in the same binary. Selection is per-restart.

**`stretch` (default).** Surgical replacement of the original BufBlitPa
pair with a single `MI_DIVP_StretchBuf` direct-buffer call. Same
control flow as v0.11.0: stab thread does Get/Put on both VPE and VENC,
DIVP only handles the crop arithmetic. OSD attaches to VPE port (id 0),
so it lives in pre-crop coordinates and lags the crop by one frame.

**`channel`.** Full DIVP-channel pipeline (`VPE → DIVP → VENC` bind chain).
DIVP applies the crop via `stCropRect`, updated per frame via
`MI_DIVP_SetChnAttr`. RGN OSD attaches to DIVP (id 1), so the canvas is
in **post-crop / encoded-frame coordinates** — the OSD is pixel-static
regardless of how the crop window moves. JPEG snapshot is rebound to
DIVP output so `/api/v1/snapshot.jpg` works while stab is active.

See `DIVP_CHANNEL_OSD_ARCH.md` for the full architectural rationale.

User-tested combos:
- `venc 80 10` (80 % crop, τ=10) — described as "perfect" before the
  exp-decay change.
- `venc 90 6` — looser stabilization, flyable.

After the exp-decay rework, τ semantic became "frames" rather than
"frames-per-pixel-leak", so equivalent feel needs a larger number.
60–90 was the figure tested last and matched the original feel.

### Pipeline architecture (Star6E, ch0 only)

**`stretch` backend:**

```
VIF ──► VPE port0 (manual drain)
            │
            ├──► star6e_stab_thread:
            │     1. ChnOutputPortGetBuf  (NV12 frame from VPE)
            │     2. MI_IVE_Shift_Detector → dx, dy (int8)
            │     3. accumulate, clamp, optional exp-decay recenter
            │     4. ChnInputPortGetBuf   (VENC ch0 input)
            │     5. MI_DIVP_StretchBuf   (Y+UV crop, no channel)
            │     6. ChnInputPortPutBuf
            │     7. ChnOutputPortPutBuf  (release VPE frame)
            │
            └──► RGN debug OSD attaches to VPE port (module id 0)
                 — pre-crop canvas, per-frame anchor compensation
```

**`channel` backend:**

```
VIF ──► VPE port0 ──bind──► DIVP chn0 ──bind──► VENC ch0
              │                   │                  │
              │                   │                  └─bind─► JPEG-VENC chn7
              │                   │
              │                   └── RGN debug OSD attaches to DIVP (id 1)
              │                       — post-crop canvas, static in encoded coords
              │
              └──► star6e_stab_channel_thread:
                    1. ChnOutputPortGetBuf VPE   (IVE drain only)
                    2. MI_IVE_Shift_Detector → dx, dy
                    3. accumulate, clamp, optional exp-decay recenter
                    4. MI_DIVP_SetChnAttr (update stCropRect for next frame)
                    5. ChnOutputPortPutBuf VPE
```

Key facts (`stretch`):
- VPE→VENC bind is **skipped**; stab thread hand-rolls the bind via DIVP StretchBuf.
- Pipeline reports the **crop dim** as `pconf.image_w/h` so VENC encodes the cropped size (no rescale).
- ch1 (dual VENC) and JPEG snapshot read VPE port1 / VENC subports — un-stabilized, un-OSD'd.
- Zoom integrates: `star6e_pipeline_apply_zoom` detects `g_stab_running` and routes pan into `star6e_stab_set_pan`.

Key facts (`channel`):
- VPE→VENC bind is **also skipped**, replaced by the VPE→DIVP→VENC chain.
- Pipeline reports the **encoded dim** (same as the DIVP output port attr).
- JPEG-VENC is bound to **DIVP output** instead of VPE port0 — snapshot works while stab is on.
- `star6e_pipeline_stab_panel_anchor()` returns 0 (no per-frame anchor compensation needed; OSD is already in post-crop coords).
- IVE drains **VPE port 1** (not port 0). VPE port 0 is FRAMEBASE-bound to DIVP; a manual `GetBuf` on the same port returns stale frames, so port 1 is opened as a sibling mirror dedicated to the IVE drain (`star6e_stab_enable_vpe_port1_for_ive`, commit `76101e1`).

### Debug OSD integration

**`stretch` backend (legacy path):**
- OSD attaches to **VPE port** (module id 0). Canvas sized to full VPE port dim (e.g. 1280×960). Encoded view is a smaller crop (e.g. 1024×768).
- Per-frame in `star6e_runtime.c` the code reads `star6e_pipeline_stab_panel_anchor()` and calls `debug_osd_set_panel_offset(x, y)` to shift the stats-panel anchor to the current crop-window origin.
- This compensation always lags by one frame — see §2.

**`channel` backend (PR #119):**
- OSD attaches to **DIVP** (module id 1) via `debug_osd_create_for_divp(enc_w, enc_h, STAB_DIVP_CHN)`. Canvas sized to the DIVP output dim, which equals the encoded dim.
- No per-frame anchor compensation. Panel offset is identity. RGN composites onto the already-cropped DIVP output, so OSD is pixel-static in encoded-frame coordinates.

### Recenter math (smoothing)

Per frame, after computing the new dx/dy from IVE and accumulating:
```c
if (tau > 0) {
    if (tau < 2) tau = 2;
    if (acc_x > 0)
        acc_x = (int)((uint32_t)acc_x * (tau - 1) / tau);
    else if (acc_x < 0)
        acc_x = -(int)((uint32_t)(-acc_x) * (tau - 1) / tau);
    /* same for acc_y */
}
```
Integer-only, truncates toward zero, so the buffer naturally reaches 0
even when `(tau-1)/tau` would otherwise round-trip the last pixel
forever.

### Files touched
**v0.11.0 (PR #118):**
- `include/venc_config.h` — `stabCropPct`, `stabRecenterSpeed`
- `src/venc_config.c` — defaults, JSON load/render, clamp
- `src/venc_api.c` — schema entries (`MUT_RESTART`), camelCase aliases
- `config/waybeam.default.json` — defaults 0/0
- `web/dashboard.html` — number widgets + tooltips
- `src/star6e_pipeline.c` — stab thread, lifecycle, public anchor accessor
- `src/star6e_runtime.c` — per-frame OSD anchor update (now no-op in channel backend)
- `include/debug_osd.h` + `src/debug_osd.c` — `set_panel_offset`

**PR #119 (DIVP rework + channel backend):**
- `include/venc_config.h` — `stabBackend` field
- `src/venc_config.c`, `src/venc_api.c` — schema + validation + alias
- `src/star6e_pipeline.c` — channel-backend create/start/stop/thread; DIVP ABI typedefs and dlsym; JPEG re-bind to DIVP output
- `include/debug_osd.h` + `src/debug_osd.c` — `debug_osd_create_for_divp`; correct `RGN_MODID_*` enum mapping; `create_for_venc` deprecated
- `tools/divp_probe.c` — standalone Q1 verification binary
- `documentation/DIVP_STAB_TEST_PLAN.md`, `DIVP_CHANNEL_OSD_ARCH.md`, `IMAGE_STAB_OSD_STATUS.md`

---

## 2. The OSD-lag quirk — fixed in the channel backend

### Original problem (stretch backend, still applies there)

> "The OSD actually doesn't sit still — it still moves while the port
> moves, but it strives to return to its top-left position with a little
> delay."

**Root cause: the OSD anchor lags the crop by exactly one frame.**

Timeline of frame N (stretch backend):
1. Stab thread pulls VPE frame N out of port0.
2. Stab thread computes dx,dy from IVE on frame N (relative to frame N−1
   reference), updates `g_stab_off_x/y`.
3. Stab thread DIVP-stretches the shifted crop of frame N into VENC ch0 input.
4. Runtime main loop, on its **next** iteration, sees the new
   `g_stab_off_x/y` via `star6e_pipeline_stab_panel_anchor()` and writes
   the new panel offset into the OSD canvas — this canvas is painted by
   RGN onto VPE port output **frame N+1**.

So in the stretch backend the OSD pixels live on a different VPE port
frame than the crop they're supposed to track. The visible effect is
bounded — at 60 fps the panel position is one frame stale (~16.7 ms
behind the cropped image). Worst during sustained motion; disappears
when crop is stationary.

### Fix (channel backend)

PR #119 added the `channel` backend, which moves RGN attach from VPE
(pre-crop) to DIVP (post-crop). The OSD canvas is now in DIVP output
coordinates — the same coordinate system as the encoded frame. RGN
composites onto the **already-cropped** DIVP output, so there is
nothing for the OSD to "track". It is static by construction.

`star6e_pipeline_stab_panel_anchor()` returns 0 in the channel backend,
so the per-frame `debug_osd_set_panel_offset` call in `star6e_runtime.c`
becomes a no-op.

Bench proof (`051887b`): two snapshots at zoomX/Y `0.5/0.5` and
`0.15/0.85` show scene content shifting dramatically while the OSD
panel remains pixel-aligned at the top-left in both. RGN log line
`[debug_osd] overlay 1024x768 ... attached to rgn_mod=1 dev=0 chn=0`
confirms DIVP attach (rgn_mod=1).

---

## 3. Resolved questions (channel backend)

- ✅ **OSD static during stab pan** — DIVP post-crop attach (PR #119, Wave 3).
- ✅ **Snapshot works while stab is on** — JPEG-VENC rebound to DIVP output port (PR #119, Wave 4).
- ✅ **RGN attach to VENC is impossible on this BSP** — confirmed via vendor `MI_RGN_ModId_e` enum (VPE/DIVP/LDC only). `debug_osd_create_for_venc` is deprecated and falls back to VPE attach with a warning.
- ✅ **`stretch` backend is still the default** — channel backend is opt-in via `video0.stabBackend = "channel"`.

---

## 4. Pan headroom gotcha (both backends)

When `zoomX/zoomY` push the crop window toward an edge, the IVE
accumulator can be fully consumed before it makes any visible difference
because the crop is clamped to `[0, src−enc]`. Example with
`image=2560×1920`, `stabCropPct=80` (enc=2048×1536), `zoomX=0.15`:

- desired crop x = `2560·0.15 − 2048/2 + acc_x = −640 + acc_x`
- clamped to `[0, 512]`
- `acc_x` is bounded to `±max_off_x = ±256` → crop stays at `0` no
  matter what IVE reports

Symptom: IVE detects motion correctly (`raw=(−34,0)`, `raw=(96,−6)`,
etc.) but the encoded view doesn't appear stabilized. The crop is
mechanically pinned at the edge.

Workaround: keep `zoomX/zoomY` near `0.5` when relying on stab, or
lower `stabCropPct` for more pan freedom (smaller `enc` → more
`max_off` headroom). Same math in both backends.

Followup ideas (not implemented):
- Startup warning when saved pan consumes the full headroom on an axis.
- Asymmetric `acc` clamps based on actual per-side headroom.

## 5. Known followups (independent of stab)

- **IMX335 mirror+flip combo wedge.** `image.mirror=true + image.flip=true`
  together cause the IMX335 to stop producing frames on this firmware.
  mirror-only and flip-off both work; combined `flip=true` is the
  trigger. Pre-existing, unrelated to DIVP. Tracked as `IMX335_FLIP_WEDGE`.
- **Dual-VENC + channel backend.** Untested. Current dual-VENC code path
  reads VPE port1 directly, so it should be orthogonal to the channel
  backend on ch0, but no on-bench confirmation yet.
- **Maruko port of channel backend.** DIVP exists on Maruko (Infinity6C)
  too. Straightforward port; not in PR #119 scope.
- **Stab thread telemetry verbosity.** Still prints raw dx/dy and
  accumulator state every 120 frames. Consider lowering or gating
  behind a debug flag.
- **No unit test coverage for `star6e_stab_compute_crop_dims()`** —
  integer aspect math worth a self-contained test.
