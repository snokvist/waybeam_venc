#include "eis_legacy.h"
#include "eis_ring.h"
#include "star6e.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Internal config (copied from EisConfig at init)                     */
/* ------------------------------------------------------------------ */

typedef struct {
	int margin_percent;
	uint16_t capture_w;
	uint16_t capture_h;
	int vpe_channel;
	int vpe_port;
	float filter_tau;
	float pixels_per_radian;
	int test_mode;
	int swap_xy;
	int invert_x;
	int invert_y;
} LegacyConfig;

/* ------------------------------------------------------------------ */
/* EIS state                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	LegacyConfig cfg;

	uint16_t crop_w;
	uint16_t crop_h;
	uint16_t margin_x;
	uint16_t margin_y;

	uint16_t crop_x;
	uint16_t crop_y;

	float raw_angle_x;
	float raw_angle_y;
	float smooth_angle_x;
	float smooth_angle_y;

	struct timespec start_ts;
	struct timespec last_update_ts;
	uint32_t update_count;

	EisMotionRing ring;
	double sim_time;
	int imu_active;
	uint32_t last_n_samples;
} LegacyState;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static uint16_t align2(uint16_t v)
{
	return v & ~(uint16_t)1;
}

static float clampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static double ts_to_sec(struct timespec a, struct timespec b)
{
	return (double)(b.tv_sec - a.tv_sec)
		+ (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static int apply_crop(const LegacyState *st)
{
	i6_common_rect crop = {
		.x = st->crop_x,
		.y = st->crop_y,
		.width = st->crop_w,
		.height = st->crop_h,
	};
	return MI_VPE_SetPortCrop(st->cfg.vpe_channel, st->cfg.vpe_port, &crop);
}

/* ------------------------------------------------------------------ */
/* Simulated gyro producer                                            */
/* ------------------------------------------------------------------ */

#define SIM_SAMPLE_RATE  200

#define SIM_X_FREQ1  0.5
#define SIM_X_AMP1   0.15
#define SIM_X_FREQ2  2.3
#define SIM_X_AMP2   0.08

#define SIM_Y_FREQ1  0.35
#define SIM_Y_AMP1   0.10
#define SIM_Y_FREQ2  1.7
#define SIM_Y_AMP2   0.06

static struct timespec ts_interpolate(struct timespec base, long long dt_ns,
	int i, int n)
{
	long long frac_ns = dt_ns * (i + 1) / n;
	struct timespec out = base;
	out.tv_nsec += (long)(frac_ns % 1000000000LL);
	out.tv_sec += (time_t)(frac_ns / 1000000000LL);
	if (out.tv_nsec >= 1000000000L) {
		out.tv_nsec -= 1000000000L;
		out.tv_sec++;
	}
	return out;
}

static void sim_generate_samples(LegacyState *st, double dt)
{
	if (dt <= 0.0)
		return;

	int n_samples = (int)(dt * SIM_SAMPLE_RATE);
	if (n_samples < 1)
		n_samples = 1;
	if (n_samples > (int)EIS_RING_CAPACITY)
		n_samples = (int)EIS_RING_CAPACITY;

	double step = dt / n_samples;
	double t = st->sim_time;
	long long dt_ns = (long long)(dt * 1e9);

	double two_pi = 2.0 * M_PI;
	for (int i = 0; i < n_samples; i++) {
		t += step;

		EisMotionSample s;
		s.ts = ts_interpolate(st->last_update_ts, dt_ns, i, n_samples);
		s.gyro_x = (float)(SIM_X_AMP1 * sin(two_pi * SIM_X_FREQ1 * t)
			+ SIM_X_AMP2 * sin(two_pi * SIM_X_FREQ2 * t));
		s.gyro_y = (float)(SIM_Y_AMP1 * sin(two_pi * SIM_Y_FREQ1 * t)
			+ SIM_Y_AMP2 * sin(two_pi * SIM_Y_FREQ2 * t));
		s.gyro_z = 0.0f;

		eis_ring_push(&st->ring, &s);
	}

	st->sim_time = t;
}

/* ------------------------------------------------------------------ */
/* Clamp offset to margin bounds, set crop position, apply to VPE.    */
/* ------------------------------------------------------------------ */

static int clamp_and_apply_crop(LegacyState *st, float offset_x,
	float offset_y)
{
	int cx = (int)st->margin_x + (int)offset_x;
	int cy = (int)st->margin_y + (int)offset_y;

	int max_x = (int)st->cfg.capture_w - (int)st->crop_w;
	int max_y = (int)st->cfg.capture_h - (int)st->crop_h;
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	if (cx > max_x) cx = max_x;
	if (cy > max_y) cy = max_y;

	st->crop_x = align2((uint16_t)cx);
	st->crop_y = align2((uint16_t)cy);
	return apply_crop(st);
}

/* ------------------------------------------------------------------ */
/* EisOps implementation                                              */
/* ------------------------------------------------------------------ */

static int legacy_update(void *ctx)
{
	LegacyState *st = (LegacyState *)ctx;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double dt = ts_to_sec(st->last_update_ts, now);

	if (dt < 0.0001)
		return 0;

	if (!st->imu_active)
		sim_generate_samples(st, dt);

	/* Test mode: inject visible sine wobble */
	if (st->cfg.test_mode) {
		double elapsed = ts_to_sec(st->start_ts, now);
		float wobble_x = (float)(st->margin_x * 0.8
			* sin(2.0 * M_PI * 0.5 * elapsed));
		float wobble_y = (float)(st->margin_y * 0.8
			* sin(2.0 * M_PI * 0.3 * elapsed));
		int ret = clamp_and_apply_crop(st, wobble_x, wobble_y);

		st->last_update_ts = now;
		st->update_count++;
		return ret;
	}

	/* Read samples from ring buffer for this frame interval */
	EisMotionSample samples[256];
	uint32_t n = eis_ring_read_range(&st->ring, st->last_update_ts, now,
		samples, 256);
	st->last_n_samples = n;

	/* Integrate angular velocity */
	if (n > 0) {
		double sample_dt = dt / n;
		for (uint32_t i = 0; i < n; i++) {
			st->raw_angle_x += samples[i].gyro_x * (float)sample_dt;
			st->raw_angle_y += samples[i].gyro_y * (float)sample_dt;
		}
	}

	/* Low-pass filter: extract smooth camera path */
	float alpha = (float)(dt / (st->cfg.filter_tau + dt));
	st->smooth_angle_x += alpha * (st->raw_angle_x - st->smooth_angle_x);
	st->smooth_angle_y += alpha * (st->raw_angle_y - st->smooth_angle_y);

	float correction_x = st->smooth_angle_x - st->raw_angle_x;
	float correction_y = st->smooth_angle_y - st->raw_angle_y;

	/* Convert angular correction to pixel offset, clamp to margin */
	float px_x = clampf(correction_x * st->cfg.pixels_per_radian,
		-(float)st->margin_x, (float)st->margin_x);
	float px_y = clampf(correction_y * st->cfg.pixels_per_radian,
		-(float)st->margin_y, (float)st->margin_y);

	int ret = clamp_and_apply_crop(st, px_x, px_y);

	st->last_update_ts = now;
	st->update_count++;

	return ret;
}

static void legacy_push_sample(void *ctx, float gx, float gy, float gz,
	const struct timespec *ts)
{
	LegacyState *st = (LegacyState *)ctx;

	EisMotionSample s;
	if (ts)
		s.ts = *ts;
	else
		clock_gettime(CLOCK_MONOTONIC, &s.ts);

	float sx = gx, sy = gy;
	if (st->cfg.swap_xy) {
		float tmp = sx;
		sx = sy;
		sy = tmp;
	}
	if (st->cfg.invert_x) sx = -sx;
	if (st->cfg.invert_y) sy = -sy;

	s.gyro_x = sx;
	s.gyro_y = sy;
	s.gyro_z = gz;

	eis_ring_push(&st->ring, &s);
}

static void legacy_set_imu_active(void *ctx, int active)
{
	LegacyState *st = (LegacyState *)ctx;
	st->imu_active = active;
}

static void legacy_get_status(void *ctx, EisStatus *out)
{
	LegacyState *st = (LegacyState *)ctx;

	if (!out)
		return;

	out->crop_x = st->crop_x;
	out->crop_y = st->crop_y;
	out->crop_w = st->crop_w;
	out->crop_h = st->crop_h;
	out->margin_x = st->margin_x;
	out->margin_y = st->margin_y;
	out->offset_x = (st->smooth_angle_x - st->raw_angle_x)
		* st->cfg.pixels_per_radian;
	out->offset_y = (st->smooth_angle_y - st->raw_angle_y)
		* st->cfg.pixels_per_radian;
	out->update_count = st->update_count;
	out->last_n_samples = st->last_n_samples;
	out->raw_angle_x = st->raw_angle_x;
	out->raw_angle_y = st->raw_angle_y;
	out->smooth_angle_x = st->smooth_angle_x;
	out->smooth_angle_y = st->smooth_angle_y;
	out->ring_count = eis_ring_count(&st->ring);
}

static void legacy_destroy(void *ctx)
{
	LegacyState *st = (LegacyState *)ctx;
	if (!st)
		return;

	printf("  - EIS   : legacy shutdown after %u updates\n",
		st->update_count);
	eis_ring_destroy(&st->ring);
	free(st);
}

/* ------------------------------------------------------------------ */
/* Vtable                                                             */
/* ------------------------------------------------------------------ */

static const EisOps legacy_ops = {
	.update = legacy_update,
	.push_sample = legacy_push_sample,
	.set_imu_active = legacy_set_imu_active,
	.get_status = legacy_get_status,
	.destroy = legacy_destroy,
};

/* ------------------------------------------------------------------ */
/* Factory                                                            */
/* ------------------------------------------------------------------ */

EisState *eis_legacy_create(const EisConfig *cfg)
{
	if (!cfg || cfg->margin_percent <= 0 || cfg->margin_percent >= 50) {
		fprintf(stderr, "EIS: invalid margin_percent %d (must be 1..49)\n",
			cfg ? cfg->margin_percent : 0);
		return NULL;
	}
	if (cfg->capture_w < 64 || cfg->capture_h < 64) {
		fprintf(stderr, "EIS: capture dimensions too small\n");
		return NULL;
	}

	LegacyState *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->cfg.margin_percent = cfg->margin_percent;
	st->cfg.capture_w = cfg->capture_w;
	st->cfg.capture_h = cfg->capture_h;
	st->cfg.vpe_channel = cfg->vpe_channel;
	st->cfg.vpe_port = cfg->vpe_port;
	st->cfg.filter_tau = cfg->filter_tau > 0.0f ? cfg->filter_tau : 1.0f;
	st->cfg.pixels_per_radian = cfg->pixels_per_radian > 0.0f
		? cfg->pixels_per_radian : (float)cfg->capture_w / 2.0f;
	st->cfg.test_mode = cfg->test_mode;
	st->cfg.swap_xy = cfg->swap_xy;
	st->cfg.invert_x = cfg->invert_x;
	st->cfg.invert_y = cfg->invert_y;

	uint16_t total_margin_x = align2(
		(uint16_t)(cfg->capture_w * cfg->margin_percent / 100));
	uint16_t total_margin_y = align2(
		(uint16_t)(cfg->capture_h * cfg->margin_percent / 100));

	st->margin_x = total_margin_x / 2;
	st->margin_y = total_margin_y / 2;
	st->crop_w = align2(cfg->capture_w - total_margin_x);
	st->crop_h = align2(cfg->capture_h - total_margin_y);

	if (st->crop_w < 64 || st->crop_h < 64) {
		fprintf(stderr, "EIS: margin too large, crop window %ux%u\n",
			st->crop_w, st->crop_h);
		free(st);
		return NULL;
	}

	st->crop_x = st->margin_x;
	st->crop_y = st->margin_y;

	eis_ring_init(&st->ring);

	clock_gettime(CLOCK_MONOTONIC, &st->start_ts);
	st->last_update_ts = st->start_ts;
	st->sim_time = 0.0;

	printf("  - EIS   : legacy mode, margin %d%% (%u,%u px per side)\n",
		cfg->margin_percent, st->margin_x, st->margin_y);
	printf("  - EIS   : crop %ux%u within %ux%u capture\n",
		st->crop_w, st->crop_h, cfg->capture_w, cfg->capture_h);
	printf("  - EIS   : filter tau=%.2fs, px/rad=%.0f\n",
		st->cfg.filter_tau, st->cfg.pixels_per_radian);
	if (st->cfg.swap_xy || st->cfg.invert_x || st->cfg.invert_y)
		printf("  - EIS   : axis: %s%s%s\n",
			st->cfg.swap_xy ? "swap-xy " : "",
			st->cfg.invert_x ? "invert-x " : "",
			st->cfg.invert_y ? "invert-y " : "");

	int ret = apply_crop(st);
	if (ret != 0) {
		fprintf(stderr, "EIS: initial MI_VPE_SetPortCrop failed %d\n", ret);
		eis_ring_destroy(&st->ring);
		free(st);
		return NULL;
	}

	/* Wrap in dispatch handle */
	EisState *handle = calloc(1, sizeof(*handle));
	if (!handle) {
		eis_ring_destroy(&st->ring);
		free(st);
		return NULL;
	}
	handle->ops = &legacy_ops;
	handle->ctx = st;
	return handle;
}
