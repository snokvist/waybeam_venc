/*
 * Producer-side queue of packetized frames for the unix:// egress path.
 *
 * Why this exists.  Without it the only buffer between the encoder and a
 * unix:// consumer is the kernel's AF_UNIX receive queue, which venc does
 * not own: its depth is latched into the *consumer's* socket from
 * net.unix.max_dgram_qlen at that socket's creation, and it is counted in
 * datagrams.  Neither property can be worked around from the producer side
 * (documentation/UNIX_SOCKET_HANDOVER.md §2.1).
 *
 * Holding whole frames here restores what frame-shm:// already has:
 *
 *   - occupancy is frame-granular, so the 10-50x intra-GOP frame-size
 *     spread is divided out of the control signal rather than dominating
 *     it;
 *   - admission is frame-atomic — a frame is taken whole or refused whole,
 *     never truncated mid-frame into a queue that ran out of room;
 *   - the depth is a number venc chooses.
 *
 * The drain loop pushes one frame at a time and only while the socket has
 * already taken the previous one, so the kernel queue holds at most about
 * one frame and its datagram-unit behaviour stops being the binding
 * constraint.  What waits, waits here, where it can be measured.
 *
 * The copy.  Enqueueing copies the frame body, because the encoder's NAL
 * buffer is only valid until MI_VENC_ReleaseStream and deferring a send
 * past that point means owning the bytes.  This is not new tax: the
 * frame-shm path already does exactly one full-body memcpy per frame
 * (venc_frame_ring_append), and at 15 Mbps/60 fps it is ~1.9 MB/s.  The
 * alternative — holding the VENC output buffer until the send completes —
 * is what produced the 634 ms capture stall documented in
 * UNIX_SOCKET_HANDOVER.md §1.2.  Draining is zero-copy: the sender points
 * iovecs straight at queue memory.
 *
 * No partial-frame resume.  A frame that could not be fully sent is
 * abandoned, not carried over.  The first lost packet already breaks the
 * H.26x reference chain for that frame, so resuming it later would add
 * latency to something the receiver cannot decode anyway.  This matches
 * the batch-flush semantics established in PR #214.
 *
 * Sizing mirrors venc_frame_ring: 8 slots of 384 KB.  Both egress paths
 * having one story is worth more than a few hundred KB.  8 frames is
 * 133 ms at 60 fps — deliberately deeper than the controller's target, so
 * venc_codel has room to act before the queue can overflow at all.
 *
 * Threading: single producer, single consumer, both the pipeline thread.
 * Not internally synchronised and does not need to be.  Stats are plain
 * loads for the same reason.
 *
 * No SDK dependency and no clock of its own — the caller passes now_us.
 * Host-unit-testable; see tests/test_venc_frame_queue.c.
 */

#ifndef VENC_FRAME_QUEUE_H
#define VENC_FRAME_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8 -> 2, selected 2026-08-03 after a host sweep and n=3 device repeats
 * (specs/2026-08-02-unix-pacing-latency-tuning/validation.md).
 *
 * Worst-case added latency is queue depth divided by drain rate — a property
 * of the queue, not of the controller: clamp-off and clamp-on both measured
 * ~the same max sojourn at 8 slots because both are simply "queue full".
 * Depth is therefore the only lever on the tail, and it also drops the control
 * loop's dead time below VENC_CODEL_INTERVAL_US so the controller stops
 * deciding on evidence that predates its own last decision.
 *
 * 2 is the structural minimum: one frame draining, one filling.  Under an
 * RF-shaped drain profile this gave 13.8x lower average and far lower p95
 * queue latency than 8 slots, with 3 % more frames delivered, 46 % fewer
 * sequence gaps and no CPU cost. */
/* Overridable at compile time so a depth sweep does not need a source edit
 * per data point (-DVENC_FRAME_QUEUE_SLOTS=N). */
#ifndef VENC_FRAME_QUEUE_SLOTS
#define VENC_FRAME_QUEUE_SLOTS        2u
#endif
#define VENC_FRAME_QUEUE_SLOT_BYTES   (384u * 1024u)

/* Datagrams one frame may occupy.  A 384 KB frame at the smallest
 * validated payload (VENC_OUTPUT_PAYLOAD_MIN_BYTES, 576) is 683; round up
 * so admission is bounded by bytes rather than by this. */
#define VENC_FRAME_QUEUE_MAX_PACKETS  768u

typedef struct {
	uint32_t offset;  /* into the frame's slot */
	uint32_t len;
} VencFrameQueuePacket;

typedef struct {
	uint64_t enqueue_us;
	uint32_t packet_count;
	uint32_t byte_len;
} VencFrameQueueFrame;

typedef struct {
	uint8_t *data;                  /* SLOTS * SLOT_BYTES */
	VencFrameQueuePacket *packets;  /* SLOTS * MAX_PACKETS */
	VencFrameQueueFrame frames[VENC_FRAME_QUEUE_SLOTS];

	uint32_t head;   /* oldest queued frame */
	uint32_t count;  /* frames queued */

	/* In-progress write, into slot (head + count) */
	uint8_t  write_active;
	uint32_t write_offset;
	uint32_t write_packets;
	uint64_t write_enqueue_us;

	/* Lifetime stats */
	uint64_t enqueued;
	uint64_t dequeued;
	uint64_t overflows;      /* refused: queue full */
	uint64_t oversize_drops; /* refused: frame exceeds a slot */
	uint32_t last_sojourn_us;
	uint32_t peak_frames;
} VencFrameQueue;

/* Allocate the backing store (~3 MB).  Returns NULL on failure. */
VencFrameQueue *venc_frame_queue_create(void);
void venc_frame_queue_destroy(VencFrameQueue *q);

/* Drop every queued frame and zero the lifetime stats.  For transport
 * teardown / re-open, so a stale sojourn or overflow count cannot leak
 * across the restart. */
void venc_frame_queue_reset(VencFrameQueue *q);

/* Open a frame for writing.  Returns 0, or -1 when the queue is full
 * (counted into `overflows` — this is the event venc_codel exists to
 * prevent).  Abandons any unfinished previous write. */
int venc_frame_queue_begin(VencFrameQueue *q, uint64_t now_us);

/* Append one complete datagram, assembled from up to three fragments in
 * order.  p1/p2 may be NULL with a zero length.  Returns 0, or -1 when the
 * frame no longer fits its slot or has too many packets — the write is
 * marked failed and _commit will refuse it.  Mirrors the frame-ring
 * append/abort contract. */
int venc_frame_queue_append(VencFrameQueue *q,
	const void *header, uint32_t header_len,
	const void *p1, uint32_t p1_len,
	const void *p2, uint32_t p2_len);

/* Publish the frame being written.  Returns 0, or -1 if the write failed
 * or held no packets, in which case the frame is discarded. */
int venc_frame_queue_commit(VencFrameQueue *q);

/* Discard the frame being written without publishing it. */
void venc_frame_queue_abort(VencFrameQueue *q);

/* Oldest queued frame, for the drain loop.  Returns 0 and fills the
 * out-params, or -1 when the queue is empty.  `base` points at the frame's
 * bytes and `packets` at its offset/length index, both stable until the
 * matching _pop. */
int venc_frame_queue_peek(const VencFrameQueue *q,
	const VencFrameQueueFrame **frame,
	const VencFrameQueuePacket **packets,
	const uint8_t **base);

/* Remove the oldest frame, recording its sojourn.  No-op when empty. */
void venc_frame_queue_pop(VencFrameQueue *q, uint64_t now_us);

/* Control input for venc_codel: age of the oldest queued frame, or 0 when
 * the queue is empty.
 *
 * Empty reads as zero on purpose — that is the "touched bottom" sample the
 * minimum-over-interval law needs to allow recovery, and it is the exact
 * analogue of low-water reaching 0 slots on the frame ring.  Deriving the
 * signal from *completed* frames instead would freeze it precisely when a
 * consumer wedges and stops completing anything. */
uint32_t venc_frame_queue_delay_us(const VencFrameQueue *q, uint64_t now_us);

uint32_t venc_frame_queue_depth(const VencFrameQueue *q);
uint64_t venc_frame_queue_overflows(const VencFrameQueue *q);

#ifdef __cplusplus
}
#endif

#endif /* VENC_FRAME_QUEUE_H */
