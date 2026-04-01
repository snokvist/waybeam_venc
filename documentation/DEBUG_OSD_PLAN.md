# Debug OSD — Design Plan

## Goal

Add a general-purpose, debug-only on-screen display (OSD) overlay module to
waybeam_venc. The OSD must be:

- **Debug-only** — disabled by default, zero runtime cost when off
- **General-purpose** — not tied to any single consumer (optical flow, EIS, etc.)
- **Extensible** — new element types and new consumers can be added without
  changing the core interface
- **Backend-portable** — vtable dispatch allows different rendering strategies
  per SoC (RGN on Star6E, stub or framebuffer on Maruko)

## Background

PR #23 (OpenIPC/waybeam_venc, closed) added an OSD in `star6e_osd_simple.c`
tightly coupled to optical flow visualization. Review identified:

- OSD was optical-flow-specific (dot + track points only)
- Ran as a parallel RGN subsystem with no shared interface
- ~800 lines duplicated between LK and SAD optical flow backends
- `libmi_rgn.so` linked statically but not vendored in `libs/star6e/`
- Linker hack (`--unresolved-symbols=ignore-in-shared-libs`) to suppress errors

This plan incorporates the OSD concept from PR #23 as a standalone, reusable
debug module following established project patterns.

## Architecture

### Vtable Dispatch (same pattern as EIS)

```
include/debug_osd.h        — public interface, element types, DebugOsdOps vtable
src/debug_osd.c             — factory, lifecycle, inline dispatch
src/debug_osd_rgn.c         — Star6E RGN backend (MI_RGN via dlopen)
```

The EIS module (`eis.h` / `EisOps`) established the project's pattern for
pluggable subsystems. The debug OSD follows the same structure exactly.

### Element Types

The OSD renders a flat list of elements per frame. Each element is a tagged
union:

| Type | Description | Use cases |
|------|-------------|-----------|
| `DEBUG_OSD_ELEM_TEXT` | Key=value text line at a row position | Frame metrics, motion estimates, EIS status, IMU data, bitrate, fps |
| `DEBUG_OSD_ELEM_MARKER` | Small dot/crosshair at (x,y) | Feature points, motion centroid, ROI center |
| `DEBUG_OSD_ELEM_VECTOR` | Line/arrow from (x0,y0) to (x1,y1) | Motion vectors, flow direction, gyro displacement |
| `DEBUG_OSD_ELEM_RECT` | Axis-aligned rectangle outline or fill | ROI bounds, SAD blocks, detection regions, crop window |

Future element types (histogram, grid, heatmap) are added by extending the
enum and adding a draw case in each backend — no interface changes needed.

### Text Rendering

Text uses a built-in **8x8 bitmap font** (embedded lookup table, ~2 KB).
Fixed-width, ASCII-only — sufficient for debug labels and numeric values.

Text elements carry:
- `row` — vertical position (0 = top, increments downward)
- `label` — short string key (e.g., "tx", "fps", "crop_x")
- `value` — formatted string (caller formats via snprintf before submission)
- `color` — palette index

This supports both current needs (optical flow tx/ty/tz) and future needs
(any module can push key=value pairs without OSD changes).

### Consumer Pattern

Consumers do NOT own or manage the OSD. They receive a `DebugOsdState *`
pointer (may be NULL if OSD is disabled) and push elements per-frame:

```c
/* Example: optical flow consumer */
void optflow_update_osd(DebugOsdState *osd, const OptFlowResult *r)
{
    if (!osd) return;
    DebugOsdElem elems[16];
    int n = 0;

    elems[n++] = debug_osd_text(0, "tx", "%.1f", r->tx);
    elems[n++] = debug_osd_text(1, "ty", "%.1f", r->ty);
    elems[n++] = debug_osd_marker(r->cx, r->cy, DEBUG_OSD_COLOR_RED);
    for (int i = 0; i < r->num_points; i++)
        elems[n++] = debug_osd_marker(r->pts[i].x, r->pts[i].y,
                                      DEBUG_OSD_COLOR_GREEN);

    debug_osd_begin_frame(osd);
    debug_osd_draw(osd, elems, n);
    debug_osd_end_frame(osd);
}

/* Example: EIS status consumer */
void eis_update_osd(DebugOsdState *osd, const EisStatus *s)
{
    if (!osd) return;
    DebugOsdElem elems[8];
    int n = 0;

    elems[n++] = debug_osd_text(0, "eis_dx", "%.1f", s->offset_x);
    elems[n++] = debug_osd_text(1, "eis_dy", "%.1f", s->offset_y);
    elems[n++] = debug_osd_rect(s->crop_x, s->crop_y,
                                s->crop_w, s->crop_h,
                                DEBUG_OSD_COLOR_WHITE);

    debug_osd_begin_frame(osd);
    debug_osd_draw(osd, elems, n);
    debug_osd_end_frame(osd);
}
```

### RGN Backend Strategy (Star6E)

**Key insight:** The SigmaStar MI_RGN API supports multiple independent
overlay regions per VPE channel. Each region has its own handle, position,
size, and type. This is far more efficient than a single full-screen I4
bitmap that must be re-uploaded every frame.

**Region allocation strategy:**

| Element type | RGN strategy | Cost |
|-------------|-------------|------|
| Marker | Small OSD region (e.g. 8x8 I4 bitmap). Move via `SetDisplayAttr` point change — no pixel re-upload when only position changes. | Tiny bitmap upload only on color change |
| Rect | Cover region (`I6_RGN_TYPE_COVER`). Hardware-rendered solid rectangle. Position and size set via channel config. | Zero CPU — pure hardware overlay |
| Text | OSD region sized to text length (8×height per char). Bitmap rendered from font table on content change only. | Small bitmap upload only on text change |
| Vector | OSD region sized to bounding box. Line rasterized into I4 bitmap. | Small bitmap upload per change |

**Region handle budget:** Star6E supports up to ~16 simultaneous RGN handles
(hardware limit varies by SoC). The backend tracks a pool of handles and
reuses them across frames. If the pool is exhausted, lowest-priority elements
are silently dropped.

**`dlopen` for graceful degradation:**

```c
static int rgn_load_api(RgnBackend *ctx)
{
    ctx->lib = dlopen("libmi_rgn.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!ctx->lib) return -1;
    /* resolve: MI_RGN_Init, MI_RGN_Create, MI_RGN_AttachToChn,
       MI_RGN_SetDisplayAttr, MI_RGN_SetBitMap, MI_RGN_Destroy,
       MI_RGN_DetachFromChn, MI_RGN_DeInit */
    return 0;
}
```

If `libmi_rgn.so` is not present on the target, `debug_osd_create()` returns
NULL and every consumer's `if (!osd) return;` guard makes the entire OSD
path a no-op. No linker hacks needed.

**I4 pixel format & palette:**

4-bit indexed color (16 entries). Sufficient for debug overlay:

| Index | Color | Use |
|-------|-------|-----|
| 0 | Transparent | Background |
| 1 | Red (255,0,0) | Primary marker, errors |
| 2 | Green (0,255,0) | Tracked points, success |
| 3 | Blue (0,0,255) | Secondary data |
| 4 | Yellow (255,255,0) | Warnings, highlight |
| 5 | Cyan (0,255,255) | Tertiary data |
| 6 | Magenta (255,0,255) | Reserved |
| 7 | White (255,255,255) | Text, outlines |
| 8-14 | Reserved | Future use |
| 15 | Transparent alt | Alias for clear |

### Why NOT a Full-Screen CPU Buffer

A full-screen I4 bitmap for 1920x1080 = ~1 MB. Uploading this via
`MI_RGN_SetBitMap` every frame at 30+ fps = 30+ MB/s of memory bandwidth
on an embedded ARM core, plus the CPU cost of clearing and redrawing.

The multi-region approach:
- Markers: 8x8 = 32 bytes each
- Text line: 8×(8×20 chars) = 1280 bytes
- Total per frame: typically < 4 KB vs ~1 MB
- Position changes (marker moves): zero bitmap upload

The CPU-side buffer approach would be justified only for a backend that
lacks hardware region support (e.g., a raw framebuffer or a software
compositor). In that case, a future `debug_osd_fb.c` backend could use
the buffer strategy while the RGN backend stays efficient.

## Config Integration (4-Layer Sync)

### Layer 1: C struct (`include/venc_config.h`)

```c
typedef struct {
    bool show_osd;         /* master enable for debug OSD */
} VencConfigDebug;
```

Added as `VencConfigDebug debug;` field in `VencConfig`.

### Layer 2: Config parser (`src/venc_config.c`)

- Default: `show_osd = false`
- JSON key: `"debug": { "showOsd": false }`
- Load in `load_debug()`, serialize in `venc_config_to_json()`

### Layer 3: API field table (`src/venc_api.c`)

```c
FIELD("debug.show_osd", FIELD_BOOL, VencConfig, debug.show_osd),
```

Alias: `"showOsd"` → `"debug.show_osd"`

### Layer 4: Default config (`config/venc.default.json`)

```json
"debug": {
    "showOsd": false
}
```

WebUI: Add to SECTIONS with camelCase key `showOsd`.

## File Manifest

| File | Purpose | Lines (est.) |
|------|---------|-------------|
| `include/debug_osd.h` | Public interface, element types, vtable, colors, inline dispatch wrappers | ~120 |
| `src/debug_osd.c` | Factory (`debug_osd_create`), 8x8 font table | ~100 |
| `src/debug_osd_rgn.c` | Star6E RGN backend: dlopen, region pool, draw primitives | ~350 |
| Config updates | `venc_config.{h,c}`, `venc_api.c`, `venc_webui.c`, `venc.default.json` | ~30 total |
| Makefile | Add sources to `STAR6E_ONLY_SRC`, add `include/debug_osd.h` to deps | ~5 |

## Integration Points

### Pipeline init (`src/star6e_pipeline.c`)

After VPE is started, before VENC bind:

```c
if (cfg->debug.show_osd) {
    ps->debug_osd = debug_osd_create("rgn", capture_w, capture_h,
                                      &ps->vpe_port);
}
```

### Pipeline teardown

```c
debug_osd_destroy(ps->debug_osd);
ps->debug_osd = NULL;
```

### Consumer wiring

Each subsystem that wants OSD output receives the `DebugOsdState *` pointer
from the pipeline state. If NULL, all OSD calls are no-ops (inline guard).

## Future Extensibility

### New element types

Adding e.g. `DEBUG_OSD_ELEM_CIRCLE` or `DEBUG_OSD_ELEM_GRID`:
1. Add enum value in `debug_osd.h`
2. Add union member for the element's data
3. Add draw case in each backend's draw function
4. No interface or consumer changes needed

### New backends

Adding e.g. a framebuffer backend for Maruko:
1. Create `src/debug_osd_fb.c` implementing `DebugOsdOps`
2. Register in `debug_osd_create()` factory
3. No consumer changes needed

### New consumers

Any module can push elements without OSD module changes:
- Stream metrics (bitrate, frame drops, latency)
- ISP diagnostics (exposure, gain, AWB state)
- Recording status (file size, duration, dropped frames)
- Sensor debug (temperature, register reads)

### Linking to venc properties

Text elements accept arbitrary key=value strings. A future enhancement could
auto-populate from the existing `g_fields[]` API table — query any config
field by name and display its current value on the OSD. This requires no OSD
module changes, just a consumer function that reads from the config and pushes
text elements.

## Decisions & Rationale

| Decision | Rationale |
|----------|-----------|
| Vtable dispatch (not #ifdef) | Matches EIS pattern; runtime backend selection; testable on host |
| `dlopen` for MI_RGN (not static link) | Library not vendored; graceful degradation; no linker hacks |
| Multi-region RGN (not full-screen bitmap) | 100-1000x less bandwidth; hardware Cover for rectangles |
| I4 pixel format | Minimal memory (4-bit); sufficient colors for debug; matches SigmaStar examples |
| 8x8 bitmap font (not freetype/cairo) | Zero dependencies; ~2 KB table; adequate for debug text |
| Flat element array (not retained scene graph) | Simple; no lifetime management; consumers rebuild each frame |
| `debug.showOsd` config (not per-module flags) | Single kill switch; consumers already have their own enable flags |
| Star6E first, Maruko stub later | Per project policy (AGENTS.md: implement on Star6E first) |

## What NOT to Do

- Do NOT add `--unresolved-symbols=ignore-in-shared-libs` to linker flags
- Do NOT statically link `-lmi_rgn` — use `dlopen`
- Do NOT embed optical-flow-specific logic in the OSD module
- Do NOT use a full-screen I4 bitmap when individual regions suffice
- Do NOT add SDK type redefinitions — use existing headers or dlopen
- Do NOT add global mutable state beyond what's in the pipeline state struct
