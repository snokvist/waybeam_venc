# Image Stabilization — On-Device Test Plan

Self-contained schedule for validating digital image stabilization (DIS) on
Star6E. Written so a fresh session (no prior chat context) can execute it.
Tick the boxes and fill **Result** as you go; commit updates so progress
survives across sessions.

- **Branch:** `claude/cherry-pick-image-stabilizing-UkZ37` (PR #122)
- **Feature:** opt-in DIS on the VPE pipeline. A thread drains VPE port0,
  runs `MI_IVE_Shift_Detector` on a centre Y patch, and `MI_SYS_BufBlitPa`-
  crops a shifted window into VENC ch0's input. AE meter follows the crop.
  IMU gyro samples flow into a ring as a fusion seam (optical-only today).
- **Key code:** `src/star6e_pipeline.c` (stab block + `bind_and_finalize`),
  `src/star6e_runtime.c` (OSD panel anchor), `src/debug_osd.c`.
- **Config fields (video0):** `stabCropPct` (0=off, 50..100=crop %),
  `stabRecenterSpeed` (recenter time constant in frames; 0=stick). Both
  `MUT_RESTART` — edit `/etc/waybeam.json` then restart, not live `set`.

## Status

| Date | Tester | Firmware/sensor | Overall | Notes |
|------|--------|-----------------|---------|-------|
|      |        |                 | NOT RUN |       |

## Target (from AGENTS.md → Deployment Targets)

- **Host:** `root@192.168.1.13` — Star6E ssc338q / imx335, sensor-index 0
- **ISP bin:** `/etc/sensors/imx335_greg_fpvVII-gpt200.bin`
- **Stream destination:** `192.168.1.2`
- Only run if the host is reachable; if offline, mark NOT RUN and stop.

## Pre-flight

```bash
# From repo root on the build host
make verify                      # both backends build, binaries OK
ssh root@192.168.1.13 'echo ok'  # confirm device reachable
```

If `make verify` fails, fix before deploying. If the device is unreachable,
do not proceed.

## Build + deploy

Preferred path uses the production `/etc/waybeam.json` + HTTP API:

```bash
make build SOC_BUILD=star6e
ssh root@192.168.1.13 "killall waybeam; sleep 2"
scp -O out/star6e/waybeam root@192.168.1.13:/usr/bin/waybeam
```

Always edit config with `json_cli` (never `sed`):

```bash
ssh root@192.168.1.13 "json_cli -s .video0.stabCropPct 80 -i /etc/waybeam.json"
ssh root@192.168.1.13 "json_cli -s .video0.stabRecenterSpeed 60 -i /etc/waybeam.json"
ssh root@192.168.1.13 "json_cli -s .system.verbose true -i /etc/waybeam.json"
```

Start as a daemon and capture logs:

```bash
ssh root@192.168.1.13 "nohup waybeam > /tmp/waybeam.log 2>&1 &"
sleep 10
```

Restore + stop when done with a case that changed config:

```bash
ssh root@192.168.1.13 "json_cli -s .video0.stabCropPct 0 -i /etc/waybeam.json"
ssh root@192.168.1.13 "json_cli -s .system.verbose false -i /etc/waybeam.json"
ssh root@192.168.1.13 "killall waybeam"
```

## Test cases

### T0 — Baseline, stab OFF (regression guard)
- [ ] `stabCropPct=0`, start, view the stream on `192.168.1.2`.
- [ ] Stream is full-frame; `zoomX/zoomY` still pan smoothly (0.11.0 ramp);
      `zoomPct` zoom still works.
- **Expect:** identical to pre-PR behavior. **Result:**

### T1 — Stab ON, basic stabilization
- [ ] `stabCropPct=80`, `stabRecenterSpeed=60`, restart.
- [ ] Stream resolution shrinks to ~80% (smaller frame in SPS/PPS).
- [ ] Gently shake the camera: jitter is visibly reduced vs T0.
- [ ] `grep 'stab tick' /tmp/waybeam.log` → `meas`/`acc` values are sane
      (small, non-stuck), `max` matches the dead border.
- [ ] `grep 'stab: src=' /tmp/waybeam.log` → src/out dims look right.
- **Expect:** stabilized, cropped ch0; no crash. **Result:**

### T2 — Live pan under stab
- [ ] With stab ON, pan via the API (alias maps to zoom_x/zoom_y):
      ```bash
      ssh root@192.168.1.13 "wget -q -O- 'http://127.0.0.1/api/v1/set?video0.zoomX=0.2'"
      ssh root@192.168.1.13 "wget -q -O- 'http://127.0.0.1/api/v1/set?video0.zoomY=0.8'"
      ```
- [ ] Framing recenters toward the requested point (direct, not ramped).
- [ ] `pan=(…)` in the stab tick log reflects the new center.
- **Expect:** crop window pans; stabilization still active. **Result:**

### T3 — AE meter follows the crop
- [ ] With stab ON, frame so the crop covers a dark subject while the full
      sensor FOV includes bright sky.
- [ ] Pan the crop between bright and dark regions (T2 commands).
- [ ] Exposure tracks the *framed* (cropped) region, not the full sensor.
- [ ] `wget -q -O- http://127.0.0.1/api/v1/ae` before/after pan shows the
      exposure responding to the crop content.
- [ ] No `MI_ISP_CUS3A_SetAECropSize … failed` warnings in the log.
- **Expect:** AE meters on the crop, like the zoom path. **Result:**

### T4 — Stab + dual recording mutual exclusion
- [ ] Set `record.enabled=true`, `record.mode="dual"`, keep `stabCropPct=80`,
      restart.
- [ ] `grep 'dual recording disabled' /tmp/waybeam.log` → warning present.
- [ ] ch0 main stream stays alive (no stall); dual ch1 is NOT started.
- **Expect:** warning logged, ch0 healthy. **Result:**

### T5 — IMU gyro ring plumbing (only if an IMU is wired)
- [ ] `json_cli -s .imu.enabled true`, set `i2cDevice`/`i2cAddr` for the
      board, restart with stab ON.
- [ ] `grep 'stab tick' /tmp/waybeam.log` → `gyro_n` > 0 and `gyro=(…)`
      shows non-zero rates when the camera moves.
- **Expect:** gyro data is frame-aligned and flowing (fusion is future
      work; values are diagnostic only). **Result:**

### T6 — Teardown / restart stress
- [ ] Toggle `stabCropPct` 0→80→0 with a restart each time (3 cycles).
- [ ] `grep -iE 'fault|oops|panic|timeout|watchdog' ` over `dmesg` after each
      cycle → clean.
- [ ] Device stays responsive (SSH works) after each restart.
- **Expect:** no D-state hang, no kernel faults across restarts. **Result:**

## Watch-list (areas with no host-side coverage)

- `star6e_stab_reapply_vpe_port` uses `EnablePort` without a prior
  `DisablePort` and sets output depth 4/8 — confirm port0 comes up at full
  src dim and the drain keeps up (no `stab venc send failed` spam).
- First-frame path and the 2-deep buffer hold during teardown — confirm no
  leak/stall on stop (T6).

## Failure handling

- **Binary error / stall:** read `/tmp/waybeam.log`; fix; rebuild; redeploy.
- **dmesg fault/panic/timeout:** log it in `documentation/CRASH_LOG.md`
  (exact command + circumstances + dmesg), reboot before the next case.
- **Device unresponsive (SSH dead):** stop. Log in `CRASH_LOG.md`. Do not
  retry SSH. Request a power cycle, then redeploy from scratch.
- Refer to AGENTS.md → "Deployment Test Recovery" for the full loop.
