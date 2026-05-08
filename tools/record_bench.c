/*
 * record_bench — synthetic mirror-mode recording benchmark.
 *
 * Mimics the I/O pattern of src/star6e_ts_recorder.c (per-frame write of
 * TS-aligned buffers, sync_file_range cadence, fdatasync at segment
 * rotation, statvfs polling) without needing the encoder pipeline.  Point
 * it at a directory and it will simulate a video record at the requested
 * bitrate/fps, then report write latency, throughput, CPU usage, and a
 * guesstimated max sustainable bitrate for mirror recording.
 *
 * Build via Makefile targets: `record-bench` (host), `record-bench-star6e`,
 * `record-bench-maruko`.  See documentation/SD_CARD_RECORDING.md for usage.
 *
 * The benchmark generates pseudo-random frame payloads (incompressible, so
 * the SD card's controller cannot cheat with internal compression) and
 * issues one write() per simulated frame plus a sync_file_range every
 * --sync-interval frames, matching the production recorder.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Match src/star6e_ts_recorder.c defaults. */
#define TS_PACKET_SIZE                188
#define DEFAULT_SYNC_INTERVAL_FRAMES  900
#define DEFAULT_SPACE_CHECK_FRAMES    300
#define MIN_FREE_BYTES                (50ULL * 1024 * 1024)
/* Match production TS_BUF_SIZE in src/star6e_ts_recorder.c (3000 × 188).
 * The real recorder can emit a single drained-audio + IDR write of this
 * size; the bench should be able to simulate the same shape. */
#define MAX_FRAME_BYTES               (3000 * TS_PACKET_SIZE)   /* 564000 */

/* Bench-only tuning. */
#define MAX_LATENCY_SAMPLES           (1u << 18)     /* 262 144 frames */
#define SEGMENT_PATH_MAX              512

static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig)
{
	(void)sig;
	g_interrupted = 1;
}

typedef struct {
	const char *dir;
	uint32_t    bitrate_kbps;
	uint32_t    audio_kbps;
	uint32_t    fps;
	uint32_t    duration_s;
	uint32_t    rotate_mb;
	uint32_t    rotate_secs;
	uint32_t    sync_interval;
	bool        keep_files;
	bool        json;
	bool        no_audio;
	/* Sweep mode */
	bool        sweep;
	uint32_t    sweep_start_kbps;
	uint32_t    sweep_step_kbps;
	uint32_t    sweep_max_kbps;
	uint32_t    sweep_duration_s;
} BenchOpts;

typedef struct {
	uint32_t bitrate_kbps;
	uint32_t fps;
	uint64_t frames;
	uint64_t bytes_written;
	uint32_t segments;
	double   wall_s;
	double   user_cpu_s;
	double   sys_cpu_s;
	double   cpu_pct;
	double   throughput_mb_s;
	double   p50_us;
	double   p95_us;
	double   p99_us;
	double   max_us;
	double   mean_us;
	uint64_t stalls;          /* frames whose write exceeded budget */
	double   frame_budget_us;
	double   final_fsync_us;
	double   rotation_fsync_mean_us;
	double   rotation_fsync_max_us;
	uint32_t rotation_fsync_count;
	uint64_t free_before;
	uint64_t free_after;
	int      errno_at_failure;
	bool     interrupted;
	bool     ok;
} BenchResult;

/* ── Utilities ───────────────────────────────────────────────────────── */

static double timespec_to_s(const struct timespec *ts)
{
	return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

static double monotonic_now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return timespec_to_s(&ts);
}

static uint64_t free_bytes(const char *dir)
{
	struct statvfs st;
	if (statvfs(dir, &st) != 0)
		return 0;
	return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a;
	uint32_t y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static double percentile_us(uint32_t *samples, size_t n, double p)
{
	if (n == 0)
		return 0.0;
	size_t idx = (size_t)(p * (double)(n - 1));
	if (idx >= n)
		idx = n - 1;
	return (double)samples[idx];
}

/* Fill the first `len` bytes with deterministic pseudo-random data so the
 * SD card cannot dedupe or compress.  Cheaper than rand(): xorshift32. */
static void fill_random(uint8_t *buf, size_t len, uint32_t *state)
{
	uint32_t s = *state;
	size_t i = 0;

	while (i + 4 <= len) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		buf[i++] = (uint8_t)(s);
		buf[i++] = (uint8_t)(s >> 8);
		buf[i++] = (uint8_t)(s >> 16);
		buf[i++] = (uint8_t)(s >> 24);
	}
	while (i < len) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		buf[i++] = (uint8_t)s;
	}
	*state = s;
}

static ssize_t write_all(int fd, const uint8_t *data, size_t len)
{
	size_t total = 0;
	while (total < len) {
		ssize_t r = write(fd, data + total, len - total);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return (ssize_t)total;
		total += (size_t)r;
	}
	return (ssize_t)total;
}

/* ── Per-frame size model ────────────────────────────────────────────── */

/* Average frame payload bytes for a given bitrate/fps, then round up to
 * a TS packet boundary (the production recorder writes whole 188-byte
 * packets).  Mirror mode = ch0 video + interleaved audio. */
static size_t frame_bytes_for(uint32_t bitrate_kbps, uint32_t audio_kbps,
	uint32_t fps)
{
	if (fps == 0)
		fps = 30;
	uint64_t total_bps = ((uint64_t)bitrate_kbps + audio_kbps) * 1000ULL;
	uint64_t bytes = total_bps / 8ULL / (uint64_t)fps;
	/* Round up to TS packet boundary */
	bytes = ((bytes + TS_PACKET_SIZE - 1) / TS_PACKET_SIZE) * TS_PACKET_SIZE;
	if (bytes < TS_PACKET_SIZE)
		bytes = TS_PACKET_SIZE;
	if (bytes > MAX_FRAME_BYTES)
		bytes = MAX_FRAME_BYTES;
	return (size_t)bytes;
}

/* ── Single benchmark run ────────────────────────────────────────────── */

/* Segment-path tracker.  Every segment opened goes here; cleanup walks the
 * full list so multi-rotation runs don't leak files on the SD card.
 * Capacity grows geometrically. */
typedef struct {
	char   **paths;
	size_t   count;
	size_t   cap;
} PathList;

static int path_list_push(PathList *pl, const char *p)
{
	if (pl->count == pl->cap) {
		size_t ncap = pl->cap ? pl->cap * 2 : 8;
		char **np = realloc(pl->paths, ncap * sizeof(*np));
		if (!np)
			return -1;
		pl->paths = np;
		pl->cap = ncap;
	}
	pl->paths[pl->count] = strdup(p);
	if (!pl->paths[pl->count])
		return -1;
	pl->count++;
	return 0;
}

static void path_list_unlink_all(PathList *pl)
{
	for (size_t i = 0; i < pl->count; i++)
		(void)unlink(pl->paths[i]);
}

static void path_list_free(PathList *pl)
{
	for (size_t i = 0; i < pl->count; i++)
		free(pl->paths[i]);
	free(pl->paths);
	pl->paths = NULL;
	pl->count = pl->cap = 0;
}

static int open_segment(const char *dir, uint32_t segment_idx,
	char *path_out, size_t path_cap, PathList *pl)
{
	const char *sep = (dir[strlen(dir) - 1] == '/') ? "" : "/";
	/* Mix monotonic nanoseconds with PID so two parallel ssh sessions
	 * (or repeated runs that hit the same µs window) don't collide. */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	unsigned int rand_suffix =
		(unsigned int)((ts.tv_nsec ^ (long)getpid()) & 0xFFFF);

	snprintf(path_out, path_cap, "%s%srecbench_%03u_%04x.ts",
		dir, sep, segment_idx, rand_suffix);

	int fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "[bench] open %s failed: %s\n", path_out,
			strerror(errno));
		return fd;
	}
	if (pl && path_list_push(pl, path_out) != 0) {
		fprintf(stderr, "[bench] path tracker oom; "
			"file may not be cleaned: %s\n", path_out);
	}
	return fd;
}

static int run_bench(const BenchOpts *opts, uint32_t bitrate_kbps,
	uint32_t duration_s, BenchResult *out)
{
	memset(out, 0, sizeof(*out));
	out->bitrate_kbps = bitrate_kbps;
	out->fps = opts->fps;
	out->frame_budget_us = 1e6 / (double)opts->fps;

	uint32_t audio_kbps = opts->no_audio ? 0 : opts->audio_kbps;
	size_t frame_bytes = frame_bytes_for(bitrate_kbps, audio_kbps,
		opts->fps);
	uint64_t target_frames = (uint64_t)opts->fps * duration_s;
	if (target_frames == 0)
		target_frames = 1;

	uint8_t *buf = malloc(frame_bytes);
	if (!buf) {
		fprintf(stderr, "[bench] malloc(%zu) failed\n", frame_bytes);
		return -1;
	}
	uint32_t prng_state = 0xC0FFEEu ^ (uint32_t)bitrate_kbps;
	fill_random(buf, frame_bytes, &prng_state);

	uint32_t *samples = NULL;
	size_t sample_cap = (size_t)target_frames;
	if (sample_cap > MAX_LATENCY_SAMPLES)
		sample_cap = MAX_LATENCY_SAMPLES;
	if (sample_cap > 0) {
		samples = malloc(sample_cap * sizeof(*samples));
		if (!samples) {
			free(buf);
			fprintf(stderr, "[bench] sample buffer alloc failed\n");
			return -1;
		}
	}
	size_t sample_n = 0;
	uint64_t stride = 1;
	if (target_frames > MAX_LATENCY_SAMPLES)
		stride = (target_frames + MAX_LATENCY_SAMPLES - 1)
			/ MAX_LATENCY_SAMPLES;

	out->free_before = free_bytes(opts->dir);
	if (out->free_before > 0 && out->free_before < MIN_FREE_BYTES) {
		fprintf(stderr, "[bench] insufficient free space on %s\n",
			opts->dir);
		free(buf);
		free(samples);
		return -1;
	}

	PathList segs = { 0 };
	uint32_t rotation_count = 0;
	double rotation_sum_us = 0.0;
	double rotation_max_us = 0.0;

	char path[SEGMENT_PATH_MAX];
	uint32_t segment_idx = 0;
	int fd = open_segment(opts->dir, segment_idx, path,
		sizeof(path), &segs);
	if (fd < 0) {
		free(buf);
		free(samples);
		path_list_free(&segs);
		return -1;
	}

	uint64_t segment_bytes = 0;
	uint64_t segment_start_frame = 0;
	uint64_t rotate_bytes = (uint64_t)opts->rotate_mb * 1024ULL * 1024ULL;
	uint64_t rotate_frames = (uint64_t)opts->rotate_secs * opts->fps;
	uint64_t frames_since_sync = 0;
	uint64_t space_check_countdown = DEFAULT_SPACE_CHECK_FRAMES;
	uint64_t total_bytes = 0;
	uint64_t stalls = 0;
	double max_us = 0.0;
	double sum_us = 0.0;

	struct rusage ru_start, ru_end;
	getrusage(RUSAGE_SELF, &ru_start);
	double wall_start = monotonic_now_s();
	double next_frame_at = wall_start;
	const double frame_period_s = 1.0 / (double)opts->fps;

	out->ok = true;
	uint64_t f;
	for (f = 0; f < target_frames; f++) {
		if (g_interrupted) {
			out->interrupted = true;
			out->ok = false;
			break;
		}
		/* Pace to fps so the SD card sees bursts the way the encoder
		 * would actually deliver them.  Don't sleep the slack away
		 * if we're already late — that mimics encoder backpressure. */
		double now = monotonic_now_s();
		if (now < next_frame_at) {
			struct timespec rq;
			double dt = next_frame_at - now;
			rq.tv_sec = (time_t)dt;
			rq.tv_nsec = (long)((dt - (double)rq.tv_sec) * 1e9);
			nanosleep(&rq, NULL);
		}
		next_frame_at += frame_period_s;

		/* Vary one byte per frame so the kernel doesn't elide
		 * identical writes in any future optimization. */
		buf[0] = (uint8_t)(f & 0xFF);

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		ssize_t n = write_all(fd, buf, frame_bytes);
		clock_gettime(CLOCK_MONOTONIC, &t1);

		if (n < 0) {
			out->errno_at_failure = errno;
			fprintf(stderr, "[bench] write failed at frame %"
				PRIu64 ": %s\n", f, strerror(errno));
			out->ok = false;
			break;
		}

		double dt_us = (timespec_to_s(&t1) - timespec_to_s(&t0)) * 1e6;
		sum_us += dt_us;
		if (dt_us > max_us)
			max_us = dt_us;
		if (dt_us > out->frame_budget_us)
			stalls++;
		/* Mean/max/stalls use every frame.  Percentiles use a strided
		 * subset when the run exceeds MAX_LATENCY_SAMPLES (≈ 73 min @
		 * 60 fps); the stride keeps memory bounded without skewing the
		 * distribution. */
		if (samples && (f % stride) == 0 && sample_n < sample_cap)
			samples[sample_n++] = (uint32_t)
				(dt_us > (double)UINT32_MAX
				? UINT32_MAX : dt_us);

		total_bytes += (uint64_t)n;
		segment_bytes += (uint64_t)n;
		frames_since_sync++;

		if (opts->sync_interval > 0
		    && frames_since_sync >= opts->sync_interval) {
			sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
			frames_since_sync = 0;
		}

		if (space_check_countdown > 0) {
			space_check_countdown--;
		} else {
			space_check_countdown = DEFAULT_SPACE_CHECK_FRAMES;
			uint64_t fb = free_bytes(opts->dir);
			if (fb > 0 && fb < MIN_FREE_BYTES) {
				fprintf(stderr,
					"[bench] disk space low at frame %"
					PRIu64 ", stopping\n", f);
				out->ok = false;
				break;
			}
		}

		bool rotate = false;
		if (rotate_bytes > 0 && segment_bytes >= rotate_bytes)
			rotate = true;
		if (rotate_frames > 0
		    && (f - segment_start_frame) >= rotate_frames)
			rotate = true;
		if (rotate) {
			/* Time the rotation fdatasync — the production recorder
			 * pays this cost too and it's invisible in per-frame
			 * latency (the write clock has already stopped). */
			double rfs_start = monotonic_now_s();
			fdatasync(fd);
			double rfs_us =
				(monotonic_now_s() - rfs_start) * 1e6;
			close(fd);
			rotation_count++;
			rotation_sum_us += rfs_us;
			if (rfs_us > rotation_max_us)
				rotation_max_us = rfs_us;
			segment_idx++;
			fd = open_segment(opts->dir, segment_idx, path,
				sizeof(path), &segs);
			if (fd < 0) {
				out->ok = false;
				break;
			}
			segment_bytes = 0;
			segment_start_frame = f + 1;
		}
	}

	/* Capture wall time BEFORE the final sync so throughput reflects
	 * steady-state recording, not the one-shot stop cost. */
	double wall_end = monotonic_now_s();
	getrusage(RUSAGE_SELF, &ru_end);

	/* Final sync — report its latency separately so users can see how
	 * long a "stop recording" call would block. */
	double fsync_start = monotonic_now_s();
	if (fd >= 0) {
		fdatasync(fd);
		close(fd);
	}
	out->final_fsync_us = (monotonic_now_s() - fsync_start) * 1e6;

	out->wall_s = wall_end - wall_start;
	out->frames = f;
	out->bytes_written = total_bytes;
	out->segments = segment_idx + 1;
	out->stalls = stalls;
	out->max_us = max_us;
	out->mean_us = (f > 0) ? (sum_us / (double)f) : 0.0;
	out->throughput_mb_s = (out->wall_s > 0)
		? ((double)total_bytes / 1048576.0) / out->wall_s : 0.0;
	out->user_cpu_s =
		(double)(ru_end.ru_utime.tv_sec - ru_start.ru_utime.tv_sec)
		+ (double)(ru_end.ru_utime.tv_usec - ru_start.ru_utime.tv_usec)
		  / 1e6;
	out->sys_cpu_s =
		(double)(ru_end.ru_stime.tv_sec - ru_start.ru_stime.tv_sec)
		+ (double)(ru_end.ru_stime.tv_usec - ru_start.ru_stime.tv_usec)
		  / 1e6;
	out->cpu_pct = (out->wall_s > 0)
		? ((out->user_cpu_s + out->sys_cpu_s) / out->wall_s * 100.0)
		: 0.0;

	if (samples && sample_n > 0) {
		qsort(samples, sample_n, sizeof(*samples), cmp_u32);
		out->p50_us = percentile_us(samples, sample_n, 0.50);
		out->p95_us = percentile_us(samples, sample_n, 0.95);
		out->p99_us = percentile_us(samples, sample_n, 0.99);
	}

	out->rotation_fsync_count = rotation_count;
	out->rotation_fsync_max_us = rotation_max_us;
	out->rotation_fsync_mean_us = rotation_count
		? (rotation_sum_us / (double)rotation_count) : 0.0;

	out->free_after = free_bytes(opts->dir);

	if (!opts->keep_files)
		path_list_unlink_all(&segs);
	path_list_free(&segs);

	free(buf);
	free(samples);
	return out->ok ? 0 : -1;
}

/* ── Reporting ───────────────────────────────────────────────────────── */

static uint32_t effective_audio_kbps(const BenchOpts *opts)
{
	return opts->no_audio ? 0u : opts->audio_kbps;
}

static void print_human(const BenchResult *r, const BenchOpts *opts)
{
	printf("\n=== record_bench result ===\n");
	printf("  dir              : %s\n", opts->dir);
	printf("  bitrate target   : %u kbps (+%u kbps audio)\n",
		r->bitrate_kbps, effective_audio_kbps(opts));
	printf("  fps              : %u (frame budget %.0f us)\n",
		r->fps, r->frame_budget_us);
	printf("  frames written   : %" PRIu64 " in %u segments\n",
		r->frames, r->segments);
	printf("  bytes written    : %.2f MiB\n",
		(double)r->bytes_written / 1048576.0);
	printf("  wall time        : %.3f s\n", r->wall_s);
	printf("  effective rate   : %.2f MB/s (%.2f Mbps)\n",
		r->throughput_mb_s, r->throughput_mb_s * 8.0);
	printf("  cpu user / sys   : %.3f s / %.3f s  (%.1f%% wall)\n",
		r->user_cpu_s, r->sys_cpu_s, r->cpu_pct);
	printf("  write latency    : mean %.0f us  p50 %.0f  p95 %.0f  "
		"p99 %.0f  max %.0f us\n",
		r->mean_us, r->p50_us, r->p95_us, r->p99_us, r->max_us);
	printf("  stalls (>budget) : %" PRIu64 " (%.2f%% of frames)\n",
		r->stalls,
		r->frames ? 100.0 * (double)r->stalls / (double)r->frames : 0.0);
	if (r->rotation_fsync_count > 0)
		printf("  rotation fdatasync : %u events  mean %.1f ms  "
			"max %.1f ms\n",
			r->rotation_fsync_count,
			r->rotation_fsync_mean_us / 1000.0,
			r->rotation_fsync_max_us / 1000.0);
	printf("  final fdatasync  : %.1f ms\n", r->final_fsync_us / 1000.0);
	printf("  free space delta : %.1f MiB used (%.1f -> %.1f MiB free)\n",
		(double)((int64_t)r->free_before - (int64_t)r->free_after)
			/ 1048576.0,
		(double)r->free_before / 1048576.0,
		(double)r->free_after / 1048576.0);
	if (r->interrupted)
		printf("  status           : INTERRUPTED (SIGINT)\n");
	else if (!r->ok)
		printf("  status           : FAILED (errno=%d %s)\n",
			r->errno_at_failure,
			r->errno_at_failure ? strerror(r->errno_at_failure)
			: "early stop");
}

static void print_json(const BenchResult *r, const BenchOpts *opts)
{
	printf("{");
	printf("\"type\":\"step\",");
	printf("\"dir\":\"%s\",", opts->dir);
	printf("\"bitrate_kbps\":%u,", r->bitrate_kbps);
	printf("\"audio_kbps\":%u,", effective_audio_kbps(opts));
	printf("\"fps\":%u,", r->fps);
	printf("\"frames\":%" PRIu64 ",", r->frames);
	printf("\"segments\":%u,", r->segments);
	printf("\"bytes\":%" PRIu64 ",", r->bytes_written);
	printf("\"wall_s\":%.3f,", r->wall_s);
	printf("\"throughput_mb_s\":%.3f,", r->throughput_mb_s);
	printf("\"cpu_user_s\":%.3f,", r->user_cpu_s);
	printf("\"cpu_sys_s\":%.3f,", r->sys_cpu_s);
	printf("\"cpu_pct\":%.2f,", r->cpu_pct);
	printf("\"latency_mean_us\":%.0f,", r->mean_us);
	printf("\"latency_p50_us\":%.0f,", r->p50_us);
	printf("\"latency_p95_us\":%.0f,", r->p95_us);
	printf("\"latency_p99_us\":%.0f,", r->p99_us);
	printf("\"latency_max_us\":%.0f,", r->max_us);
	printf("\"frame_budget_us\":%.0f,", r->frame_budget_us);
	printf("\"stalls\":%" PRIu64 ",", r->stalls);
	printf("\"rotation_fsync_count\":%u,", r->rotation_fsync_count);
	printf("\"rotation_fsync_mean_ms\":%.2f,",
		r->rotation_fsync_mean_us / 1000.0);
	printf("\"rotation_fsync_max_ms\":%.2f,",
		r->rotation_fsync_max_us / 1000.0);
	printf("\"final_fdatasync_ms\":%.2f,", r->final_fsync_us / 1000.0);
	printf("\"free_before\":%" PRIu64 ",", r->free_before);
	printf("\"free_after\":%" PRIu64 ",", r->free_after);
	printf("\"errno\":%d,", r->errno_at_failure);
	printf("\"interrupted\":%s,", r->interrupted ? "true" : "false");
	printf("\"ok\":%s", r->ok ? "true" : "false");
	printf("}\n");
}

/* Heuristic: a bitrate is "sustainable" if
 *   - throughput >= 95% of nominal target
 *   - p99 frame write < 50% of frame budget
 *   - stalls < 1% of frames
 * The 50% p99 rule leaves headroom for the encoder, RTP send, and audio
 * mux to share the same wall clock budget. */
static bool sustainable(const BenchResult *r, uint32_t audio_kbps)
{
	if (!r->ok)
		return false;
	double target_mb_s =
		(double)(r->bitrate_kbps + audio_kbps) * 1000.0
		/ 8.0 / 1048576.0;
	if (r->throughput_mb_s < target_mb_s * 0.95)
		return false;
	if (r->p99_us > r->frame_budget_us * 0.50)
		return false;
	if (r->frames > 0
	    && (double)r->stalls / (double)r->frames > 0.01)
		return false;
	return true;
}

/* ── Sweep mode ──────────────────────────────────────────────────────── */

static int run_sweep(const BenchOpts *opts)
{
	uint32_t last_ok = 0;
	uint32_t first_fail = 0;
	bool interrupted = false;
	BenchResult last_ok_result;
	memset(&last_ok_result, 0, sizeof(last_ok_result));

	for (uint32_t kbps = opts->sweep_start_kbps;
	     kbps <= opts->sweep_max_kbps;
	     kbps += opts->sweep_step_kbps) {
		if (g_interrupted) {
			interrupted = true;
			break;
		}
		fprintf(stderr, "[bench] sweep step: %u kbps for %u s\n",
			kbps, opts->sweep_duration_s);
		BenchResult r;
		int rc = run_bench(opts, kbps, opts->sweep_duration_s, &r);
		if (opts->json)
			print_json(&r, opts);
		else
			print_human(&r, opts);

		if (r.interrupted) {
			interrupted = true;
			break;
		}
		if (rc == 0 && sustainable(&r, effective_audio_kbps(opts))) {
			last_ok = kbps;
			last_ok_result = r;
		} else {
			first_fail = kbps;
			break;
		}
	}

	uint32_t recommended = last_ok
		? (uint32_t)((double)last_ok * 0.9) : 0u;
	bool reached_max = (last_ok > 0) && (first_fail == 0) && !interrupted;

	if (opts->json) {
		printf("{");
		printf("\"type\":\"summary\",");
		printf("\"highest_sustainable_kbps\":%u,", last_ok);
		printf("\"first_failing_kbps\":%u,", first_fail);
		printf("\"recommended_max_kbps\":%u,", recommended);
		printf("\"reached_sweep_max\":%s,",
			reached_max ? "true" : "false");
		printf("\"interrupted\":%s,",
			interrupted ? "true" : "false");
		printf("\"last_ok_p99_us\":%.0f,", last_ok_result.p99_us);
		printf("\"last_ok_cpu_pct\":%.2f,", last_ok_result.cpu_pct);
		printf("\"last_ok_throughput_mb_s\":%.3f",
			last_ok_result.throughput_mb_s);
		printf("}\n");
		return last_ok ? 0 : 1;
	}

	printf("\n=== sweep summary ===\n");
	if (last_ok == 0) {
		if (interrupted)
			printf("  Interrupted before any step completed.\n");
		else
			printf("  No bitrate met the sustainability bar "
				"(start=%u kbps).\n"
				"  Try lowering --sweep-start or increasing "
				"--sweep-step.\n",
				opts->sweep_start_kbps);
		return 1;
	}
	printf("  Highest sustainable : %u kbps  (%.2f Mbps)\n",
		last_ok, (double)last_ok / 1000.0);
	printf("    p99 latency       : %.0f us  (budget %.0f us)\n",
		last_ok_result.p99_us, last_ok_result.frame_budget_us);
	printf("    cpu               : %.1f%%\n", last_ok_result.cpu_pct);
	printf("    throughput        : %.2f MB/s\n",
		last_ok_result.throughput_mb_s);
	if (interrupted) {
		printf("  Interrupted (SIGINT) before sweep finished.\n");
	} else if (first_fail) {
		printf("  First failing step : %u kbps\n", first_fail);
		printf("  Recommended max    : %u kbps  (90%% of last good "
			"for safety margin)\n", recommended);
	} else {
		printf("  Reached --sweep-max without failing; the SD/storage\n"
			"  has more headroom — raise --sweep-max to keep going.\n");
	}
	return 0;
}

/* ── CLI ─────────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
	fprintf(stderr,
"Usage: %s --dir <path> [options]\n"
"\n"
"Single-shot options:\n"
"  --dir <path>            Output directory (must exist, writable)\n"
"  --bitrate <kbps>        Video bitrate to simulate (default 20000)\n"
"  --audio-kbps <kbps>     Audio bitrate overhead (default 1536 = 48k stereo s16)\n"
"  --no-audio              Disable audio overhead (overrides --audio-kbps)\n"
"  --fps <n>               Frame rate (default 60)\n"
"  --duration <s>          Run length in seconds (default 20)\n"
"  --rotate-mb <n>         Segment size MiB, 0=disabled (default 500)\n"
"  --rotate-secs <n>       Segment time seconds, 0=disabled (default 0)\n"
"  --sync-interval <n>     sync_file_range every N frames (default 900)\n"
"  --keep-files            Don't unlink the .ts file at the end\n"
"  --json                  Emit a JSON line per result + summary\n"
"\n"
"Frame size cap:\n"
"  The simulated per-frame write is capped at 564000 bytes (3000 × 188),\n"
"  matching production TS_BUF_SIZE. At 60 fps that limits the simulated\n"
"  bitrate to ~270 Mbps; raise --fps to probe higher card throughputs.\n"
"\n"
"Sweep mode (find max sustainable bitrate):\n"
"  --sweep                 Enable sweep\n"
"  --sweep-start <kbps>    Start bitrate (default 4000)\n"
"  --sweep-step <kbps>     Step size (default 4000)\n"
"  --sweep-max <kbps>      Upper bound (default 60000)\n"
"  --sweep-duration <s>    Per-step duration (default 8)\n"
"\n"
"Examples:\n"
"  %s --dir /mnt/mmcblk0p1 --bitrate 20000 --fps 60 --duration 20\n"
"  %s --dir /mnt/mmcblk0p1 --fps 120 --sweep --sweep-max 80000\n",
		argv0, argv0, argv0);
}

static int parse_u32(const char *s, uint32_t *out)
{
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 10);
	if (!end || *end != '\0' || v > 0xFFFFFFFFul)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

int main(int argc, char **argv)
{
	BenchOpts opts;
	memset(&opts, 0, sizeof(opts));
	opts.bitrate_kbps      = 20000;
	opts.audio_kbps        = 1536;        /* 48k stereo s16 */
	opts.fps               = 60;
	opts.duration_s        = 20;
	opts.rotate_mb         = 500;
	opts.rotate_secs       = 0;
	opts.sync_interval     = DEFAULT_SYNC_INTERVAL_FRAMES;
	opts.sweep_start_kbps  = 4000;
	opts.sweep_step_kbps   = 4000;
	opts.sweep_max_kbps    = 60000;
	opts.sweep_duration_s  = 8;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

		#define NEEDARG() do { if (!next) { \
			fprintf(stderr, "missing arg for %s\n", a); \
			return 2; } } while (0)
		#define U32(field) do { NEEDARG(); \
			if (parse_u32(next, &opts.field) != 0) { \
				fprintf(stderr, "bad number: %s\n", next); \
				return 2; } i++; } while (0)

		if (!strcmp(a, "--dir"))             { NEEDARG(); opts.dir = next; i++; }
		else if (!strcmp(a, "--bitrate"))    U32(bitrate_kbps);
		else if (!strcmp(a, "--audio-kbps")) U32(audio_kbps);
		else if (!strcmp(a, "--no-audio"))   opts.no_audio = true;
		else if (!strcmp(a, "--fps"))        U32(fps);
		else if (!strcmp(a, "--duration"))   U32(duration_s);
		else if (!strcmp(a, "--rotate-mb"))  U32(rotate_mb);
		else if (!strcmp(a, "--rotate-secs"))U32(rotate_secs);
		else if (!strcmp(a, "--sync-interval")) U32(sync_interval);
		else if (!strcmp(a, "--keep-files")) opts.keep_files = true;
		else if (!strcmp(a, "--json"))       opts.json = true;
		else if (!strcmp(a, "--sweep"))      opts.sweep = true;
		else if (!strcmp(a, "--sweep-start"))   U32(sweep_start_kbps);
		else if (!strcmp(a, "--sweep-step"))    U32(sweep_step_kbps);
		else if (!strcmp(a, "--sweep-max"))     U32(sweep_max_kbps);
		else if (!strcmp(a, "--sweep-duration"))U32(sweep_duration_s);
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(argv[0]); return 0;
		} else {
			fprintf(stderr, "unknown arg: %s\n", a);
			usage(argv[0]);
			return 2;
		}
		#undef U32
		#undef NEEDARG
	}

	if (!opts.dir || !opts.dir[0]) {
		fprintf(stderr, "error: --dir is required\n");
		usage(argv[0]);
		return 2;
	}
	if (opts.fps == 0) {
		fprintf(stderr, "error: --fps must be > 0\n");
		return 2;
	}
	if (opts.sweep && opts.sweep_step_kbps == 0) {
		fprintf(stderr, "error: --sweep-step must be > 0\n");
		return 2;
	}

	struct stat st;
	if (stat(opts.dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "error: %s is not a directory\n", opts.dir);
		return 2;
	}

	/* Catch Ctrl-C so the in-flight segment(s) get cleaned up instead
	 * of leaking on the SD card.  SA_RESETHAND so a second Ctrl-C kills
	 * us hard if the first didn't make it out of a stuck syscall. */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fprintf(stderr,
		"[bench] dir=%s fps=%u sync_interval=%u rotate=%uMB/%us%s\n",
		opts.dir, opts.fps, opts.sync_interval, opts.rotate_mb,
		opts.rotate_secs, opts.no_audio ? " no-audio" : "");

	if (opts.sweep)
		return run_sweep(&opts);

	BenchResult r;
	int rc = run_bench(&opts, opts.bitrate_kbps, opts.duration_s, &r);
	if (opts.json)
		print_json(&r, &opts);
	else
		print_human(&r, &opts);

	if (rc == 0 && !opts.json) {
		printf("\nGuesstimate for mirror recording:\n");
		if (sustainable(&r, effective_audio_kbps(&opts))) {
			printf("  %u kbps appears SUSTAINABLE on this medium.\n"
				"  Conservative max ~%u kbps (90%% of target).\n"
				"  Run --sweep to probe the true ceiling.\n",
				r.bitrate_kbps,
				(uint32_t)((double)r.bitrate_kbps * 0.9));
		} else {
			printf("  %u kbps does NOT meet the sustainability bar\n"
				"  (p99 %.0f us > 50%% of %.0f us frame budget,\n"
				"   or stalls/throughput out of spec).\n"
				"  Try a lower bitrate or run --sweep.\n",
				r.bitrate_kbps, r.p99_us, r.frame_budget_us);
		}
	}

	if (r.interrupted)
		return 130;
	return rc == 0 ? 0 : 1;
}
