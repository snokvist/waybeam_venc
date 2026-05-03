# IntraRefresh Mode Design

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
self-heal window. GOP and QP are explicitly **not** overridden — periodic IDRs
stack on top of whatever `gopSize` the user has configured, and `req_iqp`
passes through to the SDK as-is.

This document proposes replacing the boolean `intraRefresh` with a single
`intraRefreshMode` enum that drives the full set of derived parameters
(lines, GOP, QP) from one human-readable choice. Explicit per-field overrides
remain available for power users.

---

## Proposed config surface

```json
"video0": {
  "intraRefreshMode": "balanced",   // off | fast | balanced | robust
  "intraRefreshLines": 0,           // 0 = use mode default
  "intraRefreshQp": 0,              // 0 = use mode default
  "gopSize": 0.0                    // 0 = use mode default (when mode != off)
}
```

One human-readable knob picks intent; existing fields stay as targeted
overrides. Field names match the existing camelCase JSON convention
(`gopSize`, `intraRefreshLines`, `intraRefreshQp`) — no new naming variants
are introduced.

### Mode → behavior

| Mode | target_ms | Use case | Typical 1080p H.265 @ 60fps |
|---|---|---|---|
| `off` | — | feature disabled | (no override) |
| `fast` | 150 | FPV racing, low-latency, clean link | `lines=8, gop≈0.07s, qp=48` |
| `balanced` | 500 | general FPV (default) | `lines=2, gop≈0.28s, qp=48` |
| `robust` | 1000 | lossy long-range, high packet loss | `lines=1, gop≈0.57s, qp=48` |

`target_ms` is the design-time self-heal window — the time from a corrupted
frame to a fully-refreshed picture under steady-state. It is not a runtime
field; it is the constant that drives each mode's auto formula.

---

## Backward compatibility

`intraRefresh: true|false` from PR #92 continues to parse and is mapped
silently into `intraRefreshMode`:

| Legacy field | Mapped mode |
|---|---|
| `intraRefresh: false` | `intraRefreshMode: "off"` |
| `intraRefresh: true` | `intraRefreshMode: "balanced"` |
| Both fields present | `intraRefreshMode` wins; warn-log on mismatch |

The boolean form remains parsed indefinitely (no removal date). The default
config (`config/venc.default.json`) migrates to `intraRefreshMode: "off"` on
the next release; existing deployed configs keep working unchanged.

---

## Auto formulas (per mode)

All computed when `intraRefreshMode != "off"` and the corresponding override
field is `0`:

```
target_ms      = mode_target_ms(mode)            // 150 / 500 / 1000
lcu_h          = (codec == H265) ? 32 : 16
total_rows     = ceil(height / lcu_h)
refresh_frames = max(1, round(fps * target_ms / 1000))
auto_lines     = clamp(1, ceil(total_rows / refresh_frames), total_rows)

auto_gop_frames = ceil(total_rows / effective_lines)   // one IDR per GDR pass
auto_gop_sec    = auto_gop_frames / fps

auto_req_iqp    = (codec == H265) ? 48 : 45
```

The GOP formula is the natural inverse of the lines formula: the IDR fires
exactly when the rolling stripe completes one full frame pass. No IDR fires
mid-cycle, and no cycle runs without a hard recovery anchor at its end.

`effective_lines` is the explicit `intraRefreshLines` if non-zero, otherwise
`auto_lines`. Auto-GOP always derives from the *effective* line count, so an
explicit lines override still produces a coherent GOP without the user having
to compute it.

### Worked examples (all modes, H.265)

| Resolution | FPS | Mode | lines | gop_frames | gop_sec | req_iqp |
|---|---|---|---|---|---|---|
| 1080p | 60 | fast | 4 | 9 | 0.15s | 48 |
| 1080p | 60 | balanced | 2 | 17 | 0.28s | 48 |
| 1080p | 60 | robust | 1 | 34 | 0.57s | 48 |
| 1080p | 30 | balanced | 3 | 12 | 0.40s | 48 |
| 720p  | 60 | fast | 3 | 8 | 0.13s | 48 |
| 720p  | 60 | balanced | 1 | 23 | 0.38s | 48 |

(`total_rows = 34` for 1080p H.265, `23` for 720p H.265.)

---

## Override hierarchy

Mode picks defaults; any non-zero per-field override replaces that one
default. Order is fixed and easy to memorize: **mode → field overrides →
clamp/warn**.

| Field | Value | Behavior |
|---|---|---|
| `intraRefreshMode` | `"off"` | feature disabled; all other fields ignored |
| `intraRefreshMode` | `"fast"`/`"balanced"`/`"robust"` | drives auto defaults |
| `intraRefreshLines` | `0` | use mode auto-lines |
| `intraRefreshLines` | `> 0` | use explicit; auto-GOP recomputes from this |
| `intraRefreshQp` | `0` | use mode auto-QP (codec-dependent) |
| `intraRefreshQp` | `> 0` | use explicit QP |
| `gopSize` | `0.0` | use mode auto-GOP |
| `gopSize` | `> 0` | honor explicit GOP, suppress auto-GOP entirely |

Explicit `gopSize` always wins, preserving the belt-and-suspenders pattern
(long GOP + rolling stripe) for users who want it.

### Clamp and warn rules

- `intraRefreshLines > total_rows` → clamp to `total_rows`, warn-log:
  `"intra: lines=%u exceeds total_rows=%u, clamped"`.
- `gopSize > 0` while mode is active → log once at startup:
  `"intra auto-GOP suppressed: explicit gopSize=%.2fs"`.
- `intraRefreshMode != "off"` and codec is unsupported by SDK → warn and
  treat as `off` (already the PR #92 behavior for the boolean form).

---

## Pre-implementation prerequisites

These must land **before** the auto-mode implementation, not as part of it.
Both are latent issues in the shipped PR #92 code today.

### 1. Verify SDK `u32ReqIQp = 0` semantics

`MI_VENC_IntraRefresh_t.u32ReqIQp = 0` may mean "SDK default" or "QP=0
(maximum quality / bitrate spike)." PR #92 ships `intraRefreshQp: 0` as the
default already, so the wrong interpretation here is a latent bandwidth bomb
for any user enabling intraRefresh today.

Bench plan (192.168.1.13, 1080p60 H.265):
- Capture 10s with `intraRefreshLines: 4, intraRefreshQp: 0`, log avg bitrate
  and stripe size.
- Repeat with `intraRefreshQp: 48`. Compare.
- If `0` produces a spike, the auto-QP default is mandatory and the boolean
  `intraRefresh: true` form must inject `48` (H.265) / `45` (H.264)
  immediately as a hotfix to PR #92.

### 2. Extract shared helper to prevent backend drift

PR #83 → hotfix #90 (`g_fields[]` registration miss) confirms that
"mirror by discipline" between Star6E and Maruko backends fails. Before
adding more parallel logic, extract:

```c
// src/intra_refresh.h
typedef enum {
  INTRA_MODE_OFF = 0,
  INTRA_MODE_FAST,
  INTRA_MODE_BALANCED,
  INTRA_MODE_ROBUST,
} IntraRefreshMode;

typedef struct {
  uint32_t lines;          // effective (post-override, post-clamp)
  uint32_t gop_frames;     // 0 if user supplied explicit gopSize
  double   gop_sec;        // 0.0 if user supplied explicit gopSize
  uint32_t req_iqp;
  uint32_t total_rows;     // for status reporting
  uint32_t target_ms;      // mode constant, for status reporting
} IntraRefreshDerived;

IntraRefreshMode intra_refresh_parse_mode(const char *s);
const char      *intra_refresh_mode_name(IntraRefreshMode m);

void intra_refresh_compute(
    IntraRefreshMode mode,
    uint32_t width, uint32_t height, uint32_t fps, PAYLOAD_TYPE_E codec,
    uint32_t override_lines, uint32_t override_qp, double override_gop_sec,
    IntraRefreshDerived *out);
```

Both `star6e_pipeline.c` and `maruko_pipeline.c` consume this; the
backend-specific code shrinks to "call helper, plug result into
`MI_VENC_IntraRefresh_t`, plug `gop_sec` into pipeline config." One
implementation, one set of unit tests.

---

## Implementation touch points

### 1. Schema registration (`src/api_fields.c` — `g_fields[]` + aliases)

Per the schema-field 7-touch-point checklist (lessons from PR #83 → #90):
add `video0.intra_refresh_mode` to `g_fields[]` and any alias tables.
**This step has been the silent failure mode twice — it is non-optional.**

Existing fields (`intra_refresh`, `intra_refresh_lines`, `intra_refresh_qp`)
remain registered. The boolean `intra_refresh` becomes a legacy compat alias
that maps to `intra_refresh_mode` on read/write.

### 2. Config parse (`src/config.c`)

- Parse `intraRefreshMode` string → `IntraRefreshMode` enum.
- If `intraRefresh` boolean is also present, apply legacy mapping and
  warn-log if both fields disagree.
- Default if neither present: `INTRA_MODE_OFF`.

### 3. Star6E backend (`src/star6e_pipeline.c`)

In `star6e_pipeline_start()`, before computing `pconf.venc_gop_size`:

```c
IntraRefreshDerived ir;
intra_refresh_compute(vcfg->video0.intra_refresh_mode,
                      width, height, fps, codec,
                      vcfg->video0.intra_refresh_lines,
                      vcfg->video0.intra_refresh_qp,
                      vcfg->video0.gop_size,   // 0.0 = "no override"
                      &ir);

if (ir.gop_sec > 0.0) pconf.venc_gop_size = ir.gop_sec;
```

`star6e_pipeline_apply_intra_refresh()` consumes `ir.lines` and `ir.req_iqp`
directly (replaces the current ad-hoc `star6e_pipeline_intra_refresh_lines()`
helper).

### 4. Maruko backend (`src/maruko_pipeline.c`)

Identical pattern in `maruko_apply_intra_refresh()` and the GOP injection in
`maruko_pipeline_start()`. Both backends call the same helper — no
hand-mirrored math.

### 5. `/api/v1/intra/status` response

Return both requested and effective values so callers can debug
"why is my GOP wrong":

```json
{
  "mode": "balanced",
  "target_ms": 500,
  "total_rows": 34,
  "lines":   { "requested": 0,    "effective": 2 },
  "gop":     { "requested": 0.0,  "effective_sec": 0.28, "auto": true },
  "qp":      { "requested": 0,    "effective": 48 },
  "mi_supported": true,
  "active": true
}
```

`auto: false` on `gop` indicates the user supplied an explicit `gopSize`
that suppressed auto-GOP.

### 6. One-shot enable endpoint

```
POST /api/v1/intra/mode
Content-Type: application/json
{"mode": "balanced"}
```

Sets `intra_refresh_mode`, clears any non-zero override fields (so the
mode's defaults take effect), persists, and triggers the existing restart
path. Single HTTP call, no JSON-edit-and-SIGHUP dance.

### 7. Config documentation (`config/venc.default.json` + README)

Replace the three-zero-sentinel comment with a single block explaining the
mode enum, the override fields, and the legacy boolean mapping.

---

## Open questions

1. **`auto: true/false` field naming** in `/api/v1/intra/status` — is a
   per-field auto flag clearer than a top-level `overrides: ["gopSize"]`
   list? Decision before implementation.
2. **Mutability** — current `MUT_RESTART` for intraRefresh fields.
   `intraRefreshMode` should match. Live-apply is not a goal.
3. **ch1 (dual-stream recorder)** — ch1 already skips intraRefresh; mode
   handling must also leave ch1 `gopSize` alone (TS containers need IDR
   for seeking).
4. **Future modes** — is there demand for `"adaptive"` (mode chosen at
   runtime from telemetry like packet loss / RSSI)? Out of scope here, but
   the enum + helper structure supports adding it later without breaking
   the API.
