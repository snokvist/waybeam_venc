/* test_attitude_est.c — complementary-filter attitude estimator checks.
 *
 * Drives attitude_est with synthetic accel/gyro sequences at the default
 * 200 Hz ODR and asserts roll/pitch converge to known ground truth within
 * tolerance (spec: multi-telemetry HUD group 6, review finding #4).
 */
#include "test_helpers.h"
#include "attitude_est.h"

#include <math.h>
#include <stdlib.h>

#define G 9.80665f
#define DT_US 5000ULL /* 200 Hz */

/* Feed n static samples of a gravity vector tilted by (roll, pitch). */
static void feed_static(AttitudeEst *e, float roll, float pitch,
	uint64_t *ts, int n)
{
	/* Gravity in sensor frame for x-fwd/y-right/z-down convention:
	 * ax = -sin(pitch)*g, ay = sin(roll)*cos(pitch)*g,
	 * az = cos(roll)*cos(pitch)*g. */
	float ax = -sinf(pitch) * G;
	float ay = sinf(roll) * cosf(pitch) * G;
	float az = cosf(roll) * cosf(pitch) * G;
	for (int i = 0; i < n; i++) {
		*ts += DT_US;
		attitude_est_update(e, 0.0f, 0.0f, 0.0f, ax, ay, az, *ts);
	}
}

int test_attitude_est(void)
{
	int failures = 0;

	/* 1: level start snaps from the first accel sample */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 1);
		CHECK("att_init_level_roll",
		      fabsf(attitude_est_roll_cdeg(&e)) <= 5);
		CHECK("att_init_level_pitch",
		      fabsf(attitude_est_pitch_cdeg(&e)) <= 5);
		CHECK("att_not_settled_yet", !attitude_est_settled(&e));
	}

	/* 2: static 30° roll converges within 1° and settles */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		float roll = 30.0f * (float)M_PI / 180.0f;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, roll, 0.0f, &ts, 400); /* 2 s */
		CHECK("att_roll30_converged",
		      abs(attitude_est_roll_cdeg(&e) - 300) <= 10);
		CHECK("att_roll30_pitch_zero",
		      abs(attitude_est_pitch_cdeg(&e)) <= 10);
		CHECK("att_roll30_settled", attitude_est_settled(&e));
	}

	/* 3: static -15° pitch converges within 1° */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		float pitch = -15.0f * (float)M_PI / 180.0f;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, pitch, &ts, 400);
		CHECK("att_pitchm15_converged",
		      abs(attitude_est_pitch_cdeg(&e) - (-150)) <= 10);
	}

	/* 4: gyro carries a fast roll rotation (accel held at level gravity —
	 * a manoeuvre the accel actively disagrees with). 90°/s for 0.5 s
	 * with tc=2 s must land near 45°: the complementary leak over 0.5 s
	 * is ~1-e^(-0.25) ≈ 22% toward 0, so expect ~35-45°. */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 200); /* settle level */
		float rate = 90.0f * (float)M_PI / 180.0f;
		for (int i = 0; i < 100; i++) { /* 0.5 s */
			ts += DT_US;
			attitude_est_update(&e, rate, 0.0f, 0.0f,
			                    0.0f, 0.0f, G, ts);
		}
		int16_t r = attitude_est_roll_cdeg(&e);
		CHECK("att_gyro_roll_tracks", r >= 330 && r <= 460);
	}

	/* 5: free-fall / manoeuvre accel (|a| far from 1 g) is rejected —
	 * attitude holds on gyro alone instead of chasing garbage. */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 200);
		for (int i = 0; i < 200; i++) {
			ts += DT_US;
			attitude_est_update(&e, 0.0f, 0.0f, 0.0f,
			                    0.5f, 0.5f, 1.0f, ts); /* ~0.13 g */
		}
		CHECK("att_lowg_rejected",
		      abs(attitude_est_roll_cdeg(&e)) <= 10 &&
		      abs(attitude_est_pitch_cdeg(&e)) <= 10);
	}

	/* 6: yaw integrates gyro z and is relative */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 10);
		float rate = 45.0f * (float)M_PI / 180.0f;
		for (int i = 0; i < 200; i++) { /* 1 s */
			ts += DT_US;
			attitude_est_update(&e, 0.0f, 0.0f, rate,
			                    0.0f, 0.0f, G, ts);
		}
		int16_t y = attitude_est_yaw_cdeg(&e);
		CHECK("att_yaw_integrates", y >= 430 && y <= 470);
	}

	/* 7: out-of-order timestamps are ignored, big gaps clamped */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 200);
		int16_t before = attitude_est_roll_cdeg(&e);
		/* out-of-order sample with wild gyro must be a no-op */
		attitude_est_update(&e, 10.0f, 10.0f, 10.0f,
		                    0.0f, 0.0f, G, ts - 100000);
		CHECK("att_ooo_ignored",
		      attitude_est_roll_cdeg(&e) == before);
		/* 10 s gap with 1 rad/s roll rate: dt clamps to 0.1 s →
		 * ~5.7° not ~570° */
		attitude_est_update(&e, 1.0f, 0.0f, 0.0f,
		                    0.0f, 0.0f, G, ts + 10000000);
		CHECK("att_gap_clamped",
		      abs(attitude_est_roll_cdeg(&e)) < 100);
	}

	return failures;
}
