# Star6E NPU detection sidecar — implementation plan

Read `requirements.md` first. This plan is staged so each phase is independently
verifiable (`AGENTS.md` "Scope Control"). Phase 1 is a **go/no-go hardware gate**:
do not build the wire format or OSD path until the IPU actually invokes on device.

## Module layout

```
Makefile                         DETECT ?= 0  → src/star6e_npu.c, src/detect_*.c
                                 -DHAVE_NPU_DETECT=1 when set
sdk/ssc338q/lib/chacha20poly1305.c   vendored single-file AEAD (D4)
src/star6e_npu.c                 IPU device/chn lifecycle, VPE tap, infer thread
include/star6e_npu.h             public hooks called from star6e_pipeline.c
src/detect_model_yolov8.c        YOLOv8 decode + NMS + distance (the only
include/detect_model.h           model-specific file; dispatch by model_id)
src/detect_sidecar.c             DETECT UDP: subscribe/fanout + envelope+TLV pack
include/detect_sidecar.h         wire ABI (frozen)
src/detect_overlay.c             OverlayList → debug_osd_* renderer (model-agnostic)
include/detect_overlay.h
tests/test_detect_wire.c         host test: envelope+TLV pack/skip-unknown
tests/test_detect_decode.c       host test: YOLOv8 decode + distance math (no SDK)
```

Rationale for **not** reusing the `FramingModule` registry: detection is a
read-only frame *consumer*, not an owner of the port0→VENC path, and must run
concurrently with a framing preset (stab). A framing slot is mutually exclusive
(`star6e_framing_select()` first-`enabled()`-wins, `FRAMING_MAX=4`), which is the
wrong semantics. Detection gets its own small subsystem.

## Phase 1 — IPU bring-up (HARDWARE GATE)

Prove the silicon path before anything else.

1. Confirm `libmi_ipu.so` is present in `/usr/lib` on the Star6E bench
   (`root@192.168.1.13`). If absent, the whole feature is blocked — stop and
   report.
2. Minimal standalone probe (`tools/ipu_probe.c`, dev-only, not shipped): call
   `i6_ipu_load()`, `MI_IPU_CreateDevice(fw)`, `MI_IPU_CreateCHN(net)` with a
   known YOLOv8n `.img`, `MI_IPU_GetInOutTensorDesc`, feed a static NV12 buffer,
   `MI_IPU_Invoke`, dump raw output tensor shapes/scales.
3. Verify the output tensor layout matches expected YOLOv8n export
   (grid/anchors, INT8 + `scalar`/`zeroPnt`).

**Gate:** invoke returns plausible tensors on device. If the SGS toolchain
output doesn't match the `i6_ipu_tend` contract, resolve the model-conversion
recipe (private repo) before proceeding. No further phases until green.

## Phase 2 — VPE tap + inference thread

1. Add a dedicated VPE scaler output port at model input res (config
   `detect.input_w/h`, default 640×640, NV12). Follow the depth/getbuf pattern
   in `star6e_framing_stab.c` (`MI_SYS_SetChnOutputPortDepth`,
   `MI_SYS_ChnOutputPortGetBuf` → `phyAddr[0]`). Feed the phys addr straight to
   the IPU input tensor (zero-copy).
2. Low-priority inference thread: GetBuf → Invoke → PutBuf, decimated by
   `detect.infer_interval` (default: aim ~10 Hz regardless of encode fps).
3. Post-processor (`detect_model_yolov8.c`): dequant, decode anchor-free boxes,
   sigmoid scores, NMS, distance-from-size (D8). Output a host-order
   `DetectFrame { model_id, count, Det[] }` where
   `Det { class_id, score, cx, cy, w, h, distance_cm }`.
4. Publish into a **double-buffered latest-snapshot** (ping-pong pointer +
   seq counter). Mirrors how the inline scene detector hands per-frame telemetry
   to the sidecar without locking the encode hot path.

**Verify:** log decoded boxes for a known test scene; encode FPS/bitrate
unchanged vs `DETECT=0`.

## Phase 3 — Wire ABI (frozen) + sidecar

Envelope (`include/detect_sidecar.h`, network byte order, `pack(1)`):

```c
#define DETECT_MAGIC   0x44544354u   /* "DTCT" */
#define DETECT_VERSION 1
#define DETECT_MSG_SUBSCRIBE 1
#define DETECT_MSG_FRAME     2
/* 3 = DESCRIPTOR — RESERVED, future; do not implement in v1 */

#define DETECT_FLAG_TRUNCATED 0x01   /* objects dropped to fit one datagram */

typedef struct {
    uint32_t magic;            /* DETECT_MAGIC */
    uint8_t  version;          /* DETECT_VERSION */
    uint8_t  msg_type;         /* DETECT_MSG_FRAME */
    uint16_t model_id;         /* public id→name registry */
    uint16_t schema_ver;       /* payload TLV schema version for this model */
    uint16_t object_count;     /* objects actually included */
    uint32_t flags;            /* DETECT_FLAG_* (reserved bits = 0) */
    uint64_t frame_id;         /* == RtpSidecarFrame.frame_id for correlation */
    uint64_t capture_us;       /* same clock domain as RTP sidecar */
    uint16_t payload_len;      /* bytes of TLV that follow */
    uint16_t _reserved;        /* 0; future use */
    /* ── TLV payload: payload_len bytes ── */
} DetectEnvelope;              /* 32 bytes */
```

TLV record (per field): `[tag u8][len u8][value len bytes]`, values network
byte order, objects separated by a `TAG_OBJ` boundary tag. Reserve the v1 tag
set once (`TAG_OBJ`, `TAG_BBOX`, `TAG_CLASS`, `TAG_SCORE`, `TAG_TRACK_ID`,
`TAG_DISTANCE_CM`); leave the tag space open. **Consumers skip unknown tags by
`len`** — the same forward-compat rule the RTP sidecar already uses for its
flag-gated trailers.

Sidecar (`src/detect_sidecar.c`): copy the subscribe/fanout/TTL machinery from
`rtp_sidecar.c` (bind own `detect.sidecar_port`, learn subscriber from packet
source, ≤4 subs, 5 s TTL). Reserve two `_pad` bytes in the subscribe struct for
a future `max_schema_ver` capability field.

**One-datagram rule:** the packer fills objects (highest score first) until the
next TLV object would exceed the MTU budget; remaining objects are dropped and
`DETECT_FLAG_TRUNCATED` is set. Exactly one `sendto` per encoded frame per
subscriber.

**Verify:** `tests/test_detect_wire.c` on host — pack N objects, parse back,
confirm skip-unknown-tag and truncation-flag behaviour byte-for-byte.

## Phase 4 — Generic OSD path

`detect_overlay.c`: translate the latest `DetectFrame` snapshot into an
`OverlayList` (boxes as `RECT`, labels as `TEXT`, centroids as `POINT`), then a
model-agnostic renderer walks it calling `debug_osd_rect/text/point`. The
renderer never learns class semantics.

Coordinate mapping: boxes are in inference-frame (scaler port) space; map into
the encoded frame. When a framing crop is active, apply the same crop-origin
transform the OSD already uses for its stats panel
(`debug_osd_set_panel_offset`). Read the snapshot on the encode thread between
`debug_osd_begin_frame`/`end_frame`; detections are the most recent (a few
frames stale by design).

**Verify:** boxes land on-target with and without an active stab crop.

## Phase 5 — Config, crypto, docs, ship

1. `detect.*` config section through the full sync contract for persisted
   fields (`VencConfig` sub-struct + defaults, `load_detect()`/serialize,
   `render_detect()` printer, `g_fields[]`/`g_aliases[]`,
   `config/waybeam.default.json`) — **but no `SECTIONS[]` and no `FIELD_UI`**
   (posture per requirements). Fields: `enabled`, `model_path`, `key_path`,
   `sidecar_port`, `osd` (bool), `infer_interval`, `input_w`, `input_h`.
   - **Risk check:** confirm adding a sub-struct to `VencConfig` does not shift
     layout the ISP-bin loader depends on (`star6e_pipeline.h:78-79` heap-
     allocates `Star6eDualVenc` specifically to *avoid* growing `VencConfig`).
     If growth is unsafe, heap-allocate the detect config the same way.
2. Vendored `chacha20poly1305.c` (D4). Load path: if `model_path` bytes carry
   the AEAD header + tag, require a key (`key_path`/env); decrypt into a private
   buffer fed to the IPU via the `i6_ipu_rdfn` callback. Missing/wrong key →
   log once, leave detection dormant, pipeline runs normally.
3. Terse `HTTP_API_CONTRACT.md` entry for any `detect.*` set/get; no examples
   (examples live private).
4. `VERSION` bump + one `HISTORY.md` line. `make verify` (both backends;
   `DETECT=0` for Maruko, both `DETECT=0/1` for Star6E).

## Risks

- **R1 — model conversion (highest).** The IPU eats an `SGS_IPU_SDK`-compiled
  `.img`, not ONNX. If the toolchain recipe (quantization/calibration) is not
  reproducible, Phase 1 stalls. Owned by the private repo; must be settled
  before Phase 2.
- **R2 — IPU memory footprint.** 64–128 MB board already runs sensor+ISP+VENC.
  Measure IPU device/model residency in Phase 1; if it starves VB pools, cap
  input res / model size.
- **R3 — `VencConfig` growth vs ISP bin load** (see Phase 5 risk check).
- **R4 — one-datagram truncation** under crowded scenes. Accepted per hard
  constraint; `TRUNCATED` flag makes loss observable rather than silent.
- **R5 — coordinate drift** between scaler-port space and encoded crop. De-risked
  by reusing the panel-offset transform; verify on device with stab active.

## Simplicity review (fit-for-purpose)

What this plan deliberately **does not** build, to stay minimal:

- No in-band `DESCRIPTOR` negotiation — `model_id` + private schema covers v1;
  TLV skip-unknown already gives forward-compat. (Reserved msg_type only.)
- No fragmentation / multi-packet frames — the one-datagram rule removes an
  entire reassembly subsystem; truncation is a flag, not a protocol.
- No device-side model *registry* — one YOLOv8 decoder behind a `model_id`
  dispatch; adding a decoder later is additive, no framework.
- No per-device key binding in v1 — single shared key is enough to gate blob
  redistribution.
- No `FramingModule` reuse — avoided the mutual-exclusion mismatch.

The generality that matters (new models without touching OSD/transport) lives
entirely in **two frozen contracts** — the `OverlayList` draw vocabulary and the
`DetectEnvelope`+TLV wire — not in device-side plugin machinery. That is the
minimum surface that satisfies the future-proofing requirement.
