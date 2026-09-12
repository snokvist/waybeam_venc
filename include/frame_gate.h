#ifndef FRAME_GATE_H
#define FRAME_GATE_H

#include <stdint.h>

/* Adaptive frame gate — an overload reflex for the stream VENC channel.
 *
 * When the frame-shm egress ring stops draining, the gate pauses the
 * encoder's frame intake (MI_VENC_StopRecvPic and its per-backend
 * equivalents) and resumes it when the consumer catches up.  Sensor, ISP,
 * 3A and the scaler keep running at full rate throughout — only the
 * scaler->VENC handoff is gated — so AE/AWB never see a frame-rate step
 * and there is no exposure pump on reopen.
 *
 * Why a gate and not a rate write.  video0.bitrate is the only proportional
 * rate actuator on the SigmaStar backends and MI_VENC_SetChnAttr implicitly
 * emits an IDR (measured; see the note above apply_bitrate() in
 * src/star6e_controls.c).  Responding to a congested link with a bitrate
 * write therefore injects the largest frame in the stream into an already
 * overflowing link.  No IDR-free *proportional* substitute exists: qpDelta
 * writes s32IPQPDelta and only redistributes bits between I and P, and
 * min_qp is a cliff that abandons CBR rather than a dial (README).  The gate
 * writes neither RC state nor ROI config, so the CBR contract and the ROI
 * QP gradient are both untouched.
 *
 * This module is pure: no SDK types, no I/O, no clock of its own.  The
 * caller supplies ring occupancy and a monotonic timestamp and applies the
 * returned action.  That keeps the policy host-testable and identical
 * across all three backends.
 *
 * Relationship to waybeam-link.  The gate does NOT move rate policy back
 * into the daemon (0.69.0 deliberately moved it out).  It is a local
 * overload reflex acting on a signal and an actuator that both live inside
 * venc and cannot be reached from outside at frame cadence; waybeam-link
 * still owns the rate model and the gate absorbs the transients between its
 * decisions.  See documentation/ADAPTIVE_FRAME_GATE_PLAN.md.
 */

/* Reopen threshold, in slots.  Pinned, not configurable: the ring's healthy
 * idle occupancy is one frame and not zero — the producer samples just after
 * writing — so "<= 1" is the documented healthy band and ">= 2" is standing
 * backlog (include/venc_frame_ring.h).  A different value would not mean
 * "more/less aggressive", it would mean "wrong". */
#define FRAME_GATE_OPEN_SLOTS 1u

/* Defaults for the two knobs that DO want bench tuning. */
#define FRAME_GATE_DEFAULT_CLOSE_SLOTS   3u
#define FRAME_GATE_DEFAULT_MAX_CLOSED_MS 500u

/* Debounce floor while closed.  One frame at 120 fps is ~8.3 ms; 20 ms is a
 * shade over two, enough that a close is never undone before the consumer
 * has had a realistic chance to drain a slot. */
#define FRAME_GATE_MIN_CLOSED_US 20000u

/* Minimum time the gate stays open after a SAFETY ESCAPE, and only then.
 *
 * The escape exists so a dead consumer cannot stop the stream for good: it
 * reopens, lets a frame through, and the ring reports full_drops — a
 * diagnosable failure rather than a silent one.  Without a dwell it cannot do
 * that.  Occupancy is still above close_slots at the instant it reopens, so
 * the very next observation closes again, and on Star6E the idle path polls
 * every 1 ms while a frame takes 10 ms at 100 fps.  The escape therefore
 * re-closed before the encoder could deliver anything: measured 2026-09-12,
 * a dead consumer froze framesSent with every drop counter at zero, under
 * both the recv-stop and drain-stall actuators.
 *
 * 20 ms is two frame periods at 100 fps, so at least one frame reaches the
 * ring and is counted.  Normal closes are NOT debounced by this — only the
 * pulse that follows an escape, so burst response is unchanged. */
#define FRAME_GATE_MIN_OPEN_US 20000u

typedef struct {
	uint32_t close_slots;    /* close when used_slots >= this */
	uint32_t open_slots;     /* reopen when used_slots <= this */
	uint32_t min_closed_us;  /* debounce floor while closed */
	uint32_t max_closed_us;  /* safety escape */
} FrameGateConfig;

typedef enum {
	FRAME_GATE_ACTION_NONE = 0,
	FRAME_GATE_ACTION_CLOSE,
	FRAME_GATE_ACTION_OPEN,
} FrameGateAction;

typedef struct {
	FrameGateConfig cfg;
	int      enabled;            /* armed: the output is a frame ring */
	int      open;               /* 1 = intake enabled (the initial state) */
	uint64_t closed_since_us;    /* valid only while !open */
	uint64_t closed_total_us;    /* cumulative time gated, for duty cycle */
	uint32_t close_events;       /* closes since setup */
	uint64_t escape_open_us;     /* when an escape reopened; 0 = not an escape */
	uint32_t escape_events;      /* reopens forced by max_closed_us */
} FrameGate;

/* What frame_gate_setup() made of the request.  The caller reports it; the
 * checks live here so both backends cannot drift on what counts as a
 * misconfiguration. */
typedef enum {
	FRAME_GATE_SETUP_READY = 0,      /* armed */
	FRAME_GATE_SETUP_NO_RING,        /* transport has no occupancy signal */
	FRAME_GATE_SETUP_CLOSE_TOO_HIGH, /* threshold above the ring's capacity */
} FrameGateSetupStatus;

/* Resolve the config, initialise the gate open, and sanity-check the
 * threshold against the ring.
 *
 * There is no enable switch.  The gate arms itself whenever the output is a
 * frame ring, because the ring IS the signal it needs and every other
 * transport lacks one — so "enabled" and "has a frame ring" were always the
 * same question asked twice.  `slot_count` is the egress ring's capacity,
 * or 0 when this transport has no frame ring at all.
 *
 * `close_slots` 0 and `max_closed_ms` 0 select the defaults above.
 * close_slots is clamped to at least open_slots+1: equal thresholds make the
 * gate chatter open/closed on a single slot, which is worse than not gating.
 *
 * A non-READY status still leaves a valid, inert gate — the caller only has
 * to log it. */
FrameGateSetupStatus frame_gate_setup(FrameGate *g,
	uint32_t close_slots, uint32_t max_closed_ms, uint32_t slot_count);

/* Feed one observation.  `used_slots` is the instantaneous egress ring
 * occupancy and `now_us` a monotonic microsecond clock.
 *
 * Instantaneous, not the ring header's low_water_slots: that is published on
 * a 200 ms window (VENC_RING_LOW_WATER_WINDOW_US), which is the right
 * cadence for an external rate controller and far too slow to catch a burst.
 *
 * Returns the action the caller must apply.  The gate's own state is updated
 * before returning, so a caller whose actuator refuses the action must roll
 * it back with frame_gate_force_open() (failed CLOSE) or
 * frame_gate_restore_closed() (failed OPEN). */
FrameGateAction frame_gate_observe(FrameGate *g, uint32_t used_slots,
	uint64_t now_us);

/* Drop back to the open state without emitting an action.  For teardown, for
 * a reinit, and for a caller whose actuator refused the last CLOSE. */
void frame_gate_force_open(FrameGate *g, uint64_t now_us);

/* Restore the closed policy state after the actuator refused an OPEN.  The
 * next observation retries after min_closed_us instead of leaving policy
 * open while encoder intake is still stopped. */
void frame_gate_restore_closed(FrameGate *g, uint64_t now_us);

static inline int frame_gate_is_open(const FrameGate *g)
{
	return g ? g->open : 1;
}

static inline int frame_gate_enabled(const FrameGate *g)
{
	return g && g->enabled;
}


#endif /* FRAME_GATE_H */
