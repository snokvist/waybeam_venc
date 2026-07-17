# Star6E NPU detection sidecar — requirements

Status: **Design. Not implemented. Phase-1 gate — no code until this + plan.md
are signed off.**
Date: 2026-07-17
Device class: Star6E / SSC338Q (Infinity6E), which carries the SigmaStar IPU
(~1 TOPS int8) exposed by `libmi_ipu.so`.

## Goal

Run on-device neural networks on the idle Star6E IPU (first target: YOLOv8n
object detection) and expose their results two ways:

1. A **generic on-device debug OSD** overlay (boxes / ticks / stat rows) — a
   preview/debug aid, deliberately model-agnostic.
2. A **model-agnostic payload on the existing RTP sidecar**, carried off-box to
   `waybeam-hub`, which owns the real OSD and downstream parsing/logic.

The design must let *different* models with *different* output parameters ship
**without changing the on-device OSD code or the sidecar wire transport**. Only a
model's own decoder module (and its private schema/config) changes per model.
v1 ships one model but must already have every multi-model seam in place.

## What already exists in-tree (leverage, don't rebuild)

| Capability | Where | Note |
|---|---|---|
| IPU dlopen vtable | `sdk/ssc338q/include/i6_ipu.h` | `i6_ipu_load()` binds `MI_IPU_CreateDevice/CreateCHN/GetInOutTensorDesc/PutInput/PutOutputTensors/Invoke`. Unused today. |
| Tensor dequant metadata | `i6_ipu_tend` (`i6_ipu.h:37-46`) | `format`, `shape[8]`, `scalar`, `zeroPnt`, `alignedBufSize`. |
| NV12 IPU input | `I6_IPU_FMT_NV12` (`i6_ipu.h:16`) | IPU eats NV12 directly; no colour convert. |
| Model-load callback | `i6_ipu_rdfn` (`i6_ipu.h:10`) | Reads model bytes from an arbitrary source → the hook for optional decrypt. |
| Zero-copy frame tap | `src/star6e_framing_stab.c` (`MI_SYS_ChnOutputPortGetBuf`, `stFrameData.phyAddr[0]`) | Proven: pull NV12 off a VPE port, get virt+phys. |
| Generic OSD primitives | `include/debug_osd.h` | `debug_osd_rect/line/point/text` — a universal draw vocabulary. |
| **Flag-gated sidecar trailers** | `include/rtp_sidecar.h` (`RTP_SIDECAR_FLAG_ATTITUDE`, `rtp_sidecar_send_frame_full`) | **The model for this feature.** One subscribe, one packet per frame, optional trailers appended by flag; unknown flags → trailing bytes ignored. |
| Compile-gating precedent | `Makefile` `STAB` (`:47-52`, `:76-96`) | Source dropped + `-DHAVE_*` unset in default builds. |

## Hard constraints

- **Detection rides the existing RTP sidecar as one more trailer.** waybeam-hub
  already subscribes once and reads the per-frame sidecar packet for ATTITUDE;
  detection is a new `RTP_SIDECAR_FLAG_DETECT` trailer on that same packet. One
  subscribe, **one read**, one packet per encoded frame, auto-correlated by
  `frame_id`. No second socket, no second subscription.
- **One datagram per frame** (already true of the sidecar). The detection trailer
  is variable-length and appended **last**; if a crowded frame would exceed the
  ~1400 B MTU, drop lowest-score objects and set a `TRUNCATED` bit. No
  fragmentation until/unless it actually bites.
- **The transport hardcodes nothing model-specific** — not class, not bbox, not
  distance. It carries `model_id` + `schema_ver` + an **opaque TLV blob**. All
  field meanings live in the model's private schema.
- **No new external dependencies** (`AGENTS.md`). Any crypto is a single vendored
  public-domain `.c` under `sdk/ssc338q/lib/` (cf. `miniz`/`tinysvcmdns`/`shine`).
- **Star6E first** (`AGENTS.md`). Maruko (`i6c_ipu.h`) is a deferred follow-up.
- **Same engineering quality bar**: spec→draft→simplify→verify, lint/verify
  clean, config-sync honoured for persisted fields, `VERSION` + `HISTORY.md`.

## Documentation & distribution posture ("subtly less documented")

- **Public repo:** the C code, a terse minimal `detect.*` config section, a
  minimal `HISTORY.md` line + `HTTP_API_CONTRACT.md` entry, and the public
  `model_id → name` registry. Generic field names. **No** WebUI `SECTIONS[]` and
  **no** `FIELD_UI` metadata → usable via config/API, not advertised in the
  dashboard.
- **Private repo:** models (`.img`), ONNX→`SGS_IPU_SDK` conversion notes,
  calibration data, **per-model TLV schemas**, **per-model config/manifests**,
  distance-calibration guides, examples, keys.

## Design decisions (locked — do not oscillate mid-build)

- **D1 — In-process module, not a separate program.** Frames are already DMA
  buffers in the venc address space; the IPU is a dlopen'd lib. Inference runs
  inside waybeam_venc on its own low-priority thread. The only separate program
  is the *consumer* (waybeam-hub) parsing the sidecar.
- **D2 — Dedicated inference tap.** A new VPE scaler output port at the active
  model's input size, independent of the framing port0→VENC path, so
  stabilization and detection coexist. Decimated (`detect.infer_interval`).
- **D3 — Two generic outputs, neither transport- nor OSD-hardcoded:**
  - `OverlayList` (rect/line/point/text primitives) → generic `debug_osd`
    renderer. The model's decoder produces the list; the renderer never learns
    class semantics.
  - Opaque **TLV blob** → appended to the RTP sidecar as the DETECT trailer.
  The model's decoder is the *only* place that knows the model's semantics.
- **D4 — Optional model encryption.** Vendored ChaCha20-Poly1305 (software; A7
  has no AES accel). Operator supplies the key (`detect.key_path` or env). No
  key + plaintext blob → runs. Encrypted blob + missing/wrong key → dormant,
  pipeline unaffected. Single shared key in v1; per-device binding deferred.
- **D5 — Detection is an RTP-sidecar trailer**, modelled exactly on ATTITUDE:
  new `RTP_SIDECAR_FLAG_DETECT`, appended **after** the attitude trailer (last
  in flag order, because it is the only variable-length trailer — fixed trailers
  keep computable offsets). The sidecar carries the blob opaquely; it gains no
  model knowledge. Attach-on-fresh: the flag is set only on frames where a new
  inference result is ready since the last send (carrying a `detect_seq` so hub
  can tell fresh from unchanged).
- **D6 — Self-describing payload, frozen transport.** The DETECT trailer is a
  small fixed header (`model_id`, `schema_ver`, `object_count`, `flags`,
  `detect_seq`, `payload_len`) + a TLV body (`[tag u8][len u8][value…]`, network
  byte order, objects delimited by an object-boundary tag). Consumers skip
  unknown tags by `len` — the same rule the sidecar already uses for unknown
  flags. `model_id` keys the private schema that assigns tag meanings. An in-band
  `DESCRIPTOR` handshake is a **reserved future extension**, not v1.
- **D7 — model_id registry public, schemas private.** Tiny `id→name` table in
  the public repo prevents collisions; all field meanings live in private
  per-model schemas.
- **D8 — No privileged outputs.** Distance-from-size, keypoints, action stats,
  etc. are *examples* a model may emit — each is just a model-defined TLV tag and
  a model-produced OSD primitive. The transport and the OSD renderer hardcode
  none of them. (Reference math for a size→distance model lives in the private
  schema/decoder: `distance = focal_px · real_H / bbox_h`.)
- **D9 — Multi-model in v1 (seams, not fleet).** A "model" = a compiled-in
  decoder (dispatched by `model_id`) + its `.img` blob + its per-model config
  (input dims, thresholds, class/label table, tag schema, any distance
  constants). Per-model config is **hardcoded in the decoder or read from a
  separate per-model config file** — **not** in `waybeam.json`. The main config
  only selects the active model + paths. v1 implements one decoder (YOLOv8);
  adding another is purely additive — a new decoder file + `model_id` + private
  blob/config, with **zero** changes to transport, OSD, or `waybeam.json` schema.

## Out of scope (explicit)

- Maruko / i6c IPU port.
- Multi-packet / fragmented sidecar frames (see hard constraint).
- In-band `DESCRIPTOR` schema negotiation (reserved, future).
- Per-device key binding (reserved, future).
- Running multiple models *concurrently* on the NPU — v1 has one active model at
  a time (the seams allow more, the schedule doesn't force it).

## Success criteria

1. Default build (`DETECT=0`) is free of the feature — no symbols/strings,
   `make verify` green.
2. `DETECT=1` build loads a YOLOv8n `.img` (plaintext or decrypted), runs
   `MI_IPU_Invoke` on a live VPE tap, emits correct boxes.
3. waybeam-hub, with its **existing single subscription**, receives the DETECT
   trailer on the same per-frame packet as ATTITUDE, parses the TLV via the
   model's `model_id`, and correlates by `frame_id` — no new socket.
4. Debug OSD draws boxes + a stat row from the same detection snapshot, with
   correct coordinate mapping when a framing crop is active.
5. A second (stub) model_id + decoder can be added touching only its decoder
   file + the registry — proving transport/OSD/config are model-agnostic.
6. Encode path FPS/bitrate unchanged vs `DETECT=0`.
