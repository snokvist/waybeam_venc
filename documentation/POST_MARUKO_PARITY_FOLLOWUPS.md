# Post-Maruko-Parity Follow-Ups

Captured 2026-05-02 immediately after the eight-PR Maruko parity stack
landed on fork master (PRs #81, #83, #84, #85, #86, #87, #88, #89, plus
audit-discovered hotfix #90).  Master tip is `c3bb245`, version 0.9.15.

This file is a **plan, not a TODO list**.  Each section captures
opportunities — schema, REST API, cleanup — with enough context to pick
up later without re-deriving the analysis.  Items are sorted within
each section by perceived value × cost ratio (top = best ROI), but the
order is a starting point, not a commitment.

## 1. Schema additions — gap audit + checklist enforcement

### Background

PR #83 (Phase 9 throttle, `isp.aeMode`) landed five of the seven touch
points required to add a config field but missed the `g_fields[]` and
alias entries in `src/venc_api.c`.  The bug class is **silent**: the
field round-trips through `/etc/venc.json` and `/api/v1/config` (which
dumps the whole struct) but `/api/v1/get|set?<section>.<member>`
returns `404 unknown config field`.  PR #90 patched `isp.aeMode`
specifically; the underlying audit gap is wider.

The seven touch points (now also captured in user memory
`feedback_venc_schema_field_checklist.md`):

1. `include/venc_config.h` — struct member.
2. `src/venc_config.c::venc_config_defaults()` — compiled-in default.
3. `src/venc_config.c::parse_*_section()` — JSON parse.
4. `src/venc_config.c::pretty_print_*()` — human-readable dump.
5. `src/venc_config.c::venc_config_to_json_string()` — API JSON serializer.
6. `src/venc_api.c::g_fields[]` — `FIELD()` row.
7. `src/venc_api.c::g_field_aliases[]` — camelCase alias.

### Hot-spots to audit

- **Sweep every existing config field** for the same gap.  A targeted
  way: for each `FIELD(<section>, <member>, …)` in `venc_api.c`, grep
  the corresponding struct in `venc_config.h` and assert every member
  has a `FIELD` row OR is intentionally read-only (e.g. nested structs
  like `imu`).  Several fields landed pre-API-table and may still be
  reachable only through `/api/v1/config`.
- **Sub-struct fields** (`audio.*`, `imu.*`, `record.*`, `fpv.*`) are
  the highest-risk areas because they're the newest.  Spot-check:
  `audio.codec`, `audio.volume`, `imu.cal_file`, `record.server`,
  `record.bitrate`.
- **Fields touched by callbacks** but missing the `apply_*` /
  `query_*` plumbing in `VencApplyCallbacks`.  A `MUT_LIVE` field is
  meaningless if no backend implements the apply hook.

### Programmatic enforcement options

In rough order of effort:

- **(Low) Test fixture.** Extend `tests/test_venc_api.c` with a runtime
  assertion: every member visible through `venc_config_to_json_string()`
  must also resolve via `find_field()` OR be on an explicit
  exclusion list (e.g. nested struct anchors).  Catches the next #83.
- **(Medium) Codegen.** Replace `g_fields[]` with a generator that
  reads `venc_config.h` and emits the FIELD list.  Requires a struct
  metadata convention (e.g. `// API: live` or `// API: restart` doc
  comments parsed by a small Python script run from `make webui`'s
  preprocess step).
- **(High) Reflection-style schema.** Move the schema description
  into a single declarative table (CSV / TOML / JSON) and codegen
  both `venc_config.h` (as part of `venc_config_schema.h`) AND
  `g_fields[]` from it.  Also unblocks auto-generating
  `/api/v1/capabilities` instead of the hand-curated current form.

### Other schema cleanup

- `config_schema_version` is hard-coded to `"1.0.0"` in
  `src/venc_api.c` despite multiple field additions.  Decide a bump
  cadence (semver minor on every restart-required field?) or remove
  the field if it is not actionable.
- `contract_version` was stuck at `"0.3.0"` while the doc walked
  forward to `"0.8.3"` — fixed in PR #89 but indicates the constant is
  too easy to forget.  Consider lifting it into `HTTP_API_CONTRACT.md`
  itself (e.g. via a `make webui`-style preprocess step that injects
  the doc's stated version into `venc_api.c`).

## 2. REST API follow-ups

### Phase 6.5 — Maruko recording HTTP control (highest user value)

PR #89 makes `/api/v1/record/{start,stop}` return `501` on Maruko to
stop the silent-noop misbehavior.  The remaining work is to wire the
runtime poll loop and status callback so the API actually drives the
recorder on Maruko, parity with Star6E.

Concrete tasks:

- **Runtime poll loop.** Add `venc_api_get_record_start()` /
  `venc_api_get_record_stop()` calls inside
  `maruko_runtime.c::stream_loop()` mirroring
  `star6e_runtime.c:783,800`.  Each tick after a successful frame send,
  poll the request flags and call into the `MarukoBackendContext`'s
  `ts_recorder` object.
- **Record-status callback.** Implement `maruko_record_status_cb()`
  that reads from `ctx->ts_recorder` (and `ctx->dual->ts_recorder` in
  dual mode) and emits the `VencRecordStatus` struct.  Register it via
  `venc_api_set_record_status_fn()` in `maruko_runtime_init()`.  Once
  registered, the existing `record_http_supported()` gate in
  `venc_api.c` will let `/record/start|stop` through and
  `/record/status` will report live counters.
- **Mode-aware behavior.** When `record.mode="dual"` or
  `"dual-stream"`, mirror the Star6E pattern: HTTP `start/stop` are
  silently skipped because `ps->dual` owns the recorder.  Document in
  `MARUKO_PARITY_PLAN.md` Phase 6.5.

### `/api/v1/record/status` lying on Maruko

Currently returns all-zero even when `record.mode="mirror"` is
recording config-driven, because `g_record_status_fn` is null.  Fixed
implicitly by the work above — once a Maruko status callback exists,
status will reflect reality regardless of who started the recording.

### `/api/v1/audio/status` counter additions

PR #89 ships state-only fields (`enabled`, `lib_loaded`, `running`,
codec/rate/channels, `opus_loaded`).  Add observability counters that
already exist in capture state but are not yet exposed:

- `frames_captured` (uint64) — increment in
  `*_audio_capture_thread` after each successful read.
- `rtp_bytes_sent` (uint64) — accumulate in the encode/RTP send path.
- `encode_errors` (uint32) — bump when Opus encode returns negative or
  when the RTP packetizer truncates.
- `ring_drops` (uint32) — capture→encode bridge drops (already in
  `AudioRing` stats; just expose).

Star6E and Maruko already have parallel state structs; add the
counters once in each `*Audio.*State` struct, increment in the
existing thread bodies, and emit in the existing
`*_audio_query_status()` JSON builder.

### `/api/v1/imu/status` endpoint (POC consumer slot)

Phase 3 wired `imu_bmi270.c` on both backends, but the push callback
is a stub that discards samples.  When a real consumer lands
(telemetry export, sidecar `gcsv` logging, EIS Phase C), expose:

- `enabled` (bool)
- `chip_id` (string, e.g. `"BMI270"` or `"unavailable"`)
- `i2c_device` / `i2c_addr`
- `running` (bool)
- `samples_drained` / `samples_dropped` (uint64)
- `last_gyro_xyz` / `last_accel_xyz` (3-tuples in canonical SI units)

Defer until a consumer needs the data — adding a status endpoint in
front of a stub is busywork.

### Cross-backend `record_status_callback` proxy refactor

`g_record_status_fn != NULL` is currently used as the proxy for
"backend supports HTTP record control" (the gating in
`record_http_supported()` in `venc_api.c`).  Cleaner: add an explicit
`bool record_http_supported` flag to `VencApplyCallbacks` and key the
gate on that.  Keeps the callback-registration semantics from
implicitly carrying capability info.

### `/api/v1/transport/status` counter completeness

The Star6E-side `pressureDrops` counter is reset between sidecar
subscriptions.  Audit whether `/transport/status` should expose
lifetime totals separately from the in-subscription counter.  Same for
`fillPct` — the SHM ring path returns ring-internal fill, but the
UDP/Unix path returns kernel-buffer fill; consumers may want a unified
"output backpressure 0..100" instead.

## 3. Cleanup / simplification

### Cross-backend code duplication (highest value)

Recurring pattern from this stack: every Phase ports a Star6E feature
to Maruko by writing a parallel `maruko_*.c` file with ~80% identical
control flow and ~20% MI-API differences.  The audit pass already
extracted `audio_codec.{c,h}` (Opus / G.711 / stdout filter) and
`star6e_recorder.c`/`star6e_ts_recorder.c`/`ts_mux.c` into a shared
`RECORDER_SRC` build group.  Extend the pattern to:

- **`audio_codec_name(int codec_type)`** is duplicated in
  `src/star6e_audio.c` and `src/maruko_audio.c` (added by PR #89).
  Lift into `audio_codec.h` as `audio_codec_type_name()` and call from
  both query functions.
- **`*_audio_query_status()`** — the two functions are nearly
  identical; the only divergence is field name (`device_enabled` vs
  `device_opened`, `channel_enabled` vs `group_enabled`).  Could share
  a builder that takes a small `struct AudioStatusFields` and lets
  each backend pass the labels it wants.
- **`*_query_transport_status()`** — same pattern.  Worth checking
  whether the Star6E and Maruko bodies can share a helper that takes
  the `Output` pointer + a function-table for ring vs socket fill.
- **`*_controls.c` callback wiring** — the Star6E and Maruko callback
  registration tables are 90% the same.  Could share an init macro
  that fills in everything except the `apply_*` overrides.

### Dead / stale doc references

- `documentation/CURRENT_STATUS_AND_NEXT_STEPS.md` claims Maruko
  recording / audio / IMU are not yet ported.  Master has all three;
  refresh after merge.
- `documentation/DUAL_BACKEND_SPLIT_PLAN.md` "Maruko Follow-Up Backlog"
  section is older than this audit and may have stale items.
- README JSON example `config_schema_version` and `contract_version`
  examples are stale — though they are illustrative not
  prescriptive.
- `documentation/PRECROP_ASPECT_RATIO.md` was written for Star6E only;
  Phase 1 ported precrop to Maruko but the doc may not reflect that.
- `Makefile` `HELPER_SRC` / `MARUKO_ONLY_SRC` / `STAR6E_ONLY_SRC` /
  `RECORDER_SRC` lists are getting unwieldy; consider splitting into
  per-feature `*.mk` includes (audio, recorder, imu, debug_osd) once
  another similar group lands.

### Unused or near-unused code

- The `imu_bmi270.c` push callback is a stub on both backends.  Either
  wire a real consumer (telemetry export or sidecar gcsv) or mark the
  callback as planned-future and add a TODO comment with a link to
  `EIS_INTEGRATION_PLAN.md`.
- `vendor-libs/maruko/libmi_ao.so` is shipped but not loaded yet
  (Phase 5b deferred).  Either wire AO playback or drop the lib until
  a use case appears.

### HTTP_API_CONTRACT.md vs WebUI dashboard

`src/venc_webui.c` ships a hand-rolled "API Reference" tab.  This is a
known drift surface — every new endpoint needs an entry both in
`HTTP_API_CONTRACT.md` and in the embedded HTML.  Audit the dashboard
HTML against the contract doc; anything missing is a bug.  Long-term:
generate the dashboard's API Reference tab from `HTTP_API_CONTRACT.md`
in `make webui`.

### Format-truncation audit

PR #87's `record.server` was a `char[128]` truncating from a
`char[256]` source — caught by `make test-ci`'s asan build with
`-Werror=format-truncation`.  Sweep all `MarukoBackendConfig` /
`Star6e*Config` string members against their `VencConfig` sources and
either widen them to match or document the intentional truncation.

## 4. Hot-spots to revisit

These are the files / patterns that have absorbed the most churn this
session and are most likely to harbor subtle regressions.  Worth a
fresh pair of eyes when there's downtime.

| File | Why it's hot |
|---|---|
| `src/maruko_pipeline.c` | Touched by every Maruko Phase (1, 2b, 3, 5, 6, 7, 9).  954 insertions / 50 deletions in the merge diff.  Init ordering is sensitive (IMU before VENC, AE adaptor before SetFps, `bind_maruko_pipeline` before audio init).  Refactor candidate. |
| `src/maruko_config.c` + `include/maruko_config.h` | Six new config sections in one stack.  Watch for size mismatches and missing default-init lines. |
| `src/venc_api.c` | `g_fields[]` + alias table grew without a checklist; PR #90 caught one gap.  Test fixture is the cheapest hedge against the next one. |
| `Makefile` | Source-list groups grew; conflicts during the rebase chain were entirely in this file.  Consider per-feature `.mk` fragments. |
| `HISTORY.md` + `VERSION` | Both changed by every Phase; chronological prepend → easy to merge wrong. |

## 5. Out of scope (intentionally deferred)

- **Phase 4 — live AR-change reinit on Maruko.**  Medium-architectural,
  unblocks per-channel resolution.  Lower priority now that Phase 7
  provides a separate channel for recording at a different resolution.
- **Star6E adoption of the `isp.aeMode="throttle"` no-op AE adaptor
  pattern.**  Star6E doesn't have the same CPU cost profile (different
  SoC clock + different SDK version), and the Maruko-side overhead
  shows the cost of the workaround; revisit only on a specific
  Star6E perf request.
- **AO playback path on Maruko (Phase 5b).**  Vendored lib but no
  consumer; defer until a use case appears.
- **Live mode switch for `isp.aeMode`.**  The no-op AE adaptor install
  and `CUS3A_Enable` flags are init-only; switching modes mid-run
  would require a full pipeline reinit.  Process restart is the
  documented path.

## How to use this document

When picking up a follow-up:

1. Read the relevant subsection in full — context matters more than
   the bullet titles.
2. Check whether the item is still relevant (hot-spots and stale-doc
   findings rot quickly).
3. If still relevant, open a focused PR.  Keep the scope narrow:
   schema audits in one PR, code dedup in another.
4. Cross off (delete) the entry in the same PR so this doc stays a
   live shopping list rather than a graveyard.
