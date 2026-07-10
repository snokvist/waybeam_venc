#include "attitude_est.h"

#include <math.h>
#include <string.h>

#define ATT_GRAVITY      9.80665f
/* Accept the accel gravity reference only when |a| is near 1 g —
 * during manoeuvres the specific force is not gravity and would drag
 * the estimate; the gyro carries attitude through those windows. */
#define ATT_ACC_MIN      (0.7f * ATT_GRAVITY)
#define ATT_ACC_MAX      (1.3f * ATT_GRAVITY)
/* Sample gaps beyond this are clamped (drain hiccups, thread stalls). */
#define ATT_DT_MAX_S     0.1f

static float wrap_pi(float a)
{
	while (a > (float)M_PI)
		a -= 2.0f * (float)M_PI;
	while (a < -(float)M_PI)
		a += 2.0f * (float)M_PI;
	return a;
}

void attitude_est_init(AttitudeEst *e, float tc_s)
{
	memset(e, 0, sizeof(*e));
	e->tc_s = tc_s > 0.0f ? tc_s : ATTITUDE_EST_DEFAULT_TC_S;
}

void attitude_est_update(AttitudeEst *e,
	float gx, float gy, float gz,
	float ax, float ay, float az,
	uint64_t ts_us)
{
	float amag = sqrtf(ax * ax + ay * ay + az * az);
	int acc_ok = amag > ATT_ACC_MIN && amag < ATT_ACC_MAX;

	if (!e->have_init) {
		if (!acc_ok)
			return; /* wait for a usable gravity vector */
		e->roll_rad  = atan2f(ay, az);
		e->pitch_rad = atan2f(-ax, sqrtf(ay * ay + az * az));
		e->yaw_rad   = 0.0f;
		e->last_us   = ts_us;
		e->have_init = 1;
		e->samples   = 1;
		return;
	}

	float dt = (float)((int64_t)(ts_us - e->last_us)) * 1e-6f;
	e->last_us = ts_us;
	if (dt <= 0.0f)
		return; /* out of order / duplicate timestamp */
	if (dt > ATT_DT_MAX_S)
		dt = ATT_DT_MAX_S;

	/* Gyro propagation (per-axis Euler integration — adequate for the
	 * HUD's AHI at FPV attitude ranges; not a full DCM). */
	e->roll_rad  = wrap_pi(e->roll_rad + gx * dt);
	e->pitch_rad = wrap_pi(e->pitch_rad + gy * dt);
	e->yaw_rad   = wrap_pi(e->yaw_rad + gz * dt);

	/* Complementary accel correction: first-order lean toward the
	 * gravity reference with time constant tc_s. */
	if (acc_ok) {
		float roll_acc  = atan2f(ay, az);
		float pitch_acc = atan2f(-ax, sqrtf(ay * ay + az * az));
		float k = dt / (e->tc_s + dt);
		e->roll_rad  = wrap_pi(e->roll_rad +
		                       wrap_pi(roll_acc - e->roll_rad) * k);
		e->pitch_rad = wrap_pi(e->pitch_rad +
		                       wrap_pi(pitch_acc - e->pitch_rad) * k);
	}

	if (e->samples < UINT32_MAX)
		e->samples++;
}

int attitude_est_settled(const AttitudeEst *e)
{
	return e->have_init && e->samples >= ATTITUDE_EST_SETTLE_SAMPLES;
}

static int16_t rad_to_cdeg(float rad)
{
	float cdeg = rad * (1800.0f / (float)M_PI);
	if (cdeg > 32767.0f)
		return 32767;
	if (cdeg < -32768.0f)
		return -32768;
	return (int16_t)lrintf(cdeg);
}

int16_t attitude_est_roll_cdeg(const AttitudeEst *e)
{
	return rad_to_cdeg(e->roll_rad);
}

int16_t attitude_est_pitch_cdeg(const AttitudeEst *e)
{
	return rad_to_cdeg(e->pitch_rad);
}

int16_t attitude_est_yaw_cdeg(const AttitudeEst *e)
{
	return rad_to_cdeg(e->yaw_rad);
}
