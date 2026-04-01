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

### RGN Backend Strategy (Star6E)

**Key insight from waybeam-hub's `mod_osd_render.c`:** The SigmaStar MI_RGN
API provides `MI_RGN_GetCanvasInfo()` for direct memory-mapped canvas access.
Instead of building bitmaps in CPU memory and uploading via `MI_RGN_SetBitMap`
per frame, the backend maps the canvas once, writes pixels directly into the
mapped buffer, and commits with `MI_RGN_UpdateCanvas()`. This is the proven
pattern used in the production OSD.

**Rendering approach — single canvas region with direct writes:**

1. Create one OSD region sized to the video frame (or a smaller debug area)
2. `MI_RGN_GetCanvasInfo()` → get `virtAddr`, `u32Stride`, `stSize`
3. Each frame: clear only dirty areas, draw elements directly into mapped memory
4. `MI_RGN_UpdateCanvas()` to commit — no per-frame copy or upload

This replaces the earlier multi-region strategy. A single canvas with direct
writes is simpler (one RGN handle, no pool management) and proven in production.

**Per-element rendering into canvas:**

| Element type | Canvas strategy | Cost |
|-------------|----------------|------|
| Marker | Write a small glyph (e.g. 8×8 crosshair) at (x,y) into canvas | 128 bytes written |
| Rect | Rasterize outline or fill directly into canvas pixels | Stride × height bytes |
| Text | Render 8×8 font glyphs into canvas at row offset | ~width × 16 bytes per char |
| Vector | Bresenham line rasterized into canvas pixels | Proportional to length |

**Dirty-rect optimization:** Track a bounding box of changed regions per frame.
On `begin_frame`, clear only the previous frame's dirty rect (not the entire
canvas). This minimizes memset cost — typical debug overlays touch < 5% of
the canvas area.

**Pixel format — ARGB4444:**

16-bit per pixel with 4-bit alpha. This is the format used by waybeam-hub's
`mod_osd_render.c` (`E_MI_RGN_PIXEL_FORMAT_ARGB4444`) and provides:

- Full RGB color (4096 colors) — no palette limitations
- Per-pixel alpha transparency — clean overlay compositing
- 2 bytes/pixel — a 1920×1080 canvas = ~4 MB, but a reduced debug area
  (e.g. 480×270 quarter-screen) = ~256 KB

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

**Canvas region sizing:** The debug OSD does not need a full-screen canvas.
A quarter-resolution region (e.g. 480×270 at a corner) is sufficient for
debug text and markers, using ~256 KB. Full-screen is available if vector/rect
elements span the frame, at ~4 MB — still acceptable since it's memory-mapped
(no upload bandwidth), and only drawn when debug OSD is enabled.

**`dlopen` for graceful degradation:**

```c
static int rgn_load_api(RgnBackend *ctx)
{
    ctx->lib = dlopen("libmi_rgn.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!ctx->lib) return -1;
    /* resolve: MI_RGN_Init, MI_RGN_Create, MI_RGN_AttachToChn,
       MI_RGN_GetCanvasInfo, MI_RGN_UpdateCanvas,
       MI_RGN_SetDisplayAttr, MI_RGN_Destroy,
       MI_RGN_DetachFromChn, MI_RGN_DeInit */
    return 0;
}
```

If `libmi_rgn.so` is not present on the target, `debug_osd_create()` returns
NULL and every consumer's `if (!osd) return;` guard makes the entire OSD
path a no-op. No linker hacks needed.

### Why Canvas API, Not SetBitMap or Multi-Region

**vs. `MI_RGN_SetBitMap` (per-frame upload):**
The bitmap approach requires allocating a CPU-side buffer, rendering into it,
then copying the entire bitmap to the RGN subsystem every frame. The canvas
API provides a memory-mapped pointer — writes go directly to the backing
store with zero copy overhead. `MI_RGN_UpdateCanvas()` just commits (flips),
no data transfer.

**vs. multi-region (one RGN handle per element):**
The multi-region approach avoids full-screen uploads but introduces complexity:
handle pooling, per-element create/destroy lifecycle, hardware handle limits
(~16 on Star6E). A single canvas region with direct pixel writes is simpler,
uses one handle, and scales to any number of drawn elements without hardware
limits.

**Proven in production:** waybeam-hub's `mod_osd_render.c` uses exactly this
pattern — `GetCanvasInfo` → direct memory writes via LVGL → `UpdateCanvas`.
The debug OSD replaces LVGL with a lightweight 8×8 bitmap font and primitive
rasterizers, but the RGN interaction is identical.

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
| Canvas API (not SetBitMap or multi-region) | Proven in waybeam-hub; zero-copy direct writes; one handle; no pool management |
| ARGB4444 pixel format (not I4) | Full RGB color + alpha; no palette setup; matches mod_osd_render production format |
| Single canvas region (not per-element regions) | Simpler lifecycle; no hardware handle limits; dirty-rect approach minimizes writes |
| 8x8 bitmap font (not freetype/cairo) | Zero dependencies; ~2 KB table; adequate for debug text |
| Flat element array (not retained scene graph) | Simple; no lifetime management; consumers rebuild each frame |
| `debug.showOsd` config (not per-module flags) | Single kill switch; consumers already have their own enable flags |
| Star6E first, Maruko stub later | Per project policy (AGENTS.md: implement on Star6E first) |

## What NOT to Do

- Do NOT add `--unresolved-symbols=ignore-in-shared-libs` to linker flags
- Do NOT statically link `-lmi_rgn` — use `dlopen`
- Do NOT embed optical-flow-specific logic in the OSD module
- Do NOT use `MI_RGN_SetBitMap` — use `GetCanvasInfo` + direct writes + `UpdateCanvas`
- Do NOT allocate one RGN handle per element — use a single canvas region
- Do NOT use I4 indexed pixel format — use ARGB4444 for full color and alpha
- Do NOT add SDK type redefinitions — use existing headers or dlopen
- Do NOT add global mutable state beyond what's in the pipeline state struct
