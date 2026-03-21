# GyroGlide-Lite Design

**A low-latency frame-synchronous 2-axis translational EIS using batched gyro
integration and crop-window control.**

---

## Plain-language summary

A camera on a drone vibrates and tilts. A gyroscope chip (BMI270) measures
rotation speed 200 times per second. Every time the video encoder finishes a
frame (~30-60 fps), we collect all the gyro samples that accumulated since
the last frame, multiply each sample's rotation rate by its time slice, and
sum them to get "how much did the camera rotate during this frame interval."
We convert that angle into pixels, then shift a crop window inside an
oversized sensor capture area to cancel the motion. The visible image slides
opposite to the shake. A spring pulls the crop window slowly back toward
center so we don't run out of travel. The whole thing costs one
`MI_VPE_SetPortCrop()` call per frame and a few microseconds of arithmetic.

---

## 1. Why gyro-only is the right fast-path

**Accelerometers measure linear acceleration, not rotation.** To get position
from an accelerometer you must double-integrate, which drifts unboundedly
within seconds — unusable for real-time stabilization without GPS or vision
correction. For crop-window EIS on an FPV camera:

- **Gyro directly measures what we correct.** Camera shake is rotational
  (yaw/pitch). Gyro outputs angular velocity in rad/s. One integration gives
  angle. Angle maps to pixel displacement through the lens focal length. One
  integration is stable over frame-sized intervals (16-33 ms).

- **Accelerometer would add complexity for marginal gain.** It could provide
  a gravity reference for horizon leveling, but that requires an AHRS filter
  (complementary or Kalman), adds latency, and the crop-only system can't
  correct rotation anyway. Not worth it for the fast path.

- **Gyro bandwidth matches the problem.** At 200 Hz, we get 3-7 samples per
  60fps frame — enough to capture vibration content up to ~100 Hz (Nyquist).
  Higher frequencies are beyond the crop system's correction bandwidth anyway
  (pixel-level displacement at high freq is sub-pixel).

- **Latency is minimal.** Gyro sample → integrate → crop offset. No filter
  convergence, no model fitting. First frame after startup already has valid
  data.

The existing POC in `eis_crop.c` validates this: the entire signal chain is
`gyro_rad_s × dt → angle → pixels → crop`, with one LPF to extract the
smooth path. GyroGlide-Lite refines this with proper timestamp-based
integration, a spring-damper recenter, deadband, and slew limiting.

**Accelerometer role:** reserved for optional future horizon lock (static
tilt correction). Not in the hot path.

---

## 2. Minimal architecture

### Block diagram

```
 IMU SAMPLING PATH (imu_bmi270.c — existing)
 ────────────────────────────────────────────
 BMI270 FIFO ──I2C burst──> imu_drain()
                               │
                               ▼
                         push_fn callback
                               │
 TIMESTAMPED RING BUFFER       │
 ───────────────────────       │
                               ▼
                    eis_push_sample()   [via EIS dispatch]
                         MotionRing
                    [ts, gx, gy, gz] × 2048
                               │
 PER-FRAME BATCH EXTRACTION    │
 ──────────────────────────    │
                               │◄── frame event (MI_VENC_GetStream returns)
                               ▼
                    ring_read_range(t_prev, t_now)
                         MotionSample[N]
                               │
 PER-FRAME MOTION INTEGRATION  │
 ─────────────────────────     │
                               ▼
                    for each consecutive pair:
                      dt_i = ts[i] - ts[i-1]
                      Δyaw   += gyro_y[i] * dt_i
                      Δpitch += gyro_x[i] * dt_i
                    apply deadband
                               │
 PER-FRAME CROP DECISION       │
 ────────────────────────      │
                               ▼
                    position += Δ × gain
                    position -= position × recenter_rate × frame_dt
                    position = slew_limit(position, prev, max_step)
                    position = clamp(position, -margin, +margin)
                               │
 HARDWARE CROP UPDATE          │
 ─────────────────────         │
                               ▼
                    crop_x = center_x + (int)position_x
                    crop_y = center_y + (int)position_y
                    align to even pixel
                    MI_VPE_SetPortCrop(ch, port, &rect)
```

### Component mapping to existing code

| Block | File | Status |
|---|---|---|
| IMU sampling | `imu_bmi270.c` | Exists — FIFO + polling, push callback |
| Ring buffer | `eis_crop.c:MotionRing` | Exists — needs timestamp-based integration |
| Batch extraction | `eis_crop.c:ring_read_range()` | Exists — correct |
| Motion integration | `eis_crop.c:eis_crop_update()` L344-351 | **Rework** — currently uses uniform `dt/n` |
| Crop decision | `eis_crop.c:eis_crop_update()` L353-367 | **Rework** — LPF→spring-damper + deadband + slew |
| HW crop write | `eis_crop.c:clamp_and_apply_crop()` | Exists — correct |
| Frame trigger | `star6e_runtime.c:683-685` | Exists — `imu_drain()` then `eis_crop_update()` |

---

## 3. Control law — minimal state variables

### State (4 floats + 1 timestamp)

```c
float pos_x, pos_y;          /* crop offset in pixels, relative to center */
float bias_x, bias_y;        /* slow-adapting gyro bias estimate (rad/s) */
struct timespec t_prev;       /* timestamp of last frame update */
```

The simplified exponential recenter needs only position — no separate
velocity state. Compare to current POC which tracks `raw_angle_x/y` and
`smooth_angle_x/y` (4 floats).

### Control law per frame

```
# Inputs: Δangle_x, Δangle_y (integrated gyro over frame interval, radians)
#         frame_dt (seconds since last update)

# 1. Convert angular displacement to pixel displacement
Δpx_x = Δangle_x × pixels_per_radian × gain

# 2. Update position (accumulate + exponential recenter)
pos_x = pos_x + Δpx_x
pos_x = pos_x × (1.0 - recenter_rate × frame_dt)

# 3. Edge-aware recenter (accelerate near margin limits)
if |pos_x| / margin_x > 0.8:
    extra_decay ∝ (edge_fraction - 0.8)
    pos_x *= (1 - extra_decay)

# 4. Slew limit
pos_x = clamp(pos_x - prev_pos_x, -max_slew, +max_slew) + prev_pos_x

# 5. Clamp to margin
pos_x = clamp(pos_x, -margin_x, +margin_x)
```

### Why this over the existing POC's LPF approach

The existing POC uses `smooth = smooth + alpha × (raw - smooth)`, then
`correction = smooth - raw`. This is mathematically a high-pass filter on
the gyro integral. The spring-recenter formulation is equivalent when
`recenter_rate ≈ 1/filter_tau`, but gives:

- **More intuitive tuning**: recenter_rate directly controls return speed
- **Better edge behavior**: edge-aware recenter prevents sticky saturation
- **Direct crop semantics**: pos_x IS the crop offset — no indirection
  through two diverging angle accumulators that can overflow over time

---

## 4. Processing the IMU batch between frames

### Defining the time interval

```
t0 = timestamp of previous frame event (stored as t_prev)
t1 = timestamp of current frame event (clock_gettime(CLOCK_MONOTONIC))
frame_dt = t1 - t0   (seconds, typically 16.6ms @ 60fps)
```

### Integrating gyro over dt — timestamp-based

The current POC divides frame_dt evenly across N samples:

```c
/* Current (approximate): */
double sample_dt = dt / n;
for (i = 0; i < n; i++)
    raw_angle_x += samples[i].gyro_x * sample_dt;
```

This is wrong when samples have non-uniform spacing. The correct approach
uses per-sample timestamps:

```c
/* GyroGlide-Lite (timestamp-based): */
float delta_x = 0.0f, delta_y = 0.0f;

for (uint32_t i = 0; i < n; i++) {
    float dt_i;
    if (i == 0)
        dt_i = ts_to_sec(t_prev, samples[0].ts);
    else
        dt_i = ts_to_sec(samples[i-1].ts, samples[i].ts);

    dt_i = clamp(dt_i, 0.0f, 0.05f);  /* sanity cap */

    float gx = samples[i].gyro_x - bias_x;
    float gy = samples[i].gyro_y - bias_y;

    delta_x += gx * dt_i;
    delta_y += gy * dt_i;
}

/* Tail extrapolation: last sample → frame time */
if (n > 0) {
    float dt_tail = ts_to_sec(samples[n-1].ts, t_now);
    if (dt_tail > 0.0f && dt_tail < 0.02f) {
        delta_x += (samples[n-1].gx - bias_x) * dt_tail;
        delta_y += (samples[n-1].gy - bias_y) * dt_tail;
    }
}
```

### Deadband

After integration, before applying to position:

```c
if (fabsf(delta_x) < deadband_rad) delta_x = 0.0f;
if (fabsf(delta_y) < deadband_rad) delta_y = 0.0f;
```

Suppresses sensor noise when stationary. ~0.001 rad/frame (≈0.06°/frame).

### Optional 1-pole smoothing

On the per-frame delta (not raw gyro — that would add lag):

```c
delta_x = alpha * delta_x + (1 - alpha) * prev_delta_x;  /* alpha ≈ 0.7-1.0 */
```

Start with alpha = 1.0 (disabled). Reduce only if frame-to-frame jitter is visible.

---

## 5. Float vs. fixed-point

**Recommendation: use `float` (IEEE 754 single-precision).**

- ARM Cortex-A7 (SSC30KQ/SSC338Q) has a VFPv4 FPU. Single-precision float
  operations are 1 cycle throughput. No penalty vs. integer math.
- The entire per-frame computation is ~30-50 float ops. At 500 MHz: ~100 ns.
- The existing codebase (`imu_bmi270.c`, `eis_crop.c`) is all `float`.
- Gyro values span many orders of magnitude (bias ~0.001, signal ~5 rad/s).
  Float handles this naturally; fixed-point needs careful Q format selection.

**Exception:** final crop coordinates are `uint16_t` — float→int at the end.

---

## 6. Frame timing and crop-application timing

### The core question

When `MI_VPE_SetPortCrop()` is called at the moment frame N exits the
encoder, does the new crop position affect frame N+1 or frame N+2?

### Expected behavior

The VPE pipeline is VIF→VPE (REALTIME) → VENC (FRAMEBASE). SetPortCrop()
is likely latched at next VPE VSYNC → **one-frame delay** (most likely).

### How to confirm experimentally

1. Enable test_mode sine wobble at known frequency
2. Record encoded video + log crop_x per frame
3. Compare phase of visible motion vs. logged crop values
4. Phase match → zero delay. One-frame shift → one-frame delay.

### Compensation

If confirmed one-frame late, apply predictive extrapolation:

```c
float predicted_x = pos_x + delta_x * px_per_rad * gain;
/* Use predicted_x for crop write */
```

---

## 7. Tuning guidance

| Parameter | Default | Range | Effect |
|---|---|---|---|
| `gain` | 0.8 | 0.0–1.0 | Fraction of detected motion to correct |
| `deadband_rad` | 0.001 | 0.0–0.01 | Per-frame angle below which correction zeroed |
| `recenter_rate` | 1.0 | 0.1–5.0 | Return-to-center speed (1/tau, per second) |
| `margin_percent` | 10 | 5–20 | Overscan reserved for stabilization |
| `max_slew_px` | 8.0 | 2.0–20.0 | Max crop change per frame (pixels) |
| `px_per_rad` | W/2 | auto | Lens FOV dependent (554 for 120° on 1920px) |
| `bias_alpha` | 0.001 | 0.0001–0.01 | Runtime bias adaptation rate |

### Tuning procedure

1. Start with defaults. Camera stationary → crop should stay at center ±1px.
2. Slow deliberate pan → should track briefly then recenter. Adjust recenter_rate.
3. Sharp shake → should respond immediately. If overcorrecting, reduce gain.
4. Sustained rotation → should hit margin then smoothly recenter.
5. Enable slew limiting if single-frame jumps are visible.

---

## 8. Failure modes and limitations

| Limitation | Explanation |
|---|---|
| No rotation compensation | Roll shake can't be fixed by crop |
| Resolution loss | 10% overscan = 10% fewer output pixels |
| Limited correction range | ±5% margin ≈ ±5° at typical FOV |
| No rolling-shutter correction | Line-by-line readout skew not addressed |
| Sustained rotation saturates | Deliberate pan fights the recenter spring |
| IMU failure | Graceful: zero correction, spring recenters |
| Gyro bias drift | Runtime adaptation + deadband masks residual |

---

## 9. Future improvements (preserving low complexity)

| Improvement | Complexity | Benefit |
|---|---|---|
| Gravity-referenced tilt | Medium | Horizon leveling via accel complementary filter |
| Exposure-aware scaling | Low | Better correction with fast shutter |
| One-frame prediction | Low | Compensate pipeline delay |
| Adaptive gain | Low | Soft saturation near margins |
| Intent detection | Medium | Reduce correction during deliberate pans |
| LDC warp path | High | Full rotation + rolling-shutter via VPE LDC |
| HTTP API tuning | Low | Live parameter adjustment without restart |

---

## 10. Changes from existing POC

| Aspect | POC (`eis_crop.c`) | GyroGlide-Lite |
|---|---|---|
| Integration | Uniform `dt/n` per sample | Per-sample `ts[i]-ts[i-1]` |
| Stabilization model | LPF smooth path, correction=smooth-raw | Direct position + spring recenter |
| Recenter | Implicit via LPF tau | Explicit exponential decay + edge boost |
| Bias tracking | Startup calibration only | Runtime slow-adapt in hot path |
| Deadband | None | Per-frame delta gating |
| Slew limit | None | Optional per-frame max step |
| Edge behavior | Hard clamp only | Soft accelerated recenter before clamp |
| Tail gap | Ignored | Extrapolated from last sample |
| Modularity | Monolithic | EIS dispatch interface for multiple backends |
