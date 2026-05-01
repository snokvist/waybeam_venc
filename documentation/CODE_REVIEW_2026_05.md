# waybeam_venc Code Review — May 2026

**Branch reviewed:** `feature/v0.9.2-upstream` @ `2aa290a`
**Scope:** `src/`, `include/`, `libs/star6e/*.{c,h}`, `libs/maruko/*.{c,h}` — vendor trees skipped.
**Method:** three parallel reviewers (embedded-C bug scan, refactor advisor, perf/architecture). Findings deduplicated and ranked.

This document is the **umbrella tracker** for follow-up PRs. Each item lists
file:line, impact, and a concrete fix. Implementation PRs should reference
this doc by section heading (e.g. "Closes Bug #1 in CODE_REVIEW_2026_05.md").

---

## Bugs — correctness, crash/corruption under realistic conditions

### Bug #1 — `.ts` recorder silent corruption on oversized frames
- **Location:** `src/star6e_ts_recorder.c:363-388`
- **Impact:** Truncation handler `break`s only the inner pack loop; outer
  `for (i)` over `stream->packet[]` continues and may append later packs
  after a gap, producing a broken bitstream that downstream muxers happily
  write to disk. The existing `truncated` log line gives a false sense the
  truncation is bounded.
- **Fix:** Replace inner `break` with `goto done` (or zero `nal_len` and
  skip writing this frame entirely).

### Bug #2 — UDP socket fd UAF in immediate-send fallback
- **Location:** `src/star6e_output.c:514-523` (mirror in `maruko_output.c`)
- **Impact:** `apply_server()` runs on the HTTP thread, closes
  `output->socket_handle` and reuses the fd number, while the encoder-thread
  *immediate-send fallback* path reads `output->socket_handle` / `->dst`
  without going through the seqlock snapshot held in `b->socket_handle` /
  `b->dst`. Concurrent close + reopen between the read and `sendmsg` can
  send to a closed fd or the wrong destination. Manifests as
  "RTP went to old destination after `/set`" — hard to repro on bench.
- **Fix:** Always read through the per-frame snapshot taken in
  `begin_frame`, or move `apply_server` to a deferred queue applied between
  frames on the encoder thread. Best done as part of the
  `output_common.c` extraction (Improvement #1 / Cleanup #1).

### Bug #3 — half-MB stack arrays in TS recorder
- **Location:** `src/star6e_ts_recorder.c:241,326`
- **Impact:** `uint8_t ts_buf[3000*188]` (~564 KB) and
  `uint8_t nal_buf[512*1024]` (512 KB) on the encoder-thread stack. Works
  on glibc default 8 MB stacks; instant overflow on musl / RT-priority
  threads / smaller configured stacks. One toolchain swap on the OpenIPC
  builder hides this until the first IDR.
- **Fix:** Move both buffers to persistent fields in
  `Star6eTsRecorderState` (already long-lived). Same fix nets ~10–30 µs/frame
  during recording — see Perf #2.

### Honorable mentions
- `src/venc_api.c:2152` — `handle_record_start` dereferences
  `g_cfg->record.dir[0]` without the NULL guard the rest of the file uses.
- `include/venc_ring.h:273-274` — `venc_ring_read` silently truncates
  (`len = buf_size`) when consumer buffer < slot payload, advances
  `read_idx` anyway, no counter, no error returned.
- `src/venc_httpd.c:364` — `httpd_strcasestr(headers, "content-length:")`
  is unanchored; an `X-Original-Content-Length:` header smuggles past it.

---

## Issues — latent / fragile, works today but easy to break

### Issue #1 — `g_cfg_mutex` held across blocking syscalls + torn-string reads
- **Location:** `src/venc_api.c:1577` and `src/venc_api.c:1264-1294`
- **Impact:** `apply_live_group_sequence_locked` invokes `apply_server`
  (which performs `socket()`/`connect()`/`close()`) while holding
  `g_cfg_mutex`. Worse: `*g_cfg = new_cfg` mutates strings (`outgoing.server`,
  `awb_mode`) that the encoder thread reads without synchronization —
  torn-string reads at 120 fps.
- **Fix:** Snapshot `g_cfg` into a per-frame local at the top of
  `process_stream`, or use a shadow-pointer swap with RCU semantics. Never
  call user-supplied `apply_*` callbacks under `g_cfg_mutex`.

### Issue #2 — torn pointer load on `state->rec_ring`
- **Location:** `src/star6e_runtime.c:787,792,803` ↔ `src/star6e_audio.c:331`
- **Impact:** `state->rec_ring` set/cleared from the encoder thread, read
  from the audio encode thread, no atomic / no lock. Torn pointer load on
  32-bit ARM. Underlying ring isn't destroyed until teardown so worst case
  today is a few stale frames pushed after stop, but as soon as the ring
  lifecycle tightens this becomes a UAF.
- **Fix:** `__atomic_store_n` / `__atomic_load_n` with `memory_order_release` /
  `_acquire`.

### Issue #3 — SHM single-instance race
- **Location:** `src/venc_ring.c:68-69`
- **Impact:** `shm_unlink` then `O_EXCL` open for single-instance
  enforcement races against another process's `is_another_venc_running()`
  check. `prctl(PR_SET_NAME,"venc")` happens in `main()` after any in-flight
  peer's name probe may have already passed.
- **Fix:** Pidfile + `flock` for single-instance enforcement, or
  `O_CREAT|O_EXCL` plus `flock` on the SHM fd itself.

### Honorable mentions
- `src/star6e_output.c:392-407` — seqlock spin without `cpu_relax` /
  `sched_yield`; if HTTP thread is preempted between the two
  `transport_gen` increments, encoder thread burns a core spinning.
- `src/star6e_runtime.c:158` — `g_signal_count++` in handler is non-atomic
  on `sig_atomic_t`; near-simultaneous SIGINT+SIGTERM can drop one.
- `src/venc_api.c:1018-1095` — `parse_query_params` does not deduplicate
  keys; duplicate `server=...` causes `apply_server` to fire twice
  (= two full UDP socket close+open cycles).

---

## Improvements — architecture

### Improvement #1 — extract `output_common.c`
- **Location:** `src/star6e_output.c:256-430` ↔ `src/maruko_output.c:78-263`
- **Problem:** `batch_flush`, `observe_pressure`, `drain_send_errors`,
  `apply_server`, `begin_frame`, `end_frame` are byte-for-byte identical
  between backends except for struct prefix. ~130 LOC of active drift
  surface. Tier C (sendmmsg) and sidecar idle servicing are currently
  Star6E-only, partly because the duplication makes parity fixes painful.
- **Proposed shape:** Lift into `src/output_common.c` keyed on a
  `void *opaque + fd_provider_fn` (or a small vtable). Backends keep only
  their `Output` struct. This is also the natural moment to fix Bug #2.

### Improvement #2 — NAL iteration via vtable
- **Location:** `src/star6e_video.c:73` (`*_send_frame_hevc`) vs
  `src/maruko_video.c:96`
- **Problem:** Same `pack->packNum` / `packetInfo[k]` / `strip_start_code` /
  `nal_type` walk reimplemented against two struct types
  (`MI_VENC_Stream_t` vs `i6c_venc_strm`).
- **Proposed shape:** A single `nal_iter` helper taking
  `(get_pack_count, get_pack_at, get_nal_at)` factory functions; backends
  only own the iterator factory.

### Improvement #3 — split monster functions
- `src/maruko_pipeline.c:1265` `maruko_pipeline_run()` — **301 lines**,
  mixes init, ISP config, bind, run-loop, and teardown. The Star6E
  equivalent is already split (`star6e_runtime_process_stream`); this
  divergence is the main reason the two backends drift in control flow.
- `src/star6e_cus3a.c:204` `cus3a_thread()` — 269 lines.
- `src/venc_config.c:1101` `venc_config_save()` — 183 lines.
- `src/star6e_runtime.c:683` `star6e_runtime_process_stream()` — 175 lines.

### Honorable mention — `g_fields[]` codegen
`src/venc_api.c:278` (80+ entries) + `src/venc_api.c:374` `g_field_aliases[]`
maintained by hand, with O(n) `strcmp` scans on every set/query
(`find_field`, `canonicalize_field_key`). Codegen from a single TOML/JSON
spec at build time + perfect-hash lookup.

---

## Performance — hot path on Cortex-A7 @ 25 Mbps / 120 fps

### Perf #1 — eliminate per-frame mutex on record start/stop
- **Location:** `src/venc_api.c:216,229` (`venc_api_get_record_start`,
  `venc_api_get_record_stop`)
- **Impact:** `g_record_mutex` is taken every frame at 120 fps even when no
  record request is pending — ~240 lock/unlock pairs/sec. Real contention
  risk against the HTTP thread that is currently invisible only because
  nobody hammers `/record`.
- **Fix:** `__atomic_load_n` on a `volatile sig_atomic_t` flag, take the
  mutex only when the flag is non-zero. ~1–2 µs/frame saved.

### Perf #2 — TS recorder buffers off-stack
- **Location:** `src/star6e_ts_recorder.c:241,326` (same as Bug #3)
- **Impact:** ~125 MB/s of stack zeroing during recording at 120 fps; large
  stack frames on Cortex-A7. ~10–30 µs/frame avoided.
- **Fix:** Move to persistent `Star6eTsRecorderState` fields.

### Perf #3 — `MI_VENC_GetStream` blocking timeout after Query
- **Location:** `src/star6e_runtime.c:730` and `src/maruko_pipeline.c:1418`
- **Impact:** `MI_VENC_GetStream` is called with timeout 40 ms / 10 ms
  even though the prior `Query` already confirmed `curPacks > 0` and the
  fd-poll has already woken us. Pass `0`: a frame is known-ready, the
  kernel-side blocking adds wake-to-return scheduler jitter.
- **Fix:** One-line change to the timeout argument.

### Honorable mentions
- `src/hevc_rtp.c:189` + `src/h26x_param_sets.c:44` —
  `hevc_rtp_prepend_param_sets` invoked per-NAL but no-op for ~95% of NALs.
  Add a `nal_type` fast-out branch in callers (`star6e_hevc_rtp.c:89`,
  `maruko_video.c:172`) before the function call.
- `src/rtp_packetizer.c:24-36` — RTP header built byte-by-byte with shifts
  then memcpy'd into batch scratch (`star6e_output.c:457`). Replace with a
  packed 12-byte struct + `__builtin_bswap` and write the header directly
  into the slot.

### Hot-path latency budget (estimate)

1. `sendmsg`/`sendmmsg` syscalls + kernel UDP path — dominant, tens of µs.
2. memcpy into batch scratch + payload copies into venc_ring slot — 5–10 µs.
3. Recorder `writev` when active — 10–30 µs typical, occasional spike on
   `sync_file_range` flush.
4. Per-NAL overhead (`strip_start_code` + `prepend_param_sets` no-op +
   `param_sets_update`) — 1–3 µs total/frame.
5. `clock_gettime` × 4–6/frame — ~2 µs.
6. `g_record_mutex` 2× — sub-µs uncontended, multi-µs under contention.

---

## Cleanup

### Cleanup #1 — `output_common.c` extraction
Same as Improvement #1. ~130 LOC removed. Highest-confidence merge target:
both backends already share `output_socket.c` and `venc_ring.c`.

### Cleanup #2 — `iq_engine.c` extraction
- **Location:** `src/star6e_iq.c:326-756` ↔ `src/maruko_iq.c:329-720`
- **Impact:** `read_value`, `write_value`, `emit_fields_json`, and the
  `iq_set` / `iq_import` parsing logic — ~800 lines, only ~63 lines truly
  differ. The real divergence is the SDK-specific `iq_fn_t` signature
  (star6e: `(channel, param)`, maruko: `(dev, channel, param)`) plus the
  descriptor tables.
- **Fix:** Extract value codec and JSON emit/parse into
  `include/iq_engine.h` + `src/iq_engine.c`. Keep only descriptor tables
  and SDK-call adapters per backend. ~250 LOC saved.

### Cleanup #3 — move SNR investigation tools out of `src/`
- `src/snr_sequence_probe.c` — **1266 LOC**, own `main()` at line 795.
- `src/snr_toggle_test.c` — **1256 LOC**, own `main()` at line 805.

Neither is in `STAR6E_ONLY_SRC`. They are diagnostic artifacts and belong
under `tools/` alongside `clock_bench.c`. Removes ~2.5k LOC from the main
source tree.

### Honorable mentions
- `hex_nibble()` defined byte-for-byte identically in `src/venc_api.c:712`
  and `src/venc_httpd.c:180`. Promote to a shared `http_util.h`.
- **Zero TODO/FIXME/XXX/HACK** annotations across `src/`, `include/`,
  `libs/star6e/`, `libs/maruko/` — the codebase is annotation-clean.

---

## Backend duplication map (one-glance reference)

| Star6E (file:lines) | Maruko (file:lines) | Nature |
|---|---|---|
| `star6e_output.c:329-371` `batch_flush` | `maruko_output.c:175-214` | Identical except prefix |
| `star6e_output.c:256-307` `observe_pressure` | `maruko_output.c:78-119` | Identical |
| `star6e_output.c:309-317` `drain_send_errors` | `maruko_output.c:155-163` | Identical |
| `star6e_output.c:676-705` `apply_server` | `maruko_output.c:121-153` | Identical (one error string differs) |
| `star6e_output.c:374-430` `begin_frame`/`end_frame` | `maruko_output.c:216-263` | Identical |
| `star6e_iq.c:326-415` `read/write_value`/`emit_fields_json` | `maruko_iq.c:329-412` | Near-identical |
| `star6e_iq.c:552-699` `iq_set` | `maruko_iq.c:519-645` | Same state machine, extra `dev` arg |
| `star6e_iq.c:700-756` `iq_import` | `maruko_iq.c:646-720` | Same JSON walk |
| `star6e_controls.c:193-283` `apply_bitrate` | `maruko_controls.c:195-228` | Same SDK-call structure |
| `star6e_controls.c:347-440` `apply_fps` | `maruko_controls.c:272-815` | Similar core, maruko adds resolution side effects |

---

## Recommended follow-up PRs

Three highest-leverage PRs, each closing multiple findings:

1. **`fix/ts-recorder-corruption-and-stack`** — Bug #1 + Bug #3 + Perf #2.
   Single file, small diff. Closes a silent corruption bug, removes a
   stack-overflow timebomb under non-glibc, and is the largest per-frame
   perf win when recording is active.

2. **`refactor/output-common`** — Improvement #1 + Cleanup #1 (and Bug #2).
   Medium effort. Extracting `output_common.c` is the natural moment to
   fix the immediate-send-fallback fd UAF properly. Closes the active
   Star6E↔Maruko drift surface in one stroke.

3. **`perf/record-mutex-atomic`** — Perf #1. Trivial, well-scoped. Removes
   a real contention risk between HTTP thread and the 120 fps consumer
   that's only invisible because nobody hammers `/record` today.

Deferred (real but lower payoff right now):
- Improvement #2 (NAL iteration vtable) — aesthetic divergence, no current
  bug payoff. Park until next backend lands.
- Cleanup #2 (`iq_engine.c`) — same; large enough to deserve its own PR
  after #1 lands.
- Improvement #3 (split monster functions) — pair with the relevant
  feature work that touches each function.

## Reviewer notes

- Three independent reviewers converged on TS recorder stack buffers (as
  both a bug and a perf win) and on output-layer duplication (as both an
  architectural problem and a cleanup target). Treat that convergence as
  signal.
- Annotation debt is zero. Whatever process is currently keeping
  TODO/FIXME out of this codebase is working — keep doing it.
- The biggest *unmeasured* risk surface is the HTTP-thread ↔ encoder-thread
  boundary. Issue #1 (g_cfg under mutex), Bug #2 (apply_server fd UAF),
  and Perf #1 (g_record_mutex contention) are all instances of the same
  underlying issue: live-config mutation paths are not designed for the
  120 fps reader.
