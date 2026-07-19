# Star6E NPU detection sidecar — implementation plan

Read `requirements.md` first. Staged so each phase is independently verifiable
(`AGENTS.md` "Scope Control"). Phase 1 is a **go/no-go hardware gate**: do not
build the wire trailer or OSD path until the IPU actually invokes on device.

## Module layout

```
Makefile                         DETECT ?= 0  → adds detect sources,
                                 -DHAVE_NPU_DETECT=1 when set
sdk/ssc338q/lib/chacha20poly1305.c   vendored single-file AEAD (D4)
src/star6e_npu.c                 IPU device/chn lifecycle, VPE tap, infer thread
include/star6e_npu.h             public hooks called from star6e_pipeline.c
src/detect_model.c               model_id → decoder dispatch + per-model config load
include/detect_model.h           DetectModel vtable; DetectFrame result struct
src/detect_model_yolov8.c        the one v1 decoder: NV12 tensors → boxes (+ any
                                 model-defined extras); produces TLV + OverlayList
src/detect_overlay.c             OverlayList → debug_osd_* renderer (model-agnostic)
include/detect_overlay.h
include/rtp_sidecar.h  (edit)    add RTP_SIDECAR_FLAG_DETECT + variable trailer +
src/rtp_sidecar.c      (edit)    a send-frame variant that appends an opaque blob
tests/test_detect_wire.c         host: DETECT trailer pack/parse + skip-unknown + trunc
tests/test_detect_decode.c       host: YOLOv8 decode/NMS (no SDK)
```

**Not** a `FramingModule` (mutually exclusive slot, wrong semantics) and **not** a
separate sidecar socket (D5 — one subscription). The sidecar stays a dumb carrier;
detection semantics live only in the decoder and in waybeam-hub.

## Phase 1 — IPU bring-up (HARDWARE GATE)

1. Confirm `libmi_ipu.so` in `/usr/lib` on the Star6E bench
   (`root@192.168.1.13`). Absent → feature blocked; stop and report.
2. Dev-only probe (`tools/ipu_probe.c`, not shipped): `i6_ipu_load()` →
   `MI_IPU_CreateDevice(fw)` → `MI_IPU_CreateCHN(net)` with a known YOLOv8n
   `.img` → `GetInOutTensorDesc` → feed a static NV12 buffer → `Invoke` → dump
   output tensor shapes/`scalar`/`zeroPnt`.
3. Confirm the layout matches the YOLOv8n export (grid, INT8 dequant).

**Gate:** plausible tensors on device. If the SGS-toolchain output doesn't match
the `i6_ipu_tend` contract, fix the conversion recipe (private repo) first. No
further phases until green. (Risk R1/R2 measured here.)

## Phase 2 — VPE tap + inference thread + decoder dispatch

1. Dedicated VPE scaler output port at the active model's input size (from the
   model's config, default 640×640 NV12). Follow the depth/getbuf pattern in
   `star6e_framing_stab.c` (`MI_SYS_SetChnOutputPortDepth`,
   `MI_SYS_ChnOutputPortGetBuf` → `phyAddr[0]`) and feed the phys addr straight
   into the IPU input tensor (zero-copy).
2. Low-priority inference thread: GetBuf → Invoke → PutBuf, decimated by
   `detect.infer_interval` (target ~10 Hz regardless of encode fps).
3. **Decoder dispatch** (`detect_model.c`): a `DetectModel` vtable keyed by
   `model_id` — `decode(tensors) → DetectFrame`, `to_overlay(DetectFrame) →
   OverlayList`, `to_tlv(DetectFrame) → bytes`. v1 registers one entry
   (`detect_model_yolov8.c`). Per-model params (thresholds, class table, input
   dims, any distance constants, tag schema id) are hardcoded in the decoder or
   loaded from a separate per-model config file — never from `waybeam.json`.
4. Publish `DetectFrame` into a **double-buffered latest-snapshot** (ping-pong
   pointer + `detect_seq`), mirroring how the scene detector hands telemetry to
   the sidecar without locking the encode hot path.

**Verify:** decoded boxes logged for a known scene; encode FPS/bitrate unchanged
vs `DETECT=0`.

## Phase 3 — RTP sidecar DETECT trailer (frozen transport)

> **Reconciliation (2026-07-19).** This section predates the **plugin-split**
> (public host dlopens a private `DetectBackend` plugin; the public ABI
> `detect_plugin.h` hands the host only `DetectBox[]` — pure geometry, net-px
> corner form, `score`, `cls`). The original text below assumed the *decoder*
> builds the wire blob (`to_tlv`), but the decoder is now **private** and the
> public host has boxes and nothing else. The design is therefore refactored to
> **host-built boxes over a still-opaque envelope**, keeping model-agnostic
> transport while matching what the host actually holds. Decisions folded in:
> canonical sidecar header re-homed to `waybeam_venc/include/rtp_sidecar.h`
> (waybeam_wfb_ng was deregistered 2026-07-18), and radeon-vrx is the first
> confirming consumer.

Extend `rtp_sidecar.h/.c`, modelled on the attitude trailer. New flag and a
variable-length trailer appended **after** attitude (last, because it is the only
variable-length trailer — the fixed ones keep computable offsets):

```c
#define RTP_SIDECAR_FLAG_DETECT 0x10   /* detect trailer follows (last) */
#define DETECT_TLR_TRUNCATED    0x01   /* objects dropped to fit MTU */

/* Fixed trailer header (network byte order, pack(1)); TLV body follows. */
typedef struct {
    uint16_t model_id;      /* public id→label-table registry (0 = VisDrone-10) */
    uint16_t schema_ver;    /* wire schema version (v1 = standard BOX tag)       */
    uint16_t object_count;  /* objects actually included                         */
    uint16_t flags;         /* DETECT_TLR_*                                       */
    uint32_t detect_seq;    /* monotonic inference id (dedup / freshness)         */
    uint16_t payload_len;   /* bytes of TLV that follow                           */
    uint16_t age_ms;        /* staleness of this snapshot at frame encode, sat.   */
} RtpSidecarDetectHdr;      /* 16 bytes + payload_len */
```

**TLV body:** `[tag u8][len u8][value…]`, network byte order. Consumers skip
unknown tags by `len`. One **public standard tag** is defined; the host emits it
directly from `DetectBox[]` (no plugin involvement needed for v1):

```c
#define DETECT_TAG_BOX   0x01   /* value = 10 B, one detection */
/* value: x1,y1,x2,y2  u16 each, NORMALIZED 0..65535 of frame W/H, corner form;
 *        score u8 (prob*255);  cls u8 (mapped to a label via model_id).       */
```

**Normalized coords, not net-px:** the VPE port1 tap squashes the full frame into
the model input (640×352) *linearly per-axis* (`star6e_ipu_yolo.c:284`), so a
net-normalized coordinate equals the display-normalized coordinate. The consumer
multiplies by its own decoded W/H — no net dims, no aspect math on the wire,
model-geometry-agnostic. Host converts `x_norm = x_net / net_w * 65535` etc.

Tags `0x80–0xFF` are **reserved for private, plugin-defined records** (keypoints,
distance, classifier vectors). A future plugin ABI addition can hand the host an
opaque extra-blob to append after the standard boxes — deferred, not built. This
keeps the public spec stable while private models evolve their payload.

**Who builds it:** the **host** serializes the published `DetectBox[]` snapshot
into standard BOX tags. The sidecar stays a dumb carrier (it copies the assembled
trailer, understands only the byte layout); the host understands only "boxes"
(universal geometry, `cls` is an opaque int). Model *semantics* (label strings,
distance formulas) stay private / in the consumer's `model_id` table.

**Wiring (also fixes the fixed-92 B-buffer trap):** the sender assembles trailers
by running offset into a fixed `RtpSidecarFrameExtFull` struct
(`rtp_sidecar.c:265`) — too small for a variable trailer. Replace that local with
`uint8_t buf[RTP_SIDECAR_DGRAM_MAX]` (512) and add a superset
`rtp_sidecar_send_frame_detect(... , const void *detect_blob, uint16_t
detect_len)` that sets `RTP_SIDECAR_FLAG_DETECT` and appends the blob after
ATTITUDE; `rtp_sidecar_send_frame_full` becomes a `detect=NULL` wrapper. Max
datagram = 52 + 12 + 16 + 12 + (16 + 24×10) = **360 B** ≪ MTU; bump the spec's
receive-buffer floor 128 → 512 B.

**Snapshot handoff:** the port1 reader (`iy_reader_main`) today fills
`d->boxes[64]` and drops them (`star6e_ipu_yolo.c:204`). Add a mutex-guarded
double-buffered snapshot `{boxes_pub, ndet_pub, detect_seq, produced_us}` to
`Star6eIpuDetect`; the reader publishes after `process()`, the encode thread
reads it via a new `star6e_ipu_yolo_snapshot()` inside `star6e_video_send_frame`
(new `detect_info` param threaded alongside `enc_info`/`att_info`,
`star6e_video.c:157`). The lock is held only for a ≤24-box memcpy (µs) — **never
across the ~96 ms invoke** — so the encoder hot path is untouched.

**Attach policy — every frame, not on-fresh.** Emit the latest snapshot on every
FRAME while subscribed (the original "attach only when `detect_seq` advanced" is
reversed): on a lossy RF video link, repeating the current boxes every frame is
cheap (~20 KB/s at 60 fps) and loss-resilient, and `detect_seq` still lets the
consumer dedup / detect freshness. The trailer is omitted only when no valid
snapshot exists yet (mirrors ATTITUDE dropping when invalid). `age_ms` lets the
consumer fade stale boxes. *(Reversible: restore on-fresh to minimize bytes.)*

**One-datagram rule:** the host fills BOX tags highest-score-first until the next
would exceed the remaining MTU budget (packet minus the frame/enc/transport/
attitude trailers already present, cap `RTP_SIDECAR_DETECT_MAX = 24`); the rest
are dropped and `DETECT_TLR_TRUNCATED` set. Exactly one `sendto` per frame per
subscriber.

**Canonical-header + wire guard.** Update `protocols/rtp-sidecar.md` FIRST, then
re-home its "canonical header" pointer to `waybeam_venc/include/rtp_sidecar.h`
(the vendored `waybeam_wfb_ng/shared/…` original is deregistered). The venc-side
`tests/test_detect_wire.c` host round-trip (pack N boxes → parse back →
skip-unknown-tag → truncation → normalized-coord round-trip) **replaces the
deregistered wfb_ng frozen Python parser** as the wire-conformance guard; keep
the hub's `rtp_sidecar_wire.h` in sync when the ground consumer lands.

**Verify:** `tests/test_detect_wire.c` green on host; on `.201`, emit while
subscribed and confirm the trailer decodes; **radeon-vrx** (x86 RTP viewer that
already decodes the matching stream) subscribes, draws the normalized boxes, and
they overlay the worker's own burned-in boxes 1:1 — end-to-end wire confirmation
with zero hub risk. The ground waybeam-hub OSD consumer (parse DETECT → unified
LVGL OSD data layer) is a separate follow-on PR.

## Phase 4 — Generic OSD path

`detect_overlay.c`: the decoder's `to_overlay` emits an `OverlayList` (boxes →
`RECT`, labels → `TEXT`, centroids → `POINT`); a model-agnostic renderer walks it
calling `debug_osd_rect/text/point`. The renderer learns no class semantics.

Coordinate mapping: boxes are in inference-frame (scaler port) space; map into
the encoded frame. With a framing crop active, apply the same crop-origin
transform the OSD already uses (`debug_osd_set_panel_offset`). Read the snapshot
on the encode thread inside `debug_osd_begin_frame`/`end_frame`; detections are
the most recent (a few frames stale by design).

**Verify:** boxes land on-target with and without an active stab crop.

## Phase 5 — Config, crypto, docs, ship

1. Minimal `detect.*` in `waybeam.json` through the full sync contract for
   persisted fields (`VencConfig` + defaults, `load_detect()`/serialize,
   `render_detect()` printer, `g_fields[]`/`g_aliases[]`,
   `config/waybeam.default.json`) — **no `SECTIONS[]`, no `FIELD_UI`**. Fields:
   `enabled`, `model_id`, `model_path`, `key_path`, `osd`, `infer_interval`.
   Per-model tuning does **not** live here (D9).
   - **Risk check R3:** confirm adding a sub-struct to `VencConfig` doesn't
     shift layout the ISP-bin loader depends on (`star6e_pipeline.h:78-79`
     heap-allocates `Star6eDualVenc` specifically to avoid growing `VencConfig`).
     If unsafe, heap-allocate the detect config the same way.
2. Vendored `chacha20poly1305.c`. Load: if `model_path` bytes carry the AEAD
   header+tag, require a key (`key_path`/env), decrypt into a private buffer fed
   to the IPU via `i6_ipu_rdfn`. Missing/wrong key → log once, stay dormant.
3. Terse `HTTP_API_CONTRACT.md` entry for `detect.*` set/get; no examples.
4. `VERSION` bump + one `HISTORY.md` line. `make verify` (Maruko `DETECT=0`;
   Star6E `DETECT=0` and `DETECT=1`).

## Risks

- **R1 — model conversion (highest).** IPU eats an `SGS_IPU_SDK` `.img`, not
  ONNX. Non-reproducible quantization stalls Phase 1. Private repo; settle before
  Phase 2.
- **R2 — IPU memory footprint** on a 64–128 MB board. Measure residency in
  Phase 1; cap input res / model size if VB pools starve.
- **R3 — `VencConfig` growth vs ISP bin load** (see Phase 5).
- **R4 — trailer truncation** under crowded scenes. Accepted; `TRUNCATED` makes
  loss observable, not silent.
- **R5 — coordinate drift** scaler-port vs encoded crop. De-risked by reusing the
  panel-offset transform; verify on device with stab active.
- **R6 — sidecar edit touches a core/shared file.** The DETECT append is generic
  (opaque blob) so it compiles in both backends without pulling in detection;
  keep the model-specific code entirely behind `HAVE_NPU_DETECT`.

## Simplicity review (fit-for-purpose)

Deliberately **not** built, to stay minimal:

- **No second socket / second subscription** — detection rides the ATTITUDE
  packet as one more flag-gated trailer. One read for hub; auto-correlated by
  `frame_id`. (Removed the separate-port design entirely.)
- **No fragmentation** — one datagram per frame; truncation is a flag.
- **No privileged fields in transport or OSD** — distance/keypoints/stats are all
  model-defined TLV tags + model-produced OSD primitives. Transport carries
  opaque bytes; the OSD renderer draws opaque primitives.
- **No in-band DESCRIPTOR, no per-device key** — reserved.
- **No plugin framework** — a small `model_id` vtable dispatch; a second model is
  additive.

The generality that matters — new models without touching OSD or transport —
lives in exactly two frozen contracts (the `OverlayList` draw vocabulary and the
opaque DETECT trailer) plus one dispatch point (`model_id` → decoder). That is the
minimum surface that satisfies both "one read" and "any future model."

## Phase 1 — RESULT (2026-07-19, bench .201 Star6E/IMX335, OpenIPC)

**Gate verdict: GO — the IPU runs YOLOv8n on real silicon, end to end.** On
.201 the full path works: sensor → VPE port1 (640×352 NV12) → IPU YOLOv8n
inference → detection→OSD (`[RGN-Y8]` bounding boxes) → RTP H.265 out. Verified
live: RTP feed to a ground viewer (radeon-vrx) shows video **with bounding
boxes overlaid**. IPU invoke ~51 ms (≈19.6 fps), model
`yolov8n352drone.img` (352-input, 10-class), IPU/model SDK `1.2.2`.

The enabling piece was a **correctly-built `mi_ipu.ko`** (provided by a
collaborator, Apr-2026 build): vermagic-matched, **no `__memzero` dependency**,
and built against the current `mi_common` ABI — so it loads with **no shims and
no oops, and `request_irq` succeeds** on the same stock OpenIPC image. Both
"blockers" recorded in the superseded NO-GO below were artifacts of testing
with the *wrong* module (the 2022 SDK `eed835e` build), not real board limits:

- The **DLA IRQ maps fine** with the right module. `request_irq` failed only
  with the mismatched `.ko`; the corrected module requests the DLA interrupt
  and the CCIF/RISC-V handshake completes. No DT/GIC change was needed.
- The **`fail to get ipu mma heap name` messages are non-fatal** — they still
  print with the working stack; the IPU falls back to the MMU-backed MMA heap
  and inference runs. Not a blocker.

The two shims (`memzero_shim`, `ipu_fixup`) are therefore **not needed** with a
proper `mi_ipu.ko`; they were scaffolding to get the SDK module far enough to
localize the failure. The only real dependency is the matched `mi_ipu.ko` (plus
`ipu_firmware.bin`), which belongs in the builder's
`sigmastar-osdrv-infinity6e` package.

### Superseded NO-GO analysis (kept for context — was the wrong-module artifact)

The following described the failure seen with the **2022 SDK `mi_ipu.ko`
(`eed835e`)** only. It is retained because the ABI/mutex-drift and mhal
capability findings are accurate and reusable, but its *gate conclusion* is
overturned by the GO above.

### What was proven to work
- `libmi_ipu.so` ships in OpenIPC; all 22 low-level `MDRV_IPU_*` symbols are
  already exported by the on-device `mhal` — the hard part of the driver stack
  is present.
- OpenIPC ships **no `mi_ipu.ko`**. The SigmaStar SDK's build (Pudding
  ILC02V009, kernel 4.9.84, vermagic-identical) loads after two shims:
  1. **`__memzero` export shim** — OpenIPC's 4.9.84 dropped the legacy ARM
     `__memzero`; a 6-line module (`memset(p,0,n)` + `EXPORT_SYMBOL`) built
     against the builder kernel tree restores it.
  2. **mi_common device-mutex ABI bridge** — the OpenIPC-shipped
     `mi_common`/`mi_sys` (build `dc99390`, 2022-06-07) added a per-device
     `struct mutex *` at device-struct offset **+0x30**, locked in
     `MI_DEVICE_Open`; the SDK's `mi_ipu.ko` (build `eed835e`, 2022-06-01)
     predates that field and leaves it NULL → first `/dev/mi_ipu` open oopsed
     (NULL deref at +4 in `__mutex_lock_slowpath`). A tiny fixup module owns a
     real mutex and writes its address into `mi_ipu`'s dev struct `.data+0x90`
     after load. (Confirmed by a two-build disassembly diff.)
- With both shims: `MI_SYS_Init` OK; `MI_IPU_GetOfflineModeStaticInfo` parses
  the model (ssd_mobilenet stand-in: var_buf 2.88 MB, model 7.54 MB);
  `MI_IPU_CreateDevice` reaches **firmware init** (i.e. device open, attr, and
  the vendor path-mode `CreateDevice(&attr, NULL, fw_path, 0)` all succeed).

### Where it stops
`CreateDevice` fails at **firmware init, error 14 = the RISC-V CCIF handshake
timeout**, for two independent infrastructure reasons:

1. **DLA interrupt not mappable.** `mi_ipu` init does
   `irq_of_parse_and_map(dla_node, 0)` → returns **virq 0** → `request_irq`
   fails ("request_irq failed"). The DLA handshake semaphore is posted *only*
   by that ISR, so init always times out (error 14). The board DT `/soc/dla`
   is byte-identical to the vendor's (`interrupts=<GIC_SPI 85 LEVEL_HIGH>`,
   `status="ok"`), yet the OpenIPC GIC domain does not map SPI 85 — a
   kernel/GIC-config gap, not a DT-authoring gap.
2. **No named IPU MMA heap.** Kernel prints "fail to get ipu mma heap name when
   {enable,disable} miu protection". The vendor carves a dedicated IPU/DLA heap
   in specific `MMAP_I6E_*` profiles; OpenIPC's memory layout exposes only
   `mma_heap_name0` (bootarg `mma_heap=mma_heap_name0,...`). `MMU`-enabled
   `mi_sys` also makes `MI_SYS_ConfigPrivateMMAPool` a no-op ("private pool is
   disabled by default"), so a userspace carve-out cannot substitute.

### Builder work required to ship (Phase 2 is UNBLOCKED)
The gate is GO, so Phase 2 (the in-tree venc integration) can proceed now on
the bench. For a shippable image the builder needs only:
- **Ship the matched `mi_ipu.ko` + `ipu_firmware.bin`** in the
  `sigmastar-osdrv-infinity6e` package, and load `mi_ipu` from the module init
  (it is not auto-loaded today). No shims required with the correct module.
- Provide the model + firmware at their runtime paths
  (`/config/dla/ipu_firmware.bin`, model under a persisted path — on the bench
  the 4.4 MB model lives on SD, symlinked into `/root/models/`, because the
  NOR overlay is too small).

*Not needed* (proven non-issues with the correct module): DLA-IRQ/GIC changes
and a named IPU MMA-heap carveout — see the GO note above.

### Repo artifacts from this phase
- `tools/ipu_probe.c` reworked to the **vendor CreateDevice sequence**:
  pulls `libcam_os_wrapper` / `libcam_fs_wrapper` / `libmi_sys` (libmi_ipu's
  unlinked deps) with `RTLD_GLOBAL`, calls `MI_SYS_Init`, sizes the device from
  `GetOfflineModeStaticInfo`, and uses vendor path-mode
  `CreateDevice(&attr, NULL, fw, 0)` / `CreateCHN(&attr, NULL, model)`.
- `include/star6e_ipu.h` / `src/star6e_ipu.c`: added the `cam_os`/`cam_fs`/
  `mi_sys` dependency handles, `MI_SYS_Init` + `GetOfflineModeStaticInfo`
  bindings, and `IpuOfflineInfo`.
- The two shim modules (`memzero_shim.c`, `ipu_fixup.c`) are **dev-bench
  scaffolding**, not part of this repo — the real fix is the matched `.ko` in
  the builder. They live in the session scratchpad and are documented here for
  reproducibility.
