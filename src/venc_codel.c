#include "venc_codel.h"

#include <string.h>

static uint16_t clamp_permille(uint32_t v)
{
	if (v < VENC_CODEL_FLOOR_PERMILLE)
		return (uint16_t)VENC_CODEL_FLOOR_PERMILLE;
	if (v > VENC_CODEL_FULL_PERMILLE)
		return (uint16_t)VENC_CODEL_FULL_PERMILLE;
	return (uint16_t)v;
}

/* Record a floor entry/exit edge for the caller's log-once reporting. */
static void note_floor_edge(VencCodel *c)
{
	uint8_t now_at_floor =
		(c->permille <= VENC_CODEL_FLOOR_PERMILLE) ? 1u : 0u;

	if (now_at_floor == c->at_floor)
		return;
	c->at_floor = now_at_floor;
	c->floor_edge_pending = now_at_floor ? 1u : 2u;
}

static void set_permille(VencCodel *c, uint32_t value)
{
	uint16_t next = clamp_permille(value);

	if (next == c->permille)
		return;
	c->permille = next;
	c->pending_change = 1;
	note_floor_edge(c);
}

void venc_codel_reset(VencCodel *c, uint64_t now_us)
{
	if (!c)
		return;
	memset(c, 0, sizeof(*c));
	c->permille = (uint16_t)VENC_CODEL_FULL_PERMILLE;
	c->interval_min_us = VENC_CODEL_NO_SAMPLE;
	c->reported_min_us = VENC_CODEL_NO_SAMPLE;
	c->interval_start_us = now_us;
	c->enabled = 1;
}

void venc_codel_set_enabled(VencCodel *c, int enabled, uint64_t now_us)
{
	uint8_t want = enabled ? 1u : 0u;

	if (!c || c->enabled == want)
		return;

	c->enabled = want;
	c->interval_min_us = VENC_CODEL_NO_SAMPLE;
	c->interval_start_us = now_us;
	c->seeded = 0;
	c->overflow_charged = 0;

	/* Both directions land on 1000: disabling must actively release the
	 * clamp (otherwise the encoder stays pinned wherever the loop last
	 * left it), and enabling starts from unclamped so the first decision
	 * is made on fresh evidence. */
	set_permille(c, VENC_CODEL_FULL_PERMILLE);
}

int venc_codel_is_enabled(const VencCodel *c)
{
	return (c && c->enabled) ? 1 : 0;
}

void venc_codel_observe(VencCodel *c, uint32_t sojourn_us, uint64_t overflows)
{
	if (!c || !c->enabled)
		return;

	if (sojourn_us < c->interval_min_us)
		c->interval_min_us = sojourn_us;

	/* First observation only seeds the baseline — a queue that has been
	 * overflowing since before the controller was enabled must not be
	 * charged for that history in one step. */
	if (!c->seeded) {
		c->last_overflows = overflows;
		c->seeded = 1;
		return;
	}

	if (overflows > c->last_overflows) {
		c->last_overflows = overflows;
		/* A frame was already refused: react now, do not wait for the
		 * interval to close.  Harder than the sojourn decrease because
		 * sojourn is a prediction and this is proof.
		 *
		 * Once per interval, though: a full queue refuses on every
		 * frame, and an uncapped charge would be 0.6^N in one interval
		 * -- straight to the floor with no chance to settle at an
		 * intermediate rate. */
		if (!c->overflow_charged) {
			c->overflow_charged = 1;
			set_permille(c, (uint32_t)c->permille * 3u / 5u);
		}
	} else if (overflows < c->last_overflows) {
		/* Counter went backwards — the queue was re-created under us.
		 * Re-seed rather than treating the wrap as a huge burst. */
		c->last_overflows = overflows;
	}
}

int venc_codel_tick(VencCodel *c, uint64_t now_us)
{
	int changed;

	if (!c)
		return 0;

	if (now_us - c->interval_start_us >= VENC_CODEL_INTERVAL_US) {
		/* NO_SAMPLE means no frame was encoded in this interval at all
		 * (encoder idle, output disabled).  No evidence either way,
		 * so hold -- decreasing on a silent interval would clamp an
		 * idle encoder for no reason. */
		if (c->enabled &&
		    c->interval_min_us != VENC_CODEL_NO_SAMPLE) {
			c->reported_min_us = c->interval_min_us;
			/* EXPERIMENT (not for merge as-is): an overflow means
			 * the queue was full, which means sojourn was already
			 * far past TARGET — the two are the same event, and
			 * charging both in one interval compounds to x0.48 and
			 * walks straight to the floor before the first cut can
			 * possibly be observed.  Take the harder charge only. */
			if (c->interval_min_us >= VENC_CODEL_TARGET_US &&
			    !c->overflow_charged)
				set_permille(c,
					(uint32_t)c->permille * 4u / 5u);
			else if (c->interval_min_us <= VENC_CODEL_RECOVER_US)
				set_permille(c, (uint32_t)c->permille +
					VENC_CODEL_AI_STEP);
			/* Between RECOVER and TARGET: hold.  The band is what
			 * keeps a continuous signal from alternating decrease
			 * and increase around a single threshold. */
		}
		c->interval_min_us = VENC_CODEL_NO_SAMPLE;
		c->overflow_charged = 0;
		c->interval_start_us = now_us;
	}

	changed = c->pending_change ? 1 : 0;
	c->pending_change = 0;
	return changed;
}

uint16_t venc_codel_permille(const VencCodel *c)
{
	return c ? c->permille : (uint16_t)VENC_CODEL_FULL_PERMILLE;
}

uint32_t venc_codel_reported_min_us(const VencCodel *c)
{
	return c ? c->reported_min_us : VENC_CODEL_NO_SAMPLE;
}

uint32_t venc_codel_scale(uint16_t permille, uint32_t kbps)
{
	if (permille >= VENC_CODEL_FULL_PERMILLE)
		return kbps;
	if (permille < VENC_CODEL_FLOOR_PERMILLE)
		permille = (uint16_t)VENC_CODEL_FLOOR_PERMILLE;
	return (uint32_t)(((uint64_t)kbps * permille) /
		VENC_CODEL_FULL_PERMILLE);
}

uint32_t venc_codel_apply(const VencCodel *c, uint32_t kbps)
{
	return venc_codel_scale(venc_codel_permille(c), kbps);
}

int venc_codel_floor_edge(VencCodel *c)
{
	uint8_t edge;

	if (!c || !c->floor_edge_pending)
		return 0;
	edge = c->floor_edge_pending;
	c->floor_edge_pending = 0;
	return (edge == 1u) ? 1 : -1;
}
