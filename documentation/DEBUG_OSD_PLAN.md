# Debug OSD — Design Plan

## Goal

Add a debug-only on-screen display overlay to waybeam_venc for two purposes:

1. **Stats panel** — fixed-position text region showing encoder metrics
   (fps, bitrate, exposure, crop position, EIS offsets, custom fields)
2. **Spatial debug layer** — full-frame blitting of primitives (rects, points,
   vectors) for visualizing module internals like EIS crop windows and
   optical flow features

Design principle: **debug-rich first, efficient second**. This is a
troubleshooting tool, not a production overlay.

## Prerequisites

**Mutual exclusivity with waybeam-hub OSD.** The debug OSD requires
waybeam-hub's `mod_osd_render` to be disabled/stopped. Both use `MI_RGN_Init`
(global SDK state) and attach to VPE Channel 0, Port 0. Only one process
can own MI_RGN on the SoC at a time. This is acceptable — the debug OSD
is for encoder-level troubleshooting where the production OSD is either
unavailable or not suitable (no direct access to venc internals like
EIS crop state, ISP AE limits, frame-level encoder decisions).

## First Consumer: EIS Crop Visualization

The EIS module already exposes everything needed via `EisStatus` (`eis.h:25-35`):

```c
typedef struct {
    uint16_t crop_x, crop_y;       /* current crop window position */
    uint16_t crop_w, crop_h;       /* crop window size (constant) */
    uint16_t margin_x, margin_y;   /* max displacement per axis */
    float offset_x, offset_y;      /* filtered displacement in pixels */
    /* ... counters ... */
} EisStatus;
```

**Moving-rect-in-fixed-rect visualization:**

```
┌───────────────────────────────┐  ← Full sensor area (image_width × image_height)
│                               │
│   ┌───────────────────────┐   │  ← Max crop travel (margin_x/y inset)
│   │                       │   │
│   │   ┌───────────────┐   │   │  ← Current crop window (crop_x/y, crop_w/h)
│   │   │               │   │   │    Moves frame-to-frame based on gyro
│   │   │               │   │   │
│   │   └───────────────┘   │   │
│   │                       │   │
│   └───────────────────────┘   │
│                               │
└───────────────────────────────┘
```

Three nested rectangles:
- **Outer** (outline, white): full sensor area boundary
- **Middle** (outline, yellow): margin boundary — crop window can move within this
- **Inner** (filled, semi-transparent green): current crop position — moves each frame

Plus text stats in the stats panel:
- `crop: (123, 45)` — current position
- `off: (-2.3, 1.1)` — filtered displacement
- `margin: 96×54` — available travel

**Integration point** — `star6e_runtime.c:687-712`, where EIS status is
already queried each frame in verbose mode. The debug OSD update slots in
right next to this existing code path:

```c
if (ps->eis && ps->debug_osd) {
    EisStatus est;
    eis_get_status(ps->eis, &est);
    debug_osd_eis_update(ps->debug_osd, &est,
                         ps->image_width, ps->image_height);
}
```

## Architecture

### Two rendering zones, one canvas

A single full-frame OSD canvas region handles both zones. Two separate
regions would work but add lifecycle complexity for no benefit — a single
canvas can draw text in a corner and rects anywhere on the frame.

**Canvas setup:**
1. Create one OSD region (`E_MI_RGN_PIXEL_FORMAT_ARGB4444`) at full frame size
2. `MI_RGN_GetCanvasInfo()` → memory-mapped `virtAddr`, `u32Stride`
3. Each frame: clear dirty areas, draw stats panel + spatial elements
4. `MI_RGN_UpdateCanvas()` to commit

**Memory cost:** 1920×1080 × 2 bytes = ~4 MB. Allocated by MI_RGN subsystem
(memory-mapped, no per-frame upload). Only allocated when debug OSD is
enabled. Acceptable for a debug tool — this is transient developer use,
not production runtime.

**Alternative considered — small panel only (~256 KB):** A quarter-screen
region (e.g. 480×270) is enough for the stats panel but can't draw spatial
elements at their actual frame positions. EIS crop visualization needs
rects at real pixel coordinates. A single full-frame canvas serves both.

### Stats panel zone

Fixed-position text region in a corner (e.g. top-left, 320×200 area).
Renders key=value lines using an 8×8 bitmap font (~2 KB lookup table).

Default stats (populated automatically from encoder state):
- `fps`, `bitrate`, `exposure_us`, `soc_temp`
- EIS: `crop_x/y`, `offset_x/y`, `margin`
- Custom: modules can push additional key=value lines

Semi-transparent black background behind text for readability over
any video content.

### Spatial debug zone

Full-frame area for primitives drawn at real pixel coordinates:

| Primitive | Use cases |
|-----------|-----------|
| Rect (outline) | EIS margin boundary, detection regions |
| Rect (filled, alpha) | Current crop window position |
| Point/marker | Optical flow feature points, motion centroid |
| Vector/line | Motion vectors, gyro displacement arrows |

Primitives drawn directly into ARGB4444 canvas. Alpha blending is
per-pixel (format supports 4-bit alpha), so filled rects with e.g.
`0x4F04` (semi-transparent green) overlay the video naturally.

### No vtable dispatch

The adversarial review correctly identified vtable dispatch as
over-engineering. The EIS module needs vtable because it has genuinely
interchangeable algorithm backends. The debug OSD has one backend (RGN
on Star6E) with no concrete second backend planned.

Instead: a single `debug_osd.c` / `debug_osd.h` with the RGN
implementation behind `#ifdef PLATFORM_STAR6E`, matching the existing
conditional compilation pattern used throughout the codebase. On non-Star6E
platforms, `debug_osd_create()` returns NULL (same no-op guard pattern).

If a second backend materializes later, refactoring to vtable is a
straightforward lift — the public API (`create`/`destroy`/`begin_frame`/
`draw`/`end_frame`) stays the same.

### `dlopen` for MI_RGN

`libmi_rgn.so` is not vendored in `libs/star6e/` and should not be.
Runtime dlopen with graceful NULL return on failure:

```c
static int rgn_load_api(DebugOsdState *ctx)
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

**SDK header note:** venc's SDK uses `i6_rgn_*` type names
(`sdk/ssc338q/include/i6_rgn.h`), while waybeam-hub's vendor headers use
`MI_RGN_*` names. The dlopen symbols are `MI_RGN_*` regardless. For
canvas info and other types not declared in venc's SDK headers, define
minimal structs locally in `debug_osd.c` (only what dlopen needs — this
is not a type redefinition of the SDK, just the dlopen ABI contract).

### Pixel format — ARGB4444

16-bit per pixel with 4-bit alpha. Same format used by waybeam-hub's
`mod_osd_render.c` in production. Provides full RGB color (4096 colors)
and per-pixel alpha transparency.

Color constants as ARGB4444 uint16:

| Color | Value | Use |
|-------|-------|-----|
| Transparent | `0x0000` | Background / clear |
| Red | `0xF00F` | Errors, warnings |
| Green | `0x0F0F` | Tracked points, active crop |
| Blue | `0x00FF` | Secondary data |
| Yellow | `0xFF0F` | Boundaries, margins |
| White | `0xFFFF` | Text, outlines |
| Semi-transparent green | `0x4F04` | EIS crop window fill |
| Semi-transparent black | `0x800A` | Text background |

## Public API

```c
/* Opaque state */
typedef struct DebugOsdState DebugOsdState;

/* Create/destroy — returns NULL if RGN unavailable or disabled */
DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                 const MI_SYS_ChnPort_t *vpe_port);
void debug_osd_destroy(DebugOsdState *osd);

/* Frame lifecycle */
void debug_osd_begin_frame(DebugOsdState *osd);  /* clears previous frame */
void debug_osd_end_frame(DebugOsdState *osd);    /* commits canvas */

/* Stats panel — text key=value in the fixed panel region */
void debug_osd_text(DebugOsdState *osd, int row, const char *label,
                    const char *fmt, ...);

/* Spatial primitives — drawn at real frame coordinates */
void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, uint16_t color, int filled);
void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
                     uint16_t color);
void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1, uint16_t color);
```

All functions are no-ops when `osd` is NULL. No element array, no tagged
union — direct draw calls are simpler for the caller and match the
"debug tool" character. The `begin_frame`/`end_frame` pair brackets a
single frame's drawing; all calls between them draw into the same canvas.

**Multi-consumer coordination:** Only one caller drives `begin_frame`/
`end_frame` per frame (the main frame loop in `star6e_runtime.c`). It
calls each consumer's update function sequentially between begin/end.
No thread safety concern — the frame loop is single-threaded.

```c
/* In star6e_runtime.c frame loop, after eis_update */
if (ps->debug_osd) {
    debug_osd_begin_frame(ps->debug_osd);

    /* Stats panel — always shown */
    debug_osd_text(ps->debug_osd, 0, "fps", "%u", current_fps);
    debug_osd_text(ps->debug_osd, 1, "kbps", "%u", bitrate_kbps);

    /* EIS visualization — if active */
    if (ps->eis) {
        EisStatus est;
        eis_get_status(ps->eis, &est);
        debug_osd_eis_draw(ps->debug_osd, &est,
                           ps->image_width, ps->image_height);
    }

    debug_osd_end_frame(ps->debug_osd);
}
```

## Config Integration

### Config struct (`include/venc_config.h`)

```c
typedef struct {
    bool show_osd;         /* master enable for debug OSD */
} VencConfigDebug;
```

Added as `VencConfigDebug debug;` field in `VencConfig`.

### Config parser (`src/venc_config.c`)

- Default: `show_osd = false`
- JSON key: `"debug": { "showOsd": false }`

### API field table (`src/venc_api.c`)

```c
FIELD("debug.show_osd", FIELD_BOOL, VencConfig, debug.show_osd),
```

Enables runtime toggle via `GET /api/v1/set?debug.show_osd=true`.

### Default config (`config/venc.default.json`)

```json
"debug": {
    "showOsd": false
}
```

## File Manifest

| File | Purpose | Lines (est.) |
|------|---------|-------------|
| `include/debug_osd.h` | Public API (functions above), color constants | ~60 |
| `src/debug_osd.c` | RGN canvas lifecycle, dlopen, 8×8 font, draw primitives | ~450 |
| Config updates | `venc_config.{h,c}`, `venc_api.c`, `venc.default.json` | ~20 total |
| Makefile | Add `debug_osd.c` to `STAR6E_ONLY_SRC` | ~2 |
| Pipeline wiring | `star6e_pipeline.h` (add field), `star6e_pipeline.c` (init/destroy), `star6e_runtime.c` (frame loop) | ~40 total |

## Integration Points

### Pipeline state (`include/star6e_pipeline.h`)

```c
struct DebugOsdState;  /* forward declaration */

typedef struct {
    /* ... existing fields ... */
    struct DebugOsdState *debug_osd;  /* NULL if debug OSD disabled */
} Star6ePipelineState;
```

### Pipeline init (`src/star6e_pipeline.c`)

After VPE channel is started and attached, before VENC bind:

```c
if (vcfg->debug.show_osd) {
    state->debug_osd = debug_osd_create(
        state->image_width, state->image_height, &state->vpe_port);
    if (!state->debug_osd)
        fprintf(stderr, "WARN: debug OSD requested but MI_RGN unavailable\n");
}
```

### Pipeline teardown

```c
debug_osd_destroy(state->debug_osd);
state->debug_osd = NULL;
```

### Frame loop (`src/star6e_runtime.c`)

Insert after the existing EIS verbose logging block (line ~712):

```c
if (ps->debug_osd) {
    debug_osd_begin_frame(ps->debug_osd);
    debug_osd_stats(ps);      /* fps, bitrate, exposure, temp */
    debug_osd_eis_draw(ps);   /* crop rects if EIS active */
    debug_osd_end_frame(ps->debug_osd);
}
```

## EIS Crop Visualization Detail

`debug_osd_eis_draw()` renders three nested rects + text:

```c
static void debug_osd_eis_draw(Star6ePipelineState *ps)
{
    EisStatus est;
    eis_get_status(ps->eis, &est);
    DebugOsdState *osd = ps->debug_osd;

    /* Outer rect: full sensor area (white outline) */
    debug_osd_rect(osd, 0, 0,
                   ps->image_width, ps->image_height,
                   DEBUG_OSD_WHITE, 0);

    /* Middle rect: margin boundary (yellow outline) */
    debug_osd_rect(osd, est.margin_x, est.margin_y,
                   ps->image_width - 2 * est.margin_x,
                   ps->image_height - 2 * est.margin_y,
                   DEBUG_OSD_YELLOW, 0);

    /* Inner rect: current crop window (semi-transparent green fill) */
    debug_osd_rect(osd, est.crop_x, est.crop_y,
                   est.crop_w, est.crop_h,
                   DEBUG_OSD_SEMITRANS_GREEN, 1);

    /* Stats */
    debug_osd_text(osd, 3, "crop", "%u,%u", est.crop_x, est.crop_y);
    debug_osd_text(osd, 4, "off", "%.1f,%.1f", est.offset_x, est.offset_y);
    debug_osd_text(osd, 5, "margin", "%u×%u", est.margin_x, est.margin_y);
}
```

## Decisions & Rationale

| Decision | Rationale |
|----------|-----------|
| No vtable (just `#ifdef PLATFORM_STAR6E`) | One backend; matches codebase pattern; refactorable later |
| `dlopen` for MI_RGN | Not vendored; graceful NULL return; no linker hacks |
| Single full-frame canvas (not separate panel + spatial) | Simpler lifecycle; both zones draw into same buffer |
| ARGB4444 (not I4) | Full color + alpha; semi-transparent fills; matches hub OSD |
| Direct draw calls (not element array + tagged union) | Debug tool — simpler caller code; no batch/dispatch overhead |
| Single-threaded begin/end (not per-consumer) | Frame loop owns the bracket; consumers called sequentially |
| Mutual exclusivity with hub OSD | Debug mode; MI_RGN_Init is global; acceptable constraint |
| 8×8 bitmap font | Zero dependencies; ~2 KB; adequate for debug text |

## What NOT to Do

- Do NOT add `--unresolved-symbols=ignore-in-shared-libs` to linker flags
- Do NOT statically link `-lmi_rgn` — use `dlopen`
- Do NOT embed consumer-specific logic in the OSD module — consumers call
  the draw API, OSD module just renders primitives
- Do NOT use `MI_RGN_SetBitMap` — use `GetCanvasInfo` + direct writes + `UpdateCanvas`
- Do NOT run debug OSD concurrently with waybeam-hub `mod_osd_render`
- Do NOT add vtable/factory pattern unless a second backend actually materializes
