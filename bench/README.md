# Pipeline performance benchmarks

Baseline-and-after measurements for the Tier A/B/C perf work identified by
the Codex review (April 2026).

## Target

- Host: `root@192.168.1.13` (SSC338Q + IMX335)
- Sensor bin: `/etc/sensors/imx335_greg_fpvVII-gpt200.bin`
- ISP profile: legacy AE, `overclockLevel=1`, `verbose=true`
- Codec: H.265, CBR, GOP 10, `qpDelta=-4`, `frameLost=true`

## Method

- Host probe: `tools/rtp_timing_probe --venc-ip 192.168.1.13 --stats`
- Duration: 60 s (7000+ frames at 120 fps)
- Metrics captured per frame via sidecar MSG_FRAME:
  - `capture_us → frame_ready_us`  = **encode duration**
  - `frame_ready_us → last_pkt_send_us`  = **send spread** (packetize + all
    per-packet sendmsg() syscalls + anything else holding the stream)
- `--stats` prints percentile summary on exit.

## Baseline results

### 120 fps H.265 @ 8 Mbps (default vehicle config)

```
Frames:            ~7000 (115.1 fps actual)
Encode:            mean 9615 us, max 15610 us
Send spread:       mean  344 us, P50  272 us, P95  672 us, P99 1289 us, max 7990 us
```

### 120 fps H.265 @ 25 Mbps (stress baseline — target working point)

```
Frames:            7305 (117.8 fps actual)
RTP packets:       142628 (19.5 pkts/frame avg)
RTP gaps:          0

Encode:            mean 8931 us, max 15660 us
Send spread:       mean 1123 us, P50  996 us, P95 2108 us, P99 4829 us, max 8775 us

Sidecar overhead:  7.4 KB/s rx (1 MSG_FRAME per frame + 0.7 sync pps)
Clock sync RTT:    best 928 us, stabilised ~1.7-2.5 ms
```

Raw TSVs: `baseline-master-120fps-h265.tsv`,
`baseline-master-120fps-25mbps.tsv`.
Summary files alongside each.

## Observations before any fix

1. **Send spread scales with bitrate** — 344 us at 8 Mbps → 1123 us at 25 Mbps.
   Confirms that per-packet `sendmsg()` cost (~19.5 packets/frame) is real
   CPU work on Cortex-A7, not just kernel overhead.
2. **Send-spread P99/max are pathological at stress level** — 4.8 ms / 8.8 ms.
   At 120 fps (8.33 ms frame period) an 8.8 ms send spread means we miss a
   frame boundary. This matches the Codex finding that Star6E holds the
   encoder stream across recorder writes, HTTP poll, verbose IMU output and
   debug OSD before `MI_VENC_ReleaseStream`.
3. **Encode duration is bitrate-insensitive** — encoder hardware is not the
   bottleneck. All wins will come from the post-encode path.
4. **Sidecar itself is ~zero cost on the wire** (7 KB/s, 1 syscall/frame).
   Its CPU cost is frame-synchronous `now_us()` sampling, not bandwidth.

## Planned work (in order)

- **Tier A** — raise `SO_SNDBUF`; release Star6E and Maruko encoder stream
  immediately after UDP send, move verbose/record/HTTP/OSD work onto
  post-release state.
- **Tier B** — use connected-UDP fast path (both backends); collapse
  redundant `now_us()` calls in the sidecar hot path.
- **Tier C** — evaluate `sendmmsg()` batching. Decide after Tier A+B
  measurements show whether syscall count still matters.

After each tier we re-run the same probe at the same 25 Mbps working point
and append the results below.

## After-fix results

(populated as tiers are applied)
