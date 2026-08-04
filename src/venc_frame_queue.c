#include "venc_frame_queue.h"

#include <stdlib.h>
#include <string.h>

/* Slot the in-progress write targets: one past the last queued frame. */
static uint32_t write_slot(const VencFrameQueue *q)
{
	return (q->head + q->count) % VENC_FRAME_QUEUE_SLOTS;
}

static uint8_t *slot_data(VencFrameQueue *q, uint32_t slot)
{
	return q->data + (size_t)slot * VENC_FRAME_QUEUE_SLOT_BYTES;
}

static VencFrameQueuePacket *slot_packets(VencFrameQueue *q, uint32_t slot)
{
	return q->packets + (size_t)slot * VENC_FRAME_QUEUE_MAX_PACKETS;
}

VencFrameQueue *venc_frame_queue_create(void)
{
	VencFrameQueue *q = calloc(1, sizeof(*q));

	if (!q)
		return NULL;
	q->data = malloc((size_t)VENC_FRAME_QUEUE_SLOTS *
		VENC_FRAME_QUEUE_SLOT_BYTES);
	q->packets = malloc((size_t)VENC_FRAME_QUEUE_SLOTS *
		VENC_FRAME_QUEUE_MAX_PACKETS * sizeof(*q->packets));
	if (!q->data || !q->packets) {
		venc_frame_queue_destroy(q);
		return NULL;
	}
	return q;
}

void venc_frame_queue_destroy(VencFrameQueue *q)
{
	if (!q)
		return;
	free(q->data);
	free(q->packets);
	free(q);
}

void venc_frame_queue_reset(VencFrameQueue *q)
{
	uint8_t *data;
	VencFrameQueuePacket *packets;

	if (!q)
		return;
	data = q->data;
	packets = q->packets;
	memset(q, 0, sizeof(*q));
	q->data = data;
	q->packets = packets;
}

int venc_frame_queue_begin(VencFrameQueue *q, uint64_t now_us)
{
	if (!q)
		return -1;

	q->write_active = 0;
	if (q->count >= VENC_FRAME_QUEUE_SLOTS) {
		q->overflows++;
		return -1;
	}

	q->write_offset = 0;
	q->write_packets = 0;
	q->write_enqueue_us = now_us;
	q->write_active = 1;
	return 0;
}

/* Copy one fragment into the frame under construction.  Caller has already
 * bounds-checked the total. */
static void append_fragment(uint8_t *dst, uint32_t *offset,
	const void *src, uint32_t len)
{
	if (!src || len == 0)
		return;
	memcpy(dst + *offset, src, len);
	*offset += len;
}

int venc_frame_queue_append(VencFrameQueue *q,
	const void *header, uint32_t header_len,
	const void *p1, uint32_t p1_len,
	const void *p2, uint32_t p2_len)
{
	uint32_t slot;
	uint32_t total;
	uint32_t offset;
	uint8_t *dst;
	VencFrameQueuePacket *pkt;

	if (!q || !q->write_active || !header || header_len == 0)
		return -1;

	total = header_len;
	if (p1 && p1_len)
		total += p1_len;
	if (p2 && p2_len)
		total += p2_len;

	if (q->write_packets >= VENC_FRAME_QUEUE_MAX_PACKETS ||
	    (uint64_t)q->write_offset + total > VENC_FRAME_QUEUE_SLOT_BYTES) {
		q->write_active = 0;
		q->oversize_drops++;
		return -1;
	}

	slot = write_slot(q);
	dst = slot_data(q, slot);
	pkt = slot_packets(q, slot) + q->write_packets;
	pkt->offset = q->write_offset;
	pkt->len = total;

	offset = q->write_offset;
	append_fragment(dst, &offset, header, header_len);
	append_fragment(dst, &offset, p1, p1_len);
	append_fragment(dst, &offset, p2, p2_len);

	q->write_offset = offset;
	q->write_packets++;
	return 0;
}

int venc_frame_queue_commit(VencFrameQueue *q)
{
	VencFrameQueueFrame *f;

	if (!q || !q->write_active || q->write_packets == 0) {
		if (q)
			q->write_active = 0;
		return -1;
	}

	f = &q->frames[write_slot(q)];
	f->enqueue_us = q->write_enqueue_us;
	f->packet_count = q->write_packets;
	f->byte_len = q->write_offset;

	q->count++;
	q->enqueued++;
	if (q->count > q->peak_frames)
		q->peak_frames = q->count;
	q->write_active = 0;
	return 0;
}

void venc_frame_queue_abort(VencFrameQueue *q)
{
	if (q)
		q->write_active = 0;
}

int venc_frame_queue_peek(const VencFrameQueue *q,
	const VencFrameQueueFrame **frame,
	const VencFrameQueuePacket **packets,
	const uint8_t **base)
{
	if (!q || q->count == 0 || !frame || !packets || !base)
		return -1;

	*frame = &q->frames[q->head];
	*packets = q->packets +
		(size_t)q->head * VENC_FRAME_QUEUE_MAX_PACKETS;
	*base = q->data + (size_t)q->head * VENC_FRAME_QUEUE_SLOT_BYTES;
	return 0;
}

void venc_frame_queue_pop(VencFrameQueue *q, uint64_t now_us)
{
	uint64_t enqueued_us;

	if (!q || q->count == 0)
		return;

	enqueued_us = q->frames[q->head].enqueue_us;
	q->last_sojourn_us = (now_us > enqueued_us) ?
		(uint32_t)(now_us - enqueued_us) : 0u;

	q->head = (q->head + 1) % VENC_FRAME_QUEUE_SLOTS;
	q->count--;
	q->dequeued++;
}

uint32_t venc_frame_queue_delay_us(const VencFrameQueue *q, uint64_t now_us)
{
	uint64_t enqueued_us;

	if (!q || q->count == 0)
		return 0;

	enqueued_us = q->frames[q->head].enqueue_us;
	return (now_us > enqueued_us) ? (uint32_t)(now_us - enqueued_us) : 0u;
}

uint32_t venc_frame_queue_depth(const VencFrameQueue *q)
{
	return q ? q->count : 0u;
}

uint64_t venc_frame_queue_overflows(const VencFrameQueue *q)
{
	return q ? q->overflows : 0u;
}

uint64_t venc_frame_queue_oversize_drops(const VencFrameQueue *q)
{
	return q ? q->oversize_drops : 0u;
}
