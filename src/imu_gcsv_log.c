/*
 * Canonical Gyroflow gcsv writer — see include/imu_gcsv_log.h.
 */

#include "imu_gcsv_log.h"

#include <string.h>

/*
 * gcsv scale factors:
 *   tscale * t  -> seconds      (t is CLOCK_MONOTONIC microseconds)
 *   gscale * gx -> rad/s        (BMI270 samples already in rad/s)
 *   ascale * ax -> g            (BMI270 samples in m/s^2; 1/9.80665)
 */
#define IMU_GCSV_HEADER \
	"GYROFLOW IMU LOG\n" \
	"version,1.3\n" \
	"id,waybeam_venc\n" \
	"orientation,xyz\n" \
	"tscale,0.000001\n" \
	"gscale,1.0\n" \
	"ascale,0.101971621\n" \
	"t,gx,gy,gz,ax,ay,az\n"

/* Flush to the kernel roughly once per second (at 200 Hz) so an abrupt
 * power-off on the vehicle loses at most ~1 s of gyro tail. */
#define IMU_GCSV_FLUSH_EVERY 256

/* Build "<rec_path sans extension>.gcsv" into out.  The extension is the part
 * after the last '.' in the final path component; if there is none, ".gcsv"
 * is appended.  Returns 0 on success, -1 if it would not fit. */
static int derive_gcsv_path(char *out, size_t out_sz, const char *rec_path)
{
	const char *slash, *dot;
	size_t stem_len;

	if (!out || out_sz == 0 || !rec_path || !rec_path[0])
		return -1;

	slash = strrchr(rec_path, '/');
	dot = strrchr(rec_path, '.');
	/* Only treat a dot in the final component as an extension. */
	if (dot && (!slash || dot > slash))
		stem_len = (size_t)(dot - rec_path);
	else
		stem_len = strlen(rec_path);

	if (stem_len + sizeof(".gcsv") > out_sz)
		return -1;

	memcpy(out, rec_path, stem_len);
	memcpy(out + stem_len, ".gcsv", sizeof(".gcsv"));  /* includes NUL */
	return 0;
}

int imu_gcsv_log_open(ImuGcsvLog *log, const char *rec_path)
{
	char path[512];
	FILE *f;

	if (!log)
		return -1;

	/* Replace any previously-open file (e.g. recorder restart). */
	imu_gcsv_log_close(log);

	if (derive_gcsv_path(path, sizeof(path), rec_path) != 0) {
		fprintf(stderr,
			"WARNING: [imu_gcsv] path too long for '%s'\n",
			rec_path ? rec_path : "(null)");
		return -1;
	}

	/* "e" = O_CLOEXEC: don't leak the recording fd (or its unflushed tail)
	 * into a fork+exec respawn child if one happens mid-recording. */
	f = fopen(path, "we");
	if (!f) {
		fprintf(stderr, "WARNING: [imu_gcsv] cannot open %s\n", path);
		return -1;
	}

	if (fputs(IMU_GCSV_HEADER, f) == EOF) {
		fprintf(stderr, "WARNING: [imu_gcsv] header write failed %s\n",
			path);
		fclose(f);
		return -1;
	}

	/* Publish the new file + reset counters under the lock so a concurrent
	 * push()/close() on another thread (Star6E dual thread) sees consistent
	 * state.  The mutex is statically initialized by the owner (see header). */
	pthread_mutex_lock(&log->lock);
	log->have_t0 = 0;
	log->t0_us = 0;
	log->rows = 0;
	log->fp = f;
	pthread_mutex_unlock(&log->lock);

	printf("> [imu_gcsv] logging to %s\n", path);
	return 0;
}

void imu_gcsv_log_push(ImuGcsvLog *log, const ImuSample *s)
{
	uint64_t us, t;

	if (!log || !s)
		return;

	/* Always take the lock: fp is written under the lock by open()/close()
	 * (possibly on another thread), so the inactive check must be locked too
	 * to avoid a data race.  Uncontended cost is ~tens of ns at 200 Hz. */
	pthread_mutex_lock(&log->lock);
	if (!log->fp) {
		pthread_mutex_unlock(&log->lock);
		return;
	}

	us = (uint64_t)s->ts.tv_sec * 1000000ull +
	     (uint64_t)s->ts.tv_nsec / 1000ull;
	if (!log->have_t0) {
		log->t0_us = us;
		log->have_t0 = 1;
	}
	t = (us >= log->t0_us) ? (us - log->t0_us) : 0;

	fprintf(log->fp, "%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
		(unsigned long long)t,
		s->gyro_x, s->gyro_y, s->gyro_z,
		s->accel_x, s->accel_y, s->accel_z);

	if ((++log->rows % IMU_GCSV_FLUSH_EVERY) == 0)
		fflush(log->fp);

	pthread_mutex_unlock(&log->lock);
}

void imu_gcsv_log_close(ImuGcsvLog *log)
{
	if (!log)
		return;

	pthread_mutex_lock(&log->lock);
	if (log->fp) {
		fflush(log->fp);
		fclose(log->fp);
		log->fp = NULL;
		printf("> [imu_gcsv] closed (%llu samples)\n",
			(unsigned long long)log->rows);
	}
	pthread_mutex_unlock(&log->lock);
}
