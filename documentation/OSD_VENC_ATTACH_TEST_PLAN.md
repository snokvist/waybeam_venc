# OSD → VENC ch0 Attach: On-Device Test Plan

This is a self-contained handoff for the next Claude Code CLI session.
The implementation has already landed; this document is the test brief.

## Context (read this first)

Before this change, the Star6E debug OSD attached the MI_RGN region to
**VPE channel 0, port 0**.  In `record.mode = "dual"` and
`"dual-stream"`, both VENC ch0 (stream) and ch1 (recording / second
stream) are bound to that same VPE port, so the OSD pixels were baked
into the frame upstream of both encoders — recordings carried the debug
overlay.

This PR moves the attach point to **VENC ch0** (RGN module ID 6,
i.e. `E_MI_RGN_MODID_VENC`), with a runtime fallback to the previous
VPE attach if the kernel rejects the VENC attach (the numeric module ID
is not in this repo's SDK headers; the value comes from the documented
i6e SigmaStar enum order).

End-result matrix:

| Mode | Stream (ch0) shows OSD | Recording / 2nd stream (ch1) shows OSD |
|---|---|---|
| `off` | n/a (no stream) | n/a |
| `mirror` | yes | yes (ch0 == recorded stream) |
| `dual` | yes | **no** (this is the change) |
| `dual-stream` | yes | **no** (this is the change) |

If the runtime fallback kicks in, **ch1 will show the OSD** in dual
modes (current behaviour) and a stderr line `[debug_osd] VENC attach
failed (...), falling back to VPE attach` will appear in `/tmp/venc.log`.

## Files changed

- `src/debug_osd.c` — Star6E branch only.  Bind module changed from
  `0` (VPE) to `6` (VENC) with channel = 0.  On `MI_RGN_AttachToChn`
  failure, retries against VPE port 0 to preserve current behaviour.
  Renamed `vpe_bind` → `rgn_bind`.
- `documentation/DEBUG_OSD_PLAN.md` — updated Architecture and
  Implementation Details sections.
- `VERSION` — patch bump.
- `HISTORY.md` — new entry.

Maruko backend untouched (already attaches at SCL; no recording path
to isolate).

## Pre-flight

Bench: Star6E ssc338q + imx335 at `root@192.168.1.13`.
ISP bin: `/etc/sensors/imx335_greg_fpvVII-gpt200.bin`.

```bash
git pull
make build SOC_BUILD=star6e
```

Confirm the binary exists at `out/star6e/venc`.

## Test 1 — single-channel: OSD still renders, no regression

```bash
ssh root@192.168.1.13 "killall venc; sleep 2"
scp -O out/star6e/venc root@192.168.1.13:/usr/bin/venc
ssh root@192.168.1.13 "json_cli -s .debug.showOsd true -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .record.mode '\"off\"' -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .record.enabled false -i /etc/venc.json"
ssh root@192.168.1.13 "nohup venc > /tmp/venc.log 2>&1 &"
sleep 10
ssh root@192.168.1.13 "grep -E 'debug_osd|RGN' /tmp/venc.log"
```

Expected log lines:
- `[debug_osd] CanvasInfo sizeof=… stride_off=…`
- `[debug_osd] overlay 1920x1080 stride=… virtAddr=…`
- **No** `VENC attach failed` line (if it appears, the module ID is
  wrong — capture the full log and stop).

Confirm visually that the stream at `udp://192.168.1.2:5600` shows the
OSD overlay (FPS, CPU rows).

## Test 2 — dual mode: ch1 recording is clean

```bash
ssh root@192.168.1.13 "killall venc; sleep 2"
ssh root@192.168.1.13 "json_cli -s .record.enabled true -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .record.mode '\"dual\"' -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .record.dir '\"/mnt/mmcblk0p1\"' -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .record.format '\"ts\"' -i /etc/venc.json"
ssh root@192.168.1.13 "rm -f /mnt/mmcblk0p1/rec_*.ts"
ssh root@192.168.1.13 "nohup venc > /tmp/venc.log 2>&1 &"
sleep 15
ssh root@192.168.1.13 "wget -q -O- http://127.0.0.1/api/v1/record/status"
sleep 10
ssh root@192.168.1.13 "wget -q -O- http://127.0.0.1/api/v1/record/stop"
ssh root@192.168.1.13 "ls -lh /mnt/mmcblk0p1/rec_*.ts"
scp -O root@192.168.1.13:/mnt/mmcblk0p1/rec_*.ts /tmp/dual_rec.ts
```

Inspection:
- Visually play `/tmp/dual_rec.ts` (ffplay or VLC).  **Pass**: no OSD
  text/overlay visible at any point.  **Fail**: OSD visible — module
  ID likely fell back to VPE; check `grep 'VENC attach failed'
  /tmp/venc.log` on device.
- The live RTP stream on the bench (`udp://192.168.1.2:5600`) must
  still show the OSD during the recording window.

## Test 3 — mirror mode: OSD still in recording (regression check)

```bash
ssh root@192.168.1.13 "killall venc; sleep 2"
ssh root@192.168.1.13 "json_cli -s .record.mode '\"mirror\"' -i /etc/venc.json"
ssh root@192.168.1.13 "rm -f /mnt/mmcblk0p1/rec_*.ts"
ssh root@192.168.1.13 "nohup venc > /tmp/venc.log 2>&1 &"
sleep 15
ssh root@192.168.1.13 "wget -q -O- http://127.0.0.1/api/v1/record/stop"
scp -O root@192.168.1.13:/mnt/mmcblk0p1/rec_*.ts /tmp/mirror_rec.ts
```

**Pass**: `/tmp/mirror_rec.ts` shows OSD (because in mirror mode the
recorded stream IS ch0, which is where the OSD now lives).
**Fail**: no OSD — would mean the attach didn't take or the canvas
isn't being updated.

## Test 4 — dual teardown: no D-state, clean exit

The dual-VENC teardown sequence is sensitive (see
`documentation/DUAL_VENC_TEARDOWN.md`).  The OSD detach now happens
before the VENC channel is destroyed; the existing call ordering in
`star6e_pipeline_stop` already satisfies that, but it's worth
confirming under load.

```bash
ssh root@192.168.1.13 "killall venc; sleep 2"
ssh root@192.168.1.13 "json_cli -s .record.mode '\"dual\"' -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .video0.fps 30 -i /etc/venc.json"
ssh root@192.168.1.13 "json_cli -s .record.fps 0 -i /etc/venc.json"  # sensor max
ssh root@192.168.1.13 "json_cli -s .sensor.mode 3 -i /etc/venc.json"  # 120fps
ssh root@192.168.1.13 "nohup venc > /tmp/venc.log 2>&1 &"
sleep 30
# SIGTERM to trigger teardown under sustained 120fps load
ssh root@192.168.1.13 "killall -TERM venc; sleep 5; pgrep venc && echo HANG || echo CLEAN"
ssh root@192.168.1.13 "tail -50 /tmp/venc.log"
ssh root@192.168.1.13 "dmesg | tail -30 | grep -iE 'fault|oops|panic|d state' || echo 'no kernel faults'"
```

**Pass**: prints `CLEAN` and dmesg reports no faults.
**Fail (HANG)**: process stuck — log in `documentation/CRASH_LOG.md`
with full dmesg, mark the change risky, request a power cycle.

## Rollback

If any test fails in a way that points at the OSD attach change, revert
with:

```bash
git revert <commit-sha-of-this-PR>
```

The change is one bind-target swap in `src/debug_osd.c` plus
documentation; no schema, no protocol, no on-disk format affected.

## Sign-off checklist

- [ ] Test 1 passed (no `VENC attach failed` log line)
- [ ] Test 2 passed (`dual_rec.ts` is clean)
- [ ] Test 3 passed (`mirror_rec.ts` has OSD)
- [ ] Test 4 passed (CLEAN teardown under 120fps)
- [ ] Restore config: `record.enabled=false`, `record.mode="mirror"`,
  `debug.showOsd=false`, `sensor.mode=0` (or whatever the bench
  default is)

Once all four pass, the PR is good to merge.
