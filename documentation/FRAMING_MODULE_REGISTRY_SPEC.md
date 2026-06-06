# Spec: Framing-Module Registry + Data-Driven Field Schema

Status: **DRAFT — awaiting review.** No code yet.
Scope: Star6E backend (stabilization is Star6E-only).
Author workflow: Phase 1 (Spec). Do not implement until approved.

## 1. Motivation

`src/star6e_pipeline.c` is 4,008 lines; roughly 1,100 of them (the
`star6e_stab_*` block, ~40 `g_stab_*` statics, lines 860–1982 plus
`star6e_stab_apply_ae_crop`) implement image stabilization. The feature is
genuinely interesting but carries a large complexity/maintainability tax:

- It is interwoven into the pipeline's bind/teardown/IMU/AE-crop paths, so
  "drop stabilization later" is currently a risky, wide surgery rather than a
  one-file delete.
- Every user-facing knob must be threaded through five layers (C struct +
  parser, API field/alias tables, the byte-exact `render_<section>` printer,
  `config/waybeam.default.json`, and the WebUI `SECTIONS[]`), and the WebUI
  step requires regenerating the embedded gzip blob in `src/venc_webui.c`.

This spec defines two **sequenced, independently shippable** deliverables that
make stabilization (and any future "framing"-style module) cheap to add,
cheap to remove, and — for the UI half — addable without rebuilding the blob.

### Goals

1. **Painless extraction.** All stabilization pipeline code lives behind one
   interface in one file (`src/star6e_framing_stab.c`). Removing it =
   delete the file, drop one `SRC` entry, drop one registration line.
2. **Blob-free module UI.** Adding a future framing module's fields to the
   dashboard must not require editing `web/dashboard.html` or running
   `make webui`.
3. **Behavior-neutral step 1.** The registry extraction must produce a
   byte-for-byte-equivalent runtime (same SDK call sequence, same fps, same
   teardown discipline). It is a *move*, not a rewrite.

### Non-goals

- No `dlopen` runtime plugins. See §3.1 decision D1 — compile-time registry
  only.
- No change to stabilization's *behavior*, presets, or tuning math.
- No Maruko work (stab is Star6E-only).
- Deliverable 2 does **not** remove the typed-config coupling (struct /
  printer / `default.json`). That is an optional, clearly-bounded follow-up
  (§4.6, Deliverable 3) with its own tradeoffs.

## 2. Current state (reference)

### 2.1 Stabilization footprint in `star6e_pipeline.c`

| Element | Location | Notes |
|---|---|---|
| `g_stab_*` statics (~40) | 785–858 | dlsym fn ptrs, IVE handle, thread, lock, geometry, feel knobs, pan, ports, IMU ring |
| `STAB_*` compile-time defaults | 768–783 | motion thresh, still frames, edge %, EMA alpha |
| `star6e_stab_*` functions | 860–1982 | load symbols, configure, blit, detect, thread main, setup_ports, start, stop, enabled, reapply_vpe_port, panel_anchor |
| `star6e_stab_apply_ae_crop` | 2540 | derives AE metering rect from stab geometry, calls `star6e_emit_ae_crop` |

### 2.2 Integration seams (calls from non-stab pipeline code)

| Seam | Site | Purpose |
|---|---|---|
| `star6e_stab_enabled(vcfg)` | 3346, 3520, 3796 | gate: does framing take port0 this run? |
| `star6e_stab_setup_ports(...)` | 3359 | configure VPE ports for the active mode |
| `star6e_stab_start()` | 3365 | spawn detector/blit threads |
| `star6e_stab_apply_ae_crop()` | 3380 | seed AE window post-bind |
| `legacy_stab = enabled && !g_stab_hw_mode` | 3520 | drain-path selection in stop |
| `star6e_stab_stop()` | 3675 | join threads, teardown |
| `star6e_pipeline_imu_push` → `g_stab_imu_ring` | 3208 | IMU samples feed the (optional) gyro seam |
| `star6e_pipeline_stab_panel_anchor` | header 136, def 1002 | OSD reads the live crop origin |
| `star6e_stab_set_pan` | 992, driven by zoomX/zoomY controls | live pan steer |
| `star6e_emit_ae_crop` | 2441 | generic AE-crop emit (stays in pipeline) |

### 2.3 Field/UI machinery

- `FieldDesc` + `FIELD()` macro + `g_fields[]`: `src/venc_api.c:296–409`.
  `FieldDesc` = `{key, type, mut, offset, size}`.
- `/api/v1/capabilities` handler: `src/venc_api.c:2084`. Today emits, per
  field, `{mutability, supported}` only.
- Dashboard renderer: `web/dashboard.html` `renderSettings()` (996–1065).
  Reads field **value** from `/api/v1/config` (`localConfig[key]`) and
  **mutability** from `/api/v1/capabilities` (`capabilities[key]`), both at
  runtime. The blob-static metadata is: `SECTIONS[]` (grouping/order, 707),
  `ENUMS[]`, `TOOLTIPS[]`, `inputAttrs()` (ranges/step), and `fieldType()`.
- Dynamic-options precedent already exists: `sensor.mode` pulls its option
  labels from `/api/v1/modes` via `window._modeLabels` (1044).
- Blob: `src/venc_webui.c` is generated from `web/dashboard.html` by
  `tools/build_webui.py`; `make webui` regenerates, `make webui-check`
  (run by `make verify`) fails on drift.

## 3. Deliverable 1 — FramingModule registry

### 3.1 Design decisions

- **D1 — Compile-time registry, not `dlopen`.** A `.so` plugin would need a
  stable ABI for `Star6ePipelineState`, the MI_SYS handles, and SDK symbol
  passing across the boundary, plus versioning — high cost, no benefit for a
  single embedded binary. We use a statically-linked module that registers a
  `const FramingModule *` into a small table. Extraction stays a one-file
  delete; the only "still compiled in" cost is gated by the `SRC` list.
- **D2 — Two-way interface.** Pipeline→module via the `FramingModule` vtable.
  Module→pipeline via a small, explicit **host-services** header
  (`star6e_framing_host.h`) exposing only what the module legitimately needs:
  `star6e_emit_ae_crop`, the active `Star6ePipelineState *`, precrop/zoom
  geometry getters, and the MI port helpers. No new global coupling.
- **D3 — IMU ring moves to the pipeline as a shared facility.** The ring is
  already documented (848–858) as populated independent of stabilization
  (telemetry/sidecar). Rename `g_stab_imu_ring` → `g_imu_ring`, keep
  `star6e_pipeline_imu_push` in the pipeline, and expose a read accessor
  `star6e_pipeline_imu_ring(void)` that the module's detector consumes. This
  removes the last stab dependency from the IMU push path.
- **D4 — Pan/zoom seam via the vtable.** The zoomX/zoomY live controls
  currently call `star6e_stab_set_pan` directly. Route them through
  `module->set_pan(x, y)` (no-op for modules that ignore pan). The same
  controls keep steering zoom presets unchanged (zoom is not a framing
  module — see D6).
- **D5 — Module owns AE-crop derivation.** `star6e_stab_apply_ae_crop` moves
  into the module and calls back via `star6e_emit_ae_crop` (host service).
  The generic `star6e_emit_ae_crop` / `star6e_apply_ae_crop` /
  `ae_crop_mark_ready` machinery stays in the pipeline (used by zoom too).
- **D6 — Zoom and pan ramp stay in the pipeline.** They are not
  stabilization; they share the `framing` preset table but program
  `MI_VPE_SetPortCrop` directly and never call a `star6e_stab_*` function.
  They are out of scope for this move and continue to work when stab is
  absent.

### 3.2 Interface — `include/star6e_framing.h`

```c
/* Pipeline → module. One module is active per run, selected by matching
 * FramingModule.preset_name against video0.framing. All hooks run on the
 * pipeline thread except the detector/blit threads the module owns. */
typedef struct FramingModule {
	const char *preset_name;        /* matched vs video0.framing, e.g. "stab" */

	int  (*enabled)(const VencConfig *);
	int  (*setup_ports)(Star6ePipelineState *, uint32_t src_fps,
	                     uint32_t dst_fps);
	int  (*start)(void);
	void (*stop)(void);
	void (*apply_ae_crop)(void);

	/* Optional (NULL = unsupported). */
	void (*set_pan)(double x, double y);          /* live pan steer */
	int  (*panel_anchor)(int *out_x, int *out_y); /* OSD crop origin */
	int  (*uses_manual_drain)(void);              /* replaces !g_stab_hw_mode */
} FramingModule;

/* Registry. Modules self-register at link time via a constructor or an
 * explicit star6e_framing_register_builtins() called from pipeline init. */
const FramingModule *star6e_framing_select(const VencConfig *);
```

### 3.3 Host services — `include/star6e_framing_host.h`

Exactly the pipeline internals the stab module needs, promoted from `static`
to backend-internal linkage (declared here, defined in `star6e_pipeline.c`):

```c
void  star6e_emit_ae_crop(const Star6eAeCropRect *);
Star6ePipelineState *star6e_framing_active_state(void);
const ImuRing *star6e_pipeline_imu_ring(void);
/* precrop/zoom geometry getters as needed by setup_ports */
```

No other `star6e_pipeline.c` statics become visible. This keeps the seam
auditable.

### 3.4 What moves to `src/star6e_framing_stab.c`

- All `g_stab_*` statics **except** the IMU ring (D3).
- All `STAB_*` compile-time defaults.
- All `star6e_stab_*` functions (860–1982) and `star6e_stab_apply_ae_crop`.
- `star6e_stab_load_sys_extra_symbols` (the `dlopen` of `MI_SYS` extras) —
  module-private; only the SW-compose path needs it.
- A `const FramingModule star6e_framing_stab` definition + registration.

`include/star6e_framing_stab.h` exposes only the module pointer/registration.

### 3.5 Pipeline integration changes

Replace the inline `star6e_stab_*` calls with vtable dispatch via a single
`const FramingModule *g_framing` resolved once in `bind_and_finalize_pipeline`:

| Before | After |
|---|---|
| `if (star6e_stab_enabled(vcfg))` (3346) | `g_framing = star6e_framing_select(vcfg); if (g_framing)` |
| `star6e_stab_setup_ports(...)` | `g_framing->setup_ports(...)` |
| `star6e_stab_start()` | `g_framing->start()` |
| `star6e_stab_apply_ae_crop()` | `g_framing->apply_ae_crop()` |
| `enabled && !g_stab_hw_mode` (3520) | `g_framing && g_framing->uses_manual_drain && g_framing->uses_manual_drain()` |
| `star6e_stab_stop()` (3675) | `if (g_framing) g_framing->stop()` |
| `star6e_pipeline_stab_panel_anchor` | thin shim → `g_framing->panel_anchor` |
| zoomX/zoomY → `star6e_stab_set_pan` | → `if (g_framing->set_pan) g_framing->set_pan(...)` |

`star6e_pipeline_stab_panel_anchor` keeps its current signature (OSD caller
in `debug_osd`/runtime unchanged); it becomes a 3-line shim over the vtable.

### 3.6 Makefile

Add `src/star6e_framing_stab.c` to `STAR6E_ONLY_SRC` (Makefile:39) and to the
test lib list if any unit test links it. Extraction later = remove this one
entry; the registry `select()` returns NULL and the pipeline runs the plain
bound path.

### 3.7 Behavior-neutrality & verification (D1)

- This is a pure move. The diff should be reviewable as "same lines, new
  file + indirection." No SDK call order, geometry, or timing changes.
- `make lint`, then `make verify` (both backends; Maruko unaffected).
- On-device regression per `documentation/STABILIZATION_TEST_PLAN.md`:
  framing=stab at the imx335 bench (192.168.1.13), confirm fps, recenter
  feel, clean teardown (no MMU faults), and OSD panel anchor tracking.
- Expected `star6e_pipeline.c` size after: ~2,900 lines (−~1,100).

## 4. Deliverable 2 — Data-driven field schema (blob-free module UI)

### 4.1 Decision

- **D7 — Promote presentation metadata into the C field registry and emit it
  from `/api/v1/capabilities`.** The renderer already consumes that endpoint
  at runtime; making it the source of grouping/label/type/range/enum/tooltip
  lets a module's fields render with no `dashboard.html` edit and no blob
  rebuild.
- **D8 — Typed config stays.** Module fields remain real `VencConfig` fields
  in Deliverable 2, so the byte-exact printer and `test_save_layout_byte_equal`
  are untouched. Deliverable 2 removes the **blob-rebuild** requirement only.
  Removing the struct/printer/`default.json` touch is the separate optional
  Deliverable 3 (§4.6).

### 4.2 `FieldDesc` extension

Add an optional UI descriptor (NULL for core fields → rendered by the existing
static `SECTIONS[]` path, zero behavior change):

```c
typedef struct {
	const char *group;     /* dynamic section id+title, e.g. "framing:Stabilization" */
	const char *label;     /* human label; NULL → derive from key */
	const char *ui_type;   /* "bool"|"int"|"double"|"enum"|"text" */
	double min, max, step; /* numeric inputs; 0/0/0 → unconstrained */
	const char *enum_csv;  /* "off,stab,zoom-2x"; NULL → not enum */
	const char *tooltip;
} FieldUi;

typedef struct {
	const char *key; FieldType type; Mutability mut;
	size_t offset; size_t size;
	const FieldUi *ui;     /* NULL for core/static-rendered fields */
} FieldDesc;
```

### 4.3 Field registration (core + module)

Introduce an append API so a module contributes its fields without editing
`g_fields[]`:

```c
void venc_api_register_fields(const FieldDesc *fields, size_t n);
size_t venc_api_field_count(void);          /* core + registered */
const FieldDesc *venc_api_field_at(size_t);
```

The stab module registers its `stabKalmanQ/R`, `pauseStab`, etc. descriptors
(with `FieldUi.group = "framing:Stabilization"`) from its init. Validation
(`venc_api.c:684–720`) similarly moves behind a per-field validator hook so
the module owns its ranges.

### 4.4 `/api/v1/capabilities` JSON extension

Per-field, add the UI block when `ui != NULL`:

```json
"video0.stabKalmanQ": {
  "mutability": "live", "supported": true,
  "ui": { "group": "framing:Stabilization", "label": "Kalman Q",
          "type": "double", "min": 0.0, "max": 1.0, "step": 0.01,
          "tooltip": "Process noise. Higher = follows camera faster." }
}
```

Backward compatible: existing consumers ignore the new `ui` key.

### 4.5 Dashboard generic renderer

- Keep the static `SECTIONS[]` path for core fields (no churn, blob stable).
- Add one generic pass in `renderSettings()`: group capabilities entries that
  carry `ui.group` by their group id, render a collapsible section per group
  using `ui.type/min/max/step/enum/label/tooltip`. Value still comes from
  `localConfig[key]`; mutability badge from `capabilities[key].mutability`.
- **Blob rebuild semantics after this lands:** the blob must be regenerated
  only to change the *generic renderer itself*. Adding/removing a framing
  module's fields needs **no** `dashboard.html` edit and **no** `make webui`.

### 4.6 Optional Deliverable 3 (documented, not scheduled)

To also escape the struct/printer/`default.json` touch, plugin config could
move into a generic params sub-object (`video0.framingParams: { ... }`)
stored as a KV map instead of typed struct fields.

| | Deliverable 2 (typed, recommended) | Deliverable 3 (params map) |
|---|---|---|
| Blob rebuild to add module fields | No | No |
| Edit `VencConfig`/printer/`default.json` to add fields | Yes (small, local) | No |
| Type safety + `test_save_layout_byte_equal` | Kept | Lost for plugin fields |
| Right when | one/few modules | genuinely many community modules |

Recommendation: ship Deliverable 2; adopt Deliverable 3 only if a real
multi-module ecosystem materializes.

### 4.7 Verification (Deliverable 2)

- `make verify` (includes `make webui-check`): confirm the static blob is
  unchanged by the C-only field-metadata work, except the one-time generic
  renderer edit.
- Unit: `tests/test_venc_api.c` — assert `/api/v1/capabilities` emits the new
  `ui` block for a registered field and omits it for a core field.
- Manual: load dashboard, confirm the dynamic "Stabilization" group renders
  from the endpoint with correct ranges/tooltips/live badges, and that a
  field added in C (no HTML edit) appears after a rebuild of the binary only.

## 5. Risks & open questions

1. **Hidden statics.** A few stab functions may touch pipeline statics not in
   the §3.3 host list (e.g. precrop dims, sensor geometry). Resolution: during
   Phase 2, compile the moved file and let the linker enumerate every
   unresolved symbol; promote each, deliberately, to `star6e_framing_host.h`
   (no blanket `extern`).
2. **`panel_anchor` callers.** Confirm every caller of
   `star6e_pipeline_stab_panel_anchor` (OSD, runtime) is satisfied by the
   shim — signature unchanged, returns 0 when no module/active crop.
3. **IMU ring rename churn.** `g_stab_imu_ring` → `g_imu_ring` touches
   `imu_push` and the detector. Mechanical but must keep `imu_ring_ready`
   gating identical.
4. **Field-count assumptions.** Anything iterating `g_fields[]` by static
   `FIELD_COUNT` must switch to `venc_api_field_count()` so registered fields
   are included (capabilities, `/api/v1/config` enumeration, tests).
5. **Validator move.** The stab range checks in `validate_field` must move
   with the module without changing error strings the API contract documents.

## 6. Sequencing, versioning, and PRs

Two PRs, each its own `VERSION` bump + `HISTORY.md` entry (one bump per PR):

1. **PR A — FramingModule registry extraction** (Deliverable 1). Behavior-
   neutral. Self-contained; mergeable and on-device-verifiable alone. This
   alone delivers painless future extraction and the ~1,100-line shrink.
2. **PR B — Data-driven field schema** (Deliverable 2). Depends on A only for
   the stab field descriptors moving into the module; the endpoint/renderer
   work is otherwise independent. Delivers the blob-free module UI.

Deliverable 3 (params map) is deferred and not part of this plan unless
requested.

Docs to update in the implementing PRs: `documentation/HTTP_API_CONTRACT.md`
(capabilities `ui` block), `AGENTS.md` "Config / WebUI / API Sync Rules" (note
the dynamic path), and this file's status → IMPLEMENTED.
