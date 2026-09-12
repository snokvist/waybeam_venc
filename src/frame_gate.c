#include "frame_gate.h"

#include <string.h>
#include <strings.h>

static void frame_gate_resolve(FrameGateMode mode, uint32_t close_slots,
	uint32_t max_closed_ms, FrameGateConfig *out)
{
	out->mode = mode;
	out->open_slots = FRAME_GATE_OPEN_SLOTS;
	out->min_closed_us = FRAME_GATE_MIN_CLOSED_US;

	out->close_slots = close_slots ? close_slots
					: FRAME_GATE_DEFAULT_CLOSE_SLOTS;
	/* Equal thresholds chatter: one slot arriving and leaving would toggle
	 * the encoder's intake on every frame.  Keep at least one slot of
	 * separation so a close has somewhere to recover to. */
	if (out->close_slots <= out->open_slots)
		out->close_slots = out->open_slots + 1u;

	out->max_closed_us = (max_closed_ms ? max_closed_ms
					    : FRAME_GATE_DEFAULT_MAX_CLOSED_MS)
				* 1000u;
	/* An escape shorter than the debounce would fire before the gate was
	 * allowed to reopen normally, turning every close into an escape. */
	if (out->max_closed_us < out->min_closed_us)
		out->max_closed_us = out->min_closed_us;
}

FrameGateSetupStatus frame_gate_setup(FrameGate *g, FrameGateMode mode,
	uint32_t close_slots, uint32_t max_closed_ms, uint32_t slot_count)
{
	if (!g)
		return FRAME_GATE_SETUP_OFF;

	memset(g, 0, sizeof(*g));
	frame_gate_resolve(mode, close_slots, max_closed_ms, &g->cfg);
	/* Start open.  A gate that began closed could never reopen on a
	 * backend whose idle path only runs while frames flow. */
	g->open = 1;

	if (mode == FRAME_GATE_OFF)
		return FRAME_GATE_SETUP_OFF;
	if (slot_count == 0)
		return FRAME_GATE_SETUP_NO_RING;
	if (g->cfg.close_slots > slot_count)
		return FRAME_GATE_SETUP_CLOSE_TOO_HIGH;
	return FRAME_GATE_SETUP_READY;
}

void frame_gate_force_open(FrameGate *g, uint64_t now_us)
{
	if (!g || g->open)
		return;
	if (now_us > g->closed_since_us)
		g->closed_total_us += now_us - g->closed_since_us;
	g->open = 1;
	g->closed_since_us = 0;
}

void frame_gate_restore_closed(FrameGate *g, uint64_t now_us)
{
	if (!g || !g->open)
		return;
	g->open = 0;
	g->closed_since_us = now_us;
}

FrameGateAction frame_gate_observe(FrameGate *g, uint32_t used_slots,
	uint64_t now_us)
{
	uint64_t elapsed_us;

	if (!g)
		return FRAME_GATE_ACTION_NONE;

	if (g->cfg.mode == FRAME_GATE_OFF) {
		/* A live switch to "off" must not strand a closed gate. */
		if (!g->open) {
			frame_gate_force_open(g, now_us);
			return FRAME_GATE_ACTION_OPEN;
		}
		return FRAME_GATE_ACTION_NONE;
	}

	if (g->open) {
		if (used_slots < g->cfg.close_slots) {
			g->escape_open_us = 0;
			return FRAME_GATE_ACTION_NONE;
		}
		/* An escape pulse is still serving its dwell: refuse to close
		 * so the admitted frame has time to reach the ring. */
		if (g->escape_open_us &&
		    now_us > g->escape_open_us &&
		    (now_us - g->escape_open_us) < FRAME_GATE_MIN_OPEN_US)
			return FRAME_GATE_ACTION_NONE;
		g->escape_open_us = 0;
		g->open = 0;
		g->closed_since_us = now_us;
		g->close_events++;
		return FRAME_GATE_ACTION_CLOSE;
	}

	/* Closed.  A monotonic clock should never run backwards, but a reinit
	 * can hand us a fresh epoch — treat that as "no time has passed"
	 * rather than as a gigantic elapsed that escapes immediately. */
	elapsed_us = (now_us > g->closed_since_us)
		? now_us - g->closed_since_us : 0u;

	/* Safety escape first: if the consumer died outright, used_slots never
	 * falls and the stream would stop for good.  Reopening lets the ring
	 * overflow and report full_drops instead, which is a diagnosable
	 * failure rather than a silent one. */
	if (elapsed_us >= g->cfg.max_closed_us) {
		g->escape_events++;
		frame_gate_force_open(g, now_us);
		/* Hold this one open long enough for a frame to actually land,
		 * or the escape reopens and re-closes without ever letting the
		 * ring report a drop.  See FRAME_GATE_MIN_OPEN_US. */
		g->escape_open_us = now_us ? now_us : 1u;
		return FRAME_GATE_ACTION_OPEN;
	}

	if (elapsed_us < g->cfg.min_closed_us)
		return FRAME_GATE_ACTION_NONE;

	if (used_slots > g->cfg.open_slots)
		return FRAME_GATE_ACTION_NONE;

	frame_gate_force_open(g, now_us);
	return FRAME_GATE_ACTION_OPEN;
}

FrameGateMode frame_gate_parse_mode(const char *s)
{
	if (s && strcasecmp(s, "on") == 0)
		return FRAME_GATE_ON;
	return FRAME_GATE_OFF;
}

const char *frame_gate_mode_name(FrameGateMode m)
{
	return (m == FRAME_GATE_ON) ? "on" : "off";
}
