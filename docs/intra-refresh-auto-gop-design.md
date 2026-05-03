# IntraRefresh Auto-GOP Design

## Context

PR #92 (`feature/intra-refresh`) added GDR-style rolling intra-refresh for the
Star6E and Maruko backends with three config fields:

```json
"video0": {
  "intraRefresh": true,
  "intraRefreshLines": 0,
  "intraRefreshQp": 0
}
```

`intraRefreshLines: 0` already triggers an auto-line formula targeting a ~500ms
self-heal window. GOP is explicitly **not** overridden — periodic IDRs stack on
top of whatever `gop_size` the user has configured.

This document proposes extending auto-mode so that `intraRefresh: true` +
`intraRefreshLines: 0` also selects optimal GOP and QP automatically, giving
a zero-knob good-experience baseline.

---

## Suggested manual values for 1080p H.265 @ 60fps, GOP=1s

As a concrete reference point (derived from Majestic-style tuning):

```json
"video0.intraRefreshLines": 4,
"video0.intraRefreshQp":    48,
"video0.gop_size":          0.15
```

- `lines=4` → full frame refreshed in ceil(34/4) = **9 P-frames (~150ms)**
- `gop_size=0.15s` → IDR every 9 frames, aligned with one complete GDR pass
- `req_iqp=48` → coarse stripe QP, keeps rolling-stripe bitrate cost neutral

This is the target the auto formula should reproduce when `intraRefreshLines: 0`.

---

## Proposed auto formula

All computed when `intraRefresh: true` and `intraRefreshLines: 0`:

```
lcu_h          = (H265) ? 32 : 16
total_rows     = ceil(height / lcu_h)
refresh_frames = ceil(fps / 2)                          // ~500ms window
lines          = max(1, ceil(total_rows / refresh_frames))

auto_gop_frames = ceil(total_rows / lines)              // one IDR per GDR pass
auto_gop_sec    = auto_gop_frames / fps

auto_req_iqp    = (H265) ? 48 : 45
```

The GOP formula is the natural inverse of the lines formula: the IDR fires
exactly when the rolling stripe completes one full frame pass. No IDR fires
mid-cycle, and no cycle runs without a hard recovery anchor at its end.

### Worked examples

| Resolution | Codec | FPS | lines (auto) | auto_gop_frames | auto_gop_sec | auto_req_iqp |
|---|---|---|---|---|---|---|
| 1080p | H.265 | 60 | 2 | 17 | 0.28s | 48 |
| 1080p | H.265 | 30 | 2 | 17 | 0.57s | 48 |
| 720p  | H.265 | 60 | 2 | 12 | 0.20s | 48 |
| 720p  | H.265 | 15 | 2 | 12 | 0.80s | 48 |
| 720p  | H.264 | 30 | 2 | 8  | 0.27s | 45 |

### Explicit lines → auto GOP

When `intraRefreshLines > 0` the user controls stripe density; auto-GOP still
derives from the effective line count:

```
auto_gop_frames = ceil(total_rows / explicit_lines)
```

For the reference 1080p60 case with `intraRefreshLines=4`:
- `auto_gop_frames = ceil(34/4) = 9` → `gop_sec = 0.15s` ✓

---

## Override hierarchy

| Field | Value | Behavior |
|---|---|---|
| `intraRefreshLines` | 0 | auto-compute lines; auto-compute GOP and QP |
| `intraRefreshLines` | > 0 | use explicit lines; still auto-compute GOP from those lines |
| `intraRefreshQp` | 0 | auto: H.265→48, H.264→45 |
| `intraRefreshQp` | > 0 | use explicit QP, skip auto |
| `gop_size` | 0 (unset) | use auto-GOP when intraRefresh is active |
| `gop_size` | > 0 | honor explicit GOP, skip auto-GOP entirely |

Explicit `gop_size` always wins, preserving the current belt-and-suspenders
pattern (long GOP + rolling stripe) for users who want it.

---

## Implementation touch points

### 1. `src/star6e_pipeline.c`

Add `star6e_pipeline_intra_refresh_gop()` alongside the existing
`star6e_pipeline_intra_refresh_lines()`:

```c
static double star6e_pipeline_intra_refresh_gop(
    uint32_t height, uint32_t fps, PAYLOAD_TYPE_E codec,
    uint32_t effective_lines)
{
    uint32_t lcu_h = (codec == PT_H265) ? 32u : 16u;
    uint32_t total_rows = (height + lcu_h - 1) / lcu_h;
    uint32_t gop_frames = (total_rows + effective_lines - 1) / effective_lines;
    if (gop_frames < 1) gop_frames = 1;
    return (double)gop_frames / fps;
}
```

In `star6e_pipeline_start()`, where `venc_gop_size` is computed (~line 969):

```c
/* Auto-GOP: override only when intraRefresh is active and gop_size not set */
if (vcfg->video0.intra_refresh && pconf.venc_gop_size == default_gop) {
    uint32_t eff_lines = star6e_pipeline_intra_refresh_lines(
        height, fps, codec, vcfg->video0.intra_refresh_lines);
    pconf.venc_gop_size = star6e_pipeline_intra_refresh_gop(
        height, fps, codec, eff_lines);
}
```

### 2. `star6e_pipeline_apply_intra_refresh()`

Default `cfg.u32ReqIQp` when `intra_refresh_qp == 0`:

```c
cfg.u32ReqIQp = vcfg->video0.intra_refresh_qp > 0
    ? vcfg->video0.intra_refresh_qp
    : (codec == PT_H265 ? 48u : 45u);
```

> **Verify**: confirm SDK treats `u32ReqIQp = 0` as "SDK default" and not as
> QP=0 (maximum quality / bitrate spike). If it means QP=0, always applying the
> codec-appropriate default is mandatory, not optional.

### 3. Maruko parity (`src/maruko_pipeline.c`)

Same `auto_gop` injection and `auto_req_iqp` default in
`maruko_apply_intra_refresh()` — mirrors the Star6E pattern exactly.

### 4. `/api/v1/intra/status` response

Add `auto_gop_sec` and `auto_req_iqp` fields so the caller can see what was
computed, not just what was configured.

### 5. Config documentation (`config/venc.default.json` + README)

Add a comment block explaining that `intraRefreshLines: 0` opts into full
auto-mode including GOP and QP override.

---

## Open questions

1. **SDK `u32ReqIQp = 0` semantics** — SDK default or QP=0? Must bench-verify
   before shipping the auto-QP default.
2. **Mutability** — current `MUT_RESTART` for intraRefresh fields. Auto-GOP
   changes `gop_size` which is also `MUT_RESTART`, so no additional constraint,
   but worth noting that live-apply of auto-GOP is not a goal here.
3. **ch1 (dual-stream recorder)** — ch1 already skips intraRefresh; auto-GOP
   should also leave ch1 `gop_size` alone (TS containers need IDR for seeking).
4. **`intraRefreshLines=0` + explicit `gop_size`** — user sets a large GOP for
   belt-and-suspenders; auto-lines fires but auto-GOP is suppressed. Ensure the
   log makes this clear (`"intra auto-GOP suppressed: explicit gop_size=2.0s"`).
