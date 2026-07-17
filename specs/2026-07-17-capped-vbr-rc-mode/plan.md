# Plan: capped VBR RC mode

Companion to `requirements.md`. Order is gated on recovering the SuperFrame /
RcParam struct layout from the full SDK (Phase 0) — everything downstream
depends on it.

## Design decisions (locked — do not re-litigate)

1. Encoder-enforced per-frame ceiling via SigmaStar SuperFrame.
2. Overflow = `REENCODE` at higher QP (keep every frame).
3. Replace `vbr`; capped behaviour is opt-in via the threshold fields
   (all-defaults reproduces plain VBR).
4. Star6E first, then Maruko. Struct modelled + round-trip-validated per SoC.

## Phase 0 — struct recovery + round-trip validation (blocked on full SDK)

1. From the SDK headers, model the real `MI_VENC_RcParam_t` and
   `MI_VENC_SuperFrmParam_t` (+ QP-window fields) for i6e and i6c. Add them
   to the tree replacing the opaque `void *pRcParam` (or alongside, guarded
   per backend).
2. **Round-trip test on device**: `GetRcParam` → flip known fields →
   `SetRcParam` → `GetRcParam`, assert the values survive. This catches a
   struct-layout mismatch before it corrupts RC silently. Gate: values
   round-trip on both `192.168.1.13` (Star6E) and `192.168.2.12` (Maruko).

Struct layout is the one hard blocker — do not proceed until Phase 0 passes.

## Phase 1 — wire capped VBR (Star6E)

1. Extend the VBR create/apply path (`venc_api.c` around the `SetChnAttr`
   switch) to also program, via `SetRcParam`:
   - QP window (`vbr_min_qp`/`vbr_max_qp`)
   - SuperFrame (`frame_max_i_bits`/`frame_max_p_bits`, mode=`REENCODE`,
     `frame_max_reencodes`)
   Apply after CreateChn, before StartRecvPic (adjust if Phase 0 shows
   SetRcParam is live — then also wire the live-update path).
2. Config: add the `video0` fields (requirements §"Config surface") with
   defaults that reproduce plain VBR when unset. `make lint` after the
   struct change and after the config change (incremental, per AGENTS.md).
3. Add `test_venc_config.c` cases (parse, defaults, round-trip serialize).

## Phase 2 — bench-validate determinism (Star6E `192.168.1.13`)

Prefer `scripts/star6e_direct_deploy.sh cycle`.

- **Ceiling holds:** stream a scene with hard cuts (or `--eis-test`-style
  motion); capture per-frame sizes via the RTP sidecar
  (`frame_size_bytes`). Gate: **no frame exceeds** its maxI/maxP threshold,
  across cuts and at GOP boundaries (IDR ≤ maxIbits).
- **No fps loss from re-encode:** run at 1080p60 and 1080p120; confirm fps
  holds and latency stays within noise of plain VBR. If 120fps drops,
  lower `frame_max_reencodes` / raise thresholds and document the envelope.
- **No limit cycle:** confirm frame sizes aren't pinned at the cap every
  frame (that means the threshold is below the VBR loop's natural size —
  raise it). Watch for quality breathing.
- **IDR composes:** force an IDR (scene-detect + `/api/v1/...`); confirm it
  is QP-bumped to fit maxIbits, not dropped, and the stream recovers.

## Phase 3 — Maruko parity

Repeat Phase 1-2 wiring/validation on Maruko (i6c struct variant). Watch the
known Maruko output disable/re-enable stall (`KNOWN_ISSUES.md`) if toggling
output during tests.

## Phase 4 — waybeam-link retires its per-frame cap

Once the encoder guarantees the ceiling, remove the link-side maxI/maxP
enforcement (separate repo). Keep link's bitrate/QP setpoint control. Do this
only after Phase 2/3 prove the encoder ceiling holds — do not run both caps
in production (double-clamp masks encoder-side bugs).

## Verify

`make verify` (both backends) before declaring done; targeted deploy tests
per `AGENTS.md` "Deployment Targets". `HISTORY.md` + `VERSION` bump land with
Phase 1 (first shipping behaviour change).

## SDK findings (fill in when the full SDK is mounted)

- [ ] `MI_VENC_RcParam_t` real layout (i6e): …
- [ ] `MI_VENC_RcParam_t` real layout (i6c): …
- [ ] QP-window fields + polarity (Qual vs Qp): …
- [ ] `MI_VENC_SuperFrmParam_t` fields + mode enum (REENCODE value): …
- [ ] SuperFrame threshold units (bits?) + `s32MaxReEncodeTimes`: …
- [ ] SetRcParam live vs create-time; ordering: …
- [ ] SDK demo reference (path): …
