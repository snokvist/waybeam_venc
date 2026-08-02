#include <stdint.h>
#include <string.h>

#include "test_helpers.h"
#include "venc_frame_queue.h"

/* Unit tests for the unix:// producer-side frame queue.
 *
 * Virtual time throughout — the queue owns no clock, so sojourn cases cost
 * nothing to drive. */

/* Enqueue one frame of `packets` datagrams, each `len` bytes of payload
 * carrying a recognisable fill.  Returns the commit result. */
static int push_frame(VencFrameQueue *q, uint64_t now_us, uint32_t packets,
	uint32_t len, uint8_t fill)
{
	uint8_t hdr[12];
	uint8_t body[1400];
	uint32_t i;

	if (venc_frame_queue_begin(q, now_us) != 0)
		return -1;

	memset(hdr, fill, sizeof(hdr));
	memset(body, fill, sizeof(body));
	if (len > sizeof(body))
		len = sizeof(body);

	for (i = 0; i < packets; ++i) {
		if (venc_frame_queue_append(q, hdr, sizeof(hdr), body, len,
		    NULL, 0) != 0)
			return -1;
	}
	return venc_frame_queue_commit(q);
}

int test_venc_frame_queue(void)
{
	int failures = 0;
	VencFrameQueue *q = venc_frame_queue_create();
	const VencFrameQueueFrame *frame;
	const VencFrameQueuePacket *packets;
	const uint8_t *base;
	uint64_t now = 1000000;
	uint32_t i;

	CHECK("fq_create", q != NULL);
	if (!q)
		return failures + 1;

	/* ── 1. Empty queue ─────────────────────────────────────────── */
	CHECK("fq_empty_depth", venc_frame_queue_depth(q) == 0);
	CHECK("fq_empty_peek",
		venc_frame_queue_peek(q, &frame, &packets, &base) == -1);
	/* Empty must read as zero delay: that is the "touched bottom"
	 * sample venc_codel's minimum rule needs in order to recover. */
	CHECK("fq_empty_delay_zero",
		venc_frame_queue_delay_us(q, now + 5000000) == 0);
	venc_frame_queue_pop(q, now);  /* no-op, must not corrupt */
	CHECK("fq_empty_pop_safe", venc_frame_queue_depth(q) == 0);

	/* ── 2. Round trip preserves packet framing and bytes ───────── */
	CHECK("fq_push", push_frame(q, now, 4, 100, 0xA5) == 0);
	CHECK("fq_depth_one", venc_frame_queue_depth(q) == 1);
	CHECK("fq_peek", venc_frame_queue_peek(q, &frame, &packets,
		&base) == 0);
	CHECK("fq_packet_count", frame->packet_count == 4);
	CHECK("fq_byte_len", frame->byte_len == 4 * (12 + 100));
	CHECK("fq_enqueue_us", frame->enqueue_us == now);

	CHECK("fq_packet_len", packets[0].len == 12 + 100);
	CHECK("fq_packet_offsets_advance",
		packets[1].offset == packets[0].offset + packets[0].len &&
		packets[3].offset == 3 * (12 + 100));
	for (i = 0; i < 4; ++i) {
		if (base[packets[i].offset] != 0xA5 ||
		    base[packets[i].offset + packets[i].len - 1] != 0xA5)
			break;
	}
	CHECK("fq_packet_bytes_intact", i == 4);

	/* ── 3. Sojourn is measured at pop ──────────────────────────── */
	CHECK("fq_delay_tracks_age",
		venc_frame_queue_delay_us(q, now + 7000) == 7000);
	venc_frame_queue_pop(q, now + 7000);
	CHECK("fq_pop_records_sojourn", q->last_sojourn_us == 7000);
	CHECK("fq_pop_empties", venc_frame_queue_depth(q) == 0);
	CHECK("fq_dequeued_counted", q->dequeued == 1);

	/* ── 4. FIFO order, and delay follows the OLDEST frame ──────── */
	venc_frame_queue_reset(q);
	for (i = 0; i < 3; ++i)
		CHECK("fq_fifo_push",
			push_frame(q, now + i * 1000, 2, 50,
				(uint8_t)(0x10 + i)) == 0);
	CHECK("fq_fifo_depth", venc_frame_queue_depth(q) == 3);
	/* Oldest is 2 ms older than the newest; the delay must report the
	 * oldest, since that is the frame whose latency is worst. */
	CHECK("fq_delay_uses_oldest",
		venc_frame_queue_delay_us(q, now + 2000) == 2000);
	(void)venc_frame_queue_peek(q, &frame, &packets, &base);
	CHECK("fq_fifo_oldest_first", base[packets[0].offset] == 0x10);
	venc_frame_queue_pop(q, now + 2000);
	(void)venc_frame_queue_peek(q, &frame, &packets, &base);
	CHECK("fq_fifo_second", base[packets[0].offset] == 0x11);

	/* ── 5. Overflow refuses whole frames and counts them ───────── */
	venc_frame_queue_reset(q);
	for (i = 0; i < VENC_FRAME_QUEUE_SLOTS; ++i)
		CHECK("fq_fill_slot", push_frame(q, now, 1, 50, 0x22) == 0);
	CHECK("fq_full_depth",
		venc_frame_queue_depth(q) == VENC_FRAME_QUEUE_SLOTS);
	CHECK("fq_overflow_refused", push_frame(q, now, 1, 50, 0x33) == -1);
	CHECK("fq_overflow_counted", venc_frame_queue_overflows(q) == 1);
	/* Refusal must not corrupt the queued frames — admission is
	 * frame-atomic, so the queue is exactly as it was. */
	CHECK("fq_overflow_depth_unchanged",
		venc_frame_queue_depth(q) == VENC_FRAME_QUEUE_SLOTS);
	CHECK("fq_peak_tracked",
		q->peak_frames == VENC_FRAME_QUEUE_SLOTS);

	/* Popping one makes room again. */
	venc_frame_queue_pop(q, now);
	CHECK("fq_recovers_after_pop", push_frame(q, now, 1, 50, 0x44) == 0);

	/* ── 6. Wraparound: head and write slot both cycle ──────────── */
	venc_frame_queue_reset(q);
	for (i = 0; i < VENC_FRAME_QUEUE_SLOTS * 3; ++i) {
		CHECK("fq_wrap_push",
			push_frame(q, now + i, 2, 50, (uint8_t)i) == 0);
		(void)venc_frame_queue_peek(q, &frame, &packets, &base);
		CHECK("fq_wrap_content", base[packets[0].offset] ==
			(uint8_t)i);
		venc_frame_queue_pop(q, now + i);
	}
	CHECK("fq_wrap_empty", venc_frame_queue_depth(q) == 0);
	CHECK("fq_wrap_no_overflow", venc_frame_queue_overflows(q) == 0);

	/* ── 7. Oversize: a frame larger than its slot is refused whole */
	venc_frame_queue_reset(q);
	CHECK("fq_oversize_begin", venc_frame_queue_begin(q, now) == 0);
	{
		static uint8_t big[64 * 1024];
		uint8_t hdr[12] = { 0 };
		int rc = 0;
		memset(big, 0x5A, sizeof(big));
		/* 384 KB slot / 64 KB = 6 fit, the 7th must fail. */
		for (i = 0; i < 8 && rc == 0; ++i)
			rc = venc_frame_queue_append(q, hdr, sizeof(hdr),
				big, sizeof(big), NULL, 0);
		CHECK("fq_oversize_append_fails", rc == -1);
	}
	CHECK("fq_oversize_commit_refused",
		venc_frame_queue_commit(q) == -1);
	CHECK("fq_oversize_not_queued", venc_frame_queue_depth(q) == 0);
	CHECK("fq_oversize_counted", q->oversize_drops == 1);

	/* ── 8. Abort discards without publishing ───────────────────── */
	venc_frame_queue_reset(q);
	CHECK("fq_abort_begin", venc_frame_queue_begin(q, now) == 0);
	{
		uint8_t hdr[12] = { 0 };
		CHECK("fq_abort_append",
			venc_frame_queue_append(q, hdr, sizeof(hdr),
				NULL, 0, NULL, 0) == 0);
	}
	venc_frame_queue_abort(q);
	CHECK("fq_abort_not_queued", venc_frame_queue_depth(q) == 0);
	CHECK("fq_abort_commit_refused", venc_frame_queue_commit(q) == -1);

	/* An empty frame (begin with no appends) must not publish either. */
	CHECK("fq_empty_begin", venc_frame_queue_begin(q, now) == 0);
	CHECK("fq_empty_commit_refused", venc_frame_queue_commit(q) == -1);
	CHECK("fq_empty_not_queued", venc_frame_queue_depth(q) == 0);

	/* ── 9. Three-fragment assembly matches the send path ───────── */
	venc_frame_queue_reset(q);
	{
		uint8_t hdr[4] = { 1, 2, 3, 4 };
		uint8_t p1[2] = { 5, 6 };
		uint8_t p2[3] = { 7, 8, 9 };
		static const uint8_t want[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };

		CHECK("fq_frag_begin", venc_frame_queue_begin(q, now) == 0);
		CHECK("fq_frag_append",
			venc_frame_queue_append(q, hdr, sizeof(hdr),
				p1, sizeof(p1), p2, sizeof(p2)) == 0);
		CHECK("fq_frag_commit", venc_frame_queue_commit(q) == 0);
		(void)venc_frame_queue_peek(q, &frame, &packets, &base);
		CHECK("fq_frag_len", packets[0].len == 9);
		CHECK("fq_frag_order",
			memcmp(base + packets[0].offset, want, 9) == 0);
	}

	/* ── 10. Reset clears state but keeps the allocation ────────── */
	venc_frame_queue_reset(q);
	CHECK("fq_reset_depth", venc_frame_queue_depth(q) == 0);
	CHECK("fq_reset_overflows", venc_frame_queue_overflows(q) == 0);
	CHECK("fq_reset_keeps_buffers", q->data != NULL && q->packets != NULL);
	CHECK("fq_reset_usable", push_frame(q, now, 1, 50, 0x77) == 0);

	/* ── 11. NULL safety ────────────────────────────────────────── */
	CHECK("fq_null_depth", venc_frame_queue_depth(NULL) == 0);
	CHECK("fq_null_overflows", venc_frame_queue_overflows(NULL) == 0);
	CHECK("fq_null_delay", venc_frame_queue_delay_us(NULL, now) == 0);
	CHECK("fq_null_begin", venc_frame_queue_begin(NULL, now) == -1);
	CHECK("fq_null_commit", venc_frame_queue_commit(NULL) == -1);
	CHECK("fq_null_peek",
		venc_frame_queue_peek(NULL, &frame, &packets, &base) == -1);
	venc_frame_queue_reset(NULL);
	venc_frame_queue_abort(NULL);
	venc_frame_queue_pop(NULL, now);
	venc_frame_queue_destroy(NULL);

	venc_frame_queue_destroy(q);
	return failures;
}
