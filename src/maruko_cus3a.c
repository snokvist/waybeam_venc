#include "maruko_cus3a.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── CUS3A / ISP ABI structures ──────────────────────────────────────── */

/* Maruko HW AE statistic cell (mi_isp_hw_dep_datatype.h: MI_ISP_AE_AVGS) —
 * 4 bytes per grid cell, 128×90 grid. */
typedef struct {
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char y;
} MarukoAeSample;

#define MARUKO_AE_GRID_X   128
#define MARUKO_AE_GRID_Y   90
#define MARUKO_AE_GRID_SZ  (MARUKO_AE_GRID_X * MARUKO_AE_GRID_Y)

/* MI_ISP_AE_HW_STATISTICS_t — 8 bytes header + grid */
typedef struct {
	uint32_t       nBlkX;
	uint32_t       nBlkY;
	MarukoAeSample nAvg[MARUKO_AE_GRID_SZ];
} MarukoAeHwStats;

/* CusAEInfo_t — Maruko packed layout (mi_isp_hw_dep_datatype.h:144). */
typedef struct {
	uint32_t Size;
	uint32_t hist1;
	uint32_t hist2;
	uint32_t AvgBlkX;
	uint32_t AvgBlkY;
	uint32_t avgs;
	uint32_t Shutter;
	uint32_t SensorGain;
	uint32_t IspGain;
	uint32_t ShutterHDRShort;
	uint32_t SensorGainHDRShort;
	uint32_t IspGainHDRShort;
} __attribute__((packed)) MarukoAeInfo;

/* CusAEResult_t — Maruko packed layout (mi_isp_hw_dep_datatype.h:175).
 * Pushed via MI_ISP_CUS3A_SetAeParam to inject userspace AE values into
 * the ISP/sensor pipeline.  Field set verified against
 * sdk/verify/mixer/src/mid/maruko_impl/isp/mid_iq_impl.cpp:3304-3324. */
typedef struct {
	uint32_t Size;
	uint32_t Change;
	uint32_t Shutter;
	uint32_t SensorGain;
	uint32_t IspGain;
	uint32_t ShutterHdrShort;
	uint32_t SensorGainHdrShort;
	uint32_t IspGainHdrShort;
	uint32_t u4BVx16384;
	uint32_t AvgY;
	uint32_t HdrRatio;
	uint32_t FNx10;
	uint32_t DebandFPS;
	uint32_t WeightY;
} __attribute__((packed)) MarukoAeResult;

#define MARUKO_ISP_STATE_NORMAL 0

/* ── ISP exposure limit (matches MI_ISP_AE_ExpoLimitType_t) ──────────── */

typedef struct {
	uint32_t minShutterUs;
	uint32_t maxShutterUs;
	uint32_t minFNx10;
	uint32_t maxFNx10;
	uint32_t minSensorGain;
	uint32_t minIspGain;
	uint32_t maxSensorGain;
	uint32_t maxIspGain;
} MarukoIspExposureLimit;

/* ── Function pointer types (resolved via dlsym) ─────────────────────── */

typedef int (*fn_ae_get_hw_stats_t)(uint32_t dev, uint32_t chn,
	MarukoAeHwStats *stats);
typedef int (*fn_cus3a_get_ae_status_t)(uint32_t dev, uint32_t chn,
	MarukoAeInfo *info);
typedef int (*fn_ae_get_state_t)(uint32_t dev, uint32_t chn, int *state);

typedef int (*fn_ae_get_exposure_limit_t)(uint32_t dev, uint32_t chn,
	MarukoIspExposureLimit *limit);
typedef int (*fn_ae_set_exposure_limit_t)(uint32_t dev, uint32_t chn,
	MarukoIspExposureLimit *limit);

typedef int (*fn_cus3a_set_ae_param_t)(uint32_t dev, uint32_t chn,
	MarukoAeResult *result);

/* ── Module state ────────────────────────────────────────────────────── */

typedef struct {
	MarukoCus3aConfig cfg;

	/* runtime caps (writable from main thread) */
	volatile uint32_t shutter_max_us;
	volatile uint32_t gain_max;

	/* baseline limits read from ISP bin */
	uint32_t bin_max_shutter_us;
	uint32_t bin_max_sensor_gain;

	pthread_t thread;
	volatile int running;

	/* refcounted handles — dlclose only on actual stop */
	void *h_isp;
	void *h_cus3a;

	fn_ae_get_hw_stats_t        fn_get_hw_stats;
	fn_cus3a_get_ae_status_t    fn_get_ae_status;
	fn_ae_get_state_t           fn_ae_get_state;
	fn_ae_get_exposure_limit_t  fn_get_exposure_limit;
	fn_ae_set_exposure_limit_t  fn_set_exposure_limit;
	fn_cus3a_set_ae_param_t     fn_set_ae_param;
} Cus3aState;

static Cus3aState g_cus3a;

/* ── Helpers ─────────────────────────────────────────────────────────── */

void maruko_cus3a_config_defaults(MarukoCus3aConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_fps = 120;
	cfg->ae_fps = 15;
}

static uint32_t compute_max_shutter(const Cus3aState *s)
{
	if (s->shutter_max_us > 0)
		return s->shutter_max_us;
	if (s->cfg.sensor_fps > 0)
		return 1000000 / s->cfg.sensor_fps;
	return 8333;  /* fallback ~120fps */
}

static int resolve_symbols(Cus3aState *s)
{
	s->h_isp = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!s->h_isp) {
		fprintf(stderr, "[maruko-cus3a] dlopen libmi_isp.so: %s\n",
			dlerror());
		return -1;
	}
	s->h_cus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!s->h_cus3a) {
		fprintf(stderr, "[maruko-cus3a] dlopen libcus3a.so: %s\n",
			dlerror());
		return -1;
	}

	s->fn_get_hw_stats = (fn_ae_get_hw_stats_t)dlsym(s->h_isp,
		"MI_ISP_AE_GetAeHwAvgStats");
	s->fn_get_ae_status = (fn_cus3a_get_ae_status_t)dlsym(s->h_isp,
		"MI_ISP_CUS3A_GetAeStatus");
	s->fn_ae_get_state = (fn_ae_get_state_t)dlsym(s->h_isp,
		"MI_ISP_AE_GetState");
	s->fn_get_exposure_limit = (fn_ae_get_exposure_limit_t)dlsym(s->h_isp,
		"MI_ISP_AE_GetExposureLimit");
	s->fn_set_exposure_limit = (fn_ae_set_exposure_limit_t)dlsym(s->h_isp,
		"MI_ISP_AE_SetExposureLimit");

	s->fn_set_ae_param = (fn_cus3a_set_ae_param_t)dlsym(s->h_isp,
		"MI_ISP_CUS3A_SetAeParam");

	if (!s->fn_get_hw_stats || !s->fn_get_ae_status) {
		fprintf(stderr,
			"[maruko-cus3a] missing AE stats symbols\n");
		return -1;
	}
	if (!s->fn_get_exposure_limit || !s->fn_set_exposure_limit) {
		fprintf(stderr,
			"[maruko-cus3a] missing exposure limit symbols\n");
		return -1;
	}
	if (s->cfg.throttle_mode && !s->fn_set_ae_param) {
		fprintf(stderr,
			"[maruko-cus3a] throttle mode requested but "
			"MI_ISP_CUS3A_SetAeParam missing — cannot drive AE\n");
		return -1;
	}
	return 0;
}

static void release_symbols(Cus3aState *s)
{
	if (s->h_cus3a) dlclose(s->h_cus3a);
	if (s->h_isp)   dlclose(s->h_isp);
	s->h_isp = NULL;
	s->h_cus3a = NULL;
}

/* ── AE control law ──────────────────────────────────────────────────── */

#define AE_TARGET_Y         80     /* avg-luma target (0-255 codes) */
#define AE_DEAD_BAND        8
#define AE_STEP_NUM         12     /* +20% per tick, ~0.26 EV */
#define AE_STEP_DEN         10
#define AE_GAIN_MIN         1024   /* 1.0x */
#define AE_GAIN_MAX_DEFAULT 32768  /* 32x sensor cap (IMX415) */
#define AE_ISP_GAIN_MAX     8192   /* 8x ISP digital gain ceiling (unused: P4) */
#define AE_SHUTTER_MIN_US   30

/* P1 — log-domain IIR proportional controller (replaces the bang-bang
 * cascade).  Converge in continuous exposure space with smooth damping so
 * there are no visible ~20% steps.  Tune ALPHA on device: lower = smoother
 * but slower.  DEADBAND stops micro-hunting; RATIO_CLAMP bounds how far a
 * single transient can move per tick. */
#define AE_IIR_ALPHA        0.20   /* fraction of log-exposure error / tick */
#define AE_RATIO_CLAMP      4.0    /* max per-tick exposure ratio (and 1/x) */
#define AE_DEADBAND_RATIO   0.015  /* +/-1.5% exposure -> hold */

/* (bang-bang step_up/step_dn removed — replaced by the IIR controller.) */

/* ── Supervisory + AE control thread ─────────────────────────────────── */

static void *cus3a_thread(void *arg)
{
	Cus3aState *s = arg;
	MarukoAeHwStats *ae_hw;
	MarukoAeInfo ae_info;
	MarukoIspExposureLimit cur_limit;
	MarukoAeResult ae_result;
	unsigned int sleep_ms;
	unsigned long ticks = 0, ae_writes = 0, limit_writes = 0;
	unsigned long last_log_ms = 0;
	uint32_t applied_shutter_max = 0;
	uint32_t applied_gain_max = 0;

	uint32_t cur_shutter_us;
	uint32_t cur_sensor_gain = AE_GAIN_MIN;
	uint32_t cur_isp_gain = AE_GAIN_MIN;
	uint32_t shutter_min_us = AE_SHUTTER_MIN_US;
	uint32_t shutter_max_us;
	uint32_t gain_max_eff;

	ae_hw = malloc(sizeof(MarukoAeHwStats));
	if (!ae_hw) {
		fprintf(stderr, "[maruko-cus3a] AE stats alloc failed\n");
		return NULL;
	}

	if (s->fn_ae_get_state) {
		int state = -1;
		s->fn_ae_get_state(0, 0, &state);
		if (state != MARUKO_ISP_STATE_NORMAL)
			fprintf(stderr,
				"[maruko-cus3a] WARN: AE state=%d (not NORMAL); "
				"exposure may not converge\n", state);
		else if (s->cfg.verbose)
			printf("[maruko-cus3a] AE state: NORMAL\n");
	}

	memset(&cur_limit, 0, sizeof(cur_limit));
	s->fn_get_exposure_limit(0, 0, &cur_limit);

	/* In throttle mode the no-op AE adaptor zeros the AE init state, so
	 * any SetExposureLimit write here would clobber the bin's defaults
	 * with zeros.  We don't need it anyway — SetAeParam ignores
	 * SetExposureLimit, and gain_max_eff (computed below) gates our
	 * push directly.  In native mode the live read is valid; we then
	 * fold in our user caps and write back. */
	if (!s->cfg.throttle_mode) {
		uint32_t want_shutter = compute_max_shutter(s);
		uint32_t want_gain = s->gain_max;

		if (want_shutter > 0 &&
		    want_shutter <= cur_limit.maxShutterUs)
			cur_limit.maxShutterUs = want_shutter;
		if (want_gain > 0 && want_gain <= cur_limit.maxSensorGain)
			cur_limit.maxSensorGain = want_gain;

		s->fn_set_exposure_limit(0, 0, &cur_limit);
		limit_writes++;
		if (s->cfg.verbose)
			printf("[maruko-cus3a] init limits: maxShutter=%uus "
				"maxGain=%u\n",
				cur_limit.maxShutterUs,
				cur_limit.maxSensorGain);

		applied_shutter_max = cur_limit.maxShutterUs;
		applied_gain_max = cur_limit.maxSensorGain;
	} else {
		/* Throttle: seed the "applied" trackers from captured bin
		 * baseline so the per-tick "changed" check doesn't trigger
		 * spurious writes. */
		applied_shutter_max = compute_max_shutter(s);
		applied_gain_max = s->gain_max > 0
			? s->gain_max
			: s->bin_max_sensor_gain;
	}

	sleep_ms = s->cfg.ae_fps > 0 ? 1000 / s->cfg.ae_fps : 66;
	if (sleep_ms < 1) sleep_ms = 1;

	shutter_max_us = compute_max_shutter(s);
	/* Prefer the bin's calibrated ceiling when the user hasn't capped
	 * gain explicitly; AE_GAIN_MAX_DEFAULT (32x) is only a last-resort
	 * fallback when bin limits are unavailable.  Pushing past the bin
	 * ceiling produces visible color/brightness drift even though
	 * SetAeParam silently accepts the higher value. */
	gain_max_eff = s->gain_max > 0
		? s->gain_max
		: (s->bin_max_sensor_gain > 0
			? s->bin_max_sensor_gain
			: AE_GAIN_MAX_DEFAULT);

	cur_shutter_us = shutter_max_us > 0 ? shutter_max_us : 8333;
	cur_sensor_gain = AE_GAIN_MIN * 4;       /* 4x mid-light start */
	if (cur_sensor_gain > gain_max_eff)
		cur_sensor_gain = gain_max_eff;

	if (s->cfg.verbose)
		printf("[maruko-cus3a] thread started: %u Hz, mode=%s, "
			"shutter %u..%uus, gain %u..%u\n",
			s->cfg.ae_fps,
			s->cfg.throttle_mode ? "throttle" : "native",
			shutter_min_us, shutter_max_us,
			AE_GAIN_MIN, gain_max_eff);

	while (s->running) {
		unsigned int avg_y = 0;
		int can_drive = 0;
		unsigned int dbg_bx = 0, dbg_by = 0, dbg_avg_lin = 0;

		/* ── Read luminance grid ───────────────────────────────── */
		if (s->fn_get_hw_stats(0, 0, ae_hw) == 0) {
			memset(&ae_info, 0, sizeof(ae_info));
			s->fn_get_ae_status(0, 0, &ae_info);

			/* Block dims come from the grid struct itself
			 * (ae_hw->nBlk*).  ae_info.AvgBlk* (from
			 * MI_ISP_CUS3A_GetAeStatus) read 0 here, so total fell
			 * back to the full 128×90 grid and divided the packed
			 * sum by 11520 instead of the real block count —
			 * pinning avgY to ~4 on a lit scene. */
			unsigned int bx = ae_hw->nBlkX;
			unsigned int by = ae_hw->nBlkY;
			if (bx == 0 || by == 0 || bx > MARUKO_AE_GRID_X ||
			    by > MARUKO_AE_GRID_Y) {
				bx = MARUKO_AE_GRID_X;
				by = MARUKO_AE_GRID_Y;
			}
			unsigned int total = bx * by;
			if (total > 0) {
				unsigned long sum2d = 0, sumlin = 0;
				unsigned int x, y;
				/* 2D: real bx×by sub-block at fixed 128 stride */
				for (y = 0; y < by; y++)
					for (x = 0; x < bx; x++)
						sum2d += ae_hw->nAvg[
						  y * MARUKO_AE_GRID_X + x].y;
				/* packed-linear: the grid is stored packed at
				 * stride nBlkX (device-confirmed: 2D stride-128
				 * read scores ~0, packed-linear ~real). */
				for (x = 0; x < total; x++)
					sumlin += ae_hw->nAvg[x].y;
				avg_y = (unsigned int)(sumlin / total);
				dbg_bx = bx;
				dbg_by = by;
				/* 2D stride-128 kept as diagnostic. This avg_y fed
				 * ONLY the retired throttle controller (throttle_mode
				 * is hard-wired off — see maruko_cus3a.h); in native
				 * mode the SDK owns the AE loop, so this readout is
				 * diagnostic-only and drives nothing live. (Historical:
				 * the packed read reported uAvgY~4 on a lit scene; the
				 * stat scale was never resolved, but it is inert.) */
				dbg_avg_lin = (unsigned int)(sum2d / total);
				can_drive = 1;
			}
		}

		/* ── Enforce static gain/shutter caps from config ──────── */
		/* Only meaningful in native mode (SDK AE respects
		 * SetExposureLimit).  In throttle mode our SetAeParam
		 * ignores SetExposureLimit, so we just track the local
		 * shutter_max_us / gain_max_eff used by the AE controller. */
		{
			uint32_t want_shutter = compute_max_shutter(s);
			uint32_t want_gain = s->gain_max;
			uint32_t effective_gain = want_gain > 0 ? want_gain :
				s->bin_max_sensor_gain;
			int changed = 0;

			if (!s->cfg.throttle_mode) {
				s->fn_get_exposure_limit(0, 0, &cur_limit);

				/* Compare against the FRESH read, not a local
				 * "applied" cache: the CUS3A AE init (first
				 * frame-syncs after start) resets limits to
				 * the bin values, silently undoing an early
				 * push — e.g. isp.gainMax above the bin
				 * ceiling never stuck (found with
				 * gainMax=32000 vs bin 8192, v0.22.0). */
				if (want_shutter > 0 &&
				    cur_limit.maxShutterUs != want_shutter) {
					cur_limit.maxShutterUs = want_shutter;
					applied_shutter_max = want_shutter;
					shutter_max_us = want_shutter;
					changed = 1;
				}
				if (effective_gain > 0 &&
				    cur_limit.maxSensorGain != effective_gain) {
					cur_limit.maxSensorGain = effective_gain;
					applied_gain_max = effective_gain;
					gain_max_eff = effective_gain;
					changed = 1;
				}

				if (changed) {
					int lret = s->fn_set_exposure_limit(
						0, 0, &cur_limit);
					limit_writes++;
					if (s->cfg.verbose)
						printf("[maruko-cus3a] limit "
							"push: shutter max "
							"%uus gain max %u "
							"ret=%d\n",
							cur_limit.maxShutterUs,
							cur_limit.maxSensorGain,
							lret);
				}
			} else {
				/* Throttle: just keep local caps current. */
				if (want_shutter > 0 &&
				    want_shutter != applied_shutter_max) {
					applied_shutter_max = want_shutter;
					shutter_max_us = want_shutter;
				}
				if (effective_gain > 0 &&
				    effective_gain != applied_gain_max) {
					applied_gain_max = effective_gain;
					gain_max_eff = effective_gain;
				}
			}
		}

		/* ── AE control law: 3-stage cascade ───────────────────── */
		/* RETIRED / DEAD CODE — throttle_mode is hard-wired off
		 * (maruko_cus3a.h: "retired; must be 0"), so this whole block
		 * is unreachable at runtime and kept only for reference. In
		 * native mode the SDK's AE runs in 3A_Proc_0 at sensor rate and
		 * driving exposure here would stomp its output. Physical removal
		 * is deferred to a separate device-verified change (it threads
		 * through the live native AE thread). */
		if (s->cfg.throttle_mode && can_drive) {
			/* P1 — log-domain IIR proportional controller (replaces
			 * the ~20% bang-bang cascade).  Converge total exposure
			 * toward the luma target with smooth damping so there are
			 * no visible steps.  P4: ISP digital gain pinned at 1.0x
			 * (it clips colour channels + amplifies noise); converge
			 * with shutter (priority) then analog sensor gain. */
			cur_isp_gain = AE_GAIN_MIN;

			/* Applied exposure in linear units: shutter_us * gain(x). */
			double e_applied = (double)cur_shutter_us *
				((double)cur_sensor_gain / (double)AE_GAIN_MIN);
			if (e_applied < 1.0)
				e_applied = 1.0;

			/* Proportional: exposure scales inversely with luma error
			 * (+1 avoids div-by-zero and softens deep shadow). */
			double ratio = ((double)AE_TARGET_Y + 1.0) /
				((double)avg_y + 1.0);
			if (ratio > AE_RATIO_CLAMP)
				ratio = AE_RATIO_CLAMP;
			else if (ratio < 1.0 / AE_RATIO_CLAMP)
				ratio = 1.0 / AE_RATIO_CLAMP;

			/* Log-domain IIR damping: e_next = e * ratio^alpha —
			 * smooth, perceptually linear, no staircase/overshoot. */
			double e_next = e_applied * pow(ratio, AE_IIR_ALPHA);

			/* Deadband in exposure ratio: hold if the move is tiny. */
			double drift = e_next / e_applied;
			if (drift < 1.0 + AE_DEADBAND_RATIO &&
			    drift > 1.0 - AE_DEADBAND_RATIO)
				e_next = e_applied;

			/* Split across shutter (priority) then analog gain. */
			{
				double e_max_shutter = (double)shutter_max_us;
				if (e_max_shutter < 1.0)
					e_max_shutter = 1.0;
				if (e_next <= e_max_shutter) {
					uint32_t ns = (uint32_t)(e_next + 0.5);
					if (ns < shutter_min_us)
						ns = shutter_min_us;
					cur_shutter_us = ns;
					cur_sensor_gain = AE_GAIN_MIN;
				} else {
					double g = (e_next / e_max_shutter) *
						(double)AE_GAIN_MIN;
					if (g > (double)gain_max_eff)
						g = (double)gain_max_eff;
					if (g < (double)AE_GAIN_MIN)
						g = (double)AE_GAIN_MIN;
					cur_shutter_us = shutter_max_us;
					cur_sensor_gain = (uint32_t)(g + 0.5);
				}
			}

			memset(&ae_result, 0, sizeof(ae_result));
			ae_result.Size = sizeof(ae_result);
			ae_result.Change = 1;
			ae_result.Shutter = cur_shutter_us;
			ae_result.SensorGain = cur_sensor_gain;
			ae_result.IspGain = cur_isp_gain;
			ae_result.ShutterHdrShort = cur_shutter_us;
			ae_result.SensorGainHdrShort = cur_sensor_gain;
			ae_result.IspGainHdrShort = cur_isp_gain;
			ae_result.HdrRatio = 1024;
			ae_result.u4BVx16384 = 16384;
			ae_result.AvgY = avg_y;
			ae_result.FNx10 = 28;
			ae_result.DebandFPS = s->cfg.sensor_fps;
			ae_result.WeightY = avg_y;

			s->fn_set_ae_param(0, 0, &ae_result);
			ae_writes++;
		}

		ticks++;

		{
			struct timespec ts;
			unsigned long now_ms;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_ms = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
			if (s->cfg.verbose && now_ms - last_log_ms >= 5000) {
				if (s->cfg.throttle_mode) {
					printf("[maruko-cus3a] %lu ticks | "
						"%lu ae | %lu lim | "
						"blk=%ux%u avgY=%u(lin=%u) "
						"target=%d | want "
						"shutter=%uus sgain=%u "
						"igain=%u | isp shutter=%uus "
						"sgain=%u igain=%u\n",
						ticks, ae_writes, limit_writes,
						dbg_bx, dbg_by,
						avg_y, dbg_avg_lin, AE_TARGET_Y,
						cur_shutter_us, cur_sensor_gain,
						cur_isp_gain,
						ae_info.Shutter,
						ae_info.SensorGain,
						ae_info.IspGain);
				} else {
					printf("[maruko-cus3a] %lu ticks | "
						"%lu lim | avgY=%u | "
						"isp shutter=%uus sgain=%u "
						"igain=%u\n",
						ticks, limit_writes, avg_y,
						ae_info.Shutter,
						ae_info.SensorGain,
						ae_info.IspGain);
				}
				last_log_ms = now_ms;
			}
		}

		{
			unsigned long ns = sleep_ms * 1000000UL;
			struct timespec req = {
				(time_t)(ns / 1000000000UL),
				(long)(ns % 1000000000UL)
			};
			nanosleep(&req, NULL);
		}
	}

	free(ae_hw);
	if (s->cfg.verbose)
		printf("[maruko-cus3a] thread stopped (%lu ticks, "
			"%lu ae writes, %lu limit writes)\n",
			ticks, ae_writes, limit_writes);
	return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int maruko_cus3a_start(const MarukoCus3aConfig *cfg)
{
	int ret;

	if (g_cus3a.running) {
		fprintf(stderr, "[maruko-cus3a] already running\n");
		return -1;
	}

	memset(&g_cus3a, 0, sizeof(g_cus3a));
	g_cus3a.cfg = *cfg;
	g_cus3a.shutter_max_us = cfg->shutter_max_us;
	g_cus3a.gain_max = cfg->gain_max;

	if (resolve_symbols(&g_cus3a) != 0) {
		release_symbols(&g_cus3a);
		return -1;
	}

	{
		MarukoIspExposureLimit lim;
		memset(&lim, 0, sizeof(lim));
		if (g_cus3a.fn_get_exposure_limit(0, 0, &lim) == 0 &&
		    lim.maxSensorGain > 0) {
			g_cus3a.bin_max_shutter_us = lim.maxShutterUs;
			g_cus3a.bin_max_sensor_gain = lim.maxSensorGain;
			if (cfg->verbose)
				printf("[maruko-cus3a] ISP bin limits: "
					"gain %u-%u, isp_gain max %u, "
					"shutter %u-%uus\n",
					lim.minSensorGain, lim.maxSensorGain,
					lim.maxIspGain, lim.minShutterUs,
					lim.maxShutterUs);
		} else if (cfg->verbose) {
			printf("[maruko-cus3a] ISP bin limits unavailable\n");
		}
	}

	g_cus3a.running = 1;
	ret = pthread_create(&g_cus3a.thread, NULL, cus3a_thread, &g_cus3a);
	if (ret != 0) {
		fprintf(stderr,
			"[maruko-cus3a] pthread_create failed: %d\n", ret);
		g_cus3a.running = 0;
		release_symbols(&g_cus3a);
		return -1;
	}

	return 0;
}

void maruko_cus3a_stop(void)
{
	maruko_cus3a_request_stop();
	maruko_cus3a_join();
}

void maruko_cus3a_request_stop(void)
{
	if (!g_cus3a.running)
		return;
	g_cus3a.running = 0;
}

void maruko_cus3a_join(void)
{
	if (g_cus3a.h_isp == NULL && g_cus3a.h_cus3a == NULL)
		return;
	pthread_join(g_cus3a.thread, NULL);
	release_symbols(&g_cus3a);
	printf("[maruko-cus3a] stopped\n");
}

int maruko_cus3a_running(void)
{
	return g_cus3a.running;
}

void maruko_cus3a_set_gain_max(uint32_t gain)
{
	g_cus3a.gain_max = gain;
}
