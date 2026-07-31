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

/* Open the tap for a scan window: claim port1, program geometry, enable, spawn
 * the reader.  Returns 0, -EBUSY when port1 is held by stab/detect, -ENODEV
 * when disabled or unconfigured, -EIO on SDK refusal.  Idempotent. */
int star6e_luma_tap_open(void);

/* Close the tap and hand port1 back, with the ENCODER STILL RUNNING.  Ordering
 * differs from pipeline teardown and is the risky path: stop the reader and
 * join it, then drain any buffers the port still holds, and only then reset the
 * depth and disable.  Disabling with buffers queued is what races an in-flight
 * mhal buffer.  Idempotent. */
void star6e_luma_tap_close(void);

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
