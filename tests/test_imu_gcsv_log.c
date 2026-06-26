/*
 * Unit tests for imu_gcsv_log — canonical Gyroflow gcsv writer.
 * Host-compiled (no hardware dependencies).
 *
 * Build: gcc -O2 -I../include -pthread tests/test_imu_gcsv_log.c \
 *            src/imu_gcsv_log.c -o tests/test_imu_gcsv_log
 * Run:   ./tests/test_imu_gcsv_log
 */

#include "imu_gcsv_log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	tests_run++; \
	printf("  %-40s ", #name); \
	fflush(stdout); \
} while (0)

#define PASS() do { \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

static const char *TMP = "/tmp/test_imu_gcsv_log.ts";
static const char *TMP_GCSV = "/tmp/test_imu_gcsv_log.gcsv";

static ImuSample make_sample(long sec, long nsec, float gx, float ax)
{
	ImuSample s = {0};
	s.ts.tv_sec = sec;
	s.ts.tv_nsec = nsec;
	s.gyro_x = gx;
	s.accel_x = ax;
	return s;
}

/* Read a whole file into a heap buffer (NUL-terminated). */
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
	return buf;
}

/* Test: header is byte-exact canonical gcsv, written next to the recording. */
static void test_canonical_header(void)
{
	TEST(canonical_header);
	ImuGcsvLog log = { .lock = PTHREAD_MUTEX_INITIALIZER };
	remove(TMP_GCSV);

	assert(imu_gcsv_log_open(&log, TMP) == 0);
	imu_gcsv_log_close(&log);

	char *txt = slurp(TMP_GCSV);
	assert(txt != NULL);
	const char *expect =
		"GYROFLOW IMU LOG\n"
		"version,1.3\n"
		"id,waybeam_venc\n"
		"orientation,xyz\n"
		"tscale,0.000001\n"
		"gscale,1.0\n"
		"ascale,0.101971621\n"
		"t,gx,gy,gz,ax,ay,az\n";
	assert(strcmp(txt, expect) == 0);  /* no samples => header only */
	free(txt);
	remove(TMP_GCSV);
	PASS();
}

/* Test: first sample rebases t to 0; subsequent t are relative microseconds. */
static void test_rebased_timebase(void)
{
	TEST(rebased_timebase);
	ImuGcsvLog log = { .lock = PTHREAD_MUTEX_INITIALIZER };
	remove(TMP_GCSV);

	assert(imu_gcsv_log_open(&log, TMP) == 0);
	/* t0 = 5.000000 s; next sample 1 ms later. */
	ImuSample a = make_sample(5, 0, 0.5f, 9.80665f);
	ImuSample b = make_sample(5, 1000000L, -0.25f, 0.0f);
	imu_gcsv_log_push(&log, &a);
	imu_gcsv_log_push(&log, &b);
	imu_gcsv_log_close(&log);

	char *txt = slurp(TMP_GCSV);
	assert(txt != NULL);
	/* Locate the data rows after the column header. */
	const char *rows = strstr(txt, "t,gx,gy,gz,ax,ay,az\n");
	assert(rows != NULL);
	rows += strlen("t,gx,gy,gz,ax,ay,az\n");
	/* First row: t=0, gx=0.5, ax=9.80665 */
	assert(strncmp(rows, "0,0.500000,", 11) == 0);
	/* Second row begins at t=1000 us */
	const char *nl = strchr(rows, '\n');
	assert(nl != NULL);
	assert(strncmp(nl + 1, "1000,-0.250000,", 15) == 0);
	free(txt);
	remove(TMP_GCSV);
	PASS();
}

/* Test: push/close on an inactive (never-opened) log are safe no-ops. */
static void test_noop_when_closed(void)
{
	TEST(noop_when_closed);
	ImuGcsvLog log = { .lock = PTHREAD_MUTEX_INITIALIZER };
	ImuSample a = make_sample(1, 0, 1.0f, 1.0f);
	imu_gcsv_log_push(&log, &a);   /* must not crash, writes nothing */
	imu_gcsv_log_close(&log);      /* must not crash */
	assert(log.fp == NULL);
	assert(log.rows == 0);
	PASS();
}

/* Test: re-opening swaps to a fresh file (row counter resets). */
static void test_reopen_resets(void)
{
	TEST(reopen_resets);
	ImuGcsvLog log = { .lock = PTHREAD_MUTEX_INITIALIZER };
	assert(imu_gcsv_log_open(&log, TMP) == 0);
	ImuSample a = make_sample(2, 0, 1.0f, 1.0f);
	imu_gcsv_log_push(&log, &a);
	assert(log.rows == 1);
	/* Re-open without explicit close: open() closes the prior file. */
	assert(imu_gcsv_log_open(&log, TMP) == 0);
	assert(log.rows == 0);
	assert(log.have_t0 == 0);
	imu_gcsv_log_close(&log);
	remove(TMP_GCSV);
	PASS();
}

int main(void)
{
	printf("imu_gcsv_log unit tests:\n");

	test_canonical_header();
	test_rebased_timebase();
	test_noop_when_closed();
	test_reopen_resets();

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
