#ifndef STAR6E_LUMA_TAP_H
#define STAR6E_LUMA_TAP_H

#include <stddef.h>
#include <stdint.h>

#include "venc_config.h"

/*
 * Read-only NV12 luma tap on VPE port1 (Star6E).
 *
 * Exists so a vision consumer (QR scanning) can read frames that are NOT
 * composited with any MI_RGN overlay.  Overlays attach per scaler output port
 * and both debug_osd and waybeam-hub's osd_render target port0, so port0 — and
 * therefore the MJPEG snapshot channel that 1:N-binds to it — carries HUD
 * pixels over anything a marker might occupy.  port1 is a separate scaler
 * output and is clean.
 *
 * Lifecycle, and the reason for it: the port is programmed and enabled exactly
 * once per pipeline run and released only at teardown.  The retired
 * /api/v1/snapshot.pgm enabled and disabled port1 per HTTP request; the
 * DisablePort half races an in-flight mhal buffer and wedges the VPE input FIFO
 * (kernel-side, ~2 hard hangs per 560 captures).  A capture here sets a flag
 * that the reader observes — it never touches port state.
 *
 * Threading: the reader thread drains EVERY frame at line rate and copies the
 * luma plane out only when a grab is pending, so a slow consumer can never sit
 * between GetBuf and PutBuf.  start/stop run on the pipeline thread; grab() is
 * called from HTTP worker threads and is internally serialized.
 */

/* Start the tap.  No-op returning 0 when cfg->qr.tap_enabled is false.
 * `main_w`/`main_h` are the main-stream dimensions used when the configured
 * geometry is 0 (inherit).  Claims VPE port1 through star6e_vpe_ports; returns
 * -1 (logged, non-fatal to the pipeline) when port1 is held by stab or detect,
 * or when the SDK refuses the requested geometry.
 *
 * Must be called on the pipeline thread, after framing and detect have had
 * their chance at port1. */
int star6e_luma_tap_start(const VencConfig *cfg, uint32_t main_w,
	uint32_t main_h);

/* Remember the tap settings without touching the port.  Called at pipeline
 * bring-up so a later open() knows its geometry.  The port is NOT claimed and
 * NOT enabled here: an always-on tap costs ~186 MB/s of SCL write bandwidth at
 * 1080p60 and holds port1 against detect/stab for the whole run, for nothing
 * when no scan is in progress. */
void star6e_luma_tap_configure(const VencConfig *cfg, uint32_t main_w,
	uint32_t main_h);

/* Start a scan window of `window_ms` (0 = the configured qr.windowMs), or
 * EXTEND the one already running.  Extending never touches port state — it only
 * moves the deadline — so repeated scan requests are cheap and cannot
 * re-introduce the Enable/Disable cycling that wedged snapshot.pgm.
 *
 * Returns 0, -EBUSY when port1 is held by stab/detect (reported immediately,
 * never queued), -ENODEV when unarmed, -EIO on SDK refusal.
 *
 * Safe from any thread.  The port itself is only ever opened and closed by the
 * supervisor thread this spawns, so no SDK port call is ever made concurrently
 * from two threads. */
int star6e_luma_tap_scan(uint32_t window_ms);

/* End the current window early.  Blocks until the supervisor has closed the
 * port and released port1, so a caller can rely on port1 being free on return.
 * Idempotent. */
void star6e_luma_tap_scan_stop(void);

typedef struct {
	int      armed;        /* qr.tap_enabled and pipeline up            */
	int      scanning;     /* a window is open                          */
	uint32_t width, height;/* latch (centre-square) geometry            */
	uint32_t window_ms;    /* budget of the current/last window         */
	int64_t  remaining_ms; /* <=0 when not scanning                     */
	uint64_t frames;       /* frames drained this window                */
	uint64_t grabs;        /* frames actually copied out this window    */
	char     port1_owner[24]; /* "" when free                           */
} Star6eLumaTapStatus;

/* Snapshot the tap state for /api/v1/qr/status.  Safe from any thread. */
void star6e_luma_tap_status(Star6eLumaTapStatus *out);

/* Stop the tap and hand port1 back: park the reader outside the GetBuf/PutBuf
 * window, join it, reset the output depth, disable the port, release the claim.
 * Idempotent; safe when start() was never called or failed.  Must run BEFORE
 * the VPE source ports are unbound. */
void star6e_luma_tap_stop(void);

/* Grab one frame's luma plane as a freshly malloc'd P5 PGM blob (tightly
 * packed, stride removed, self-describing dimensions).  Caller frees with
 * star6e_luma_tap_free().
 *
 * Returns 0 on success, -ENODEV when the tap is not running, -ETIMEDOUT when no
 * frame arrives within timeout_ms, -EIO on a malformed buffer, -ENOMEM on
 * allocation failure. */
int star6e_luma_tap_grab_pgm(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms);

/* Free a buffer returned by star6e_luma_tap_grab_pgm(). */
void star6e_luma_tap_free(uint8_t *buf);

/* True while the tap is running, for endpoint gating. */
int star6e_luma_tap_running(void);

#endif /* STAR6E_LUMA_TAP_H */
