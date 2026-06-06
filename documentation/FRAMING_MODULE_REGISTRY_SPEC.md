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
It also retires the legacy manual-drain stabilization path and the dead code
that the HW-crop path superseded (per review decision, §3.1 D9).

### Goals

1. **Painless extraction.** All stabilization pipeline code lives behind one
   interface in one file (`src/star6e_framing_stab.c`). Removing it =
   delete the file, drop one `SRC` entry, drop one registration line.
2. **Blob-free module UI.** Adding a future framing module's fields to the
   dashboard must not require editing `web/dashboard.html` or running
   `make webui`.
3. **Shed the legacy manual-drain.** The HW-crop path
   (`501d496`) was introduced to fix the teardown wedge the manual-drain
   caused; the manual-drain survives only as a never-exercised fallback on the
   shipping targets. Remove it and the code that exists only to serve it.
4. **Behavior-neutral move.** The extraction itself (separate from the legacy
   removal) must be a byte-for-byte-equivalent runtime on the HW-crop path —
   same SDK call sequence, fps, and teardown discipline. The two are split
   into separate commits so the move is independently bisectable.

### Non-goals

- No `dlopen` runtime plugins. See §3.1 D1 — compile-time registry only.
- No change to stabilization's *behavior*, presets, or tuning math on the
  HW-crop path. The only behavior change is the port1-tap-failure fallback
  (§3.1 D9).
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

### 2.2 HW-crop vs legacy manual-drain (the mode split being collapsed)

`star6e_stab_setup_ports` (1762) tries the HW path first and falls back:

- **HW-crop mode** (`g_stab_hw_mode=1`): VPE port0 hardware-crops the stab
  window straight to a VENC bind (zero-copy); a 256×256 port1 tap feeds the
  IVE detector. Per-frame work is one `SetPortCrop`. Bench-verified on all
  shipping targets (`OIS_stab-fill.md`).
- **Legacy manual-drain** (`g_stab_hw_mode=0`): port0 full-frame, detector +
  per-frame `BufBlitPa` compose into VENC input. Triggered only if the BSP
  rejects the port1 tap or the port0→VENC bind (1856–1876). This is the
  pre-`501d496` path whose heavy manual drain wedged the SoC
  (`_MI_SYS_MMU_Callback` storm → watchdog reset; see `star6e_stab_stop`
  comment 1949–1962).

### 2.3 Integration seams (calls from non-stab pipeline code)

| Seam | Site | Purpose | After legacy drop |
|---|---|---|---|
| `star6e_stab_enabled(vcfg)` | 3346, 3520, 3796 | gate: does framing take port0? | → `module->enabled` |
| `star6e_stab_setup_ports(...)` | 3359 | configure VPE ports | → `module->setup_ports` |
| `star6e_stab_start()` | 3365 | spawn detector thread | → `module->start` |
| `star6e_stab_apply_ae_crop()` | 3380 | seed AE window post-bind | → `module->apply_ae_crop` |
| `legacy_stab = enabled && !g_stab_hw_mode` | 3520 | OSD canvas dim + drain select | **removed** (always HW) |
| `star6e_stab_stop()` | 3675 | join thread, teardown | → `module->stop` |
| `imu_push` → `g_stab_imu_ring` | 3208 | IMU samples feed gyro seam | → `g_imu_ring` (D3) |
| `star6e_pipeline_stab_panel_anchor` | runtime 954, def 1002 | OSD crop-origin tracking | **removed** (HW returns 0 at 1019–1020 → already a no-op once HW-only) |
| `star6e_stab_set_pan` | 992, via zoomX/zoomY | live pan steer | → `module->set_pan` |
| `star6e_emit_ae_crop` | 2441 | generic AE-crop emit | stays in pipeline (host service) |

### 2.4 Field/UI machinery

- `FieldDesc` + `FIELD()` macro + `g_fields[]`: `src/venc_api.c:296–409`.
  `FieldDesc` = `{key, type, mut, offset, size}`.
- `/api/v1/capabilities` handler: `src/venc_api.c:2084`. Today emits, per
  field, `{mutability, supported}` only.
- Dashboard renderer: `web/dashboard.html` `renderSettings()` (996–1065).
  Reads field **value** from `/api/v1/config` and **mutability** from
  `/api/v1/capabilities`, both at runtime. Blob-static metadata: `SECTIONS[]`
  (707), `ENUMS[]`, `TOOLTIPS[]`, `inputAttrs()`, `fieldType()`. Dynamic-
  options precedent: `sensor.mode` from `/api/v1/modes` via `window._modeLabels`
  (1044).
- Blob: `src/venc_webui.c` is generated from `web/dashboard.html` by
  `tools/build_webui.py`; `make webui` regenerates, `make webui-check` (run by
  `make verify`) fails on drift.

## 3. Deliverable 1 — FramingModule registry (+ legacy removal)

### 3.1 Design decisions

- **D1 — Compile-time registry, not `dlopen`.** A `.so` plugin would need a
  stable ABI for `Star6ePipelineState`, the MI_SYS handles, and SDK symbol
  passing across the boundary — high cost, no benefit for a single embedded
  binary. We use a statically-linked module registering a
  `const FramingModule *`.
- **D2 — Two-way interface.** Pipeline→module via the `FramingModule` vtable.
  Module→pipeline via a narrow host-services header (`star6e_framing_host.h`)
  exposing only `star6e_emit_ae_crop`, the active `Star6ePipelineState *`, the
  IMU-ring accessor, and the precrop/zoom geometry getters `setup_ports`
  needs. No new global coupling.
- **D3 — IMU ring moves to the pipeline as a shared facility.** It is already
  documented (848–858) as populated independent of stabilization
  (telemetry/sidecar). Rename `g_stab_imu_ring` → `g_imu_ring`, keep
  `star6e_pipeline_imu_push` in the pipeline, expose
  `const ImuRing *star6e_pipeline_imu_ring(void)` for the module's detector.
- **D4 — Pan/zoom seam via the vtable.** Route the zoomX/zoomY live controls
  through `module->set_pan(x, y)` (no-op when the module ignores pan). Zoom
  presets and the pan-ramp thread are unaffected (D6).
- **D5 — Module owns AE-crop derivation.** `star6e_stab_apply_ae_crop` moves
  into the module and calls back via `star6e_emit_ae_crop`. The generic
  `star6e_emit_ae_crop` / `star6e_apply_ae_crop` / `ae_crop_mark_ready`
  machinery stays in the pipeline (zoom uses it too).
- **D6 — Zoom and pan-ramp stay in the pipeline.** They are not
  stabilization; they program `MI_VPE_SetPortCrop` directly and never call a
  `star6e_stab_*` function. Out of scope; they keep working when stab is
  absent.
- **D7 — `enabled()` stays in `prepare_pipeline_config`'s domain.** The 1080p
  source clamp (3796–3804) and framing-preset expansion remain pipeline-side;
  the module's `enabled()` only answers "is my preset selected and valid",
  matching today's `star6e_stab_enabled`.

- **D9 — Remove the legacy manual-drain; degrade safely on tap failure.**
  ⚠️ **This is the one runtime-behavior change in Deliverable 1 — explicit
  sign-off requested.** Today, if the port1 tap or the port0→VENC bind fails,
  stab silently falls back to the wedge-prone manual drain. After this change
  there is no manual-drain path at all. New failure behavior:
  - **port0→VENC bind fails** → genuine hardware error, return non-zero (same
    as today's other hard `SetPortMode`/`EnablePort` errors at 1790–1801).
  - **port1 detector tap fails** (the BSP-dependent step) → log a warning,
    keep port0 bound at the encoded crop dim, and **skip starting the detector
    thread**. The stream stays up at the configured stab framing as a *static*
    centre crop with no active shake compensation. Never the manual drain.

  Rationale: the only reason to keep the manual-drain is graceful degradation
  on a BSP that rejects port1 — but the manual-drain *is* the path that wedges
  the board, so it is a worse degradation than "static crop, no comp." All
  current shipping targets use the HW path (bench-verified), so this fallback
  is unexercised in practice. Recommended option: **static-crop degrade**
  (above). Alternative considered and rejected: hard-fail the whole pipeline
  start (loses the stream for a non-fatal feature).

- **D10 — Registered module, compiled in by default.** Per review decision,
  the stab module stays in `STAR6E_ONLY_SRC` and self-registers; no build-flag
  gate. Future extraction = remove the one `SRC` entry and the registration
  line; `star6e_framing_select()` then returns NULL and the pipeline runs the
  plain bound path.

### 3.2 Interface — `include/star6e_framing.h`

```c
/* Pipeline → module. One module is active per run, selected by matching
 * FramingModule.preset_name against video0.framing. All hooks run on the
 * pipeline thread except the detector thread the module owns. */
typedef struct FramingModule {
	const char *preset_name;        /* matched vs video0.framing, e.g. "stab" */

	int  (*enabled)(const VencConfig *);
	int  (*setup_ports)(Star6ePipelineState *, uint32_t src_fps,
	                     uint32_t dst_fps);
	int  (*start)(void);
	void (*stop)(void);
	void (*apply_ae_crop)(void);
	void (*set_pan)(double x, double y);   /* optional; NULL = ignores pan */
} FramingModule;

/* Registry. Built-in modules self-register from
 * star6e_framing_register_builtins(), called once in pipeline init. */
const FramingModule *star6e_framing_select(const VencConfig *);
```

The `panel_anchor` and `uses_manual_drain` hooks from the earlier draft are
gone: both existed only for the legacy path, which D9 removes.

### 3.3 Host services — `include/star6e_framing_host.h`

Pipeline internals the module needs, promoted from `static` to
backend-internal linkage (declared here, defined in `star6e_pipeline.c`):

```c
void  star6e_emit_ae_crop(const Star6eAeCropRect *);
Star6ePipelineState *star6e_framing_active_state(void);
const ImuRing *star6e_pipeline_imu_ring(void);
/* precrop/zoom geometry getters as needed by setup_ports */
```

No other `star6e_pipeline.c` static becomes visible — Phase 2 promotes each
unresolved symbol the linker reports, deliberately, no blanket `extern`.

### 3.4 What moves to `src/star6e_framing_stab.c`

- All `g_stab_*` statics **except** the IMU ring (D3) and **minus** the
  legacy-only symbols deleted in §3.6.
- The `STAB_*` compile-time defaults.
- The HW-path `star6e_stab_*` functions (configure, geometry helpers,
  make_center_y_crop, alloc_ive_image, estimate_shift, gyro_window,
  apply_port_crop, setup_ports, start, stop, thread_main, enabled) and
  `star6e_stab_apply_ae_crop`.
- `star6e_stab_load_sys_extra_symbols` (trimmed to the symbols the HW path
  actually uses — see §3.6).
- A `const FramingModule star6e_framing_stab` definition + registration.

`include/star6e_framing_stab.h` exposes only the module pointer/registration.

### 3.5 Pipeline integration changes

Resolve `const FramingModule *g_framing = star6e_framing_select(vcfg)` once in
`bind_and_finalize_pipeline`, then:

| Before | After |
|---|---|
| `if (star6e_stab_enabled(vcfg))` (3346) | `if (g_framing)` |
| `star6e_stab_setup_ports(...)` | `g_framing->setup_ports(...)` |
| `star6e_stab_start()` | `g_framing->start()` |
| `star6e_stab_apply_ae_crop()` | `g_framing->apply_ae_crop()` |
| `star6e_stab_stop()` (3675) | `if (g_framing) g_framing->stop()` |
| zoomX/zoomY → `star6e_stab_set_pan` | `if (g_framing && g_framing->set_pan) g_framing->set_pan(...)` |
| OSD `legacy_stab` branch (3519–3530) | deleted; OSD always uses `state->image_width/height` |
| runtime panel-anchor branch (954) | deleted |

### 3.6 Dead-code / legacy removal inventory (commit 2 of PR A)

Deleted outright (legacy-manual-drain-only):

- Functions: `star6e_stab_send_frame_to_venc` (1295), `star6e_stab_blit_nv12_crop`
  (1134), `star6e_stab_uv_pa` (1127), `star6e_stab_reapply_vpe_port` (1714),
  `star6e_pipeline_stab_panel_anchor` (1002, dead once HW-only per 1019–1020).
- Globals: `g_stab_hw_mode` and every `if (g_stab_hw_mode)` / `!g_stab_hw_mode`
  branch (collapse to the HW path), plus the legacy-only dlsym pointers
  `g_stab_sys_blit_pa`, `g_stab_sys_flush_inv_cache`, `g_stab_sys_va2pa`,
  `g_stab_sys_in_get_buf`, `g_stab_sys_in_put_buf`. **Kept** (HW detector
  uses them): `g_stab_sys_out_get_buf`, `g_stab_sys_out_put_buf`,
  `g_stab_sys_get_fd`, `g_stab_sys_close_fd`.
- The fallback block in `setup_ports` (1856–1876) → replaced by the D9
  static-crop degrade.
- The OSD legacy-offset seeding (3519–3530) and the runtime panel-anchor
  tracking branch (`star6e_runtime.c:954`).
- Header export `star6e_pipeline_stab_panel_anchor` (include/star6e_pipeline.h:136).

Net: the dead-code drop removes a few hundred additional lines beyond the
move and erases the wedge-prone path entirely.

### 3.7 Makefile

Add `src/star6e_framing_stab.c` to `STAR6E_ONLY_SRC` (Makefile:39) and to the
test lib list if a unit test links it. No build-flag gate (D10).

### 3.8 Verification

PR A lands as **two commits** for clean review/bisect:

1. **Pure move (behavior-neutral).** Extract HW + legacy code verbatim behind
   the vtable. `make verify`; on-device `framing=stab` regression at the
   imx335 bench (192.168.1.13) — confirm fps, recenter feel, clean teardown
   (no MMU faults). Runtime must be indistinguishable from pre-change.
2. **Legacy removal (D9).** Delete per §3.6. `make verify`; re-run the bench
   regression (HW path unchanged); confirm teardown still clean. The
   port1-failure path cannot be exercised on current hardware — document the
   new static-crop degrade in `STABILIZATION_TEST_PLAN.md` as untested-by-
   absence-of-failing-BSP.

Expected `star6e_pipeline.c` size after PR A: ~2,800 lines (−~1,200).

## 4. Deliverable 2 — Data-driven field schema (blob-free module UI)

### 4.1 Decision

- **D11 — Promote presentation metadata into the C field registry and emit it
  from `/api/v1/capabilities`.** The renderer already consumes that endpoint
  at runtime; making it the source of grouping/label/type/range/enum/tooltip
  lets a module's fields render with no `dashboard.html` edit and no blob
  rebuild.
- **D12 — Typed config stays.** Module fields remain real `VencConfig` fields
  in Deliverable 2, so the byte-exact printer and `test_save_layout_byte_equal`
  are untouched. Deliverable 2 removes the **blob-rebuild** requirement only.

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

```c
void venc_api_register_fields(const FieldDesc *fields, size_t n);
size_t venc_api_field_count(void);          /* core + registered */
const FieldDesc *venc_api_field_at(size_t);
```

The stab module registers its descriptors (with
`FieldUi.group = "framing:Stabilization"`) from its init. Per-field validators
(currently `venc_api.c:684–720`) move behind a validator hook so the module
owns its ranges and error strings. Everything iterating `g_fields[]` by static
`FIELD_COUNT` (capabilities, `/api/v1/config` enumeration, tests) switches to
`venc_api_field_count()`/`_at()`.

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
- Add one generic pass in `renderSettings()`: group capabilities entries
  carrying `ui.group` by group id, render a collapsible section per group from
  `ui.type/min/max/step/enum/label/tooltip`. Value from `localConfig[key]`,
  mutability badge from `capabilities[key].mutability`.
- **Blob rebuild semantics after this lands:** the blob is regenerated only to
  change the generic renderer itself. Adding/removing a framing module's
  fields needs **no** `dashboard.html` edit and **no** `make webui`.

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

- `make verify` (includes `make webui-check`): the static blob is unchanged by
  the C-only field-metadata work, except the one-time generic-renderer edit.
- Unit (`tests/test_venc_api.c`): assert `/api/v1/capabilities` emits the `ui`
  block for a registered field and omits it for a core field; assert
  `venc_api_field_count()` includes registered fields.
- Manual: load dashboard, confirm the dynamic "Stabilization" group renders
  from the endpoint with correct ranges/tooltips/live badges, and that a field
  added in C (no HTML edit) appears after rebuilding only the binary.

## 5. Risks & open questions

1. **D9 failure-mode (needs sign-off).** Dropping the manual-drain changes
   behavior on a BSP that rejects the port1 tap: from "manual-drain stab" to
   "static crop, no comp." Recommended as safer (the manual-drain is the
   wedge path), but it is a real, on-hardware behavior change and is
   unexercised on current targets. Confirm the static-crop degrade vs hard-
   fail choice.
2. **Hidden statics.** Some stab functions may touch pipeline statics not in
   §3.3. Resolution: compile the moved file and let the linker enumerate
   unresolved symbols; promote each deliberately — no blanket `extern`.
3. **IMU ring rename churn.** `g_stab_imu_ring` → `g_imu_ring` touches
   `imu_push` and the detector; keep `imu_ring_ready` gating identical.
4. **Validator move.** Stab range checks must move with the module without
   changing the error strings the API contract documents.
5. **`panel_anchor` removal.** Confirm the runtime caller (`star6e_runtime.c:954`)
   and any OSD assumptions tolerate its removal — it already returns 0 in HW
   mode, so the branch is dead, but verify no other caller exists.

## 6. Sequencing, versioning, and PRs

Two PRs, each its own `VERSION` bump + `HISTORY.md` entry (one bump per PR):

1. **PR A — FramingModule registry + legacy removal** (Deliverable 1). Two
   commits: (1) behavior-neutral move, (2) legacy/dead-code removal (D9).
   Self-contained, on-device-verifiable, delivers the ~1,200-line shrink and
   retires the wedge-prone path.
2. **PR B — Data-driven field schema** (Deliverable 2). Depends on A only for
   the stab field descriptors moving into the module; the endpoint/renderer
   work is otherwise independent. Delivers the blob-free module UI.

Deliverable 3 (params map) is deferred and not part of this plan unless
requested.

Docs to update in the implementing PRs: `documentation/HTTP_API_CONTRACT.md`
(capabilities `ui` block), `documentation/STABILIZATION_TEST_PLAN.md` (D9
degrade), `AGENTS.md` "Config / WebUI / API Sync Rules" (dynamic path), and
this file's status → IMPLEMENTED.
