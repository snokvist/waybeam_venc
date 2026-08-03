/*
 * Sojourn-time bitrate clamp for the unix:// egress transport.
 *
 * Problem this solves.  unix:// has no queue venc owns.  The AF_UNIX
 * datagram sender blocks on the *receiver's* queue depth, latched into that
 * socket from net.unix.max_dgram_qlen when the consumer created it — venc
 * can neither set it nor read it, and cannot change it for a consumer that
 * is already running (documentation/UNIX_SOCKET_HANDOVER.md §2.1).  So the
 * producer holds its own queue of whole frames (include/venc_frame_queue.h)
 * and this controller clamps the encoder before that queue overflows.
 *
 * Why not reuse venc_shm_throttle directly.  That controller counts ring
 * slots, and a slot is one frame regardless of size.  Intra-GOP frame size
 * varies 10-50x (a 15 Mbps IDR is ~90 RTP packets, a P-frame ~15), so on a
 * packet-unit queue the encoder would inject a large periodic disturbance
 * into the very signal it controls on, at GOP rate.  Fill percentage on a
 * socket queue has no stable meaning across frame sizes, bitrates or fps.
 *
 * Control variable: queue sojourn time, in microseconds.  Specifically the
 * age of the oldest frame still waiting, sampled once per encoded frame,
 * and zero when the queue is empty.  That one number is frame-size,
 * bitrate and fps invariant, it needs no estimate of the link rate or any
 * cooperation from the consumer, and it is measured rather than modelled.
 * It also has no stale-sample problem: a wedged consumer makes it grow
 * monotonically, where a completed-frame sojourn would simply stop
 * updating at exactly the moment the signal matters.  Completed-frame
 * sojourn is kept, but as telemetry only.
 *
 * Control law (evaluated on each interval's MINIMUM sojourn):
 *   min_sojourn >= TARGET_US   -> permille = max(FLOOR, permille * 4/5)
 *   overflows increased        -> permille = max(FLOOR, permille * 3/5),
 *                                 immediately, but at most ONCE per interval
 *   min_sojourn <= RECOVER_US  -> permille = min(1000, permille + 50)
 *
 * Minimum over the interval, not maximum, and not an instantaneous read.
 * This is CoDel's rule and it is the same rule venc_shm_throttle already
 * uses with low-water slots, for the same reason: a burst that drains is
 * not congestion, standing delay is.  If the queue reached empty at any
 * point in the interval the consumer is keeping up and the spike was
 * transient; if it never emptied across a whole interval, that is real
 * standing backlog.
 *
 * TARGET_US and RECOVER_US leave a deliberate hold band between them.  The
 * ring controller has none — its ENGAGE (2 slots) and RECOVER (1 slot) are
 * adjacent integers — but sojourn is continuous, so without a band the
 * controller would alternate decrease and increase around a single
 * threshold.
 *
 * CoDel's signal, not CoDel's response.  Upstream CoDel drops packets on a
 * 1/sqrt(count) schedule; here the actuator is the same multiplicative
 * clamp venc_shm_throttle uses, because dropping an encoded frame breaks
 * the H.26x reference chain — dropping is the damage this exists to avoid,
 * not an acceptable response to it.  The clamp is a multiplier applied to
 * whatever bitrate was configured and NEVER writes video0.bitrate, so an
 * external rate controller's write-on-change cache stays coherent and any
 * number of controllers compose with it.
 *
 * Cascade stability: the 200 ms interval must stay >=5x faster than the
 * slowest external actuator loop (currently ~1500 ms settle).  Preserve
 * that separation if either side is retuned — it is what keeps the two
 * controllers from coupling into oscillation.
 *
 * TARGET_US is provisional pending on-device calibration; see
 * documentation/UNIX_CODEL_PACING_PLAN.md §6.  Everything else is
 * compile-time policy, following include/venc_shm_throttle.h.  The runtime
 * knobs are outgoing.unix_pacing and outgoing.unix_throttle.
 *
 * No SDK dependency, no syscalls, no clock of its own — the caller passes
 * now_us.  Pure and host-unit-testable; see tests/test_venc_codel.c.
 *
 * Threading: one instance per output, owned by the pipeline thread that
 * drains the queue.  Not internally synchronised and does not need to be.
 */

#ifndef VENC_CODEL_H
#define VENC_CODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VENC_CODEL_INTERVAL_US    200000u  /* control period */
#define VENC_CODEL_TARGET_US       10000u  /* >= this -> decrease */
#define VENC_CODEL_RECOVER_US       5000u  /* <= this -> increase */
/* Overridable for sweeps.  The floor is the lowest rate the controller can
 * command, as a fraction of the *configured* bitrate — so a consumer slower
 * than FLOOR x video0.bitrate cannot be tracked at all (device-measured
 * 2026-08-03). */
#ifndef VENC_CODEL_FLOOR_PERMILLE
#define VENC_CODEL_FLOOR_PERMILLE    250u
#endif
#define VENC_CODEL_FULL_PERMILLE    1000u
#define VENC_CODEL_AI_STEP            50u  /* additive increase */

/* EXPERIMENT: asymmetric probing.  Congestion *response* must be fast, but
 * capacity *discovery* need not be — a link does not gain headroom on a
 * 200 ms timescale.  Rate-limiting the increase to one step every N control
 * intervals (decrease still every interval) is intended to stop the climb
 * walking past the sustainable rate and refilling the queue.  1 = the
 * as-shipped behaviour, so the default is a no-op. */
#ifndef VENC_CODEL_AI_EVERY
#define VENC_CODEL_AI_EVERY            1u
#endif

#define VENC_CODEL_NO_SAMPLE  0xFFFFFFFFu

typedef struct {
	uint16_t permille;           /* current clamp, FLOOR..1000 */
	uint32_t interval_min_us;    /* minimum sojourn this interval,
	                              * NO_SAMPLE until the first observation */
	uint32_t reported_min_us;    /* last closed interval's minimum, for
	                              * telemetry; NO_SAMPLE until one closes */
	uint64_t interval_start_us;
	uint64_t last_overflows;
	uint8_t  seeded;             /* last_overflows is meaningful */
	uint8_t  enabled;
	uint8_t  pending_change;     /* consumed by _tick */
	uint8_t  overflow_charged;   /* overflow MD already applied this interval */
	uint8_t  at_floor;           /* edge state for _floor_edge */
	uint8_t  floor_edge_pending; /* 1 = entered, 2 = left */
	uint8_t  ai_hold;            /* intervals still to skip before the next
	                              * additive increase (VENC_CODEL_AI_EVERY) */
} VencCodel;

/* Zero state, permille = 1000, enabled.  Safe to call at any time; a
 * caller that re-creates its queue should call this so a stale interval
 * boundary or overflow count cannot leak across the restart. */
void venc_codel_reset(VencCodel *c, uint64_t now_us);

/* Enable/disable.  Idempotent — safe to call every frame from config.
 * Disabling restores permille to 1000 and raises a pending change, so the
 * caller re-applies the unclamped bitrate rather than leaving the encoder
 * pinned at whatever the clamp last programmed.  Re-enabling starts from
 * 1000 with a fresh interval. */
void venc_codel_set_enabled(VencCodel *c, int enabled, uint64_t now_us);

int venc_codel_is_enabled(const VencCodel *c);

/* Called once per encoded frame — no syscalls, a few loads and compares.
 * `sojourn_us` is the age of the oldest frame still queued, or 0 when the
 * queue is empty, and `overflows` the queue's lifetime count of frames
 * refused for want of space. */
void venc_codel_observe(VencCodel *c, uint32_t sojourn_us, uint64_t overflows);

/* Returns 1 when the clamp factor changed and the caller must re-apply the
 * bitrate, 0 otherwise.  Call once per frame after _observe. */
int venc_codel_tick(VencCodel *c, uint64_t now_us);

uint16_t venc_codel_permille(const VencCodel *c);

/* Last closed interval's minimum sojourn, or VENC_CODEL_NO_SAMPLE before
 * any interval has closed.  Telemetry only — the control path uses the
 * in-progress accumulator, which this deliberately does not expose. */
uint32_t venc_codel_reported_min_us(const VencCodel *c);

/* Scale a bitrate by a clamp factor.  Takes the permille rather than the
 * struct because the apply path lives in the backend control layer, which
 * is reached from the HTTP thread and holds only the published factor —
 * the controller instance itself belongs to the pipeline thread. */
uint32_t venc_codel_scale(uint16_t permille, uint32_t kbps);

/* Convenience wrapper for callers that do hold the instance. */
uint32_t venc_codel_apply(const VencCodel *c, uint32_t kbps);

/* Floor-pinned transition, consumed by the caller's logging.  Returns 1
 * exactly once when the clamp reaches the floor, -1 exactly once when it
 * leaves, 0 otherwise.  Silent floor-pinning reads as "working" when it is
 * not: at the floor the clamp has spent all its authority and the queue is
 * still backing up, which usually means the consumer is not draining
 * rather than that the encoder is too fast. */
int venc_codel_floor_edge(VencCodel *c);

#ifdef __cplusplus
}
#endif

#endif /* VENC_CODEL_H */
