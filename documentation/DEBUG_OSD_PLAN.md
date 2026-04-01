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
| `DEBUG_OSD_ELEM_RECT` | Axis-aligned rectangle, outline or filled | ROI bounds, SAD blocks, detection regions, crop window |

Rect elements carry a `filled` flag. Filled rects are rendered as hardware
Cover regions (semi-transparent area highlights, visually distinct from canvas
content). Outline rects are drawn as 4 lines on the canvas. If Cover handles
are exhausted, filled rects silently fall back to canvas rasterization.

Future element types (histogram, grid, heatmap) are added by extending the
enum and adding a draw case in each backend — no interface changes needed.

### Text Rendering

Text uses a built-in **8x8 bitmap font** (embedded lookup table, ~2 KB).
Fixed-width, ASCII-only — sufficient for debug labels and numeric values.

Text elements carry:
- `row` — vertical position (0 = top, increments downward)
- `label` — short string key (e.g., "tx", "fps", "crop_x")
- `value` — formatted string (caller formats via snprintf before submission)
- `color` — ARGB4444 color value

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

### RGN Backend Strategy (Star6E) — Hybrid Canvas + Cover

The design principle is **debug-rich first, efficient second**. This is a
debug overlay — visual clarity and information density matter more than
minimizing CPU cycles. The backend uses two complementary RGN mechanisms,
each for what it does best:

**1. Canvas region — text, markers, vectors, outlines**

Based on the proven pattern from waybeam-hub's `mod_osd_render.c`:

1. Create one full-frame OSD region (`E_MI_RGN_PIXEL_FORMAT_ARGB4444`)
2. `MI_RGN_GetCanvasInfo()` → get `virtAddr`, `u32Stride`, `stSize`
3. Each frame: clear canvas, draw all pixel-based elements directly
4. `MI_RGN_UpdateCanvas()` to commit — zero-copy, just a flip

Full-frame canvas (1920×1080 ARGB4444 = ~4 MB) is acceptable for a debug
tool — it's memory-mapped (no upload bandwidth), only active when debug
OSD is enabled, and gives unrestricted drawing anywhere on screen.

**2. Cover regions — filled rectangles for area highlighting**

Cover regions (`E_MI_RGN_TYPE_COVER`) are hardware-rendered solid-color
rectangles. They're the natural tool for marking debug areas:

- ROI bounds, crop windows, detection regions, SAD blocks
- Semi-transparent colored overlays that visually stand out from canvas content
- Zero CPU cost — position, size, and color set via `MI_RGN_SetDisplayAttr`
- Visually distinct from canvas-drawn content (hardware composited at a
  different layer)

The ~16 handle limit is not a concern for debug rectangles — you rarely
need more than 4-5 area indicators simultaneously.

**Per-element rendering dispatch:**

| Element type | Mechanism | Rationale |
|-------------|-----------|-----------|
| Text | Canvas | Arbitrary position, font rasterization, any string content |
| Marker | Canvas | Small glyphs at pixel-precise locations, unlimited count |
| Vector | Canvas | Bresenham lines, arrows — arbitrary angle and length |
| Rect (outline) | Canvas | Thin outlines drawn as 4 lines into canvas pixels |
| Rect (filled) | Cover region | Hardware-rendered area highlight, visually distinct layer |

Consumers specify filled vs outline via a flag in the rect element. The
backend dispatches to Cover or canvas accordingly. If Cover handles are
exhausted, filled rects fall back to canvas rasterization silently.

**Pixel format — ARGB4444 (canvas):**

16-bit per pixel with 4-bit alpha, matching waybeam-hub's production OSD:

- Full RGB color (4096 colors) — no palette limitations
- Per-pixel alpha transparency — clean overlay compositing
- Full-frame 1920×1080 = ~4 MB — acceptable for debug-only use

Color constants as ARGB4444 uint16:

| Color | Value | Use |
|-------|-------|-----|
| Transparent | `0x0000` | Background / clear |
| Red | `0xF00F` | Primary marker, errors |
| Green | `0x0F0F` | Tracked points, success |
| Blue | `0x00FF` | Secondary data |
| Yellow | `0xFF0F` | Warnings, highlight |
| Cyan | `0x0FFF` | Tertiary data |
| White | `0xFFFF` | Text, outlines |
| Semi-transparent black | `0x0008` | Text background shadow |

**Cover region colors:** Cover regions use a separate color format (typically
ARGB8888 via `MI_RGN_ChnPortParam_t`). Semi-transparent fills (e.g. 50% alpha
red for an ROI box) make covered video content still visible beneath the
debug overlay.

**`dlopen` for graceful degradation:**

```c
static int rgn_load_api(RgnBackend *ctx)
{
    ctx->lib = dlopen("libmi_rgn.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!ctx->lib) return -1;
    /* Canvas API: MI_RGN_GetCanvasInfo, MI_RGN_UpdateCanvas
       Cover API:  MI_RGN_Create (COVER type), MI_RGN_SetDisplayAttr
       Lifecycle:  MI_RGN_Init, MI_RGN_AttachToChn, MI_RGN_Destroy,
                   MI_RGN_DetachFromChn, MI_RGN_DeInit */
    return 0;
}
```

If `libmi_rgn.so` is not present on the target, `debug_osd_create()` returns
NULL and every consumer's `if (!osd) return;` guard makes the entire OSD
path a no-op. No linker hacks needed.

### Why Hybrid, Not Canvas-Only or Multi-Region-Only

**Canvas alone** can draw everything, but filled rectangles on the canvas
are just colored pixels — they don't visually stand out from other canvas
content and can't be semi-transparent over the video independently. Cover
regions are composited by the hardware at a different layer, giving debug
area highlights a distinct visual quality.

**Multi-region alone** (one OSD handle per text/marker) introduces handle
pooling complexity and hits hardware limits with many markers. The canvas
handles unlimited pixel-based elements with one handle.

**The hybrid** uses each mechanism for what it's best at: canvas for
arbitrary pixel drawing (text, lines, dots), Cover for area highlighting
(filled rects). This matches the debug-first priority — maximum visual
information with minimal implementation complexity.

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
| `src/debug_osd_rgn.c` | Star6E RGN backend: dlopen, canvas API, font/primitive rasterizers | ~400 |
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
| Hybrid canvas + Cover (not single mechanism) | Canvas for pixels (text, markers, vectors); Cover for area highlights (filled rects). Debug-rich first, efficient second |
| ARGB4444 canvas (not I4) | Full RGB color + alpha; no palette setup; matches mod_osd_render production format |
| Full-frame canvas (not quarter-screen) | Debug tool — unrestricted drawing anywhere; ~4 MB acceptable when enabled |
| Cover regions for filled rects (not canvas fill) | Hardware-composited, visually distinct layer; semi-transparent over video; zero CPU |
| 8x8 bitmap font (not freetype/cairo) | Zero dependencies; ~2 KB table; adequate for debug text |
| Flat element array (not retained scene graph) | Simple; no lifetime management; consumers rebuild each frame |
| `debug.showOsd` config (not per-module flags) | Single kill switch; consumers already have their own enable flags |
| Star6E first, Maruko stub later | Per project policy (AGENTS.md: implement on Star6E first) |

## What NOT to Do

- Do NOT add `--unresolved-symbols=ignore-in-shared-libs` to linker flags
- Do NOT statically link `-lmi_rgn` — use `dlopen`
- Do NOT embed optical-flow-specific logic in the OSD module
- Do NOT use `MI_RGN_SetBitMap` — use `GetCanvasInfo` + direct writes + `UpdateCanvas`
- Do NOT allocate one RGN handle per text/marker/vector — use the canvas for pixel elements
- Do NOT use I4 indexed pixel format — use ARGB4444 for full color and alpha
- Do NOT draw filled rects on canvas when Cover regions give better visual separation
- Do NOT add SDK type redefinitions — use existing headers or dlopen
- Do NOT add global mutable state beyond what's in the pipeline state struct
