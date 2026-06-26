#ifndef IMU_GCSV_LOG_H
#define IMU_GCSV_LOG_H

/*
 * Canonical Gyroflow gcsv writer for frame-synced BMI270 samples.
 *
 * When the IMU is enabled and a recording is active, the pipeline routes each
 * drained sample here (alongside the shared imu_ring).  The output is a
 * standard Gyroflow gcsv file written next to the recording (matching
 * basename, .gcsv extension) so the clip can be stabilized offline.
 *
 * No custom fields and no embedded RTP sequence numbers: the file is plain
 * canonical gcsv.  Sync to the video is achieved by Gyroflow autosync, helped
 * by rebasing the t column to 0 at recording start (the first sample after
 * open) so the gyro stream and the recorded video start together.
 *
 * Lifecycle (driven by the recorder start/stop sites):
 *   imu_gcsv_log_open()  on recorder start  (iff IMU active)
 *   imu_gcsv_log_push()  from the IMU push callback, per sample
 *   imu_gcsv_log_close() on recorder stop
 *
 * All ops are cheap no-ops while inactive (fp == NULL), so push() stays on the
 * per-frame hot path with negligible cost when not recording.
 */

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#include "imu_bmi270.h"  /* ImuSample */

/*
 * Instances MUST be created with the lock statically initialized, e.g.
 *   static ImuGcsvLog g = { .lock = PTHREAD_MUTEX_INITIALIZER };
 * so push()/close() (which may run on a different thread than open() —
 * Star6E's dual recording thread) always see a valid, fully-published mutex.
 */
typedef struct {
	pthread_mutex_t lock;
	FILE *fp;            /* NULL = inactive; guarded by lock */
	uint64_t t0_us;      /* rebase origin: first sample's CLOCK_MONOTONIC us */
	int have_t0;
	uint64_t rows;       /* samples written since open (close summary)       */
} ImuGcsvLog;

/*
 * imu_gcsv_log_open — Open <rec_path> with its extension swapped to .gcsv,
 * write the canonical gcsv header, and begin logging.  Closes any
 * previously-open file first.  Returns 0 on success, -1 on failure (state
 * left inactive).
 */
int imu_gcsv_log_open(ImuGcsvLog *log, const char *rec_path);

/*
 * imu_gcsv_log_push — Append one sample (thread-safe).  No-op when inactive.
 * The t column is sample-monotonic-us minus the rebase origin.
 */
void imu_gcsv_log_push(ImuGcsvLog *log, const ImuSample *s);

/*
 * imu_gcsv_log_close — Flush, close, print a one-line summary.  Safe to call
 * on an inactive log.
 */
void imu_gcsv_log_close(ImuGcsvLog *log);

#endif
