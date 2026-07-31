# Inlining QR detection into `waybeam` — review and plan

Status: **Phase 1 (spec)**. No code has been written. This document answers
"should we inline the QR decoder, and what would it take", and specifies the
simplifications the decoder needs before it runs inside the encoder daemon.

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

Measured cost of inlining: **~30 KB of text** and **~4×W×H of transient heap
during a scan window only**. Both are affordable. See §4.

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
MJPEG VENC channel (channel 7) created once at pipeline start and pulse-encoded
per request (`src/venc_jpeg.c`, `venc_jpeg_capture()`), serialized under one
mutex. The per-request VPE/SCL tap that could wedge the SoC was retired in
0.60.0 — nothing in this plan brings it back.

## 3. Why inline — and what it does *not* buy

### It does not buy meaningful speed

The plumbing that inlining removes (HTTP round trip, `/tmp` write, `fork`+`exec`)
is roughly 10–20 ms per frame. The decode itself is 400 ms–1.3 s. **Performance
is not the argument.** Anyone who claims a 2× scan-rate win from inlining is
measuring the wrong thing.

### What it does buy

1. **The requested UX is inherently a daemon feature.** A "restart a 10–15 s
   window, then read the result" API is a state machine with a deadline. There
   is nowhere to put that except in a long-lived process. Doing it in shell
   means a supervisor, a PID file, and a way to report status — reimplementing
   in `ash` what waybeam already has.
2. **Removes a target-image dependency.** `qr_watch.sh` uses `curl`
   (`tools/qr/qr_watch.sh:191`). OpenIPC images ship busybox `wget`; `curl` is
   not guaranteed. Inlining removes curl, the temp file, and the shell entirely
   from the production path.
3. **Any client can drive it.** WebUI button, hub, phone app — all get it for
   free over the existing HTTP API. Today it requires an SSH session.
4. **Lifecycle correctness.** The scan window cannot outlive the pipeline,
   cannot run while the pipeline is down, and cannot be left running by a
   forgotten `ctrl-c`. The daemon owns and bounds it.

### What it costs

- **+~30 KB text.** Measured (§4).
- **A parser in the encoder's address space.** stb_image is a large JPEG
  parser; a fault in it now takes down video. Mitigating factor: the JPEG is
  produced by our own VENC channel, not received from the network, so this is
  not an untrusted-input surface. Risk accepted, noted in §11.
- **CPU contention during the window.** Bounded and mitigable (§10).

### The alternative considered and rejected

Ship `qr_decode` + a C supervisor as a second binary. This keeps the crash
domain separate but requires a second REST surface (or a control socket) to
implement scan/recent, duplicating waybeam's HTTP plumbing for one feature.
Not worth it for a decoder consuming our own JPEG.

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
at `-Os` was **30,288 bytes** (`tools/qr/README.md`), which includes quirc, stb,
the cascade, *and* CRT startup. **Budget ~30 KB at `-Os`, ~55 KB at `-O3`.**

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
The QR translation units need a per-object optimization override (§8).

### Memory

Peak transient heap during one decode, with the arena change in §9.6:

```
input luma (W×H) + quirc image (W×H) + 2 reusable scratch (2×W×H)  ≈ 4×W×H
  720p  ≈ 3.7 MB
  1080p ≈ 8.3 MB
```

Without the arena change the current code peaks at ~5×W×H and does ~5
malloc/free of full-frame buffers *per decoded frame* (~100 per window). All are
well above `MMAP_THRESHOLD`, so they go straight to `mmap`/`munmap` — no heap
fragmentation, but 100 round trips to the kernel per window for no reason.

**Open item:** free RAM on the SSC338Q bench has not been measured for this
plan. Verify before enabling 1080p scanning by default (§12).

### Resolution and the scan budget

Scan geometry follows `snapshot.width`/`snapshot.height` (0 = inherit main
stream). Cost scales with pixel count, so at the 1080p default a 12 s window
yields roughly **10 attempts**, versus ~24 at 720p. QR *range*, however, scales
with pixels per module. This is a real operator trade-off and should be
documented rather than papered over with a second config knob — `snapshot.width`
/`snapshot.height` stay the single lever.

## 5. Proposed API surface

Contract bump: **0.17.0** (additive, non-breaking).

### `GET /api/v1/qr/scan`

Starts a scan window. Calling it while a window is running **restarts** the
deadline rather than queueing or erroring — this is the "restart" semantic the
feature was asked for.

```json
{"ok":true,"data":{"scanning":true,"window_ms":12000}}
```

- `503` `qr_disabled` — `qr.enabled` false, or `snapshot.enabled` false /
  pipeline not up (the scanner cannot capture without the MJPEG channel).

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

`state` is one of:

| state | meaning |
|---|---|
| `idle` | no scan requested since process start |
| `scanning` | a window is open |
| `found` | the last window decoded a valid envelope |
| `not_found` | the last window expired without a valid envelope |

`payload` / `payload_age_s` are **sticky**: they survive a later `not_found`
window so a client can still read the last good code. `state` alone answers
"did the window succeed". `stage` names the cascade pass that hit — genuinely
useful for tuning marker placement and focus, and free to carry.

Scan state is process-local: a `/api/v1/restart`, SIGHUP reinit, or respawn
resets it to `idle`. Documented, not worked around.

## 6. Proposed config surface

Three fields, one new section. Per the **Config / WebUI / API Sync Rules** each
touches five layers, so the count matters — this is the minimum that is
genuinely operational.

| JSON (camelCase) | API (`section.snake_case`) | Type | Default | Mutability | Why |
|---|---|---|---|---|---|
| `qr.enabled` | `qr.enabled` | bool | `true` | live | hard gate; the path is inert until `/qr/scan` is called, but a locked-down build needs an off switch |
| `qr.windowMs` | `qr.window_ms` | uint | `12000` | live | the 10–15 s timer; clamp 1000–60000 |
| `qr.intervalMs` | `qr.interval_ms` | uint | `500` | live | minimum ms between capture starts — the CPU duty-cycle knob; clamp 100–5000, mirroring `qr_watch.sh`'s existing 0.5 s floor |

All three are read once at window start, so all three are `MUT_LIVE` — no
restart-required plumbing.

Layers to update (all in the same PR, per AGENTS.md):
`VencConfig` + `venc_config_defaults()` → `load_qr()` + `venc_config_to_json()`
→ `render_qr()` in the hand-rolled printer → `g_fields[]`/`g_aliases[]` →
`config/waybeam.default.json`. UI comes free via `FIELD_UI` descriptors (the
data-driven path, as `snapshot.*` already does) — **no `SECTIONS[]` edit and no
`make webui` rebuild.**

`test_save_layout_byte_equal` enforces printer/default-file byte equality; the
new section must be added to both or it fails.

## 7. File and module layout

```
include/qr_scan.h      NEW  cascade API: context, options, result
src/qr_scan.c          NEW  the cascade, moved verbatim-ish from qr_decode.c (~370 lines)
include/venc_qr.h      NEW  scan-window state machine + HTTP handlers
src/venc_qr.c          NEW  worker thread, deadline, state, /qr/scan + /qr/recent
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

/* 0 = decoded, 1 = no decode (incl. deadline expiry), -1 = fatal (alloc). */
int qr_scan_gray(QrScanCtx *ctx, const uint8_t *gray, int w, int h,
	const QrScanOptions *opt, QrScanResult *out);
```

The daemon owns one `QrScanCtx` for the lifetime of the scan thread; quirc
already retains its high-water image/flood-fill allocations across resizes, so
reuse across a window is nearly allocation-free after the first frame.

## 8. Makefile changes

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

`-O2` rather than `-O3` is the recommendation: it captures most of the
float-loop win at roughly half the size cost. **Confirm on the bench** (§12) —
if `-O3` is materially better on target, take it; +25 KB is affordable.

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

## 9. Simplifications required before this is production-ready

The decoder is bench-grade today. These are the changes it needs, in priority
order. Items 1–3 are blocking; 4–9 are the "simplify and streamline" pass.

### 9.1 Deadline enforcement — blocking

`decode_image()` (`tools/qr/qr_decode.c:879-954`) runs all ten passes
unconditionally. Inline, that means a shutdown or a window expiry waits for the
full cascade — 550 ms at 720p, over 1 s at 1080p. Add `deadline_us` to
`QrScanOptions` and check it at each pass boundary (nine natural check points
already exist as the `if (stats->fatal_error)` guards). Without this, the
window length is only approximately honoured and teardown stalls.

### 9.2 Result by struct, not stdout — blocking

`decode_candidates()` writes the payload with
`fwrite(data.payload, 1, ..., stdout)` (`qr_decode.c:390`). Must write into the
caller's `QrScanResult`. The CLI then prints it from `main()` — one place
instead of the middle of the cascade.

### 9.3 Tracing: counters stay, per-stage `fprintf` goes — blocking

`struct decode_stats` gates ~8 `fprintf(stderr, ...)` sites on `stats->enabled`.
The **counters** are cheap increments and worth keeping (they feed `/qr/recent`).
The **per-stage formatting** is bench diagnostics and should not be in a daemon
by default: keep it behind the CLI's `--stats` and the daemon's
`system.verbose`, and emit one summary line per window rather than per pass.
Removes roughly 60 lines of formatting from the hot path.

### 9.4 Collapse the `raw` parameter thread

`raw` is threaded as a bare `int` through eight functions. It becomes one field
in `QrScanOptions`, which the cascade already needs to pass around for the
deadline. Net: eight signatures get shorter.

### 9.5 Kill the per-region `snprintf` stage strings

`decode_region`/`decode_full`/`decode_tiles`/`decode_bounded_refinement` build
stage strings with `snprintf` per call (`qr_decode.c:606, 754, 775, 805`). In
the tile passes that is nine `snprintf`s per image pass, existing purely to
label a trace line that is off by default. Replace with static string literals
selected by a small enum.

### 9.6 Scratch arena instead of per-pass malloc

`box_blur3`, `downscale2`, `lens_correct_radial`, and `decode_inverted` each
malloc *and free* a full-frame buffer per invocation; `lens_correct_radial` also
mallocs its `x_terms` table every call (`qr_decode.c:701`). That is ~5
full-frame allocations per decoded frame, ~100 per window.

Two reusable `W×H` scratch buffers owned by `QrScanCtx`, sized on first use and
freed in `qr_scan_free()`, cover every pass (no two live full-frame temporaries
are needed simultaneously once the free-ladder below is fixed). Peak drops from
~5×W×H to ~4×W×H and becomes deterministic.

### 9.7 Fix the `decode_image` free-ladder

`decode_image()` frees `blur` in **nine separate places**
(`qr_decode.c:879-954`) — a textbook leak/double-free surface that only survives
because the function is short-lived in a one-shot process. In a daemon it runs
thousands of times. With the arena (9.6) there is nothing to free and the
function collapses to a linear sequence of guarded passes.

### 9.8 Bound input geometry against config, not `MAX_INPUT_DIM`

`MAX_INPUT_DIM 4096` (`qr_decode.c:58`) is the right guard for a CLI reading
arbitrary files. Inline, geometry comes from our own snapshot config, so the
scanner should refuse anything above an explicit pixel budget — a mis-set
`snapshot.width` should fail the scan with a logged reason, not attempt a
multi-hundred-megabyte allocation inside the encoder.

### 9.9 Drop the PGM path from the daemon

`pgm_read_uint` / `pgm_from_mem` / `slurp` / `image_load`
(`qr_decode.c:171-341`) exist for bench corpora and stdin. They stay in the CLI
and never enter waybeam — the daemon gets luma from
`stbi_load_from_memory(..., 1)` on the JPEG the MJPEG channel just produced.
~170 lines that simply do not move.

## 10. Lifecycle and safety

**Thread.** One worker, created lazily on the first `/qr/scan` and parked on a
condvar between windows (not spawned per window). Restarting a window is a
`pthread_cond_signal` with a new deadline.

**Priority — nothing to do.** The encoder thread runs elevated `SCHED_FIFO`
(`src/star6e_runtime.c:1698`), audio and IMU at `SCHED_FIFO` 1. A default
`SCHED_OTHER` scan thread is already outranked by everything that matters. No
scheduling code needed; this is a point in favour of the design.

**Capture safety.** The scanner calls `venc_jpeg_capture()`, the same entry
point `/api/v1/snapshot.jpg` uses, serialized under `g_jpeg_mutex`. A user
hitting `/snapshot.jpg` mid-scan simply queues. No new SDK surface, no new port,
no new teardown race.

**Teardown.** `venc_jpeg_shutdown()` clears `g_initialized` under the same
mutex, so a scan capture racing pipeline teardown gets a clean `-ENODEV` rather
than touching a dead channel — the scanner is safe by construction. The thread
should still be signalled to stop and joined before the pipeline tears down, so
the deadline check in 9.1 is what makes that join prompt.

**CPU during a window.** With `qr.intervalMs` at its 500 ms floor and ~425 ms
decodes, a window runs near 100% of one core. The Star6E bench measures the
whole system at 68% of a 200% budget (`documentation/STAR6E_CPU_PROFILE.md`),
so a window lands around 168/200 — tight but bounded, and `qr.intervalMs` is the
knob to back it off. **Must be confirmed on-device (§12), not assumed.**

## 11. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| stb_image fault takes down video | med | input is our own VENC JPEG, not network data; geometry bounded (9.8); optionally fuzz `stbi_load_from_memory` on host |
| Scan window starves the encoder | med | `SCHED_OTHER` vs the encoder's `SCHED_FIFO`; `qr.intervalMs`; on-device confirmation required |
| 1080p heap spike (~8.3 MB) on a low-RAM board | med | **unmeasured** — verify free RAM before defaulting; document 720p as the recommended scan geometry |
| Cascade forks between tool and daemon | high | single `src/qr_scan.c` linked by both; corpus keeps testing the shipped code |
| `-Os` silently halves scan throughput | low | per-object `-O2` override (§8), verified on bench |

## 12. Work breakdown and verification

| # | Step | Verify |
|---|---|---|
| 1 | Extract cascade → `src/qr_scan.c` + `include/qr_scan.h`; reduce `qr_decode.c` to a CLI | `make qr-test-host`, `make qr-test-cli`, `make qr-test-extended` — **counts must be identical** (768/768 frames, 709/768 framed decode) |
| 2 | Simplifications 9.1–9.9 | same three suites, same counts; this is a refactor, not a tuning change |
| 3 | Config section `qr.*` across all five layers | `make test` (`test_save_layout_byte_equal`), `make verify` |
| 4 | `src/venc_qr.c` state machine + two endpoints | `make lint`, `make verify` (both backends) |
| 5 | Makefile wiring + per-object `-O2`; measure `-Os`/`-O2`/`-O3` on target | binary size delta; decode time on the saved hard 720p capture |
| 6 | Device validation | `scripts/star6e_direct_deploy.sh cycle`, then a live scan against the phone fixture; capture window CPU with `scripts/waybeam_thread_watch.sh` |
| 7 | Free-RAM check at 720p and 1080p scan geometry | `/proc/meminfo` before/during/after a window |
| 8 | `HTTP_API_CONTRACT.md` 0.17.0, `tools/qr/README.md`, `VERSION`, `HISTORY.md` | `make pre-pr` |

Steps 1 and 2 are independently verifiable against the existing corpus and
should land as their own commit before any daemon code exists. That keeps the
"did the refactor change recognition behaviour" question separate from "does the
daemon integration work" — per the Scope Control rule in AGENTS.md.

## 13. Documentation drift found during this review

Not part of the integration, but found while measuring and worth fixing:

- `tools/qr/README.md` §"Size and Star6E performance" states the standalone
  target is built `-Os` and calls the `-O3` build "earlier". This inverted in
  0.60.0: `Makefile:252` is `QR_OPT_CFLAGS := -O3`, and `HISTORY.md` records the
  switch. The README's stated 30,288-byte size is the `-Os` figure and no longer
  describes what `make qr-decode` produces.
