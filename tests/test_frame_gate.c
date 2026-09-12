#include "frame_gate.h"
#include "test_helpers.h"

#include <string.h>

#define MS 1000u   /* microseconds per millisecond */

/* Ring big enough that the threshold is never the thing under test. */
#define RING 64u

static void gate_on(FrameGate *g, uint32_t close_slots, uint32_t max_closed_ms)
{
	(void)frame_gate_setup(g, FRAME_GATE_ON, close_slots, max_closed_ms,
		RING);
}

int test_frame_gate(void)
{
	int failures = 0;
	FrameGate g;

	/* ── parse / name round-trip ─────────────────────────────────── */
	CHECK("parse_off",   frame_gate_parse_mode("off") == FRAME_GATE_OFF);
	CHECK("parse_on",    frame_gate_parse_mode("on")  == FRAME_GATE_ON);
	CHECK("parse_upper", frame_gate_parse_mode("ON")  == FRAME_GATE_ON);
	CHECK("parse_null",  frame_gate_parse_mode(NULL)  == FRAME_GATE_OFF);
	CHECK("parse_empty", frame_gate_parse_mode("")    == FRAME_GATE_OFF);
	CHECK("parse_bogus", frame_gate_parse_mode("yes") == FRAME_GATE_OFF);
	CHECK("name_off", strcmp(frame_gate_mode_name(FRAME_GATE_OFF), "off") == 0);
	CHECK("name_on",  strcmp(frame_gate_mode_name(FRAME_GATE_ON),  "on")  == 0);

	/* ── setup: resolution + sanity checks ──────────────────────── */
	CHECK("setup_default_ready",
		frame_gate_setup(&g, FRAME_GATE_ON, 0, 0, RING)
			== FRAME_GATE_SETUP_READY);
	CHECK("setup_default_close",
		g.cfg.close_slots == FRAME_GATE_DEFAULT_CLOSE_SLOTS);
	CHECK("setup_default_open", g.cfg.open_slots == FRAME_GATE_OPEN_SLOTS);
	CHECK("setup_default_escape",
		g.cfg.max_closed_us == FRAME_GATE_DEFAULT_MAX_CLOSED_MS * MS);
	CHECK("setup_debounce", g.cfg.min_closed_us == FRAME_GATE_MIN_CLOSED_US);
	CHECK("setup_starts_open", frame_gate_is_open(&g));

	/* close_slots must stay strictly above open_slots or the gate
	 * chatters on a single slot arriving and leaving. */
	(void)frame_gate_setup(&g, FRAME_GATE_ON, 1, 0, RING);
	CHECK("setup_clamps_equal", g.cfg.close_slots == g.cfg.open_slots + 1u);
	(void)frame_gate_setup(&g, FRAME_GATE_ON, 8, 0, RING);
	CHECK("setup_honours_explicit", g.cfg.close_slots == 8);

	/* An escape below the debounce would make every close an escape. */
	(void)frame_gate_setup(&g, FRAME_GATE_ON, 0, 1, RING);
	CHECK("setup_escape_floor",
		g.cfg.max_closed_us == FRAME_GATE_MIN_CLOSED_US);

	/* Status reporting: off is silent, no ring and an unreachable
	 * threshold are both flagged, and each still leaves an inert gate. */
	CHECK("setup_status_off",
		frame_gate_setup(&g, FRAME_GATE_OFF, 0, 0, RING)
			== FRAME_GATE_SETUP_OFF);
	CHECK("setup_status_no_ring",
		frame_gate_setup(&g, FRAME_GATE_ON, 0, 0, 0)
			== FRAME_GATE_SETUP_NO_RING);
	CHECK("setup_no_ring_open", frame_gate_is_open(&g));
	CHECK("setup_status_close_too_high",
		frame_gate_setup(&g, FRAME_GATE_ON, 9, 0, 8)
			== FRAME_GATE_SETUP_CLOSE_TOO_HIGH);
	CHECK("setup_too_high_open", frame_gate_is_open(&g));
	/* Exactly at capacity is legitimate, not an error. */
	CHECK("setup_close_at_capacity",
		frame_gate_setup(&g, FRAME_GATE_ON, 8, 0, 8)
			== FRAME_GATE_SETUP_READY);

	/* Counters must not survive a re-setup. */
	gate_on(&g, 3, 500);
	(void)frame_gate_observe(&g, 5, 0);
	CHECK("setup_resets_counters",
		(frame_gate_setup(&g, FRAME_GATE_ON, 3, 500, RING),
		 g.close_events == 0 && g.closed_total_us == 0 &&
		 frame_gate_is_open(&g)));

	/* ── off mode is inert ───────────────────────────────────────── */
	(void)frame_gate_setup(&g, FRAME_GATE_OFF, 0, 0, RING);
	CHECK("off_starts_open", frame_gate_is_open(&g));
	CHECK("off_not_enabled", !frame_gate_enabled(&g));
	CHECK("off_never_closes",
		frame_gate_observe(&g, 99, 1000) == FRAME_GATE_ACTION_NONE);
	CHECK("off_still_open", frame_gate_is_open(&g));

	/* ── basic close / reopen ────────────────────────────────────── */
	gate_on(&g, 3, 500);
	CHECK("on_starts_open", frame_gate_is_open(&g));
	CHECK("on_enabled", frame_gate_enabled(&g));
	CHECK("below_threshold_none",
		frame_gate_observe(&g, 2, 1000) == FRAME_GATE_ACTION_NONE);
	CHECK("still_open_at_2", frame_gate_is_open(&g));
	CHECK("at_threshold_closes",
		frame_gate_observe(&g, 3, 2000) == FRAME_GATE_ACTION_CLOSE);
	CHECK("now_closed", !frame_gate_is_open(&g));
	CHECK("close_counted", g.close_events == 1);

	/* Idempotent while the backlog persists — no repeated CLOSE. */
	CHECK("no_repeat_close",
		frame_gate_observe(&g, 5, 3000) == FRAME_GATE_ACTION_NONE);

	/* Debounce: drained, but too soon. */
	CHECK("debounce_holds",
		frame_gate_observe(&g, 0, 2000 + FRAME_GATE_MIN_CLOSED_US - 1)
			== FRAME_GATE_ACTION_NONE);
	CHECK("debounce_still_closed", !frame_gate_is_open(&g));

	/* Debounce elapsed and drained → reopen. */
	CHECK("reopens",
		frame_gate_observe(&g, 1, 2000 + FRAME_GATE_MIN_CLOSED_US)
			== FRAME_GATE_ACTION_OPEN);
	CHECK("open_again", frame_gate_is_open(&g));
	CHECK("no_escape_counted", g.escape_events == 0);
	CHECK("closed_total_accrued",
		g.closed_total_us == FRAME_GATE_MIN_CLOSED_US);

	/* ── reopen requires <= open_slots, not merely < close_slots ─── */
	gate_on(&g, 4, 500);
	CHECK("hyst_close",
		frame_gate_observe(&g, 4, 0) == FRAME_GATE_ACTION_CLOSE);
	/* 2 and 3 are below close_slots but above open_slots: the dead band.
	 * Reopening here is what would cause oscillation. */
	CHECK("hyst_deadband_2",
		frame_gate_observe(&g, 2, 100 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("hyst_deadband_3",
		frame_gate_observe(&g, 3, 200 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("hyst_still_closed", !frame_gate_is_open(&g));
	CHECK("hyst_reopen_at_1",
		frame_gate_observe(&g, 1, 300 * MS) == FRAME_GATE_ACTION_OPEN);

	/* ── safety escape: consumer died, ring never drains ─────────── */
	gate_on(&g, 3, 500);
	CHECK("escape_close",
		frame_gate_observe(&g, 8, 0) == FRAME_GATE_ACTION_CLOSE);
	CHECK("escape_not_yet",
		frame_gate_observe(&g, 8, 499 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("escape_fires",
		frame_gate_observe(&g, 8, 500 * MS) == FRAME_GATE_ACTION_OPEN);
	CHECK("escape_open", frame_gate_is_open(&g));
	CHECK("escape_counted", g.escape_events == 1);
	/* Still congested, so it closes again immediately — the escape is a
	 * pressure valve, not a latch. */
	CHECK("escape_recloses",
		frame_gate_observe(&g, 8, 501 * MS) == FRAME_GATE_ACTION_CLOSE);

	/* ── live switch to off must release a closed gate ───────────── */
	gate_on(&g, 3, 500);
	CHECK("liveoff_close",
		frame_gate_observe(&g, 5, 0) == FRAME_GATE_ACTION_CLOSE);
	g.cfg.mode = FRAME_GATE_OFF;
	CHECK("liveoff_releases",
		frame_gate_observe(&g, 5, 1 * MS) == FRAME_GATE_ACTION_OPEN);
	CHECK("liveoff_open", frame_gate_is_open(&g));
	CHECK("liveoff_then_inert",
		frame_gate_observe(&g, 99, 2 * MS) == FRAME_GATE_ACTION_NONE);

	/* ── clock regression (reinit hands over a fresh epoch) ──────── */
	gate_on(&g, 3, 500);
	CHECK("clockback_close",
		frame_gate_observe(&g, 5, 10ull * 1000ull * MS)
			== FRAME_GATE_ACTION_CLOSE);
	/* now_us < closed_since_us: must read as "no time passed", not as a
	 * huge elapsed that escapes on the spot. */
	CHECK("clockback_no_escape",
		frame_gate_observe(&g, 0, 5) == FRAME_GATE_ACTION_NONE);
	CHECK("clockback_still_closed", !frame_gate_is_open(&g));
	CHECK("clockback_no_escape_counted", g.escape_events == 0);

	/* ── force_open ──────────────────────────────────────────────── */
	gate_on(&g, 3, 500);
	(void)frame_gate_observe(&g, 5, 0);
	frame_gate_force_open(&g, 50 * MS);
	CHECK("force_open_opens", frame_gate_is_open(&g));
	CHECK("force_open_accrues", g.closed_total_us == 50 * MS);
	/* Idempotent. */
	frame_gate_force_open(&g, 60 * MS);
	CHECK("force_open_idempotent", g.closed_total_us == 50 * MS);

	/* ── failed OPEN rolls policy back so the actuator is retried ─── */
	frame_gate_restore_closed(&g, 70 * MS);
	CHECK("restore_closed_closes", !frame_gate_is_open(&g));
	CHECK("restore_closed_debounces",
		frame_gate_observe(&g, 0, 70 * MS + FRAME_GATE_MIN_CLOSED_US - 1)
			== FRAME_GATE_ACTION_NONE);
	CHECK("restore_closed_retries_open",
		frame_gate_observe(&g, 0, 70 * MS + FRAME_GATE_MIN_CLOSED_US)
			== FRAME_GATE_ACTION_OPEN);
	frame_gate_restore_closed(NULL, 0);       /* must not crash */

	/* ── NULL safety ─────────────────────────────────────────────── */
	CHECK("null_observe",
		frame_gate_observe(NULL, 5, 0) == FRAME_GATE_ACTION_NONE);
	CHECK("null_is_open", frame_gate_is_open(NULL));
	CHECK("null_enabled_false", !frame_gate_enabled(NULL));
	frame_gate_force_open(NULL, 0);          /* must not crash */
	CHECK("null_setup",
		frame_gate_setup(NULL, FRAME_GATE_ON, 0, 0, RING)
			== FRAME_GATE_SETUP_OFF);

	return failures;
}
