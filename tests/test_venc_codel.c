#include <stdint.h>

#include "test_helpers.h"
#include "venc_codel.h"

/* Unit tests for the unix:// sojourn-time bitrate clamp.
 *
 * The module takes now_us as a parameter and owns no clock, so every case
 * here drives virtual time — no nanosleep, no wall-clock flake, and the
 * multi-second recovery ramp costs nothing to test. */

#define IVL VENC_CODEL_INTERVAL_US

/* Samples per interval.  Must divide IVL exactly, or the accumulated step
 * lands short of the boundary and the interval never closes. */
#define STEPS 10

/* Advance one full interval at a constant sojourn, returning whether the
 * clamp factor changed.  Constant sojourn across the interval, so the
 * minimum equals the sample; cases that need them to differ build the
 * interval by hand. */
static int step_interval(VencCodel *c, uint64_t *now, uint32_t sojourn_us,
	uint64_t overflows)
{
	int changed = 0;
	int i;

	/* Several frames per interval, as production does at 60 fps. */
	for (i = 0; i < STEPS; ++i) {
		*now += IVL / STEPS;
		venc_codel_observe(c, sojourn_us, overflows);
		if (venc_codel_tick(c, *now))
			changed = 1;
	}
	return changed;
}

int test_venc_codel(void)
{
	int failures = 0;
	VencCodel c;
	uint64_t now = 1000000;
	int i;
	int rep;

	/* ── 1. Engage ──────────────────────────────────────────────── */
	venc_codel_reset(&c, now);
	CHECK("codel_reset_unclamped", venc_codel_permille(&c) == 1000);
	CHECK("codel_reset_enabled", venc_codel_is_enabled(&c) == 1);
	CHECK("codel_reset_no_floor_edge", venc_codel_floor_edge(&c) == 0);
	CHECK("codel_reset_no_reported_min",
		venc_codel_reported_min_us(&c) == VENC_CODEL_NO_SAMPLE);

	CHECK("codel_engage_reports_change",
		step_interval(&c, &now, VENC_CODEL_TARGET_US * 3, 0) == 1);
	CHECK("codel_engage_800", venc_codel_permille(&c) == 800);
	CHECK("codel_reports_min",
		venc_codel_reported_min_us(&c) == VENC_CODEL_TARGET_US * 3);

	/* Sojourn exactly at the target still decreases. */
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US, 0);
	CHECK("codel_engage_at_threshold", venc_codel_permille(&c) == 640);

	/* ── 2. Hold band between RECOVER and TARGET ────────────────── */
	venc_codel_reset(&c, now);
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 3, 0);
	CHECK("codel_band_setup", venc_codel_permille(&c) == 800);
	CHECK("codel_band_no_change",
		step_interval(&c, &now,
			(VENC_CODEL_TARGET_US + VENC_CODEL_RECOVER_US) / 2,
			0) == 0);
	CHECK("codel_band_holds", venc_codel_permille(&c) == 800);

	/* ── 3. Minimum, not maximum: one empty sample releases ─────── */
	venc_codel_reset(&c, now);
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 3, 0);
	CHECK("codel_min_setup", venc_codel_permille(&c) == 800);
	/* An interval that is deeply backed up for all but one frame, where
	 * the queue did reach empty once.  High-water would clamp; the
	 * minimum rule must recover instead. */
	/* Repeated VENC_CODEL_AI_EVERY times: asymmetric probing allows one
	 * increase per that many quiet intervals, and every interval here is
	 * quiet by the minimum rule.  Expressed via the constant so the test
	 * does not encode today's probe rate. */
	for (rep = 0; rep < (int)VENC_CODEL_AI_EVERY; ++rep) {
		for (i = 0; i < STEPS; ++i) {
			now += IVL / STEPS;
			venc_codel_observe(&c,
				(i == 5) ? 0 : VENC_CODEL_TARGET_US * 5, 0);
			(void)venc_codel_tick(&c, now);
		}
	}
	CHECK("codel_touched_bottom_recovers",
		venc_codel_permille(&c) == 850);

	/* ── 4. Overflow fast-path: applies mid-interval ────────────── */
	{
		/* Every sojourn sample here sits in the hold band, so no
		 * interval boundary raises or lowers the clamp.  That isolates
		 * the overflow charge — with sojourn 0 the intervals would
		 * additive-increase underneath the assertions and the numbers
		 * below would be testing two rules at once. */
		const uint32_t band =
			(VENC_CODEL_TARGET_US + VENC_CODEL_RECOVER_US) / 2;

		venc_codel_reset(&c, now);
		venc_codel_observe(&c, band, 7);   /* seeds the baseline */
		CHECK("codel_seed_no_change",
			venc_codel_permille(&c) == 1000);
		now += IVL / 10;                    /* inside the interval */
		venc_codel_observe(&c, band, 8);   /* one new overflow */
		CHECK("codel_overflow_immediate_600",
			venc_codel_permille(&c) == 600);

		/* Charged at most once per interval, however many land. */
		venc_codel_observe(&c, band, 9);
		venc_codel_observe(&c, band, 20);
		CHECK("codel_overflow_charged_once",
			venc_codel_permille(&c) == 600);

		/* Closing the interval on band samples is neutral... */
		(void)step_interval(&c, &now, band, 20);
		CHECK("codel_overflow_interval_neutral",
			venc_codel_permille(&c) == 600);
		/* ...and re-arms the charge for the next one. */
		venc_codel_observe(&c, band, 21);
		CHECK("codel_overflow_recharges",
			venc_codel_permille(&c) == 360);

		/* A counter that goes backwards (queue re-created) re-seeds
		 * rather than charging for the apparent wrap. */
		(void)step_interval(&c, &now, band, 21);
		venc_codel_observe(&c, band, 3);
		CHECK("codel_overflow_backwards_reseeds",
			venc_codel_permille(&c) == 360);
	}

	/* ── 5. Floor, and the edges around it ──────────────────────── */
	venc_codel_reset(&c, now);
	for (i = 0; i < 12; ++i)
		(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 2, 0);
	CHECK("codel_floor_pinned",
		venc_codel_permille(&c) == VENC_CODEL_FLOOR_PERMILLE);
	CHECK("codel_floor_edge_enter", venc_codel_floor_edge(&c) == 1);
	CHECK("codel_floor_edge_once", venc_codel_floor_edge(&c) == 0);

	/* Asymmetric probing (VENC_CODEL_AI_EVERY) rate-limits the increase to
	 * one step every N intervals, so the climb needs N quiet intervals to
	 * produce its first step.  Written in terms of the constant so the test
	 * holds at any setting rather than encoding today's value. */
	for (i = 0; i < (int)VENC_CODEL_AI_EVERY; ++i)
		(void)step_interval(&c, &now, 0, 0);
	CHECK("codel_leaves_floor",
		venc_codel_permille(&c) == VENC_CODEL_FLOOR_PERMILLE +
			VENC_CODEL_AI_STEP);
	CHECK("codel_floor_edge_leave", venc_codel_floor_edge(&c) == -1);
	CHECK("codel_floor_edge_leave_once", venc_codel_floor_edge(&c) == 0);

	/* ── 6. Additive increase ramps to full and stops there ─────── */
	for (i = 0; i < 40 * (int)VENC_CODEL_AI_EVERY; ++i)
		(void)step_interval(&c, &now, 0, 0);
	CHECK("codel_recovers_to_full", venc_codel_permille(&c) == 1000);
	CHECK("codel_no_change_at_full",
		step_interval(&c, &now, 0, 0) == 0);

	/* ── 7. Silent interval holds ───────────────────────────────── */
	venc_codel_reset(&c, now);
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 3, 0);
	CHECK("codel_silent_setup", venc_codel_permille(&c) == 800);
	now += IVL * 4;   /* no observations at all */
	CHECK("codel_silent_no_change", venc_codel_tick(&c, now) == 0);
	CHECK("codel_silent_holds", venc_codel_permille(&c) == 800);

	/* ── 8. Enable/disable releases the clamp ───────────────────── */
	venc_codel_reset(&c, now);
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 3, 0);
	CHECK("codel_disable_setup", venc_codel_permille(&c) == 800);
	venc_codel_set_enabled(&c, 0, now);
	CHECK("codel_disable_releases", venc_codel_permille(&c) == 1000);
	CHECK("codel_disable_reports_change", venc_codel_tick(&c, now) == 1);
	CHECK("codel_disabled_flag", venc_codel_is_enabled(&c) == 0);

	/* Disabled, the law does not run however bad the sojourn looks. */
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 10, 0);
	CHECK("codel_disabled_ignores_sojourn",
		venc_codel_permille(&c) == 1000);

	venc_codel_set_enabled(&c, 1, now);
	CHECK("codel_reenable_starts_full", venc_codel_permille(&c) == 1000);
	CHECK("codel_reenabled_flag", venc_codel_is_enabled(&c) == 1);

	/* Idempotent — re-enabling an enabled controller must not reset a
	 * clamp it has legitimately applied. */
	(void)step_interval(&c, &now, VENC_CODEL_TARGET_US * 3, 0);
	venc_codel_set_enabled(&c, 1, now);
	CHECK("codel_enable_idempotent", venc_codel_permille(&c) == 800);

	/* ── 9. Scaling ─────────────────────────────────────────────── */
	CHECK("codel_scale_full", venc_codel_scale(1000, 15000) == 15000);
	CHECK("codel_scale_half", venc_codel_scale(500, 15000) == 7500);
	CHECK("codel_scale_floor_clamped",
		venc_codel_scale(10, 15000) ==
			15000 * VENC_CODEL_FLOOR_PERMILLE / 1000);
	CHECK("codel_scale_no_overflow",
		venc_codel_scale(800, 4000000u) == 3200000u);

	/* ── 10. NULL safety ────────────────────────────────────────── */
	CHECK("codel_null_permille",
		venc_codel_permille(NULL) == VENC_CODEL_FULL_PERMILLE);
	CHECK("codel_null_tick", venc_codel_tick(NULL, now) == 0);
	CHECK("codel_null_enabled", venc_codel_is_enabled(NULL) == 0);
	CHECK("codel_null_floor_edge", venc_codel_floor_edge(NULL) == 0);
	CHECK("codel_null_reported",
		venc_codel_reported_min_us(NULL) == VENC_CODEL_NO_SAMPLE);
	venc_codel_reset(NULL, now);
	venc_codel_observe(NULL, 0, 0);
	venc_codel_set_enabled(NULL, 1, now);

	return failures;
}
