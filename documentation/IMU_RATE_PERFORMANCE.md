# Spec: IMU FIFO Drain Performance & Threaded Drain

Status: **IMPLEMENTED** (v0.19.0) — threaded drain is the default on Star6E,
device-verified on `.13` (SSC338Q, IMX335, ~88 fps). 1 kHz (1600 Hz ODR) is a
deferred follow-up (requires a 1 MHz I²C bus — a devicetree/BSP change).
Scope: Star6E backend. Maruko keeps the synchronous drain for now (same code
path; not yet benchmarked on Maruko hardware).

## 1. Problem

With the BMI270 IMU enabled, raising `imu.sampleRateHz` caused video frame
drops that worsened with rate: occasional drops at 400 Hz @90 fps, severe at
800 Hz, and a near-total collapse ("crawl") at "1000 Hz".

Note on ODR steps: the BMI270 only supports ODRs of 25/50/100/200/400/800/1600
Hz. The driver maps `sampleRateHz` as `≤400→400, ≤800→800, else→1600`, so
**"1000 Hz" actually programs 1600 Hz**.

## 2. Root cause — the drain is synchronous and I²C-bus-bound

`imu_drain()` was called **synchronously in the video frame loop**
(`star6e_runtime.c`, before `MI_VENC_GetStream`). Each call does two blocking
I²C transactions on `/dev/i2c-1`:

1. read the 2-byte FIFO length (regs `0x24/0x25`),
2. bulk-read all queued FIFO bytes (regs `0x26`).

Each FIFO sample in combined accel+gyro mode is **13 bytes** (1 header + 6
accel + 6 gyro). The number of bytes per drain scales with ODR, so the drain
time grows with the rate and consumes the per-frame budget (11.4 ms @88 fps).

### Measured cost model (instrumented `fifo_drain_internal`, `.13`)

Bulk read ≈ **169 µs + 29.5 µs/byte** (bus ≈ 300–400 kHz — no explicit DT
`clock-frequency` on "Sstar I2C adapter 1"). The 2-byte length read costs a
roughly fixed ~230 µs (transaction/ioctl overhead).

| ODR | bytes/drain | bulk read | drain total (avg/max) | fps |
|-----|-------------|-----------|-----------------------|-----|
| 200 | 29 | 1.0 ms | 1.3 / 2.3 ms | 88 |
| 400 | 89 | 2.8 ms | 3.1 / **5.1** ms | 88 (max-spikes drop frames) |
| 800 | 117 | 3.6 ms | 4.0 / **8.5** ms | 76 |
| 1600 | **1024 (FIFO saturated)** | **~30 ms** | ~30 ms | 30 |

### The 1600 Hz death spiral

The BMI270 FIFO is 1024 bytes (~78 combined samples). At 1600 Hz it fills in
~49 ms. Synchronous per-frame draining cannot keep up: each drain reads a full
1024 bytes, whose bulk read (~30 ms ≈ **3 frame periods**) stalls the frame
loop, which lets the FIFO fill even more — so it stays capped at 1024 and fps
collapses to ~30.

## 3. Fix — threaded drain (default on Star6E)

Read the FIFO on a dedicated thread (`imu_fifo_reader_thread`, already present
and previously used only by `--imu-test`) instead of synchronously in the frame
loop. `imu_drain()` in the frame loop becomes a no-op; the thread drains and
pushes timestamped samples into the shared gyro ring and the gcsv log (both
mutex-protected — see the gcsv writer's thread-safety, PR #151).

Enabled via `ImuConfig.use_thread = 1` in `star6e_pipeline.c`.

### Poll interval — keep the stabilizer fed

The thread's previous "50% FIFO" poll interval is 49 ms @800 Hz and ~200 ms
@200 Hz — far too stale for the gyro-assisted stabilizer, which reads a fresh
**per-frame** gyro window. The poll interval is therefore **capped at 8 ms**
(< one frame period at ≤120 fps), which dominates the 50%-fill formula at every
supported ODR while staying well below the FIFO-overflow interval (≥49 ms even
at 1600 Hz). See `imu_fifo_reader_thread` in `src/imu_bmi270.c`.

### Validation (`.13`, IMX335, ~88 fps, threaded + 8 ms cap)

| ODR | fps (was, synchronous) | stabilizer (`gyro_n`) |
|-----|------------------------|-----------------------|
| 200 | **89** (88) | fed |
| 400 | **89** (88, spikes gone) | fed |
| 800 | **88–89** (76) | `gyro_n=9` |

Threaded drain restores full frame rate up to 800 Hz while keeping the
stabilizer's gyro window fresh.

## 4. 1 kHz / 1600 Hz — deferred follow-up

Threaded drain alone only takes 1600 Hz from 30 → ~50 fps, and gyro-only FIFO
(7 B/sample) → ~63 fps, because the I²C **bus time itself** is the wall on the
single-core Cortex-A7:

| 1600 Hz mode | data rate | I²C bus time | core share |
|--------------|-----------|--------------|------------|
| combined (13 B) | 20.8 KB/s | 614 ms/s | **61 %** |
| gyro-only (7 B) | 11.2 KB/s | 330 ms/s | 33 % |
| gyro-only headerless (6 B) | 9.6 KB/s | 283 ms/s | 28 % |

Even 28 % of the core stolen by I²C leaves the encode/ISP/RTP pipeline short of
88 fps. The only clean path to 1600 Hz @ full fps is to lower µs/byte by
**raising the i2c-1 bus clock to 1 MHz** (BMI270 supports Fast-mode Plus). At
~12 µs/byte, gyro-only 1600 Hz ≈ 11.5 % core and combined ≈ 25 % — both
feasible.

That is a **devicetree/BSP change** (`clock-frequency = <1000000>` on the i2c1
node) in the firmware/builder repo, not waybeam_venc. Before doing it, confirm
the SoC I²C controller and BMI270 wiring tolerate 1 MHz and that nothing else
shares the bus. Caveat: part of the measured 29.5 µs/byte may be controller
per-byte overhead that won't scale with SCL — verify empirically once the DT
knob changes.

A gyro-only FIFO option (drop accel to halve the bytes) was considered and
deferred — it does not by itself make 1600 Hz viable, and it would cost the
Gyroflow horizon-lock that accel provides.

## 5. Considered and rejected

- **Drop the separate 2-byte FIFO-length read.** Tempting (saves ~230 µs/drain),
  but you then have to read a fixed-size block and detect the FIFO over-read
  marker (`0x80`). Size it too large and you waste far more than 230 µs reading
  empty bytes (each at 29.5 µs); too small and data is left in the FIFO. With
  threaded draining the drain rate also drops (~125 drains/s at the 8 ms cap),
  so the length read is only ~3 % of a core regardless. Net: not a clean win,
  added parsing risk — keep the precise length read.
- **One-sample-per-ioctl polling** (the old non-FIFO polling mode): 1000+ ioctls
  per second; far worse than batched FIFO reads.

## 6. Practical guidance

- 200/400/800 Hz are all supported at full frame rate with threaded drain.
- 800 Hz is ample for FPV Gyroflow stabilization (action cams typically log
  200–500 Hz).
- 1600 Hz ("1000 Hz") needs the 1 MHz I²C bus change above; until then it will
  reduce frame rate.
