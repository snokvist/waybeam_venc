#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DURATION_SEC 10u
#define RECEIVE_BUFFER_BYTES (1024 * 1024)
#define MAX_DATAGRAM_BYTES 65536u

typedef struct {
	uint64_t packets;
	uint64_t bytes;
	uint64_t frames;
	uint64_t sequence_gaps;
	uint64_t payload_type_packets[128];
	uint64_t first_packet_us;
	uint64_t last_packet_us;
	uint64_t frame_first_us;
	uint64_t max_frame_spread_us;
	uint64_t last_marker_us;
	uint64_t max_marker_gap_us;
	uint32_t frame_timestamp;
	uint16_t last_sequence;
	int have_frame;
	int have_sequence;
} ConsumerStats;

static volatile sig_atomic_t g_running = 1;

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void handle_signal(int signo)
{
	(void)signo;
	g_running = 0;
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

static int read_max_dgram_qlen(void)
{
	FILE *file;
	int value = -1;

	file = fopen("/proc/sys/net/unix/max_dgram_qlen", "r");
	if (!file)
		return -1;
	if (fscanf(file, "%d", &value) != 1)
		value = -1;
	fclose(file);
	return value;
}

static int valid_socket_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!name || !name[0] || strlen(name) >= sizeof(((struct sockaddr_un *)0)->sun_path) - 1)
		return 0;
	for (; *p; ++p) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')
			continue;
		return 0;
	}
	return 1;
}

static uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3];
}

static void observe_rtp(ConsumerStats *stats, const uint8_t *packet,
	size_t packet_size, uint64_t now_us)
{
	uint16_t sequence;
	uint32_t timestamp;

	stats->packets++;
	stats->bytes += packet_size;
	if (stats->first_packet_us == 0)
		stats->first_packet_us = now_us;
	stats->last_packet_us = now_us;

	if (packet_size < 12 || (packet[0] >> 6) != 2)
		return;
	stats->payload_type_packets[packet[1] & 0x7fu]++;

	sequence = read_be16(packet + 2);
	timestamp = read_be32(packet + 4);
	if (stats->have_sequence) {
		uint16_t expected = (uint16_t)(stats->last_sequence + 1u);
		uint16_t gap = (uint16_t)(sequence - expected);

		if (gap < UINT16_MAX / 2u)
			stats->sequence_gaps += gap;
	}
	stats->last_sequence = sequence;
	stats->have_sequence = 1;

	if (!stats->have_frame || timestamp != stats->frame_timestamp) {
		stats->frame_timestamp = timestamp;
		stats->frame_first_us = now_us;
		stats->have_frame = 1;
	}

	if (packet[1] & 0x80u) {
		uint64_t spread_us = now_us - stats->frame_first_us;

		stats->frames++;
		if (spread_us > stats->max_frame_spread_us)
			stats->max_frame_spread_us = spread_us;
		if (stats->last_marker_us != 0) {
			uint64_t marker_gap_us = now_us - stats->last_marker_us;

			if (marker_gap_us > stats->max_marker_gap_us)
				stats->max_marker_gap_us = marker_gap_us;
		}
		stats->last_marker_us = now_us;
		stats->have_frame = 0;
	}
}

static void print_summary(const char *name, const ConsumerStats *stats,
	uint64_t started_us, uint64_t finished_us, uint32_t stall_ms)
{
	uint64_t duration_us = finished_us > started_us ? finished_us - started_us : 1;
	uint64_t bitrate_bps = stats->bytes * 8u * 1000000u / duration_us;

	printf("{\"status\":\"%s\",\"socket\":\"@%s\","
		"\"duration_ms\":%" PRIu64 ",\"packets\":%" PRIu64 ","
		"\"bytes\":%" PRIu64 ",\"bitrate_bps\":%" PRIu64 ","
		"\"frames\":%" PRIu64 ",\"sequence_gaps\":%" PRIu64 ","
		"\"max_frame_spread_us\":%" PRIu64 ","
		"\"max_marker_gap_us\":%" PRIu64 ",\"stall_ms\":%u,"
		"\"payload_type_packets\":{",
		stats->packets > 0 ? "success" : "empty", name,
		duration_us / 1000u, stats->packets, stats->bytes, bitrate_bps,
		stats->frames, stats->sequence_gaps, stats->max_frame_spread_us,
		stats->max_marker_gap_us, stall_ms);
	for (unsigned payload_type = 0, first = 1; payload_type < 128; ++payload_type) {
		if (stats->payload_type_packets[payload_type] == 0)
			continue;
		printf("%s\"%u\":%" PRIu64, first ? "" : ",", payload_type,
			stats->payload_type_packets[payload_type]);
		first = 0;
	}
	printf("}}\n");
}

/* Time-varying drain profile.  An FPV radio link does not fail as a single
 * step to a fixed rate — it drops, recovers, briefly stalls, and throttles
 * locally.  A profile is a looped list of "kbps:milliseconds" segments, so one
 * mechanism covers all four: a lower kbps is a throttle, a higher one a
 * recovery, and 0 is a hard stall for that long. */
#define MAX_PROFILE_SEGMENTS 32

typedef struct {
	uint32_t kbps;
	uint32_t ms;
} ProfileSeg;

static ProfileSeg g_profile[MAX_PROFILE_SEGMENTS];
static unsigned g_profile_len;
static uint64_t g_profile_total_ms;

/* "8000:6000,3000:2500,0:400" -> segments.  Returns 0 on success. */
static int profile_parse(const char *spec)
{
	const char *p = spec;

	g_profile_len = 0;
	g_profile_total_ms = 0;
	while (*p && g_profile_len < MAX_PROFILE_SEGMENTS) {
		char *end;
		unsigned long kbps, ms;

		kbps = strtoul(p, &end, 10);
		if (end == p || *end != ':')
			return -1;
		p = end + 1;
		ms = strtoul(p, &end, 10);
		if (end == p || ms == 0)
			return -1;
		p = end;
		g_profile[g_profile_len].kbps = (uint32_t)kbps;
		g_profile[g_profile_len].ms = (uint32_t)ms;
		g_profile_total_ms += ms;
		g_profile_len++;
		if (*p == ',')
			p++;
		else if (*p)
			return -1;
	}
	return (g_profile_len > 0 && *p == '\0') ? 0 : -1;
}

static uint32_t profile_rate_at(uint64_t elapsed_us)
{
	uint64_t t = (elapsed_us / 1000u) % g_profile_total_ms;
	unsigned i;

	for (i = 0; i < g_profile_len; ++i) {
		if (t < g_profile[i].ms)
			return g_profile[i].kbps;
		t -= g_profile[i].ms;
	}
	return g_profile[g_profile_len - 1].kbps;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--name NAME] [--duration SEC] "
		"[--stall-after-ms MS --stall-ms MS] [--drain-kbps KBPS]\n"
		"\n"
		"  --stall-*     hard wedge: stop reading entirely for a while.\n"
		"  --drain-kbps  sustained slow drain: keep reading, but no faster\n"
		"                than KBPS.  This is the standing-backlog case a\n"
		"                producer-side sojourn controller is actually for;\n"
		"                a hard stall only exercises the overflow path.\n"
		"  --profile     looped \"kbps:ms,kbps:ms,...\" drain schedule, for\n"
		"                RF-like behaviour: drops, recoveries, brief stalls\n"
		"                (kbps 0) and partial throttles in one run.\n"
		"                Overrides --drain-kbps.\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *name = "waybeam_venc_test";
	uint32_t duration_sec = DEFAULT_DURATION_SEC;
	uint32_t stall_after_ms = 0;
	uint32_t stall_ms = 0;
	uint32_t drain_kbps = 0;
	uint64_t drained_bits = 0;
	uint64_t drain_started_us = 0;
	int64_t  profile_credit_bits = 0;
	uint64_t profile_last_us = 0;
	uint32_t profile_rate = UINT32_MAX;  /* force a RATE line on entry */
	struct sockaddr_un addr;
	ConsumerStats stats;
	uint8_t *buffer = NULL;
	uint64_t started_us;
	uint64_t deadline_us;
	int stalled = 0;
	int recvbuf = RECEIVE_BUFFER_BYTES;
	int fd = -1;
	int rc = 1;

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
			name = argv[++i];
		} else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &duration_sec) != 0 || duration_sec == 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--stall-after-ms") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &stall_after_ms) != 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--stall-ms") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &stall_ms) != 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--drain-kbps") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &drain_kbps) != 0 ||
			    drain_kbps == 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
			if (profile_parse(argv[++i]) != 0) {
				usage(argv[0]);
				return 2;
			}
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (!valid_socket_name(name) || (stall_ms > 0 && stall_after_ms == 0)) {
		usage(argv[0]);
		return 2;
	}

	buffer = malloc(MAX_DATAGRAM_BYTES);
	if (!buffer) {
		fprintf(stderr, "[unix_dgram_consumer] allocation failed\n");
		return 1;
	}
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "[unix_dgram_consumer] socket failed: %s\n", strerror(errno));
		goto done;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path + 1, name, strlen(name));
	if (bind(fd, (const struct sockaddr *)&addr,
	    (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name))) != 0) {
		fprintf(stderr, "[unix_dgram_consumer] bind @%s failed: %s\n",
			name, strerror(errno));
		goto done;
	}

	(void)signal(SIGINT, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&stats, 0, sizeof(stats));
	started_us = monotonic_us();
	deadline_us = started_us + (uint64_t)duration_sec * 1000000u;
	drain_started_us = started_us;
	profile_last_us = started_us;
	printf("READY @%s qlen=%d duration_sec=%u drain_kbps=%u\n", name,
		read_max_dgram_qlen(), duration_sec, drain_kbps);

	while (g_running) {
		struct pollfd pfd;
		uint64_t now_us = monotonic_us();
		int timeout_ms;
		int polled;

		if (now_us >= deadline_us)
			break;
		if (!stalled && stall_ms > 0 &&
		    now_us - started_us >= (uint64_t)stall_after_ms * 1000u) {
			struct timespec pause;

			printf("STALL_BEGIN after_ms=%u duration_ms=%u\n",
				stall_after_ms, stall_ms);
			pause.tv_sec = stall_ms / 1000u;
			pause.tv_nsec = (long)(stall_ms % 1000u) * 1000000L;
			while (nanosleep(&pause, &pause) != 0 && errno == EINTR && g_running)
				;
			stalled = 1;
			printf("STALL_END\n");
			continue;
		}

		timeout_ms = (int)((deadline_us - now_us) / 1000u);
		if (timeout_ms > 100)
			timeout_ms = 100;
		if (timeout_ms < 1)
			timeout_ms = 1;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		polled = poll(&pfd, 1, timeout_ms);
		if (polled < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[unix_dgram_consumer] poll failed: %s\n",
				strerror(errno));
			goto done;
		}
		/* Rate-limited drain: hold off reading whenever the bits taken
		 * so far are ahead of the allowed schedule.  Leaving the data
		 * in the kernel queue (rather than reading and discarding) is
		 * the point — it is what produces standing backlog on the
		 * sender without ever fully wedging. */
		if (g_profile_len > 0) {
			/* Token bucket rather than the cumulative model used by
			 * --drain-kbps: with a schedule the rate changes, and a
			 * cumulative allowance would let the consumer bank
			 * credit during a stall and then drain it in a burst,
			 * which is the opposite of what a stalled radio does.
			 * Credit is capped at PROFILE_BURST_MS worth so a rate
			 * change takes effect within that bound. */
			const uint64_t burst_ms = 20;
			uint32_t rate = profile_rate_at(now_us - started_us);

			if (rate != profile_rate) {
				printf("RATE t=%" PRIu64 "ms kbps=%u\n",
					(now_us - started_us) / 1000u, rate);
				fflush(stdout);
				profile_rate = rate;
			}
			if (rate == 0) {
				/* Hard stall: no accrual, and drop any credit so
				 * the recovery does not start with a burst. */
				struct timespec nap = { 0, 1000000L };
				profile_credit_bits = 0;
				profile_last_us = now_us;
				(void)nanosleep(&nap, NULL);
				continue;
			}
			profile_credit_bits += (int64_t)rate * 1000 *
				(int64_t)(now_us - profile_last_us) / 1000000;
			profile_last_us = now_us;
			if (profile_credit_bits > (int64_t)rate * (int64_t)burst_ms)
				profile_credit_bits = (int64_t)rate * (int64_t)burst_ms;
			/* Credit goes into debt after a read, and reading is
			 * blocked until it recovers.  Gating on "credit > 0"
			 * instead would let a single bit of credit buy a whole
			 * 1400 B datagram, degenerating to one datagram per
			 * poll (~11 Mbps) for every rate below that — the
			 * shaping would silently not happen. */
			if (profile_credit_bits <= 0) {
				struct timespec nap = { 0, 500000L };  /* 0.5 ms */
				(void)nanosleep(&nap, NULL);
				continue;
			}
		} else if (drain_kbps > 0) {
			uint64_t elapsed_us = now_us - drain_started_us;
			uint64_t allowed_bits =
				(uint64_t)drain_kbps * 1000u * elapsed_us /
				1000000u;

			if (drained_bits >= allowed_bits) {
				struct timespec nap = { 0, 1000000L };  /* 1 ms */
				(void)nanosleep(&nap, NULL);
				continue;
			}
		}

		if (polled > 0 && (pfd.revents & POLLIN)) {
			ssize_t received = recv(fd, buffer, MAX_DATAGRAM_BYTES, 0);

			if (received > 0) {
				uint64_t bits = (uint64_t)received * 8u;

				drained_bits += bits;
				profile_credit_bits -= (int64_t)bits;
				observe_rtp(&stats, buffer, (size_t)received, monotonic_us());
			}
			else if (received < 0 && errno != EINTR) {
				fprintf(stderr, "[unix_dgram_consumer] recv failed: %s\n",
					strerror(errno));
				goto done;
			}
		}
	}

	print_summary(name, &stats, started_us, monotonic_us(), stall_ms);
	rc = stats.packets > 0 ? 0 : 3;

done:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return rc;
}
