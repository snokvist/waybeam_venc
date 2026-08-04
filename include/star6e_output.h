#ifndef STAR6E_OUTPUT_H
#define STAR6E_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "output_socket.h"
#include "rtp_packetizer.h"
#include "star6e.h"
#include "venc_config.h"
#include "venc_frame_queue.h"
#include "venc_frame_ring.h"
#include "venc_ring.h"

/* Per-frame sendmmsg() batch capacity. Typical frame at 25 Mbps/120 fps is
 * 19 RTP packets; IDR can exceed that but rarely >50. If it does, the send
 * path mid-flushes (one extra sendmmsg call) rather than dropping. */
#define STAR6E_OUTPUT_BATCH_MAX 64
/* Size of owned per-slot scratch that holds a copy of the RTP header
 * concatenated with payload1. payload1 is either a 3-byte FU header
 * (tiny) or an AP packet (up to one MTU). Worst case at the
 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES (4000) live-validated max is
 * 12 (RTP) + 4000 = 4012 bytes; rounded up to 4096 for slack and
 * alignment. Sized for jumbo-frame links such as the Realtek 3993 MTU. */
#define STAR6E_OUTPUT_BATCH_SLOT_SCRATCH 4096

/* Wall-clock ceiling on flushing one frame's batch.
 *
 * SO_SNDTIMEO bounds a single unix:// sendmsg, but sendmmsg() applies that
 * timeout per message, so a 64-packet batch against a stalled consumer can
 * still accumulate far past a frame period.  This deadline bounds the whole
 * frame: once it passes, the remaining packets are dropped and counted as
 * transport drops rather than stalling the encode thread further.
 *
 * 4 ms sits under a 120 fps frame period (8.3 ms).  With an adequately
 * sized max_dgram_qlen it is never reached — a healthy 15 Mbps frame
 * flushes in ~150 us. */
#define STAR6E_OUTPUT_FLUSH_BUDGET_US 4000

/* Wall-clock ceiling on one drain pass of the paced frame queue.
 *
 * The drain runs on the pipeline thread but *outside* the VENC critical
 * section, so overrunning costs cadence rather than a held output slot —
 * still bounded, for the same reason as the flush budget above.
 *
 * 4 ms is ~26 healthy frame flushes at the ~150 us measured in
 * UNIX_SOCKET_HANDOVER.md, so a backlog clears in a single pass whenever
 * the consumer is actually draining.  That matters more than it looks:
 * a drain that pushed one frame per call could never shrink a backlog,
 * since production is also one frame per call. */
#define STAR6E_OUTPUT_DRAIN_BUDGET_US 4000

typedef enum {
	STAR6E_STREAM_MODE_COMPACT = 0,
	STAR6E_STREAM_MODE_RTP = 1,
} Star6eStreamMode;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUri uri;
	int requested_connected_udp;
	int unix_legacy_blocking;
	int unix_pacing;
	int has_server;
} Star6eOutputSetup;

/* Per-frame sendmmsg batch. We own `scratch[slot]` containing
 * [RTP header || payload1] concatenated — both the header (built on the
 * caller's stack by rtp_packetizer_send_packet) and payload1 (an FU-A
 * header or an AP builder buffer, both reused across packets) must be
 * copied because the caller reuses that memory before we flush.
 *
 * payload2, when present, is a slice of the encoder NAL buffer — that
 * memory is stable until MI_VENC_ReleaseStream which is called after
 * end_frame(), so we keep it as a zero-copy iovec pointer.
 *
 * iovec layout: 2 iovs per slot. iov[2*slot] -> scratch[slot], length =
 * header_len+payload1_len. iov[2*slot+1] -> external payload2 pointer
 * (msg_iovlen becomes 1 if payload2 is absent).
 *
 * Transport snapshot (socket_handle/dst/dst_len/connected_udp) is
 * captured at begin_frame() under the transport_gen seqlock so that a
 * concurrent apply_server() on the HTTP thread cannot retarget queued
 * packets mid-frame. Enqueue and flush dereference only batch fields,
 * never the live Star6eOutput transport state. */
typedef struct {
	uint8_t scratch[STAR6E_OUTPUT_BATCH_MAX][STAR6E_OUTPUT_BATCH_SLOT_SCRATCH];
	struct iovec iov[STAR6E_OUTPUT_BATCH_MAX * 2];
	struct mmsghdr msgs[STAR6E_OUTPUT_BATCH_MAX];
	size_t count;
	int active;
	/* Transport snapshot taken at begin_frame() under transport_gen */
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
	int unix_legacy_blocking;
	uint32_t flush_budget_us;
	int discard_remaining;
	int discard_as_error;
	/* Paced egress: `paced` mirrors output->frame_queue != NULL for this
	 * frame, `queue_open` tracks whether the queue accepted it.  A frame
	 * the queue refused is counted once as an overflow, so its packets
	 * are swallowed silently here rather than counted again per-packet
	 * as transport drops — the two are different events and telemetry
	 * must not conflate them. */
	int paced;
	int queue_open;
	/* transport_gen this batch's snapshot was taken at, so begin_frame
	 * can spot a live redirect and drop frames queued for the old
	 * destination. */
	uint32_t snapshot_gen;
} Star6eOutputBatch;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUriType transport;
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
	int requested_connected_udp;
	int unix_legacy_blocking;
	/* Config intent, so the pipeline thread can re-evaluate pacing after a
	 * live redirect (star6e_output_apply_server only reconfigures the
	 * socket). */
	int unix_pacing;
	/* Pacing active for the CURRENT transport.  Distinct from
	 * frame_queue != NULL: the queue is allocated lazily and kept across a
	 * redirect, so this is what star6e_output_is_paced() reports. */
	int paced_active;
	venc_ring_t *ring;
	venc_frame_ring_t *frame_ring;
	/* Paced unix:// egress (include/venc_frame_queue.h).  Non-NULL only
	 * when outgoing.unix_pacing is set and the transport is unix:// in
	 * RTP mode; every other path leaves it NULL and is unaffected. */
	VencFrameQueue *frame_queue;
	uint32_t send_errors;
	uint32_t transport_gen; /* seqlock: odd = write in progress, even = stable */
	OutputSocketQueue send_queue; /* SO_SNDBUF + learned unix:// capacity */
	/* Lifetime socket-transport counters, producer-thread only.  Mirrored
	 * into last_full_drops / last_writes at observation time so the sidecar
	 * trailer reports transport_drops / packets_sent for udp:// and unix://
	 * the same way it already does for the SHM rings. */
	uint32_t socket_drops;
	uint32_t socket_writes;
	Star6eOutputBatch batch;
	/* Paced-egress telemetry, producer-thread only, read off-thread by
	 * the HTTP status callback with RELAXED atomics — same class as the
	 * pressure cache below.  queue_delay_us is the control input handed
	 * to venc_codel (age of the oldest queued frame, 0 when empty);
	 * queue_sojourn_us is the last completed frame's measured queue
	 * time, which is telemetry only. */
	uint32_t queue_depth;
	uint32_t queue_delay_us;
	uint32_t queue_sojourn_us;
	uint32_t queue_overflows;
	uint32_t queue_oversize_drops;
	/* Last clamp factor published by the frame-shm ring-fill throttle
	 * (include/venc_shm_throttle.h), 1000 = unclamped.  Cached here so
	 * the sidecar emit path can report it without reaching into the
	 * backend control layer. */
	uint16_t throttle_permille;
	/* Transport-pressure observation cache (telemetry only — never gates
	 * frame transmission).  Populated by star6e_output_observe_pressure
	 * once per frame on the producer thread and read by the sidecar emit
	 * path on the same thread (one query/frame instead of two) and by
	 * the HTTP /api/v1/transport/status callback on a separate thread.
	 *
	 * Hysteresis flag enters at fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT
	 * (75) and exits at fill_pct < LOW (50).  pressure_drops counts
	 * frames observed in pressure — the wire trailer field name is
	 * preserved for ABI stability across the v0.9.2 post-encode skip
	 * rollback.  uint32_t fields are read with __atomic_load_n
	 * RELAXED off-thread; naturally aligned on ARMv7 so single-load
	 * atomic in practice.
	 *
	 * last_full_drops / last_writes / last_oversize_drops carry the
	 * lifetime counters cached at observation time so the sidecar trailer
	 * can report transport_drops / packets_sent without a second
	 * venc_ring_get_fill().  Sourced from the ring for shm:// and
	 * frame-shm://, and from socket_drops / socket_writes for udp:// and
	 * unix:// (which have no oversize concept, so that one stays 0). */
	int in_pressure;
	uint32_t pressure_drops;
	uint8_t last_fill_pct;
	uint32_t last_full_drops;
	uint32_t last_writes;
	uint32_t last_oversize_drops;
	int gdr_active;
	int svct_active;
	uint8_t gdr_cycle_len;
	uint8_t gdr_counter;
} Star6eOutput;

typedef struct {
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
} Star6eAudioSendTarget;

typedef struct {
	const Star6eOutput *video_output;
	int socket_handle;
	struct sockaddr_storage fallback_dst;
	socklen_t fallback_dst_len;
	uint16_t port_override;
	uint16_t max_payload_size;
	Star6eAudioSendTarget cached_target;
	uint32_t cached_gen;
	int cache_valid;
} Star6eAudioOutput;

typedef size_t (*Star6eOutputRtpSendFn)(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque);

/** Validate and prepare output config from URI and stream mode name. */
int star6e_output_prepare(Star6eOutputSetup *setup, const char *server_uri,
	const char *stream_mode_name, int connected_udp);

/** Check if output setup is configured for RTP streaming. */
int star6e_output_setup_is_rtp(const Star6eOutputSetup *setup);

/** Reset output state to uninitialized. */
void star6e_output_reset(Star6eOutput *output);

/** Create socket and connect to destination per setup config. */
int star6e_output_init(Star6eOutput *output, const Star6eOutputSetup *setup);

/** Check if active output uses RTP mode. */
int star6e_output_is_rtp(const Star6eOutput *output);

/** Check if active output uses shared memory mode. */
int star6e_output_is_shm(const Star6eOutput *output);

/** Check if active output uses frame-level shared memory mode. */
int star6e_output_is_frame_shm(const Star6eOutput *output);

/** Observe transport pressure for telemetry. Updates the hysteresis
 *  flag (`output->in_pressure`), the in-pressure counter
 *  (`output->pressure_drops`), and caches the latest fill_pct + SHM
 *  lifetime counters (`output->last_*`) for later sidecar emit / HTTP
 *  status read with no extra query.  Never directs the caller to skip
 *  — the caller MUST always emit the frame.  See
 *  `venc_observe_pressure` in venc_ring.h for the rationale (skip-on-
 *  pressure broke H.265 reference chains).  Should only be called when
 *  there is a sidecar subscriber — the data has no other live consumer
 *  on the producer hot path. */
void star6e_output_observe_pressure(Star6eOutput *output);

/* Snapshot the frame-shm ring occupancy for the bitrate clamp
 * (include/venc_shm_throttle.h).  Returns -1 when the active transport is
 * not frame-shm://.  Deliberately NOT star6e_output_observe_pressure() -- that
 * one is gated on a live sidecar subscription, and the clamp is a safety
 * mechanism that must run whether or not anyone is watching. */
int star6e_output_frame_ring_fill(
	const Star6eOutput *output, venc_frame_ring_fill_t *out);


/** Begin accumulating RTP packets for a frame. When the transport is UDP
 *  and SHM is not in use, subsequent star6e_output_send_rtp_parts() calls
 *  queue into a sendmmsg() batch instead of per-packet sendmsg(). The
 *  batch is flushed by star6e_output_end_frame(). Safe to call when the
 *  transport is SHM — it becomes a no-op in that case. */
void star6e_output_begin_frame(Star6eOutput *output);

/** Flush any batched RTP packets for the current frame via sendmmsg().
 *  Returns the number of messages successfully sent, or 0 if batching is
 *  inactive (no packets queued or SHM transport).
 *
 *  Under paced egress nothing reaches the wire here: the frame is published
 *  to the queue and star6e_output_drain_paced() sends it. */
int star6e_output_end_frame(Star6eOutput *output);

/** Push queued frames into the socket, one at a time, for as long as the
 *  consumer keeps taking them and STAR6E_OUTPUT_DRAIN_BUDGET_US allows.
 *  Returns the number of frames sent; 0 when pacing is off.
 *
 *  MUST be called after MI_VENC_ReleaseStream, not before: the whole point
 *  of the queue is that nothing blocks inside the VENC critical section
 *  (UNIX_SOCKET_HANDOVER.md §1.2).  Sends several frames per call by
 *  design — one per call could never shrink a backlog, since production is
 *  also one frame per call. */
int star6e_output_drain_paced(Star6eOutput *output);

/** Sample the paced queue for telemetry and for venc_codel's control
 *  input, caching depth/delay/sojourn/overflows into @p output.  No-op when
 *  pacing is off.  Producer thread only. */
void star6e_output_observe_queue(Star6eOutput *output, uint64_t now_us);

/** Whether paced egress is active on this output. */
void star6e_output_refresh_pacing(Star6eOutput *output);
int star6e_output_is_paced(const Star6eOutput *output);

/** Send RTP header and payload parts as a single UDP datagram.
 *  payload2 may be NULL/0 for single-part payloads.
 *  When a batch is active (between begin_frame/end_frame) and the transport
 *  is UDP, the packet is queued into the batch. Otherwise it is sent
 *  immediately. */
int star6e_output_send_rtp_parts(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/** Return and reset accumulated send error count. */
uint32_t star6e_output_drain_send_errors(Star6eOutput *output);

/** Send one raw packet in compact stream mode. */
int star6e_output_send_compact_packet(Star6eOutput *output,
	const uint8_t *packet, uint32_t packet_size, uint32_t max_size);

/** Send entire encoder frame in compact stream mode. */
size_t star6e_output_send_compact_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size);

/** Send encoder frame via configured output mode (RTP or compact). */
size_t star6e_output_send_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque);

/** Change output destination URI without stopping streaming. */
int star6e_output_apply_server(Star6eOutput *output, const char *uri);

/** Close socket and release output resources. */
void star6e_output_teardown(Star6eOutput *output);

/** Reset audio output state to uninitialized. */
void star6e_audio_output_reset(Star6eAudioOutput *audio_output);

/** Initialize audio output.
 *  port_override=0 shares the active video destination.
 *  port_override!=0 uses dedicated UDP audio, following the video host for
 *  udp:// output and falling back to 127.0.0.1 for unix:// or shm:// video. */
int star6e_audio_output_init(Star6eAudioOutput *audio_output,
	const Star6eOutput *video_output, uint16_t port_override,
	uint16_t max_payload_size);

/** Return the configured UDP audio port, or the shared UDP video port. */
uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output);

/** Send audio frame as RTP packets. */
int star6e_audio_output_send_rtp(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Send audio frame in compact mode (raw bytes, no RTP). */
int star6e_audio_output_send_compact(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len);

/** Send audio frame using the configured output mode. */
int star6e_audio_output_send(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Release audio output resources. */
void star6e_audio_output_teardown(Star6eAudioOutput *audio_output);

#endif
