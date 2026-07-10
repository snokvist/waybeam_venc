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

/* ── Sensor→camera axis remap ────────────────────────────────────────── */

/* "+x".."-z" → signed unit axis; -1 on parse failure. */
static int axis_parse(const char *s, float v[3])
{
	float sign;
	int i;

	v[0] = v[1] = v[2] = 0.0f;
	if (!s)
		return -1;
	if (s[0] == '+')      sign = 1.0f;
	else if (s[0] == '-') sign = -1.0f;
	else return -1;
	if (s[1] == 'x')      i = 0;
	else if (s[1] == 'y') i = 1;
	else if (s[1] == 'z') i = 2;
	else return -1;
	if (s[2] != '\0')
		return -1;
	v[i] = sign;
	return 0;
}

static void axis_cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void axis_row(AttitudeAxisMap *m, int row, const float v[3])
{
	for (int i = 0; i < 3; i++) {
		if (v[i] != 0.0f) {
			m->idx[row] = i;
			m->sgn[row] = v[i];
			return;
		}
	}
	m->idx[row] = row;   /* unreachable for valid signed unit axes */
	m->sgn[row] = 1.0f;
}

int attitude_axis_map_init(AttitudeAxisMap *m,
	const char *fwd, const char *down)
{
	float f[3], d[3], r[3];

	/* Identity: camera fwd/right/down = sensor x/y/z. */
	m->idx[0] = 0; m->idx[1] = 1; m->idx[2] = 2;
	m->sgn[0] = m->sgn[1] = m->sgn[2] = 1.0f;

	if (axis_parse(fwd, f) != 0 || axis_parse(down, d) != 0)
		return -1;
	{
		int fi = f[0] != 0.0f ? 0 : (f[1] != 0.0f ? 1 : 2);
		int di = d[0] != 0.0f ? 0 : (d[1] != 0.0f ? 1 : 2);
		if (fi == di)
			return -1;   /* parallel / anti-parallel */
	}
	axis_cross(d, f, r);   /* right = down × fwd (right-handed) */
	axis_row(m, 0, f);
	axis_row(m, 1, r);
	axis_row(m, 2, d);
	return 0;
}

void attitude_axis_map_apply(const AttitudeAxisMap *m,
	float in_x, float in_y, float in_z, float out[3])
{
	float in[3] = { in_x, in_y, in_z };

	for (int i = 0; i < 3; i++)
		out[i] = m->sgn[i] * in[m->idx[i]];
}
