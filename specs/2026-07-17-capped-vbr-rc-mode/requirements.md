# Capped VBR RC mode (bounded bitrate + deterministic per-frame size)

**Status: SPEC — design locked, not yet implemented.** Replaces the existing
`vbr` mode with a link-matched rate-control mode. Design decisions below are
settled (see "Locked decisions"); remaining work is SDK-struct recovery
(needs full SDK headers), wiring, and bench validation.

## Motivation

waybeam-link runs the encoder in **CBR** and enforces **per-frame maxI/maxP
size caps on the link side**. Two limits of that arrangement:

- CBR fills the channel to the target even on easy scenes (spends airtime /
  loss exposure on low-value bits).
- A link-side per-frame cap can only react *after* an oversized frame exists
  (drop/fragment) — it cannot shrink the frame, so a scene cut still emits a
  burst of packets before the cap bites.

We want **one** encoder mode that gives both properties the link needs:

1. **Bounded upper bitrate** — never exceed a ceiling the channel can carry.
2. **Deterministic per-frame size** — a hardware-guaranteed maxI / maxP so a
   single frame can never spew a correlated packet burst onto a lossy link.

This replaces `vbr` (little-used today) rather than adding a fourth mode —
we do not want to maintain two VBR variants.

## Locked decisions (from design review)

1. **Enforce the per-frame ceiling in the encoder**, via the SigmaStar
   SuperFrame RC mechanism — not on the link side. The encoder emits a frame
   that already fits; waybeam-link can retire its own per-frame cap logic.
2. **Overflow behaviour = re-encode at higher QP** (`REENCODE`), not drop.
   Every frame is kept (preserves the P-reference chain and SVC-T layering);
   an oversized frame just takes a one-frame quality dip.
3. **Replace `vbr`** — `rc_mode = "vbr"` becomes capped VBR. No separate mode
   string. (Old configs keep parsing; new fields default per "Migration".)
4. **Star6E first**, then Maruko (per `AGENTS.md` dual-backend policy). The
   SuperFrame struct must be modelled and validated per backend.

## What the mode is

Three bounds, one mode:

| Bound | Mechanism | Where |
|---|---|---|
| Upper bitrate | VBR `maxBitrate` | ChnAttr rate struct (**already wired**) |
| Quality window | QP floor/ceiling (`minQual`/`maxQual` or RcParam min/max Qp) | ChnAttr rate struct or RcParam (**confirm — SDK hunt #2**) |
| Per-frame size | SuperFrame `u32SuperIFrmBitsThr` / `u32SuperPFrmBitsThr` + `REENCODE` | RcParam (**new — not modelled in this tree**) |

## SDK inventory — verified from this repo (2026-07-17)

- The encoder-side per-frame size cap is **absent from this tree.** Greps for
  `SuperFrm`/`Iprop`/`ReEncode`/`FrmBitsThr` hit nothing in `src/` or
  `include/`. The maxI/maxP control the team built lives in **waybeam-link**.
- `MI_VENC_RcParam_t` is modelled as **opaque** here:
  `struct { ...; void *pRcParam; }` (`include/star6e.h:451`). `GetRcParam` /
  `SetRcParam` are bound both backends (`star6e_mi.c:286-289`,
  `maruko_mi.h:127-128`) but no concrete SuperFrame/RcParam layout exists.
- VBR is wired for **`maxBitrate` + `gop` only** (`venc_api.c:2895-2932`,
  via `GetChnAttr` → patch → `SetChnAttr`, the live-reconfigure pattern).
  The rate struct already carries `maxQual`/`minQual`
  (`i6_venc_rate_h26xvbr`, `sigmastar_types.h:569-577`) — present but
  **not exposed** as config.
- `rc_mode` accepts `"cbr"|"vbr"|"avbr"|"qvbr"`
  (`include/venc_config.h:103`); the apply switch handles CBR/VBR/AVBR only.

### SDK reference hunt (do when full SDK is mounted)

The one hard dependency is the exact **RcParam + SuperFrame** layout, which
must byte-match `libmi_venc` for both SoCs (version-sensitive, like the IVE
blob lesson).

1. **`MI_VENC_RcParam_t` real layout** — the concrete struct behind the
   opaque `pRcParam`, for i6e (Star6E) and i6c (Maruko). Capture common
   fields (min/max Qp, I-frame Qp bounds, `s32ChangePos`, reencode count)
   and the union of per-mode params.
2. **QP-window home** — is the quality window the VBR rate struct's
   `minQual`/`maxQual`, or RcParam `u32MinQp`/`u32MaxQp` (+ `u32MinIQp`/
   `u32MaxIQp`)? Confirm **polarity** (which bound is the QP floor vs
   ceiling — Qual vs Qp conventions are inverted on some SigmaStar gens).
3. **SuperFrame struct + enum** — `MI_VENC_SuperFrmParam_t` (or equivalent):
   `eSuperFrmMode` values (`NONE` / `REENCODE` / `DROP`), the I/P bit
   threshold field names/units (**bits**, confirm), and the
   `s32MaxReEncodeTimes` cap.
4. **Set path + ordering** — does SuperFrame/QP-window go through
   `MI_VENC_SetRcParam` (vs ChnAttr)? Must it be set between CreateChn and
   StartRecvPic, or is it live? (We already do live `SetChnAttr` for
   bitrate; confirm `SetRcParam` is equally live.)
5. **Demo/example** — grep SDK `ipc_demo`/`sample` for a SuperFrame +
   REENCODE example and a min/max-Qp VBR example, for both chips.

## Config surface (proposed)

`rc_mode = "vbr"` (capped VBR). New `video0` fields:

- `vbr_max_bitrate` — existing `video0.bitrate` semantics (ceiling). Live.
- `vbr_min_qp` / `vbr_max_qp` — QP window. Live if RcParam set is live.
- `frame_max_i_bits` / `frame_max_p_bits` — SuperFrame thresholds. Live.
- `frame_max_reencodes` — `s32MaxReEncodeTimes` (default small, e.g. 2).

(Exact names to match existing `venc_config` conventions; add
`test_venc_config.c` cases.)

## Migration (replacing `vbr`)

- Old `rc_mode="vbr"` configs must keep loading. New fields default so the
  mode reproduces sane behaviour when unset: `frame_max_*_bits = 0` ⇒
  SuperFrame off (pure bounded VBR), QP window = full range ⇒ today's VBR.
  The capped behaviour is opt-in by setting the thresholds.
- Document in `HISTORY.md` under the version that ships it: `vbr` now means
  capped VBR; the old plain-VBR behaviour is the all-defaults case.

## Open questions / risks

1. **Opaque struct layout** (SDK hunt #1/#3) — the sole hard blocker; a
   mismatched RcParam struct silently corrupts RC or returns errors. Validate
   with a known-value round-trip (`Set` then `Get`) on device.
2. **Re-encode cost at high fps** — `REENCODE` re-runs the encoder for an
   overflow frame within the same frame budget. At imx335 **120fps** (~8.3ms
   budget) a re-encode could blow the deadline / drop fps. Measure; cap
   `frame_max_reencodes` low and set thresholds so overflow is rare.
3. **IDR interaction** — an IDR is an I-frame, so it is subject to
   `maxIbits` REENCODE. Confirm scene-detect IDR and resilience-preset IDRs
   compose with the cap (a forced IDR that can't fit the I-threshold gets
   QP-bumped, not dropped). Reconcile with `idr_rate_limit.h`.
4. **Resilience-preset / intra-refresh ownership** — presets currently drive
   intra-refresh + SVC-T + GOP under a CBR assumption. Decide whether capped
   VBR composes with the presets or is selected independently of them.
5. **maxP-cap ↔ VBR-loop interaction** — a threshold set well below what the
   VBR loop wants can produce a cap-clipping limit cycle (quality breathing).
   Set thresholds ≥ the VBR loop's natural per-frame size at `maxBitrate`;
   widen `statTime` to soften.

## Verified fact sources (this repo)

- `include/sigmastar_types.h:527-616` (VBR/QP/CBR rate structs, `maxQual`/
  `minQual`), `514-525` / `895-908` (rate-mode enums, i6e + i6c)
- `include/star6e.h:451-452` (opaque `MI_VENC_RcParam_t`)
- `src/venc_api.c:2891-2932` (VBR wired for maxBitrate+gop; live SetChnAttr)
- `src/venc_config.c:110,543` + `include/venc_config.h:103` (`rc_mode`)
- `src/star6e_mi.c:286-289` (`GetRcParam`/`SetRcParam` bound)
