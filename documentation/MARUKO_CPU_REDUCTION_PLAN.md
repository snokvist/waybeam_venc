# Maruko CPU Reduction — Hardware Verification Plan

Tracks the Maruko-side CPU reduction work that depends on on-device testing.
Items already merged (safe in source-only form) are listed at the bottom for
context.

Target device for this plan:

| Backend | SoC | Sensor | Host | ISP Bin |
|---------|-----|--------|------|---------|
| Maruko | ssc378qe | imx415 | `root@192.168.2.12` | `/etc/sensors/imx415.bin` |

Every run below assumes the deploy cycle from `AGENTS.md`
("JSON Config Deploy & Test" section):

```bash
make build SOC_BUILD=maruko
scp -O out/maruko/venc root@192.168.2.12:/usr/bin/venc
ssh root@192.168.2.12 "killall venc; sleep 2; nohup venc > /tmp/venc.log 2>&1 &"
sleep 10
```

## Baseline Measurements (run before any risky change)

Collect CPU numbers first so every candidate change has a concrete delta to
beat.  Run for at least 60 s per mode.

```bash
# pick a quiescent stream config
ssh root@192.168.2.12 "json_cli -s .isp.gainMax 10000 -i /etc/venc.json"

# per-thread CPU while streaming
ssh root@192.168.2.12 "top -H -b -n 15 -d 2 | grep -E 'venc|3A_Proc|CPU:' > /tmp/cpu.log"
scp root@192.168.2.12:/tmp/cpu.log cpu-baseline-<mode>.log
```

Record:

- total `venc` CPU%
- `3A_Proc_0` thread CPU% (this is the firmware userspace-3A thread)
- idle% from the `CPU:` header
- sustained fps (`/api/v1/config` → `stream_metrics`, or verbose log)

Capture one profile at each of: 30 fps, 60 fps, 90 fps, 120 fps (if
supported by the sensor mode).  Save as `cpu-baseline-<fps>.log`.

---

## Item 2 (risky part): Conditional `EnableUserspace3A`

### Goal

Skip `MI_ISP_EnableUserspace3A()` (and the `3A_Proc_0` thread it spawns)
when the configured fps is low enough that the ISP FIFO does not require
CUS3A to stay live.  Comment at `src/maruko_pipeline.c:92-95` claims CUS3A
is required "at >=60 fps" — but the threshold has never been measured on
current firmware, and the cost is paid unconditionally today.

### Source-level design

1. Add a config field in `include/venc_config.h`:
   ```c
   int32_t userspace3a_fps_threshold;  /* 0 = always on */
   ```
   Default `60` to match the legacy comment.  Set `0` to force the current
   behavior.  Plumb through `src/venc_config.c`,
   `src/venc_api.c` (`g_fields` + `g_aliases`), `src/venc_webui.c` dashboard,
   and `config/venc.default.json` per **Config / WebUI / API Sync Rules**.

2. In `src/maruko_pipeline.c::maruko_enable_cus3a`, wrap the
   `MI_ISP_EnableUserspace3A` dlsym + call and the trailing
   `CUS3A_Enable(1,1,0)` override in:
   ```c
   int threshold = ctx->cfg.isp.userspace3a_fps_threshold;
   int required = threshold == 0 || (int)ctx->sensor.fps >= threshold;
   if (required) { ...existing Userspace3A + AF-off path... }
   else          { /* keep CUS3A (1,1,0) without userspace3A */ }
   ```
   Context pointer must be threaded into `maruko_enable_cus3a` — currently
   it's called from the pipeline init with no args, so take the fps from
   `ctx->sensor.fps` that's already available at the call site in
   `maruko_pipeline.c:1098`.

3. Add a one-line startup log: `> [maruko] userspace3A: enabled (fps >= %d)`
   or `disabled (fps %u < threshold %d)` so baseline + test runs are
   distinguishable in `/tmp/venc.log`.

### Hardware verification matrix

Run on the imx415 device.  Sensor modes come from
`--list-sensor-modes` (do this first):

```bash
make remote-test ARGS='--host root@192.168.2.12 --soc-build maruko \
  --run-bin venc -- --list-sensor-modes --sensor-index 0'
```

| Test | Config | Expected |
|------|--------|----------|
| T1 | threshold 0 (force-on), every reported fps | Matches baseline CPU + fps |
| T2 | threshold 60, 30 fps | `3A_Proc_0` absent from `top -H`; CPU drop vs T1 |
| T3 | threshold 60, 30 fps | AE still converges: `/api/v1/ae` shows `ae.stable=true` within 5 s |
| T4 | threshold 60, 30 fps | AWB still converges: `/api/v1/awb` shows non-unity gains + CT > 2500 K |
| T5 | threshold 60, 60 fps | `3A_Proc_0` present; same fps as baseline |
| T6 | threshold 60, 90 fps | `3A_Proc_0` present; same fps as baseline |
| T7 | threshold 60, 120 fps | Same as baseline; ISP FIFO does not stall (no `dmesg` vif/isp errors over 60 s) |
| T8 | threshold 60, cold boot at 30 fps | Stream up + first IDR delivered, no AE stuck-dark regression |
| T9 | threshold 60, live-toggle `isp.gainMax` at 30 fps | Change observed; `/api/v1/ae` reflects new cap |

Pass criteria for the risky path:

- T2/T5/T6/T7: on-device `top -H` CPU of the main `venc` thread drops by
  the difference between `3A_Proc_0` CPU% and the cost of whatever
  lightweight replacement runs (often 0).  Require **>= 30 % relative
  reduction** in total `venc` CPU at 30 fps to justify landing.
- T3/T4/T8/T9: functional parity vs baseline.  Any regression → revert.
- All tests: `dmesg` hits for `isp`/`vif`/`snr` must match baseline (within
  ±2 hits over 60 s).  If `snr` `fifo overflow` shows up where the baseline
  had none, the threshold is wrong for that mode; raise it and re-test.

### Known risks

- **Stale comment**: the "CUS3A required at >=60 fps" claim might be a
  historical artefact from early firmware.  The threshold might actually be
  higher (e.g. 90 fps) on current Maruko firmware, which would expand the
  savings window.
- **Dark-image regression**: earlier notes in `maruko_pipeline.c:195-196`
  reference a "dark image issue" tied to AE param reset.  If T3 shows AE
  not converging within 5 s, keep Userspace3A on for now and reopen in a
  follow-up.
- **First-frame cadence**: Star6E needs tight (<= 5 ms) `DisableUserspace3A`
  pacing before the first frame on the 90 fps path
  (`documentation/AE_PORTING_MINIMAL_DELTA_FROM_99301FE.md:119-144`).  If
  Maruko shows the same pattern at its threshold, keep Userspace3A on
  during pipeline startup and only evaluate the threshold after the first
  encoded frame.

---

## Item 3: Port Star6E supervisory AE thread to Maruko

### Goal

Give Maruko a lightweight 15 Hz supervisory AE thread equivalent to Star6E's
`src/star6e_cus3a.c::cus3a_thread_main`, so users can get gain/shutter cap
enforcement **without** paying for the full per-frame `3A_Proc_0` thread.
Only meaningful in combination with item 2's conditional Userspace3A —
otherwise Maruko has *two* 3A threads running simultaneously.

### Source-level design

1. New file `src/maruko_cus3a.c` + header, closely mirroring
   `src/star6e_cus3a.c`.  Structure:
   ```c
   typedef struct {
       pthread_t       thread;
       volatile int    running;
       MarukoCus3aCfg  cfg;         /* ae_fps, gain_max, shutter_max_us, verbose */
       /* resolved ISP fn pointers via dlsym */
       iq_fn_t fn_get_limit, fn_set_limit;
       iq_fn_t fn_get_ae_status, fn_get_hw_stats;
       /* baseline + applied caps */
       uint32_t bin_max_sensor_gain, applied_shutter_max, applied_gain_max;
   } MarukoCus3aState;
   ```

2. Resolve symbols: `MI_ISP_AE_GetExposureLimit`, `MI_ISP_AE_SetExposureLimit`,
   `MI_ISP_CUS3A_GetAeStatus`, `MI_ISP_AE_GetAeHwAvgStats` — all take
   `(dev, channel, data*)` on Maruko.  `maruko_controls.c` already uses the
   first two (reuse those function types directly).

3. Main loop (identical semantics to Star6E):
   ```
   every 1000/ae_fps ms:
       read HW avg-Y stats
       read AE status
       compute want_shutter = min(1_000_000/fps, configured shutter cap)
       compute want_gain    = configured gain cap (0 => bin default)
       if (want_shutter != applied_shutter_max ||
           want_gain   != applied_gain_max):
           fetch current limits, patch shutter_max + gain, write back
           update cached applied_*
       every 5 s: log avg_y, shutter, gain, AE state
   ```

4. Gate on the same `legacyAe` config field Star6E uses — default stays
   `true` to preserve current behavior.  When `legacyAe=false`, start this
   thread AND (if item 2 landed) disable Userspace3A at the configured fps
   threshold.

5. Wire start/stop into `maruko_pipeline.c`:
   - Start after `maruko_enable_cus3a` returns but before the main stream
     loop.
   - Stop before pipeline teardown, between "signal break" and
     `maruko_pipeline_teardown_graph`.
   - SIGHUP reinit: stop + restart alongside the pipeline.

### Hardware verification matrix

Do this **after** item 2 is landed and measured.

| Test | Config | Expected |
|------|--------|----------|
| S1 | `legacyAe=true` (default) | Thread not started; matches item 2 baseline |
| S2 | `legacyAe=false`, 30 fps | Thread started; `top -H` shows ~0.x% CPU for `cus3a` thread |
| S3 | S2 + set `isp.gainMax=5000` via HTTP | Within 2 s, `/api/v1/ae` reports maxSensorGain=5000 |
| S4 | S2 + set `isp.gainMax=0` | maxSensorGain returns to bin default |
| S5 | S2 + set `isp.exposure=3` (3 ms) | `max_shutter_us=3000` observed in `/api/v1/ae` |
| S6 | S2 + `aeFps=5` vs `aeFps=30` | Thread wake-rate proportional; CPU scales linearly |
| S7 | S2, 30 fps, 60 s soak | No `SetExposureLimit` write unless a cap actually changed (log line `%d limit writes` matches changes) |
| S8 | S2, 30 fps, stream stability over 10 min | No fps drift, no dmesg ISP errors, AE still hunts in low light |
| S9 | S2, SIGHUP reinit | Thread is stopped + restarted cleanly; no leak, no deadlock |
| S10 | S2, teardown on SIGINT | `pthread_join` returns within 200 ms (no hang) |

Pass criteria:

- S2 shows the supervisory thread using **< 0.5 % CPU** at 30 fps.
- S3/S4/S5 prove the thread actually enforces live API changes.
- S7's write-count is strictly `== number of cap changes + 1` (one startup
  write).  Any excess means cache state is drifting — fix before landing.
- S10 proves no shutdown regression vs the existing teardown path.

### Dependencies on item 2

Skip item 3 entirely if item 2 fails on-device — running a supervisory
thread alongside an always-on `3A_Proc_0` is just additive cost with no
payoff.

---

## Items already implemented (source-only, safe)

Merged in the same PR as this plan:

- **Item 2 (safe parts)**: dead `MI_ISP_IQ_ApiCmdLoadBinFile` reload block
  removed from `maruko_load_isp_bin` — a disabled code path that still did
  per-reinit `fopen` + `malloc` + `fread` + `free` on the ISP bin.
- **Item 4**: `maruko_apply_awb_mode` now caches the last-applied mode/CT
  and early-returns when the incoming values are unchanged, avoiding the
  `CUS3A_Enable(1,0,1)` → `AWB_Set*` → `CUS3A_Enable(1,1,1)` thrash on
  repeated identical calls (common pattern from WebUI polling).
- **Item 5**:
  - `fps_kick_done` latch replaces `frame_counter == fps` comparison in the
    hot loop.
  - Per-frame sidecar bookkeeping (`rtp_sidecar_poll`, `monotonic_us`,
    `scene_fill_sidecar`, `rtp_sidecar_send_frame` argument setup) is
    gated on `sidecar.fd >= 0` so non-RTP stream modes skip it entirely.

None of these change SDK behavior, so they do not require the HW matrix
above.  They should still be sanity-checked on-device with the standard
`scripts/star6e_direct_deploy.sh`-style cycle (adapted for Maruko: deploy
to `/usr/bin/venc`, run 60 s stream, check `/api/v1/ae`, `/api/v1/awb`,
and `dmesg` matches baseline).
