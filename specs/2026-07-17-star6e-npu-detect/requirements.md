# Star6E NPU detection sidecar — requirements

Status: **Design. Not implemented. Phase-1 gate — no code until this + plan.md
are signed off.**
Date: 2026-07-17
Device class: Star6E / SSC338Q (Infinity6E), which carries the SigmaStar IPU
(~1 TOPS int8) exposed by `libmi_ipu.so`.

## Goal

Run an on-device neural network (first target: YOLOv8n object detection, with a
monocular distance-from-object-size estimate) on the idle Star6E IPU, and expose
its results two ways:

1. A **generic on-device debug OSD** overlay (boxes / ticks / stat rows) — a
   preview/debug aid, deliberately model-agnostic.
2. A **generic UDP sidecar feed** carrying the structured results off-box to
   `waybeam-hub`, which owns the real OSD and any downstream logic/parsing.

The design must let *future* models with *different* output parameters ship
**without changing the on-device OSD code or the sidecar wire ABI**. Only a new
model's own module (and an optional private schema) should change per model.

## What already exists in-tree (leverage, don't rebuild)

| Capability | Where | Note |
|---|---|---|
| IPU dlopen vtable | `sdk/ssc338q/include/i6_ipu.h` | `i6_ipu_load()` binds `MI_IPU_CreateDevice/CreateCHN/GetInOutTensorDesc/PutInput/PutOutputTensors/Invoke`. Unused today. |
| Tensor dequant metadata | `i6_ipu_tend` (`i6_ipu.h:37-46`) | `format`, `shape[8]`, `scalar`, `zeroPnt`, `alignedBufSize` — enough to dequant INT8→FP32. |
| NV12 IPU input | `I6_IPU_FMT_NV12` (`i6_ipu.h:16`) | IPU eats NV12 directly; no colour convert needed. |
| Model-load callback | `i6_ipu_rdfn` (`i6_ipu.h:10`) | Reads model bytes from an arbitrary source → the hook for optional decrypt. |
| Zero-copy frame tap | `src/star6e_framing_stab.c` (`MI_SYS_ChnOutputPortGetBuf`, `stFrameData.phyAddr[0]`) | Proven pattern: pull NV12 off a VPE port, get virt+phys. |
| Generic OSD primitives | `include/debug_osd.h` | `debug_osd_rect/line/point/text` — already a universal draw vocabulary. |
| UDP subscribe/fanout | `include/rtp_sidecar.h`, `src/rtp_sidecar.c` | Listen, learn subscriber from packet source, fan out to ≤4 subs with TTL, flag-gated forward-compat trailers. |
| Compile-gating precedent | `Makefile` `STAB` (`:47-52`, `:76-96`) | Source dropped + `-DHAVE_*` unset in default builds. |

## Hard constraints

- **One UDP datagram per encoded frame.** Non-negotiable: preserves frame
  uniqueness and traceability (one packet ↔ one `frame_id`). No fragmentation,
  no multi-packet frames. If the ~1400-byte MTU ever truncates a crowded frame,
  the envelope carries an explicit `TRUNCATED` flag and drops lowest-score
  objects; we solve real fragmentation only if/when that actually bites.
- **No new external dependencies** (`AGENTS.md` "Mistakes to Avoid"). Any crypto
  is a single vendored public-domain `.c` under `sdk/ssc338q/lib/`, matching the
  existing `miniz` / `tinysvcmdns` / `shine` precedent.
- **Star6E first** (`AGENTS.md` backend-split policy). Maruko (`i6c_ipu.h`) is a
  deferred follow-up, not in scope here.
- **Same engineering quality bar** as any feature: spec→draft→simplify→verify,
  `make lint`/`make verify` clean, config-sync contract honoured for persisted
  fields, `VERSION` bump + one `HISTORY.md` entry.

## Documentation & distribution posture ("subtly less documented")

- **Public repo:** the C code, a terse `detect.*` config section, a minimal
  `HISTORY.md` line, and a minimal `HTTP_API_CONTRACT.md` entry. Generic field
  names (`detect.model_path`, not `yolov8_object_detection`). **No** WebUI
  `SECTIONS[]` entry and **no** `FIELD_UI` metadata → the feature is fully usable
  via config file + HTTP API but is not advertised in the dashboard.
- **Private repo:** models (`.img` blobs), the ONNX→`SGS_IPU_SDK` conversion
  notes, calibration data, per-model TLV schemas, distance-calibration guides,
  worked examples, and any keys.
- Net effect: a maintainer sees a clean, normal-quality feature; making it
  actually *do* something requires bringing your own model + (optional) key +
  know-how from the private repo.

## Design decisions (locked — do not oscillate mid-build)

- **D1 — In-process module, not a separate program.** Frames are already DMA
  buffers in the venc address space; the IPU is a dlopen'd lib. Inference runs
  inside waybeam_venc on its own low-priority thread. The only "separate
  program" is the *consumer* (waybeam-hub) parsing UDP.
- **D2 — Dedicated inference tap.** A new VPE scaler output port sized to the
  model input (e.g. 640×640 NV12), independent of the framing port0→VENC path,
  so stabilization and detection coexist. Decimated (`detect.infer_interval`),
  not per-frame.
- **D3 — Two generic outputs, neither model-aware:**
  - `OverlayList` (array of rect/line/point/text primitives) → generic
    `debug_osd` renderer.
  - `DetectEnvelope` + TLV payload → sidecar UDP.
  The post-processor is the *only* model-specific code.
- **D4 — Optional model encryption.** Vendored ChaCha20-Poly1305 (software; the
  A7 has no AES accel). Operator supplies the key (`detect.key_path` or env).
  No key + plaintext blob → runs. Encrypted blob + missing/wrong key → module
  stays dormant, pipeline unaffected. Single shared key in v1; per-device
  binding (via `src/device_id.c`) is a documented future upgrade, not v1.
- **D5 — Separate DETECT sidecar** on its own port, reusing the
  subscribe/fanout/TTL machinery but with its own magic/msg_type — keeps
  model-output data out of the RTP timing-telemetry ABI.
- **D6 — Self-describing wire, frozen transport.** Fixed `DetectEnvelope`
  (magic, version, `model_id`, `schema_ver`, `frame_id`, `capture_us`,
  `object_count`, `flags`, `payload_len`) + a TLV payload the venc side treats
  as opaque. Consumers skip unknown tags by length. `model_id` keys richer
  parsing against private schemas. A `DESCRIPTOR` message (in-band schema on
  subscribe) is a **reserved future msg_type**, not v1.
- **D7 — model_id registry public, schemas private.** Tiny `id→name` table in
  the public repo prevents collisions; field meanings live in private schemas.
- **D8 — Distance-from-size** in the post-processor:
  `distance_cm = focal_px · real_H_cm / bbox_h_px`,
  `focal_px = image_width / (2·tan(HFOV/2))`. Per-class real-size table +
  lens FOV. Carried as one TLV tag; unknown → 0.

## Out of scope (explicit)

- Maruko / i6c IPU port.
- Multi-packet / fragmented frames (see hard constraint).
- In-band `DESCRIPTOR` schema negotiation (reserved, future).
- Per-device key binding (reserved, future).
- Bounding-box overlay *rendering quality* — the on-device OSD is a debug
  preview; real rendering is waybeam-hub's job.

## Success criteria

1. Default build (`DETECT=0`) is byte-for-byte free of the feature — no symbols,
   no strings, `make verify` green.
2. `DETECT=1` build loads a YOLOv8n `.img` (plaintext or decrypted), runs
   `MI_IPU_Invoke` on a live VPE tap, and emits correct boxes.
3. One UDP datagram per encoded frame reaches a subscribed test consumer, with
   stable envelope + parseable TLV, correlatable to the RTP sidecar `frame_id`.
4. Debug OSD draws boxes + a stat row from the same detection snapshot, with
   correct coordinate mapping when a framing crop is active.
5. Encode path FPS/bitrate unchanged vs `DETECT=0` (inference is off the hot
   path).
