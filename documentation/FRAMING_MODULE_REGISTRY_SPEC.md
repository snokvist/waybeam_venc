# Spec: Framing-Module Registry + stab-fill Port + Data-Driven Field Schema

Status: **DRAFT — awaiting review.** No code yet.
Scope: Star6E backend (stabilization is Star6E-only).
Author workflow: Phase 1 (Spec). Do not implement until approved.

## 1. Motivation

`src/star6e_pipeline.c` is 4,008 lines; ~1,100 of them (the `star6e_stab_*`
block, ~40 `g_stab_*` statics, lines 860–1982 plus `star6e_stab_apply_ae_crop`)
implement image stabilization, deeply interwoven with the pipeline's
bind/teardown/IMU/AE-crop paths. Adding a stabilization knob touches five
layers and forces a WebUI gzip-blob rebuild; removing the feature later is wide
surgery rather than a one-file delete.

This spec isolates stabilization behind one compile-gated module and folds in
tipoman9's `stab-fill` floating-image mode (PR #136) as a second preset, so the
encoder ships both looks (HW-crop shrink **and** floating-image-on-black) while
the whole stabilization surface becomes cheap to maintain, cheap to disable at
build time, and — for the UI — addable without rebuilding the blob.

### Deliverables

1. **FramingModule registry + compile flag + legacy removal.** Move master's
   `stab` (HW-crop) code into `src/star6e_framing_stab.c` behind a vtable, gated
   by a `STAB` build flag (default **on**). Drop the legacy manual-drain
   fallback and the dead code it carried.
2. **Port `stab-fill` into the module.** Bring tipoman9's floating-image
   compose (Kalman, threaded blit) in as a second registered preset, reconciled
   onto master's stab base, with the live-rebind `pauseStab` replaced by the
   software ramp (per the PR #136 review).
3. **Data-driven field schema.** Extend `/api/v1/capabilities` + a generic
   renderer so module fields render without editing `dashboard.html` or running
   `make webui`.
4. *(Optional, deferred)* generic params-map config to drop the typed-struct
   coupling entirely.

### Goals

- **Painless build-time removal.** `make build STAB=0` excludes the module
  file entirely; the registry `select()` returns NULL and the pipeline runs the
  plain bound path. No `#ifdef` scattered through the pipeline.
- **Both stabilization looks shipped.** `framing="stab"` (HW-crop shrink,
  high-fps, ≤1080p) and `framing="stab-fill"` (constant-scale floating image,
  black border, ~43 fps) as distinct presets — they are different tradeoffs,
  not substitutes.
- **Shed the wedge-prone path.** Remove the legacy single-thread manual-drain
  fallback (`501d496` introduced HW-crop precisely to fix the teardown wedge it
  caused). `stab-fill`'s *threaded, teardown-hardened* compose is retained —
  it is a different, bench-validated path.
- **Behavior-neutral moves where possible.** The registry extraction (D1) and
  the stab-fill port (D2) are separated into their own commits/PRs; the
  extraction itself must not change the HW-crop runtime.

### Non-goals

- No `dlopen` runtime plugins (D1: compile-time registry only).
- No change to the *tuning math* of either preset on its happy path.
- No Maruko work (stab is Star6E-only).
- Deliverable 3 does **not** remove the typed-config coupling; that is the
  optional Deliverable 4 (§6.4).

## 2. Current state (reference)

### 2.1 master `stab` footprint in `star6e_pipeline.c`

| Element | Location |
|---|---|
| `g_stab_*` statics (~40) | 785–858 |
| `STAB_*` compile-time defaults | 768–783 |
| `star6e_stab_*` functions | 860–1982 |
| `star6e_stab_apply_ae_crop` | 2540 |

HW-crop vs legacy manual-drain split (`star6e_stab_setup_ports`, 1762): HW path
(`g_stab_hw_mode=1`) binds port0→VENC zero-copy with a port1 IVE tap; the
legacy fallback (`g_stab_hw_mode=0`, full-frame port0 + per-frame blit) fires
only if the BSP rejects port1 — and is the pre-`501d496` path whose manual
drain stormed the MMU into a watchdog reset (see `star6e_stab_stop`, 1949–1962).

### 2.2 `stab-fill` (PR #136, tipoman9, NOT on master)

PR #136 head `8382744`, base `feature/star6e-stabilization` `b57c4d0`. **That
base diverged from master** — master cherry-picked/reworked the stab line in
#122 and consolidated to a single `stab` mode (#129–#135). So stab-fill must be
**ported** onto master's stab structure inside the new module, not cherry-picked.

New symbols stab-fill adds (from the PR #136 `star6e_pipeline.c` diff):

| Group | Symbols |
|---|---|
| Compose | `star6e_stab_fill_i8_rect`, `star6e_stab_send_frame_to_venc_fill`, `g_stab_sys_fill_pa` (`MI_SYS_BufFillPa`) |
| Threaded blit | `star6e_stab_blit_thread_main`, `star6e_stab_fill_queue_blit`, `g_stab_blit_thread/active/lock/cond_in/cond_out`, 2-slot `sw_detect` ring |
| Kalman | `g_stab_kalman_{x,y}_{est,p}`, `g_stab_kalman_{q,r}`, `g_stab_kalman_lock`, `STAB_KALMAN_{Q,R}_DEFAULT` |
| Mode/lifecycle | `star6e_stab_setup_ports_fill`, `star6e_stab_fill_enabled`, `g_stab_fill_mode`, `g_stab_detector_enabled`, `g_stab_verbose` |
| Live tuning | `star6e_pipeline_set_kalman(q,r)`, `star6e_pipeline_set_pause_stab(bool)` |
| pauseStab HW-bind | `g_stab_path_lock`, `g_stab_paused`, `g_stab_bind_chn`, `g_stab_bind_src_fps`, `g_stab_bind_dst_fps` |

`stab-fill`'s compose **legitimately uses** `BufFillPa`/`BufBlitPa`/`va2pa`/
`in_get_buf`/`in_put_buf` — symbols that were master-`stab`-legacy-only.

### 2.3 Integration seams & field/UI machinery

Seams (unchanged from prior draft): `enabled`/`setup_ports`/`start`/`stop`/
`apply_ae_crop` in `bind_and_finalize_pipeline` (3346–3380, 3796), `stop`
(3675), IMU push → `g_stab_imu_ring` (3208), pan via `star6e_stab_set_pan`
(992), AE emit via `star6e_emit_ae_crop` (2441). `FieldDesc`/`g_fields[]`
(venc_api.c 296–409), `/api/v1/capabilities` (2084), dashboard `renderSettings`
(996–1065) with static `SECTIONS[]`/`ENUMS`/`TOOLTIPS`; blob via
`tools/build_webui.py`, guarded by `make webui-check`.

## 3. Deliverable 1 — FramingModule registry + compile flag + legacy removal

### 3.1 Design decisions

- **D1 — Compile-time registry, not `dlopen`.** Statically-linked module
  registering a `const FramingModule *`. (A `.so` would need a stable ABI over
  `Star6ePipelineState` + MI handles — no benefit for one binary.)
- **D2 — Two-way interface.** Pipeline→module via the `FramingModule` vtable;
  module→pipeline via a narrow `star6e_framing_host.h` (`star6e_emit_ae_crop`,
  active `Star6ePipelineState *`, `star6e_pipeline_imu_ring()`, precrop/zoom
  geometry getters). Phase 2 promotes each linker-reported symbol deliberately
  — no blanket `extern`.
- **D3 — IMU ring → pipeline facility.** Rename `g_stab_imu_ring` → `g_imu_ring`
  (already populated independent of stab for telemetry); expose
  `const ImuRing *star6e_pipeline_imu_ring(void)`.
- **D4 — Pan via vtable.** zoomX/zoomY → `module->set_pan(x,y)` (NULL = ignore).
  Zoom presets + pan-ramp stay pipeline-side (they program `SetPortCrop`
  directly, never call stab).
- **D5 — Module owns AE-crop derivation** (`star6e_stab_apply_ae_crop` → module,
  calls back `star6e_emit_ae_crop`). Generic AE-crop machinery stays.
- **D9 — Remove the legacy manual-drain; degrade safely on port1-tap failure.**
  ✅ approved. New behavior: port0→VENC bind failure = hard error (as today);
  port1 tap failure = log + keep port0 bound at the crop dim + skip the detector
  → static crop, no shake comp. Never the manual drain. (`stab-fill` is **not**
  the manual drain — see §3.6.)
- **D10 — Compile flag, default enabled.** ✅ approved. A `STAB` build flag
  (default `1`) gates the module's `SRC` entry **and** its registration call.
  `make build STAB=0` produces a stab-free binary with no stab code linked;
  `framing="stab"/"stab-fill"` then validate-reject (fall to `off`). Implemented
  as a Makefile conditional appending `src/star6e_framing_stab.c` to
  `STAR6E_ONLY_SRC` and defining `-DHAVE_FRAMING_STAB=1`; the one registration
  site is `#if HAVE_FRAMING_STAB`-guarded. This is the *only* preprocessor gate
  — internal pipeline code stays `#ifdef`-free.

### 3.2 Interface — `include/star6e_framing.h`

```c
typedef struct FramingModule {
	const char *preset_name;        /* matched vs video0.framing */
	int  (*enabled)(const VencConfig *);
	int  (*setup_ports)(Star6ePipelineState *, uint32_t src_fps,
	                     uint32_t dst_fps);
	int  (*start)(void);
	void (*stop)(void);
	void (*apply_ae_crop)(void);
	void (*set_pan)(double x, double y);          /* optional */
	int  (*set_live)(const char *key, const char *val); /* optional: kalman/pause */
} FramingModule;

const FramingModule *star6e_framing_select(const VencConfig *);
void star6e_framing_register_builtins(void);   /* #if HAVE_FRAMING_STAB body */
```

`set_live` carries the stab-fill live fields (Kalman Q/R, pause) from the API
layer to the module without the pipeline knowing their semantics. The
`panel_anchor`/`uses_manual_drain` hooks from earlier drafts are gone (legacy
removed, and the HW path's panel anchor was already a no-op — §3.6).

### 3.3 What lands in `src/star6e_framing_stab.c`

One module file, **two** registered `FramingModule` entries (`"stab"`,
`"stab-fill"`) sharing the IVE detector, geometry helpers, IMU-ring access, and
Kalman state. Contains: the HW-crop `stab` path (D1 scope), the stab-fill
compose + threaded blit + Kalman (D2 scope), `STAB_*` defaults,
`star6e_stab_load_sys_extra_symbols` (trimmed to symbols actually used), and the
registration. `include/star6e_framing_stab.h` exposes only the registration.

### 3.4 Pipeline integration changes

Resolve `g_framing = star6e_framing_select(vcfg)` once in
`bind_and_finalize_pipeline`; replace the inline `star6e_stab_*` calls with
`g_framing->...`. Delete the OSD `legacy_stab` branch (3519–3530; OSD always
uses `state->image_width/height`) and the runtime panel-anchor branch
(`star6e_runtime.c:954`).

### 3.5 Makefile

```make
STAB ?= 1
ifeq ($(STAB),1)
  STAR6E_ONLY_SRC += src/star6e_framing_stab.c
  CFLAGS += -DHAVE_FRAMING_STAB=1
endif
```

Extraction later = `make build STAB=0`, or delete the file + the conditional.

### 3.6 Dead-code / legacy removal inventory (Deliverable 1, commit 2)

**Deleted** (master-`stab` legacy-manual-drain only):
- `star6e_stab_send_frame_to_venc` (the non-`_fill` legacy drain, 1295),
  `star6e_stab_blit_nv12_crop` (1134), `star6e_stab_uv_pa` (1127),
  `star6e_stab_reapply_vpe_port` (1714),
  `star6e_pipeline_stab_panel_anchor` (1002 — already returns 0 in HW mode at
  1019–1020, so dead once HW-only) + its header export + runtime caller (954).
- `g_stab_hw_mode` and all its branches (collapse to the HW path).
- The `setup_ports` fallback block (1856–1876) → replaced by the D9 degrade.
- OSD legacy seeding (3519–3530).

**Retained** (now used by the stab-fill port, D2 — *not* dead):
- `g_stab_sys_blit_pa`, `g_stab_sys_va2pa`, `g_stab_sys_in_get_buf`,
  `g_stab_sys_in_put_buf`, `g_stab_sys_flush_inv_cache`, plus the new
  `g_stab_sys_fill_pa`. The HW detector keeps `out_get_buf/out_put_buf/
  get_fd/close_fd`.

Note: in Deliverable 1 (master `stab` only, before the port), the blit symbols
have no caller and would be flagged unused. To keep D1 a clean compile,
**land Deliverable 1 and 2 in close succession**, or temporarily keep the
symbol loads under D2's incoming code. Sequencing (§6) lands D2 immediately
after D1 on the same branch.

### 3.7 Verification

Deliverable 1 lands as two commits: (1) behavior-neutral move of master `stab`
behind the vtable + flag; (2) legacy removal (D9). `make verify` (build both
`STAB=1` and `STAB=0` for Star6E); on-device `framing=stab` regression at the
imx335 bench — fps, recenter feel, clean teardown (no MMU faults).

## 4. Deliverable 2 — Port `stab-fill` into the module

### 4.1 Design decisions

- **D11 — Port, don't cherry-pick.** PR #136's base diverged from master
  (§2.2). Phase 2 fetches the PR #136 head source, and re-implements the
  stab-fill functions against master's stab globals/helpers inside
  `star6e_framing_stab.c`. The compose math, Kalman, and threaded-blit ring are
  carried verbatim where the underlying helpers match; geometry/port setup is
  adapted to master's `star6e_stab_*` helpers.
- **D12 — `stab` and `stab-fill` are distinct presets, both kept.** Per the PR
  #136 review: HW-crop shrink (high-fps, ≤1080p clamp) vs constant-scale
  floating image (black border, ~43 fps). Neither replaces the other.
- **D13 — `pauseStab` = software ramp, drop the live HW-bind.** ✅ confirmed.
  Per the PR #136 review, replace the `MI_VENC_StopRecvPic` +
  `BindChnPort2`/`UnBind` live rebind (the maneuver that wedged the SoC) with a
  software flag in the detector that ramps the applied offset to 0 via the
  existing recenter ramp. Effective next tick (~33 ms), stays at ~43 fps while
  paused (no 58-fps reclaim), no thread teardown, no rebind. **Delete**
  `g_stab_path_lock`, `g_stab_bind_chn`, `g_stab_bind_src_fps`,
  `g_stab_bind_dst_fps`, and the StopRecvPic/Bind/UnBind orchestration; **keep**
  `g_stab_paused` as the software flag.
- **D14 — Kalman Q/R folded into the preset by default.** Per the review's
  product direction ("known-good presets, not a wall of knobs"), bake
  `STAB_KALMAN_Q/R_DEFAULT` into the `stab-fill` preset and do **not** expose
  `stabKalmanQ/R` as fields initially. Deliverable 3 makes exposing them later a
  blob-free, one-line change if community demand appears. `pauseStab` stays a
  live field (`set_live`).

### 4.2 Config plumbing (new `stab-fill` preset)

master has no `stab-fill` row. Add it to the framing preset table
(`venc_config.c:478`), expanding into the stab-fill geometry (full-sensor
source, SCL to encode dim, `stabCropPct` → border budget). Per the **Config /
WebUI / API Sync Rules**, the single new live field `pauseStab` (no persist)
threads through: `VencConfig` + parser/printer, `g_fields[]`/`g_aliases[]`
(MUT_LIVE), `config/waybeam.default.json`, validators, and — until Deliverable 3
lands — `SECTIONS[]` + `make webui`. After Deliverable 3, further stab-fill
fields are blob-free.

### 4.3 Interface use

`stab-fill`'s `FramingModule.set_live` handles `pauseStab` (and, if ever
exposed, `stabKalmanQ/R`). `setup_ports` = `setup_ports_fill`; `start` spawns
the detector + blit threads; `stop` joins blit-then-detector (MMU-safe order
from the PR). `apply_ae_crop` reuses the shared derivation.

### 4.4 Verification

`make verify` (`STAB=1`/`STAB=0`). On-device: `framing=stab-fill` at the imx335
bench — confirm the floating-image-on-black look, ~43 fps steady state, live
`pauseStab=true/false` glides to/from center with no rebind and no MMU faults,
clean teardown. Document results in `STABILIZATION_TEST_PLAN.md` (T7).

## 5. Deliverable 3 — Data-driven field schema (blob-free module UI)

(Unchanged in intent from the prior draft.)

- **D15** — promote presentation metadata into the C field registry, emit from
  `/api/v1/capabilities`; keep typed config (`test_save_layout_byte_equal`
  intact). Removes only the blob-rebuild requirement.
- `FieldDesc` gains an optional `const FieldUi *ui` (group/label/type/min/max/
  step/enum/tooltip; NULL → core field, static `SECTIONS[]` path).
- `venc_api_register_fields()/_count()/_at()` so a module contributes fields;
  per-field validator hook moves ranges/messages into the module. Everything
  iterating `g_fields[]` by `FIELD_COUNT` switches to the accessors.
- `/api/v1/capabilities` emits a `ui` block when present (backward compatible).
- Dashboard: keep static `SECTIONS[]` for core; add one generic pass that
  groups `ui.group` entries into collapsible sections. After this, adding a
  framing-module field needs no `dashboard.html` edit and no `make webui`.
- Verify: `make verify` (`webui-check` confirms blob untouched bar the one-time
  renderer edit); `tests/test_venc_api.c` asserts the `ui` block + accessor
  counts; manual dashboard check.

## 6. Sequencing, versioning, and commits

**One PR**, landing Deliverables 1–3 as a sequence of self-contained,
individually-verified commits (Deliverable 4 deferred). One `VERSION` bump +
one `HISTORY.md` entry for the PR (squash any intermediate version churn — see
**Versioning Policy**). Each commit must `make verify` clean on its own so the
branch stays bisectable.

Commit order:

1. **Registry + compile flag (behavior-neutral move).** Move master `stab`
   into `src/star6e_framing_stab.c` behind the vtable + `STAB` flag. No
   behavior change on the HW-crop path. ~1,100-line shrink.
2. **Legacy-drain removal** (D9, §3.6). Drop the manual-drain fallback + dead
   code; static-crop degrade on port1 failure. To keep this commit
   `-Werror`-clean before the stab-fill caller arrives, the retained blit
   symbols (§3.6) land already wired into commit 3's incoming code, or their
   loads are guarded — sequence 2→3 with no gap.
3. **Port stab-fill** (Deliverable 2). Second preset, software-ramp `pauseStab`
   (D13), Kalman folded (D14); new `stab-fill` framing preset + `pauseStab`
   field plumbing.
4. **Data-driven field schema** (Deliverable 3). `FieldDesc.ui` + registry +
   `/api/v1/capabilities` `ui` block + generic renderer → blob-free module UI.

Deliverable 4 (generic params-map config: move plugin config to
`video0.framingParams` KV to drop the struct/printer/`default.json` touch)
remains a separate future PR — it trades type-safety + the byte-exact layout
test and is adopted only if a real multi-module ecosystem appears.

Docs updated within the PR: `HTTP_API_CONTRACT.md` (capabilities `ui`,
`pauseStab`), `STABILIZATION_TEST_PLAN.md` (stab-fill + D9 degrade),
`AGENTS.md` Config/WebUI/API rules (dynamic path + `STAB` flag), and this file's
status → IMPLEMENTED.

## 7. Risks & open questions

1. **D13 pauseStab simplification (confirmed).** Replacing the live HW-bind
   with the software ramp loses the paused 58-fps reclaim but removes the
   board-wedge maneuver. Matches the PR #136 review.
2. **stab-fill port reconciliation (D11).** Divergent bases mean the port is
   hand-reconciled, not a merge. Risk: subtle geometry/teardown differences
   between master's stab helpers and PR #136's. Mitigation: port behind the same
   bench regression; keep the PR's MMU-safe join order verbatim.
3. **D1/D2 link-cleanliness (§3.6).** The retained blit symbols are unused
   between PR A and PR B. Land B promptly, or guard the symbol loads so PR A
   compiles `-Werror` clean on its own.
4. **Hidden statics / validator move / IMU rename** — promote linker-reported
   symbols deliberately; keep `imu_ring_ready` gating and API error strings
   identical.
5. **`STAB=0` build path.** Confirm `framing` validation rejects `stab`/
   `stab-fill` cleanly (falls to `off`) and no other code assumes the module
   exists when `HAVE_FRAMING_STAB` is unset.
