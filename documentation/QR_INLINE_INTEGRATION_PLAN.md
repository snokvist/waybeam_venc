# Inlining QR detection into `waybeam` — review and plan

Status: **Phase 1 (spec)**. No code has been written. This document answers
"should we inline the QR decoder, and what would it take", specifies the capture
policy for the scan window, designs boot autoscan and its OSD coordination, and
lists the simplifications the decoder needs before it runs inside the daemon.

## 1. Verdict

**Yes — inline it, but inline the *cascade*, not the *tool*.**

Extract the scan cascade from `tools/qr/qr_decode.c` into a library
(`src/qr_scan.c`) that both the daemon and the existing standalone binary link.
Add a small scan-window state machine (`src/venc_qr.c`) driven by two endpoints:

| Endpoint | Purpose |
|---|---|
| `GET /api/v1/qr/scan` | (re)start a bounded scan window; restarting resets the deadline |
| `GET /api/v1/qr/recent` | last result: `scanning` / `found` / `not_found` / `idle` |

What must **not** happen is a second copy of the cascade. The 768-image
regression corpus (`tests/test_qr_marker.c`) and the bench CLI are the only
things proving this decoder works; if the daemon gets a forked copy, the
corpus stops testing what ships.

Measured cost of inlining: **~30 KB of text** and transient heap of **~4×W×H
during a scan window only**. See §4.

## 2. What exists today

| Piece | Lines | Role |
|---|---|---|
| `tools/qr/qr_decode.c` | 1030 | CLI + image loading (stb JPEG / P5 PGM) + the whole scan cascade |
| `tools/qr/quirc/` | 3477 | vendored quirc, patched with the outer-frame marker mode |
| `tools/qr/waybeam_qr_format.c` | 35 | 16-char `P`/`C` envelope validation |
| `tools/qr/qr_watch.sh` | 239 | polls `snapshot.jpg` over HTTP, forks `qr_decode` per frame |
| `tests/test_qr_marker.c` | 622 | deterministic corpus (`make qr-test-host`, `--extended` = 768 images) |
| `tests/test_qr_cli.sh` | 84 | CLI + loader hardening |

The current production flow is out-of-process:

```
qr_watch.sh → curl GET /api/v1/snapshot.jpg → /tmp file → fork qr_decode → stdout
```

The capture side is already correct and already inside waybeam: a dedicated
MJPEG VENC channel created once at pipeline start and pulse-encoded per request
(`src/venc_jpeg.c`, `venc_jpeg_capture()`), serialized under one mutex. The
per-request VPE/SCL tap that could wedge the SoC was retired in 0.60.0 —
nothing in this plan brings it back.

## 3. Why inline — and what it does *not* buy

### It does not buy meaningful speed

The plumbing that inlining removes (HTTP round trip, `/tmp` write, `fork`+`exec`)
is roughly 10–20 ms per frame. The decode itself is 400 ms–2 s. **Performance
is not the argument.**

### What it does buy

1. **The requested UX is inherently a daemon feature.** A "restart a 10–15 s
   window, then read the result" API is a state machine with a deadline.
2. **Removes a target-image dependency.** `qr_watch.sh` uses `curl`
   (`tools/qr/qr_watch.sh:191`). OpenIPC images ship busybox `wget`; `curl` is
   not guaranteed. Inlining removes curl, the temp file, and the shell entirely.
3. **Any client can drive it.** WebUI button, hub, phone app — all over the
   existing HTTP API. Today it requires an SSH session.
4. **Lifecycle correctness.** The scan window cannot outlive the pipeline,
   cannot run while the pipeline is down, and cannot be left running by a
   forgotten `ctrl-c`.
5. **Capture policy and OSD coordination become possible at all.** Overriding
   quality for the duration of a scan (§6) and ordering a boot scan ahead of OSD
   attach (§7) both require something that knows a scan is in progress. A forked
   CLI does not.

### What it costs

- **+~30 KB text.** Measured (§4).
- **A parser in the encoder's address space.** stb_image is a large JPEG
  parser; a fault in it now takes down video. Mitigating factor: the JPEG is
  produced by our own VENC channel, not received from the network, so this is
  not an untrusted-input surface. Risk accepted, noted in §13.
- **CPU and heap during the window.** Bounded, but §6's no-downscale rule makes
  both scale with the main stream resolution.

### The alternative considered and rejected

Ship `qr_decode` + a C supervisor as a second binary. Keeps the crash domain
separate but requires a second REST surface to implement scan/recent,
duplicating waybeam's HTTP plumbing for one feature. Not worth it for a decoder
consuming our own JPEG.

## 4. Measured cost

### Code size

Compiled at `-Os -ffunction-sections -fdata-sections` (host x86-64, as a
proportional guide):

| Object | text |
|---|---|
| `identify.o` | 13,160 |
| `decode.o` | 5,704 |
| `version_db.o` | 3,312 |
| `quirc.o` | 941 |
| `waybeam_qr_format.o` | 221 |
| `qr_decode.o` (mostly stb JPEG) | 28,101 |
| **total** | **51,439** |

On the real target this is smaller: the whole stripped standalone Star6E binary
at `-Os` was **30,288 bytes** (`tools/qr/README.md`), including quirc, stb, the
cascade, *and* CRT startup. **Budget ~30 KB at `-Os`, ~55 KB at `-O3`.**

### Decode latency (from the existing bench measurements)

| Build | Input | Time |
|---|---|---|
| `-Os` | 1280×720, hard fisheye capture, full cascade to `lens-blur/full/refine` | 425 ms mean / 472 ms max |
| `-Os` | complete no-code cascade, 720p | ~550 ms |
| `-Os` vs `-O3` | 3.6 MB frame, end to end | 2206 ms vs 1265 ms (**1.66×**) |

**This is a build-flag finding, not a nice-to-have.** `waybeam` is built `-Os`
globally (`COMMON_CFLAGS`, `Makefile:42`), while `qr-decode` is deliberately
built `-O3` (`Makefile:252`, with a comment explaining exactly why). Dropping
the cascade into waybeam's global `-Os` silently makes every scan ~1.66× slower.
The QR translation units need a per-object optimization override (§10).

### Memory and attempt count — now driven by the main stream

With §6's no-downscale rule, both scale with whatever the main stream is:

| Main stream | peak transient heap (≈4×W×H) | full-cascade miss | attempts in a 15 s window |
|---|---|---|---|
| 1280×720 | 3.7 MB | ~550 ms | ~20 |
| 1920×1080 | 8.3 MB | ~1.2 s | ~10 |
| 2560×1440 | 14.7 MB | ~2 s | ~6 |

Plus the JPEG blob and stb's internal decode buffers. **These are projections
from the 720p bench figure, not measurements at higher resolutions** — confirm
with experiments D and G (§14).

The heap number is why `qr.windowMs` defaults to 15000 rather than 12000: on a
high-resolution mode a 12 s window buys only about five attempts.

Without the arena change (§11.6) the current code peaks at ~5×W×H and does ~5
malloc/free of full-frame buffers *per decoded frame*. All are well above
`MMAP_THRESHOLD`, so they go straight to `mmap`/`munmap` — no fragmentation, but
dozens of kernel round trips per window for no reason.

## 5. Proposed API surface

Contract bump: **0.17.0** (additive, non-breaking).

### `GET /api/v1/qr/scan`

Starts a scan window. Calling it while a window is running **restarts** the
deadline rather than queueing or erroring.

```json
{"ok":true,"data":{"scanning":true,"window_ms":15000,"capture":"1920x1080@q90"}}
```

- `503` `qr_disabled` — `qr.enabled` false, or `snapshot.enabled` false /
  pipeline not up (the scanner cannot capture without the MJPEG channel).

Repeated calls are self-limiting: each one only moves the deadline, and
`qr.intervalMs` caps the duty cycle. No extra rate limiting is needed.

### `GET /api/v1/qr/recent`

```json
{"ok":true,"data":{
  "state":"found",
  "payload":"P23456789ABCDEFG",
  "payload_age_s":4,
  "attempts":7,
  "elapsed_ms":3120,
  "stage":"blur/full/refine"
}}
```

| `state` | meaning |
|---|---|
| `idle` | no scan requested since process start |
| `scanning` | a window is open |
| `found` | the last window decoded a valid envelope |
| `not_found` | the last window expired without a valid envelope |

`payload` / `payload_age_s` are **sticky**: they survive a later `not_found`
window so a client can still read the last good code. `state` alone answers
"did the window succeed". `stage` names the cascade pass that hit — useful for
tuning marker placement and focus, and free to carry.

Scan state is process-local: a `/api/v1/restart`, SIGHUP reinit, or respawn
resets it to `idle`.

## 6. Capture policy — the scan overrides snapshot settings

**Decided:** a scan captures at the source port's native geometry and at q90.
**It never downscales.** `snapshot.width`/`height`/`quality` do not apply.

### "Highest available" is already the only option

| Backend | JPEG channel source | Geometry |
|---|---|---|
| Star6E | `state->vpe_port` — **the same port0 the main H.265 channel consumes** (`star6e_pipeline.c:2148`) | main stream |
| Maruko | a dedicated SCL port 1 — but programmed to `out_width`/`out_height` at configure time (`maruko_pipeline.c:922-923`) | main stream |

The Maruko code says so in a comment: *"same output dims as port 0 so snapshots
match the live stream framing"*.

On both backends the frames reaching the MJPEG channel are **main-stream sized**.
`snapshot.width`/`snapshot.height` only set the MJPG *channel attributes*
(`star6e_jpeg.c:69-74`, `maruko_jpeg.c:103-104`); neither backend reprograms its
source port from them. So "highest available" == the main stream resolution, and
**there is nothing to configure** — the scanner uses what the port emits. More
pixels would require a second, larger tap: exactly the per-request VPE/SCL tap
retired in 0.60.0 for wedging the SoC. Off the table.

> **Consequence: `snapshot.width`/`snapshot.height` are probably non-functional
> today.** They are documented as *"size the MJPEG channel up when markers must
> decode from further away"* (`tools/qr/README.md:83-88`), which cannot work — a
> port does not emit more pixels because the encoder attr asks for more. Needs
> device experiment A (§14). If confirmed dead, delete both fields with a
> `HISTORY.md` note rather than leaving a knob that reads as a range control.
> Removing config fields touches all five config layers and the byte-equal
> default-file test.

### No downscale — a refusal ceiling, not a clamp

QR range scales with pixels per module, so any downscale spends exactly the
resource the feature exists to maximise. The scanner therefore scans the native
frame, always.

A bound is still required so a wild geometry cannot OOM the encoder. Replace the
CLI's `MAX_INPUT_DIM 4096` (`qr_decode.c:58`) with an explicit **pixel-count
ceiling** that **refuses and logs** rather than resizing:

```c
#define QR_SCAN_MAX_PIXELS (8u * 1000u * 1000u)   /* ~4K DCI; 4x that is ~32 MB */
```

Above the ceiling the scan fails with a logged reason and `/qr/recent` reports
`not_found`. Never a silent resize.

`downscale2()` stays in the cascade — the `sharp/half` and `blur/half` passes
are internal rescue paths that the 768-image corpus validates, and they are not
a capture-resolution decision.

### Quality override — q90

`venc_jpeg_set_quality()` is already live and already serialized under the same
mutex as capture, so raising quality for a window needs no new SDK surface.

```c
#define QR_SCAN_QUALITY 90
```

A constant, not a fourth config field. The 8×8 ringing that smears QR module
edges is largely gone by q90, while byte size — and therefore stb decode time,
which is roughly proportional — climbs steeply above it. Caveats:

- **Restore on every exit path** — success, timeout, `qr.enabled` flip, thread
  stop, pipeline teardown. A missed restore leaves the device at q90 forever:
  larger snapshots and more encoder work for every later `/snapshot.jpg`. One
  restore point at window exit; re-assert the override at window start so it is
  idempotent.
- **Restore by re-reading `cfg->snapshot.quality`, not a cached pre-window
  value.** If someone changes `snapshot.quality` via `/api/v1/set` mid-window,
  re-reading converges on the new value; a cached restore would silently revert
  their change.
- **It is globally visible.** A concurrent `/api/v1/snapshot.jpg` during a
  window gets q90. Acceptable; document it.
- **It does not touch config or disk.** `venc_jpeg_set_quality()` updates only
  the module's copy, so nothing is persisted and a mid-window pipeline reinit
  self-heals (the channel is recreated from config).

### `dstFps` 5 → 20

**Decided.** Both backends bind FRAMEBASE at `dstFps = 5`
(`star6e_jpeg.c:108`; Maruko comment at `maruko_pipeline.c:909-910`), so the
JPEG channel sees a frame at most every ~200 ms and every capture waits 0–200 ms
before decoding starts — up to ~2 s of a 15 s window. At 20 that drops to 50 ms.

Two things to check when making the change:

- **Idle cost.** `StartRecvPic` is off between captures, so extra frames should
  be delivered and dropped rather than encoded — but delivery is not free.
  Measure idle CPU before/after (experiment C, §14) and back off if it costs.
  The bind is established at init; changing it live would mean unbind/rebind, so
  this is a static value, not a per-window one.
- **`srcFps` is hardcoded 30** in the same call
  (`MI_SYS_BindChnPort2(&g_vpe_port, &jpeg_port, 30, 5, ...)`). If the pipeline
  is actually running 60/90/120 fps, the declared ratio does not match reality
  and the effective delivery rate is not what the numbers say. Worth passing the
  configured framerate as `srcFps` while touching this line — but verify, since
  the current 5/30 has been in service and the real ratio may already be
  compensating.

### Latent bug that q90 makes more likely

Both backends cap the pack count and then **silently truncate**:

```c
uint32_t n = stat.curPacks;
if (n > MAX_PACKS_PER_JPEG) n = MAX_PACKS_PER_JPEG;   /* star6e_jpeg.c:167 */
if (n > MAX_PACKS) n = MAX_PACKS;                     /* maruko_jpeg.c:211 */
```

If a frame splits into more than 8 packs, the concatenated blob is a truncated
JPEG returned to the caller as **success**. stb then fails, or decodes a partial
image. Invisible at q80; q90 at full resolution means more bytes and more splits.
Fix: treat `curPacks > MAX_PACKS` as `-EIO` with a log line. Small, independent
of the QR work, worth doing in the same PR.

## 7. Boot autoscan and OSD coordination

### The OSD does occlude snapshots on Star6E

`debug_osd` is an `MI_RGN` region attached at `RGN_MODID_VPE, dev 0, chn 0,
port 0` (`debug_osd.c:435-440`) — **the same VPE port0 the JPEG channel binds
to**. The pipeline comment states it outright: *"it composites on VPE port0"*
(`star6e_pipeline.c:2280`). So with `debug.showOsd` on, OSD pixels are burned
into every snapshot, and a marker underneath the text will not decode.

**Maruko is likely already clear.** Its OSD attaches at `RGN_MODID_SCL, dev 0,
chn 0, port 0` (`debug_osd.c:795-798`) while the JPEG channel taps SCL port
**1**. Different output port, so port 1 should carry no OSD. Verify
(experiment H) before relying on it.

An external `waybeam_hub` OSD composites at the SCL stage, downstream of the
Star6E VPE port0 tap, so it should not appear in snapshots either — and the
pipeline comment notes it and `debug_osd` are mutually exclusive anyway (single
global RGN device). Also verify.

Note `debug.showOsd` defaults to **false**, so this only bites users who turned
it on.

### There is no show/hide today

`include/debug_osd.h` exposes creation and `debug_osd_destroy()` — nothing else.
`debug.show_osd` is `MUT_RESTART` (`venc_api.c:615`) and the OSD is created only
if the flag is set at pipeline configure (`star6e_pipeline.c:2286`). So today the
only way to remove the OSD is a full pipeline reinit.

And detaching is not free. `debug_osd.c:140-148` documents a hard-won hazard:

> the detach removes the region from the VPE compositor's list, but a frame
> already mid-composite can still be reading the RGN canvas; freeing it
> immediately races that read → MMU read-fault … that storms to a HW watchdog
> reset on rapid respawn

That is between `DetachFromChn` and `Destroy`, so a pure `show = 0` re-attach
may well be safe — but "may well be" is not something to assume on a path that
has already produced watchdog resets.

### Recommended design: order the boot scan ahead of OSD attach

`venc_jpeg_init()` runs at `star6e_pipeline.c:2161`; `debug_osd_create()` runs at
`star6e_pipeline.c:2286`. The OSD is the later of the two, so the boot scan slots
in between them **by ordering alone — no attach/detach, no RGN hazard**:

```
pipeline configure
  … VPE→VENC bind, venc_jpeg_init() …
  → boot scan window opens (runs until decode or qr.bootWindowMs expires)
  → debug_osd_create()   [deferred until the boot scan finishes]
  … stream …
```

The visible cost is that a device with `debug.showOsd` on has no OSD for the
first few seconds. That is self-explanatory and reversible; a pipeline reinit to
toggle the OSD is not.

**Open question to settle first (experiment I):** frames must actually be
flowing for the scan to capture anything. If the sensor is not yet producing at
line 2161, the boot scan cannot run synchronously inside `configure()` and must
instead be a post-bring-up step with OSD creation gated on its completion — same
ordering, slightly more plumbing (a `venc_qr_boot_pending()` check before
`debug_osd_create()`). Determine which before writing the code.

### Runtime scans with the OSD on stay occluded

The ordering trick only helps at boot. A `/api/v1/qr/scan` on a running device
with `debug.showOsd` on still sees OSD pixels. Options, in order of preference:

1. **Document it and place the marker clear of the text.** The region is
   full-frame but only text pixels are opaque (I4 palette, pixel alpha), so this
   is a placement problem, not a fundamental one. Zero code.
2. **Add `debug_osd_set_visible()`** via a `show = 0` re-attach (not
   detach/destroy), toggled around the scan window. Needs experiment J to
   confirm it is safe on a live pipeline. **Follow-up, not this PR.**

Recommend shipping 1 and treating 2 as a separate change gated on its own device
test — the RGN lifecycle is not somewhere to speculate.

### Config for boot autoscan

Two more fields on the `qr` section (§8): `qr.bootScan` and `qr.bootWindowMs`.
Keeping the boot window separate from `qr.windowMs` matters — boot is the one
time the device is guaranteed OSD-free and pointed at whatever the operator is
holding, so it may want a different (likely longer) budget than a manual scan.

## 8. Proposed config surface

Five fields, one new section. Capture resolution and quality are **not** config:
they follow §6.

| JSON (camelCase) | API (`section.snake_case`) | Type | Default | Mut. | Why |
|---|---|---|---|---|---|
| `qr.enabled` | `qr.enabled` | bool | `true` | live | hard gate; inert until a scan starts, but a locked-down build needs an off switch |
| `qr.windowMs` | `qr.window_ms` | uint | `15000` | live | manual scan timer; clamp 1000–60000. 15 s not 12 s because §4's attempt count drops with resolution |
| `qr.intervalMs` | `qr.interval_ms` | uint | `500` | live | minimum ms between capture starts — the CPU duty-cycle knob; clamp 100–5000, mirroring `qr_watch.sh`'s 0.5 s floor |
| `qr.bootScan` | `qr.boot_scan` | bool | `false` | restart | run a scan window during pipeline bring-up, ahead of OSD attach (§7) |
| `qr.bootWindowMs` | `qr.boot_window_ms` | uint | `15000` | restart | boot window budget; same clamp. Separate from `windowMs` — see §7 |

The first three are read at window start, so they are `MUT_LIVE`. The two boot
fields only take effect at bring-up, so they are `MUT_RESTART` — which is honest
rather than pretending a live change does anything.

Layers to update in the same PR (per AGENTS.md): `VencConfig` +
`venc_config_defaults()` → `load_qr()` + `venc_config_to_json()` → `render_qr()`
in the hand-rolled printer → `g_fields[]`/`g_aliases[]` →
`config/waybeam.default.json`. UI comes free via `FIELD_UI` descriptors (the
data-driven path, as `snapshot.*` already does) — **no `SECTIONS[]` edit and no
`make webui` rebuild.**

`test_save_layout_byte_equal` enforces printer/default-file byte equality; the
new section must be added to both or it fails.

## 9. File and module layout

```
include/qr_scan.h      NEW  cascade API: context, options, result
src/qr_scan.c          NEW  the cascade, moved from qr_decode.c (~370 lines)
include/venc_qr.h      NEW  scan-window state machine + HTTP handlers + boot hook
src/venc_qr.c          NEW  worker thread, deadline, capture policy, state, endpoints
tools/qr/qr_decode.c   THIN reduced to CLI: options, slurp, PGM/JPEG load, main (~250 lines)
tools/qr/quirc/*       UNCHANGED
tools/qr/waybeam_qr_format.c UNCHANGED
tests/test_qr_marker.c UNCHANGED  (still links quirc directly; corpus semantics unaffected)
```

Proposed `include/qr_scan.h`:

```c
typedef struct QrScanCtx QrScanCtx;   /* opaque: quirc handle + scratch arena */

typedef struct {
	uint64_t deadline_us;   /* CLOCK_MONOTONIC; 0 = no deadline (CLI) */
	unsigned max_pixels;    /* refuse above this; 0 = no bound. Never resizes. */
	bool     raw;           /* skip envelope validation (bench only) */
	bool     trace;         /* per-stage stderr trace (CLI --stats / system.verbose) */
} QrScanOptions;

typedef struct {
	char     payload[QR_PAYLOAD_MAX];  /* NUL-terminated; empty on miss */
	char     stage[32];                /* cascade pass that decoded */
	unsigned regions, frames, refinements, qr_decoded, envelope_rejected;
	uint64_t total_us;
} QrScanResult;

QrScanCtx *qr_scan_new(void);
void       qr_scan_free(QrScanCtx *ctx);

/* 0 = decoded, 1 = no decode (incl. deadline expiry / over max_pixels),
 * -1 = fatal (alloc). */
int qr_scan_gray(QrScanCtx *ctx, const uint8_t *gray, int w, int h,
	const QrScanOptions *opt, QrScanResult *out);
```

The daemon owns one `QrScanCtx` for the lifetime of the scan thread; quirc
already retains its high-water image/flood-fill allocations across resizes, so
reuse across a window is nearly allocation-free after the first frame.

## 10. Makefile changes

```make
# QR TUs: link the cascade into waybeam.
HELPER_SRC += src/qr_scan.c src/venc_qr.c tools/qr/waybeam_qr_format.c \
              tools/qr/quirc/quirc.c tools/qr/quirc/decode.c \
              tools/qr/quirc/identify.c tools/qr/quirc/version_db.c

COMMON_CFLAGS += -Itools/qr/quirc -Itools/qr/stb $(QR_MATH_CFLAGS)

# Decode latency, not size, is what these TUs are judged on — same reasoning as
# the standalone qr-decode target. -Os here would cost ~1.66x.
$(OBJ_DIR)/src/qr_scan.o: CFLAGS += -O2
$(OBJ_DIR)/tools/qr/quirc/%.o: CFLAGS += -O2
```

Target-specific variables on pattern-rule prerequisites work in GNU make, so
this needs no restructuring of the existing `$(OBJ_DIR)/%.o: %.c` rule.

`-O2` rather than `-O3` is the recommendation: most of the float-loop win at
roughly half the size cost. **Confirm on the bench** (§14) — if `-O3` is
materially better on target, take it; +25 KB is affordable.

### Lint compatibility — verified

`make lint` compiles every file in `$(SRC)` in one invocation with
`-Wall -Wextra -Werror -Wno-unused-parameter -Wno-old-style-declaration`, plus
the force-included `include/ssc338q_compat.h`. I compiled all six QR sources
under **exactly those flags** including the forced header: **all pass clean**.

No lint exclusion list, no `-Wno-` additions, and no changes to the vendored
files are required. (The stb unused-function noise is already suppressed by a
`#pragma GCC diagnostic push/pop` around the include in the wrapper TU, and that
suppression travels with whichever TU includes it.)

`make verify` already runs `qr-test-host` and `qr-test-cli`; both keep working
unchanged because the corpus and CLI keep linking the same sources.

## 11. Simplifications required before this is production-ready

Items 1–3 are blocking; 4–9 are the "simplify and streamline" pass.

### 11.1 Deadline enforcement — blocking

`decode_image()` (`tools/qr/qr_decode.c:879-954`) runs all ten passes
unconditionally. Inline, that means a shutdown or a window expiry waits for the
full cascade — 550 ms at 720p, ~2 s at 1440p. Add `deadline_us` to
`QrScanOptions` and check it at each pass boundary (nine natural check points
already exist as the `if (stats->fatal_error)` guards). Without this, the window
length is only approximately honoured and teardown stalls. **The no-downscale
rule makes this more important, not less** — the worst-case cascade is now
whatever the main stream costs.

### 11.2 Result by struct, not stdout — blocking

`decode_candidates()` writes the payload with
`fwrite(data.payload, 1, ..., stdout)` (`qr_decode.c:390`). Must write into the
caller's `QrScanResult`. The CLI then prints it from `main()` — one place
instead of the middle of the cascade.

### 11.3 Tracing: counters stay, per-stage `fprintf` goes — blocking

`struct decode_stats` gates ~8 `fprintf(stderr, ...)` sites on `stats->enabled`.
The **counters** are cheap increments and worth keeping (they feed `/qr/recent`).
The **per-stage formatting** is bench diagnostics: keep it behind the CLI's
`--stats` and the daemon's `system.verbose`, and emit one summary line per
window rather than per pass. Removes roughly 60 lines from the hot path.

### 11.4 Collapse the `raw` parameter thread

`raw` is threaded as a bare `int` through eight functions. It becomes one field
in `QrScanOptions`, which the cascade already needs for the deadline.

### 11.5 Kill the per-region `snprintf` stage strings

`decode_region`/`decode_full`/`decode_tiles`/`decode_bounded_refinement` build
stage strings with `snprintf` per call (`qr_decode.c:606, 754, 775, 805`). In the
tile passes that is nine `snprintf`s per image pass, existing purely to label a
trace line that is off by default. Replace with static literals + a small enum.

### 11.6 Scratch arena instead of per-pass malloc

`box_blur3`, `downscale2`, `lens_correct_radial`, and `decode_inverted` each
malloc *and free* a full-frame buffer per invocation; `lens_correct_radial` also
mallocs its `x_terms` table every call (`qr_decode.c:701`). That is ~5 full-frame
allocations per decoded frame.

Two reusable `W×H` scratch buffers owned by `QrScanCtx`, sized on first use and
freed in `qr_scan_free()`, cover every pass. Peak drops from ~5×W×H to ~4×W×H and
becomes deterministic — which matters more now that W×H is the full main stream.

### 11.7 Fix the `decode_image` free-ladder

`decode_image()` frees `blur` in **nine separate places**
(`qr_decode.c:879-954`) — a leak/double-free surface that only survives because
the function is short-lived in a one-shot process. In a daemon it runs thousands
of times. With the arena (11.6) there is nothing to free and the function
collapses to a linear sequence of guarded passes.

### 11.8 Pixel-count ceiling replaces `MAX_INPUT_DIM`

See §6. Refuse-and-log above `QR_SCAN_MAX_PIXELS`; never resize. The CLI keeps
its own dimension check for arbitrary files.

### 11.9 Drop the PGM path from the daemon

`pgm_read_uint` / `pgm_from_mem` / `slurp` / `image_load`
(`qr_decode.c:171-341`) exist for bench corpora and stdin. They stay in the CLI
and never enter waybeam — the daemon gets luma from
`stbi_load_from_memory(..., 1)` on the JPEG the MJPEG channel just produced.
~170 lines that simply do not move.

## 12. Lifecycle and safety

**Thread.** One worker, created lazily on the first scan (manual or boot) and
parked on a condvar between windows. Restarting a window is a
`pthread_cond_signal` with a new deadline.

**Priority — nothing to do.** The encoder thread runs elevated `SCHED_FIFO`
(`src/star6e_runtime.c:1698`), audio and IMU at `SCHED_FIFO` 1. A default
`SCHED_OTHER` scan thread is already outranked by everything that matters.

**Capture safety.** The scanner calls `venc_jpeg_capture()`, the same entry point
`/api/v1/snapshot.jpg` uses, serialized under `g_jpeg_mutex`. A user hitting
`/snapshot.jpg` mid-scan simply queues. No new SDK surface, no new port, no new
teardown race.

**Teardown.** `venc_jpeg_shutdown()` clears `g_initialized` under the same mutex,
so a scan capture racing pipeline teardown gets a clean `-ENODEV` rather than
touching a dead channel. The thread should still be signalled to stop and joined
before the pipeline tears down, so the deadline check in 11.1 is what makes that
join prompt. The quality restore (§6) must run on that path too.

**CPU during a window.** With `qr.intervalMs` at its 500 ms floor and decodes
that now run 0.5–2 s depending on resolution, a window runs near 100% of one
core. The Star6E bench measures the whole system at 68% of a 200% budget
(`documentation/STAR6E_CPU_PROFILE.md`), so a window lands around 168/200 —
bounded, and `qr.intervalMs` is the knob to back it off. **Confirm on-device
(§14), do not assume.**

## 13. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Quality override leaks past the window | **high** | single restore point, re-read from config, restore on teardown path; explicitly tested |
| Cascade forks between tool and daemon | high | single `src/qr_scan.c` linked by both; corpus keeps testing shipped code |
| Heap at high-resolution modes (14.7 MB at 1440p) | **high** — raised by the no-downscale rule | **unmeasured**; experiments D and G before merge; `QR_SCAN_MAX_PIXELS` refusal as the backstop |
| >8-pack JPEG silently truncated (§6) | med | return `-EIO` instead of truncating; more likely at q90 |
| `dstFps` 20 raises idle CPU | med | experiment C; static value, back off if it costs |
| Boot scan delays OSD attach | med | only affects `debug.showOsd` users; bounded by `qr.bootWindowMs` |
| stb_image fault takes down video | med | input is our own VENC JPEG, not network data; ceiling in 11.8; optionally fuzz `stbi_load_from_memory` on host |
| Scan window starves the encoder | med | `SCHED_OTHER` vs the encoder's `SCHED_FIFO`; `qr.intervalMs`; on-device confirmation |
| Removing `snapshot.width`/`height` breaks a deployed config | low | confirm non-functional first (experiment A); keep parsing and ignore, or remove with a `HISTORY.md` note |
| `-Os` silently halves scan throughput | low | per-object `-O2` override (§10), verified on bench |

## 14. Work breakdown and verification

| # | Step | Verify |
|---|---|---|
| 1 | Extract cascade → `src/qr_scan.c` + `include/qr_scan.h`; reduce `qr_decode.c` to a CLI | `make qr-test-host`, `make qr-test-cli`, `make qr-test-extended` — **counts must be identical** (768/768 frames, 709/768 framed decode) |
| 2 | Simplifications 11.1–11.9 | same three suites, same counts; this is a refactor, not a tuning change |
| 3 | Config section `qr.*` across all five layers | `make test` (`test_save_layout_byte_equal`), `make verify` |
| 4 | `src/venc_qr.c` state machine, capture policy, two endpoints | `make lint`, `make verify` (both backends) |
| 5 | Boot autoscan + OSD ordering (§7) | `make verify`; on-device boot with `debug.showOsd` on and off |
| 6 | `MAX_PACKS` truncation → `-EIO`; `dstFps` 5 → 20 (both backends) | `make verify`; exercise at q90 on device |
| 7 | Makefile wiring + per-object `-O2`; measure `-Os`/`-O2`/`-O3` on target | binary size delta; decode time on the saved hard 720p capture |

Device experiments, once the above builds:

| # | Experiment | Why |
|---|---|---|
| A | Set `snapshot.width`/`height` ≠ main stream; check the returned JPEG's actual dimensions | settles whether those fields do anything (§6). Drives keep-vs-delete |
| B | Sweep MJPEG quality 80 → 90 → 95: JPEG bytes, stb decode ms, decode success on the phone fixture | validates `QR_SCAN_QUALITY 90` instead of assuming it |
| C | `dstFps` 5 → 20: idle CPU and per-capture wait | is the ~200 ms per-attempt wait cheap to remove? |
| D | `/proc/meminfo` before/during/after a window at each supported main-stream resolution | the no-downscale heap risk — **gating** |
| E | `scripts/waybeam_thread_watch.sh` across a full window while streaming | confirms the ~168/200 CPU projection and that the encoder does not drop frames |
| F | Live scan against the phone fixture (`tools/qr/test-images/phone.html`) | end-to-end: `/qr/scan` → `/qr/recent` returns `found` |
| G | Full-cascade miss timing at 1080p and 1440p | §4's attempt-count table is projected from 720p; confirm or revise `qr.windowMs` |
| H | Maruko: snapshot with `debug.showOsd` on — is the OSD present? | confirms the SCL port0-vs-port1 reasoning in §7 |
| I | Star6E: are frames flowing at `star6e_pipeline.c:2161`? | decides whether the boot scan can run inside `configure()` or needs a post-bring-up hook |
| J | *(follow-up only)* `show = 0` re-attach on a live pipeline, repeated | gates `debug_osd_set_visible()`; the RGN detach hazard is documented and real |

Then `scripts/star6e_direct_deploy.sh cycle`, `HTTP_API_CONTRACT.md` 0.17.0,
`tools/qr/README.md`, `VERSION`, `HISTORY.md`, `make pre-pr`.

Steps 1 and 2 are independently verifiable against the existing corpus and should
land as their own commit before any daemon code exists. That keeps "did the
refactor change recognition behaviour" separate from "does the daemon integration
work" — per the Scope Control rule in AGENTS.md.

## 15. Deliberately out of scope

- **Payload interpretation.** `P`/`C` transport types stay opaque. Pairing,
  commands, and action dispatch remain outside the binary, exactly as
  `tools/qr/README.md` states today.
- **`debug_osd_set_visible()`.** Wanted for runtime scans with the OSD on (§7),
  but gated on experiment J. Follow-up change.
- **Hardware downscale on Maruko.** SCL port 1 is dedicated and *could* be
  programmed independently of the main stream. Now moot for QR — we never
  downscale — but it would let `snapshot.width`/`height` actually mean something
  for non-QR snapshot consumers, if anyone wants that.

## 16. Documentation drift found during this review

- `tools/qr/README.md` §"Size and Star6E performance" states the standalone
  target is built `-Os` and calls the `-O3` build "earlier". This inverted in
  0.60.0: `Makefile:252` is `QR_OPT_CFLAGS := -O3`, and `HISTORY.md` records the
  switch. The stated 30,288-byte size is the `-Os` figure and no longer describes
  what `make qr-decode` produces.
- `tools/qr/README.md:83-88` and `HTTP_API_CONTRACT.md:857` both describe
  `snapshot.width`/`snapshot.height` as sizing the capture, which the code does
  not appear to support (§6). Pending experiment A.
