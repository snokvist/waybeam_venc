/*
 * Host soak harness for paced unix:// egress + the sojourn clamp.
 *
 * Drives the production modules — venc_frame_queue, venc_codel, the
 * output_socket pacing gate — against a real AF_UNIX datagram socket and a
 * real consumer (tools/unix_dgram_consumer), on real kernel queue
 * behaviour.  What it does NOT include is the SigmaStar encoder, so frames
 * are synthesised: constant fps, a P-frame size derived from the (clamped)
 * bitrate, and a periodic IDR at IDR_RATIO x that size.  The frame-size
 * spread is the whole point — a packet-counted queue cannot tell an IDR
 * from standing backlog, and this is what demonstrates that a frame-counted
 * one can.
 *
 * The clamp feeds back into the synthesised frame size exactly as it feeds
 * the encoder on target, so the loop that runs here is the loop that runs
 * on device, minus the encoder's own RC dynamics.
 *
 * Not part of the unit-test suite: it needs a cooperating process, real
 * time and a kernel.  Build with `make soak-tools`.
 */

#define _POSIX_C_SOURCE 200809L

#include "output_socket.h"
#include "venc_codel.h"
#include "venc_frame_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RTP_HEADER_BYTES     12
#define IDR_RATIO             5     /* IDR is this many x a P-frame */
#define DRAIN_BUDGET_US    4000     /* mirrors STAR6E_OUTPUT_DRAIN_BUDGET_US */
#define SOAK_BATCH_MAX       64     /* mirrors STAR6E_OUTPUT_BATCH_MAX */

typedef struct {
	uint64_t frames_encoded;
	uint64_t frames_sent;
	uint64_t packets_sent;
	uint64_t congestion_drops;
	uint64_t send_errors;
	uint64_t overflows;
	uint64_t sojourn_sum_us;
	uint32_t sojourn_max_us;
	uint32_t depth_max;
	uint16_t permille_min;
	uint64_t bytes_sent;
} SoakStats;

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signo)
{
	(void)signo;
	g_running = 0;
}

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX)
		return -1;
	*value = (uint32_t)parsed;
	return 0;
}

/* Queue one synthesised frame.  Payload content is irrelevant to the
 * control loop but the RTP header is real enough for the consumer's
 * sequence-gap accounting to work. */
static int encode_frame(VencFrameQueue *queue, uint32_t frame_bytes,
	uint32_t payload_bytes, uint16_t *seq, uint32_t timestamp,
	uint64_t now_us)
{
	static uint8_t payload[4096];
	uint8_t header[RTP_HEADER_BYTES];
	uint32_t remaining = frame_bytes;

	if (venc_frame_queue_begin(queue, now_us) != 0)
		return -1;

	while (remaining > 0) {
		uint32_t chunk = remaining > payload_bytes ?
			payload_bytes : remaining;
		int last = (remaining <= payload_bytes);

		header[0] = 0x80;
		header[1] = (uint8_t)(96u | (last ? 0x80u : 0u));
		header[2] = (uint8_t)(*seq >> 8);
		header[3] = (uint8_t)(*seq & 0xffu);
		header[4] = (uint8_t)(timestamp >> 24);
		header[5] = (uint8_t)(timestamp >> 16);
		header[6] = (uint8_t)(timestamp >> 8);
		header[7] = (uint8_t)timestamp;
		memset(header + 8, 0x2a, 4);
		(*seq)++;

		if (venc_frame_queue_append(queue, header, sizeof(header),
		    payload, chunk, NULL, 0) != 0) {
			venc_frame_queue_abort(queue);
			return -1;
		}
		remaining -= chunk;
	}
	return venc_frame_queue_commit(queue);
}

/* One drain pass: push frames while the consumer keeps taking them and the
 * budget allows.  Mirrors star6e_output_drain_paced. */
static void drain(int fd, const struct sockaddr_storage *dst, socklen_t dst_len,
	VencFrameQueue *queue, OutputSocketQueue *sq, SoakStats *stats)
{
	uint64_t started = monotonic_us();

	for (;;) {
		const VencFrameQueueFrame *frame;
		const VencFrameQueuePacket *packets;
		const uint8_t *base;
		uint64_t elapsed;
		uint32_t i;
		uint32_t threshold;
		int queued = 0;

		if (venc_frame_queue_peek(queue, &frame, &packets, &base) != 0)
			break;
		/* Keep the socket holding less than one frame — see the
		 * pacing-gate comment in star6e_output_drain_paced(). */
		threshold = frame->byte_len > OUTPUT_SOCKET_PACING_SLACK_BYTES ?
			frame->byte_len : (uint32_t)OUTPUT_SOCKET_PACING_SLACK_BYTES;
		if (output_socket_queued_bytes(fd, &queued) == 0 &&
		    queued > (int)threshold)
			break;
		elapsed = monotonic_us() - started;
		if (elapsed >= DRAIN_BUDGET_US)
			break;

		/* Chunked sendmmsg over queue memory, one iovec per datagram —
		 * the same shape as paced_send_frame() in star6e_output.c. */
		i = 0;
		while (i < frame->packet_count) {
			struct iovec iov[SOAK_BATCH_MAX];
			struct mmsghdr msgs[SOAK_BATCH_MAX];
			uint32_t n = frame->packet_count - i;
			uint32_t k;
			int sent;

			if (n > SOAK_BATCH_MAX)
				n = SOAK_BATCH_MAX;
			memset(msgs, 0, sizeof(msgs[0]) * n);
			for (k = 0; k < n; ++k) {
				iov[k].iov_base =
					(void *)(base + packets[i + k].offset);
				iov[k].iov_len = packets[i + k].len;
				msgs[k].msg_hdr.msg_name = (void *)dst;
				msgs[k].msg_hdr.msg_namelen = dst_len;
				msgs[k].msg_hdr.msg_iov = &iov[k];
				msgs[k].msg_hdr.msg_iovlen = 1;
			}

			sent = sendmmsg(fd, msgs, n, 0);
			if (sent < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK ||
				    errno == ENOBUFS) {
					output_socket_note_saturation(fd, sq);
					stats->congestion_drops +=
						frame->packet_count - i;
				} else {
					stats->send_errors +=
						frame->packet_count - i;
				}
				break;
			}
			for (k = 0; k < (uint32_t)sent; ++k)
				stats->bytes_sent += packets[i + k].len;
			stats->packets_sent += (uint32_t)sent;
			i += (uint32_t)sent;

			if ((uint32_t)sent < n) {
				/* Queue filled mid-batch: exact capacity
				 * reading, and the rest of the frame is lost. */
				output_socket_note_saturation(fd, sq);
				stats->congestion_drops +=
					frame->packet_count - i;
				break;
			}
		}

		venc_frame_queue_pop(queue, monotonic_us());
		if (queue->last_sojourn_us > stats->sojourn_max_us)
			stats->sojourn_max_us = queue->last_sojourn_us;
		stats->sojourn_sum_us += queue->last_sojourn_us;
		stats->frames_sent++;
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--name NAME] [--duration SEC] [--fps N]\n"
		"          [--kbps N] [--payload BYTES] [--throttle 0|1]\n"
		"          [--trace]\n"
		"\n"
		"Drives venc_frame_queue + venc_codel over a real AF_UNIX\n"
		"socket.  Start tools/unix_dgram_consumer first.\n"
		"  --throttle 0  measure only (Phase A); 1 applies the clamp.\n"
		"  --trace       one line per control interval.\n", argv0);
}

int main(int argc, char **argv)
{
	const char *name = "waybeam_venc_test";
	uint32_t duration_sec = 10;
	uint32_t fps = 60;
	uint32_t kbps = 15000;
	uint32_t payload = 1400;
	uint32_t throttle = 1;
	int trace = 0;

	VencFrameQueue *queue = NULL;
	VencCodel codel;
	OutputSocketQueue sq;
	SoakStats stats;
	struct sockaddr_storage dst;
	socklen_t dst_len = 0;
	struct sockaddr_un *un;
	uint64_t started, deadline, next_frame_us, last_trace_us;
	uint32_t timestamp = 0;
	uint16_t seq = 0;
	int fd = -1;
	int rc = 1;
	int i;

	for (i = 1; i < argc; ++i) {
		int have_next = (i + 1 < argc);

		if (strcmp(argv[i], "--name") == 0 && have_next)
			name = argv[++i];
		else if (strcmp(argv[i], "--duration") == 0 && have_next)
			rc = parse_u32(argv[++i], &duration_sec);
		else if (strcmp(argv[i], "--fps") == 0 && have_next)
			rc = parse_u32(argv[++i], &fps);
		else if (strcmp(argv[i], "--kbps") == 0 && have_next)
			rc = parse_u32(argv[++i], &kbps);
		else if (strcmp(argv[i], "--payload") == 0 && have_next)
			rc = parse_u32(argv[++i], &payload);
		else if (strcmp(argv[i], "--throttle") == 0 && have_next)
			rc = parse_u32(argv[++i], &throttle);
		else if (strcmp(argv[i], "--trace") == 0)
			trace = 1;
		else {
			usage(argv[0]);
			return 2;
		}
		if (rc != 0 && rc != 1) {
			usage(argv[0]);
			return 2;
		}
	}
	rc = 1;
	if (fps == 0 || kbps == 0 || payload < 64 || payload > 4000 ||
	    duration_sec == 0) {
		usage(argv[0]);
		return 2;
	}

	queue = venc_frame_queue_create();
	if (!queue) {
		fprintf(stderr, "[soak] venc_frame_queue_create failed\n");
		return 1;
	}

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "[soak] socket: %s\n", strerror(errno));
		goto done;
	}
	{
		struct timeval tv = { 0, 2000 };  /* SO_SNDTIMEO 2 ms */
		int sndbuf = 512 * 1024;

		(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
			sizeof(sndbuf));
		(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	memset(&sq, 0, sizeof(sq));
	(void)output_socket_capture_capacity(fd, &sq);

	memset(&dst, 0, sizeof(dst));
	un = (struct sockaddr_un *)&dst;
	un->sun_family = AF_UNIX;
	memcpy(un->sun_path + 1, name, strlen(name));
	dst_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
		strlen(name));

	(void)signal(SIGINT, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	memset(&stats, 0, sizeof(stats));
	stats.permille_min = VENC_CODEL_FULL_PERMILLE;
	started = monotonic_us();
	venc_codel_reset(&codel, started);
	deadline = started + (uint64_t)duration_sec * 1000000u;
	next_frame_us = started;
	last_trace_us = started;

	printf("SOAK name=@%s fps=%u kbps=%u payload=%u throttle=%u "
		"target_us=%u interval_us=%u\n",
		name, fps, kbps, payload, throttle,
		VENC_CODEL_TARGET_US, VENC_CODEL_INTERVAL_US);

	while (g_running) {
		uint64_t now = monotonic_us();
		uint32_t effective_kbps;
		uint32_t frame_bytes;
		uint16_t permille;
		int is_idr;

		if (now >= deadline)
			break;
		if (now < next_frame_us) {
			struct timespec nap = { 0, 200000L };  /* 0.2 ms */

			(void)nanosleep(&nap, NULL);
			continue;
		}
		next_frame_us += 1000000u / fps;

		permille = venc_codel_permille(&codel);
		effective_kbps = throttle ?
			venc_codel_scale(permille, kbps) : kbps;

		/* Frame sizes carry the whole point of the experiment: an IDR
		 * is IDR_RATIO x a P-frame, so occupancy measured in packets
		 * would swing by that ratio while occupancy measured in frames
		 * does not move at all. */
		is_idr = (stats.frames_encoded % (fps * 2u)) == 0;
		frame_bytes = (uint32_t)((uint64_t)effective_kbps * 1000u /
			8u / fps);
		if (is_idr)
			frame_bytes *= IDR_RATIO;
		if (frame_bytes < payload)
			frame_bytes = payload;

		if (encode_frame(queue, frame_bytes, payload, &seq,
		    timestamp, now) != 0)
			stats.overflows = venc_frame_queue_overflows(queue);
		stats.frames_encoded++;
		timestamp += 90000u / fps;

		drain(fd, &dst, dst_len, queue, &sq, &stats);

		now = monotonic_us();
		venc_codel_observe(&codel, venc_frame_queue_delay_us(queue, now),
			venc_frame_queue_overflows(queue));
		(void)venc_codel_tick(&codel, now);

		if (venc_frame_queue_depth(queue) > stats.depth_max)
			stats.depth_max = venc_frame_queue_depth(queue);
		if (venc_codel_permille(&codel) < stats.permille_min)
			stats.permille_min = venc_codel_permille(&codel);

		if (trace && now - last_trace_us >= VENC_CODEL_INTERVAL_US) {
			uint32_t reported = venc_codel_reported_min_us(&codel);

			last_trace_us = now;
			printf("t=%6" PRIu64 "ms permille=%4u min_sojourn_us=%s%u "
				"depth=%u overflows=%" PRIu64 "\n",
				(now - started) / 1000u,
				venc_codel_permille(&codel),
				reported == VENC_CODEL_NO_SAMPLE ? "-" : "",
				reported == VENC_CODEL_NO_SAMPLE ? 0 : reported,
				venc_frame_queue_depth(queue),
				venc_frame_queue_overflows(queue));
		}
	}

	stats.overflows = venc_frame_queue_overflows(queue);
	printf("{\"frames_encoded\":%" PRIu64 ",\"frames_sent\":%" PRIu64 ","
		"\"packets_sent\":%" PRIu64 ",\"bytes_sent\":%" PRIu64 ","
		"\"congestion_drops\":%" PRIu64 ",\"send_errors\":%" PRIu64 ","
		"\"queue_overflows\":%" PRIu64 ",\"sojourn_avg_us\":%" PRIu64 ","
		"\"sojourn_max_us\":%u,\"queue_depth_max\":%u,"
		"\"permille_min\":%u,\"permille_final\":%u,"
		"\"bitrate_bps\":%" PRIu64 "}\n",
		stats.frames_encoded, stats.frames_sent, stats.packets_sent,
		stats.bytes_sent, stats.congestion_drops, stats.send_errors,
		stats.overflows,
		stats.frames_sent ? stats.sojourn_sum_us / stats.frames_sent : 0,
		stats.sojourn_max_us, stats.depth_max, stats.permille_min,
		venc_codel_permille(&codel),
		stats.bytes_sent * 8u * 1000000u /
			(monotonic_us() - started));
	rc = 0;

done:
	if (fd >= 0)
		close(fd);
	venc_frame_queue_destroy(queue);
	return rc;
}
