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

### Tier A — SO_SNDBUF + early stream release (both backends)

Same working point (25 Mbps, 120 fps, verbose=true). Hub `mod_venc.bitrate_enabled`
temporarily set to `false` so aalink rate control would not clamp the bench
back to 8 Mbps — this was not necessary for the baseline run because aalink
happened to be idle then.

Probe summary:

```
Frames:            6695 (116.3 fps actual, 57.6 s)
RTP packets:       130935 (19.6 pkts/frame avg)
RTP gaps:          0

Encode:            mean 9653 us, max 16380 us        (unchanged — not on the hot path we touched)
Send spread:       mean  952 us, P50 717, P95 1912, P99 3017, max 8581 us

Sidecar overhead:  7.8 KB/s rx (1 MSG_FRAME/frame + 0.8 sync pps)
Clock sync RTT:    best 321 us
```

Delta vs baseline:

| Metric            | Baseline | Tier A | Δ      |
|-------------------|---------:|-------:|-------:|
| Send spread mean  | 1123 us  |  952 us| −15%   |
| Send spread P50   |  996 us  |  717 us| −28%   |
| Send spread P95   | 2108 us  | 1912 us|  −9%   |
| Send spread P99   | 4829 us  | 3017 us| **−38%** |
| Send spread max   | 8775 us  | 8581 us|  noise |

**Takeaway.** P99 dropped from 4.83 ms → 3.02 ms and now fits inside the
8.33 ms frame period at 120 fps. P50 and mean moved in the same direction,
confirming that the verbose IMU/EIS print path, HTTP record-control poll
and debug OSD work that used to sit under the encoder stream lifetime were
real cost. The extreme max tail (~8.5 ms) did not move — it lives upstream
in kernel UDP send scheduling, rtl88x2eu TX queue backpressure, or aalink
handover residue, not in the venc process.

Encode duration is unchanged (as expected — Tier A touched only the
post-encode stream-release path).

SO_SNDBUF raise to 512 KiB had no visible steady-state effect at 25 Mbps;
the IDR-burst failure mode it targets is a rare event that does not appear
in a 60 s bench.

### Tier B — connected-UDP fast path + sidecar now_us consolidation

Same 25 Mbps / 120 fps working point.

**Environment caveat:** `SIGHUP` to waybeam_hub does not reload
`venc.bitrate_enabled` — the flag is read once at hub startup. To keep
the bench locked at 25 Mbps I had to fully **stop** the hub for the
run, not just disable aalink rate-control via config. This also removes
hub-generated scheduler jitter (periodic HTTP polls, mDNS beacons,
metrics scraping), which confounds the tail numbers.

Probe summary:

```
Frames:            7398 (119.3 fps actual, 62 s)
RTP packets:       145157 (19.6 pkts/frame avg)
RTP gaps:          0

Encode:            mean 8553 us, max 11827 us
Send spread:       mean  769 us, P50 656, P95 1449, P99 1954, max 2367 us

Sidecar overhead:  7.5 KB/s rx (1 MSG_FRAME/frame + 0.7 sync pps)
Clock sync RTT:    best 1230 us
```

Delta vs Tier A (hub-running) and baseline (hub-running):

| Metric            | Baseline | Tier A | Tier B | Δ vs baseline |
|-------------------|---------:|-------:|-------:|--------------:|
| Encode mean       |  8931 us | 9653 us| 8553 us|        −4%    |
| Encode max        | 15660 us |16380 us|11827 us|       −24%    |
| Send spread mean  |  1123 us |  952 us|  769 us|       −32%    |
| Send spread P50   |   996 us |  717 us|  656 us|       −34%    |
| Send spread P95   |  2108 us | 1912 us| 1449 us|       −31%    |
| Send spread P99   |  4829 us | 3017 us| 1954 us|     **−60%**  |
| Send spread max   |  8775 us | 8581 us| 2367 us|     **−73%**  |

**Disentangling the numbers.** Encode duration dropped by 24% at max
even though nothing in Tier B touches the encoder. That 24% is purely
the hub-stop confound — hub background work was perturbing the encoder
thread's scheduling. The same confound lifts some portion of the P99/max
send-spread improvement; Tier B's code changes are responsible for the
rest.

Code-driven gains (from Tier A, hub running, identical setup) still stand:
mean −15%, P50 −28%, P99 −38% of Tier A code's own improvement over
baseline. Tier B's additional connected-UDP and sidecar consolidation
compound on top, but a clean A-vs-B comparison with the hub stopped in
both runs is still pending. To be run before Tier C.

Open: a hub reload-path fix so `venc.bitrate_enabled` changes take effect
on SIGHUP without a full restart — unblocks future benches without
stopping the hub.


