# QR scanning backend

`qr_decode` is a freestanding consumer of waybeam's grayscale snapshot
endpoint. It reads a binary P5 PGM, identifies and decodes a QR symbol, checks
the minimal Waybeam transport envelope, and prints the 16-character payload.
It does not interpret, authorize, persist, or execute that payload.

The waybeam process remains responsible only for
`GET /api/v1/snapshot.pgm`. Pairing, commands, boot scheduling, and service
integration belong to a later standalone shell/action work package.

## Pieces

| File | Role |
|---|---|
| `qr_decode.c` | Freestanding PGM decoder and scan-pass orchestration. |
| `waybeam_qr_format.c` | Minimal Version-1/Q transport-envelope validation. |
| `quirc/` | Vendored quirc library plus required outer-frame identification. |
| `qr_watch.sh` | Standalone polling helper; prints valid envelopes and performs no action. |
| `tests/test_qr_marker.c` | Deterministic host perspective corpus and envelope tests. |

## Minimal format

The default decoder accepts only the locked transport envelope:

```text
PXXXXXXXXXXXXXXX
```

- QR Version 1: 21 × 21 modules
- error correction Q
- QR alphanumeric mode
- exactly 16 uppercase QR-alphanumeric characters
- leading type `P` or `C`
- remaining 15 characters are opaque to the decoder

The example uses `P`; `CXXXXXXXXXXXXXXX` is the equivalent second transport
type.

The accepted alphabet is:

```text
0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:
```

`P` and `C` reserve transport types for future pairing/passphrase and
command/settings logic. Their meanings and the structure of the remaining
characters are intentionally not implemented here.

The outer frame is mandatory for discovery. A standards-compliant but unframed
QR is deliberately ignored, which avoids global finder-pattern scans and makes
the transport marker unambiguous. Use `--raw` for bench diagnostics when any
decodable payload inside that frame should be printed. Standard finder
localization is used only as refinement inside a tight ROI projected from an
accepted frame; the deterministic corpus also retains it as a comparison
baseline.

## Continuous outer-frame profile

Every accepted QR symbol must use this marker:

```text
33 × 33 marker units total
2-module continuous black outer frame
4-module white quiet zone inside the frame
21 × 21 Version-1 QR at marker coordinate (6, 6)
```

The full connected border supplies four stable projective corners. The
modified quirc path maps those corners directly into the existing QR grid; it
does not allocate a rectified image or add another camera pipeline stage.
Framed grids use bounded 3 × 3 majority sampling in the original source image.
Stock quirc behavior is unchanged unless a caller enables marker mode.

Render every module with integer scaling and no interpolation. The compressed
edge still needs real sensor resolution: use roughly 3 px/module as a lab
minimum and 4 px/module as a real-camera target.

## Capture robustness

The scan sequence is ordered by measured cost and likelihood:

1. sharp full frame;
2. lightly denoised full frame;
3. sharp half-scale frame;
4. radial-corrected sharp and lightly denoised full frames;
5. lightly denoised half-scale frame;
6. sharp and lightly denoised overlapping tiles;
7. one inverted full-frame pass.

Each candidate is retried with quirc's mirror flip. No extra contrast or gamma
pass is applied because quirc's thresholding already handles those cases.
The expensive nine-tile sweeps stay late, while sharp half-scale remains ahead
of lens correction because it is a measured rescue path for small optical
captures.

## Bounded-frame execution

`qr_decode` uses this cascade for each full-frame/tile/scale attempt:

1. threshold the grayscale image once;
2. find a connected continuous outer frame and validate its quadrilateral;
3. when found, map the expected 21×21 QR directly and decode it immediately;
4. when direct decoding fails, derive a tight ROI from that frame and run
   finder-pattern refinement only inside the ROI;
5. when no frame is found, move directly to the next bounded image pass without
   running finder discovery.

Unbounded finder-pattern discovery is never run by the production decoder.
Clean framed inputs stay on the cheapest direct path; curved or distorted
framed inputs pay for local refinement only after the required transport
marker has been accepted. If the normal bounded chain fails, one nearest-
neighbour radial remap with `k1=-0.30` retries corrected full-frame sharp and
light-denoised images. The corrected images still use the mandatory-frame gate;
there is no global finder fallback.

## Decode diagnostics

`--stats` keeps stdout payload-only and writes structured diagnostics to
stderr. It reports every applied image pass and region, time spent preparing,
identifying the outer frame, and decoding, the number of frame candidates, and
whether processing stopped at frame discovery, QR error correction, envelope
validation, or success. The final summary includes total/load/transform times,
region, frame, refinement, finder, lens-correction, and QR candidate counts,
mirror retries, decoded QRs, and rejected envelopes. Times are monotonic
microseconds.

```bash
qr_decode --stats capture.pgm
qr_watch.sh -c -v
```

## Build and verify

```bash
make qr-decode SOC_BUILD=star6e
make qr-decode SOC_BUILD=maruko
make qr-test-host
make qr-test-extended
```

The host corpus renders an actual Version-1/Q alphanumeric matrix into front,
rotated, mirrored, small, and projectively compressed quadrilaterals. It
reports stock and marker-assisted results and fails unless every framed case
decodes to the exact payload.

The extended series renders 768 additional 1280×720 symbols: four captured
marker widths (180–500 px), compressed-edge ratios from 1.00 down to 0.35,
three rotations, four perspective directions, and four deterministic defocus
levels. The defocus filter is a repeated separable 5-tap approximation, so the
series remains dependency-free and reproducible.

Current code-level result:

```text
outer frame identified: 768/768 (100.0%)
framed payload decoded: 709/768 (92.3%)
stock payload decoded:  506/768 (65.9%)
```

Framed decode remained 100% at edge ratios 1.00 and 0.70. It reached 77.1% at
ratio 0.35 across all sizes and blur levels, and 81.2% at the strongest
defocus level across all sizes and perspective ratios. The remaining misses
occur after successful frame identification and are concentrated where blur
and projection compress QR modules below the recoverable pixel budget.

The same 768-image series was run natively on the SSC338Q Star6E bench and
produced identical counts. Live optical validation with the phone fixture also
decoded `P23456789ABCDEFG` natively on Star6E through both
`sharp/tile/refine` and `sharp/half/refine`. The first five-frame bounded-
refinement burst decoded 2/5 captures; the remaining misses show that strong
fisheye curvature still limits frame acceptance and QR sampling at large
on-screen sizes. With the radial fallback enabled, a subsequent live
five-frame burst decoded 4/5: one through normal `blur/full/refine`, two
through `lens/full/refine`, and one through `lens-blur/full/refine`.

## Star6E performance

The production QR target builds with `-O3`, the Star6E NEON/VFPv4 flags, and
quirc's supported single-precision perspective math. GCC auto-vectorizes the
regular blur, downscale, and inversion loops; disabling vectorization was
measurably slower. Explicit NEON flags beyond the existing SoC flags and LTO
did not improve the benchmark, and the remaining projective sampling and lens
remap are irregular gathers that do not map cleanly to SIMD.

The larger wins came from avoiding work:

- outer-frame/quiet-zone geometry is scored once per symmetric frame
  candidate instead of once for all eight QR orientations;
- full-frame and half-scale passes run before the nine overlapping tiles;
- blur-half no longer delays the measured lens-correction rescue path;
- radial correction uses float arithmetic and precomputes the invariant
  per-column terms in about 10 KiB of temporary storage.

On the SSC338Q Star6E, the saved 1280×720 hard fisheye capture improved from
roughly 1.28–1.33 seconds with the original `-Os`/pass ordering to a 257 ms
mean over 30 paired final-build runs. Saved normal optical captures decoded
in 57 ms (`sharp/full/refine`) and 146 ms (`blur/full/refine`).
These are end-to-end `--stats` times including PGM load and all preceding
failed bounded passes. All three returned the exact expected envelope.

The final behavior-preserving pass changed no samples, thresholds, candidates,
or scan coverage. It uses four independent histogram counters before an exact
merge, caches repeated cell-coordinate additions, reduces each flood-fill
span to the endpoint which is algebraically maximal in the requested
direction, and rejects only quadrilaterals whose bounding-box upper area
cannot beat the current winner. quirc also retains its high-water image and
flood-fill allocations between full, half, and ROI sizes instead of repeatedly
allocating and copying them. Star6E enables compiler loop unrolling; Maruko is
left unchanged because that code-generation choice was measured only on
Star6E. Against the previous optimized build, the paired Star6E mean improved
from 291 ms to 257 ms (11.4%) with identical successful stages and payloads.

## Standalone use

```bash
curl -s http://127.0.0.1/api/v1/snapshot.pgm | qr_decode
qr_decode --raw capture.pgm
qr_decode --stats capture.pgm
```

For polling without action dispatch:

```bash
qr_watch.sh
qr_watch.sh -c
qr_watch.sh -c -v
```

`snapshot.enabled` must be true. `qr_watch.sh` reports HTTP `409` when the
scaler tap is occupied and `503` when snapshots are disabled. It exits after
the first decode by default; `-c` streams decoded envelopes continuously.
The default stress cadence starts the next capture as soon as the preceding
capture and decode return, while keeping capture starts at least 0.5 seconds
apart. `-i SECONDS` can request a slower minimum cadence; values below 0.5 are
clamped.

Deployment, init scripts, packaging, and any consumer/action hook are deferred.
