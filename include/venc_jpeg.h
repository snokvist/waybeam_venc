#ifndef VENC_JPEG_H
#define VENC_JPEG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "venc_httpd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JPEG snapshot subsystem.
 *
 * Single, dedicated MJPEG VENC channel that taps the same VPE/SCL output
 * port the main H.264/H.265 channel already consumes.  Allocated once at
 * pipeline startup, kept idle (StartRecvPic off) between requests, and
 * pulse-encoded on demand by GET /api/v1/snapshot.jpg.
 *
 * Lifecycle:
 *   pipeline-start  → venc_jpeg_init(&cfg)
 *   per-request     → venc_jpeg_capture(&buf, &len, timeout_ms)
 *   pipeline-stop   → venc_jpeg_shutdown()
 *
 * Concurrency: venc_jpeg_capture() is serialized internally.  Multiple
 * HTTP clients hitting /snapshot.jpg simultaneously queue rather than
 * stomp the JPEG channel.
 *
 * Per-backend stubs live in src/star6e_jpeg.c and src/maruko_jpeg.c.
 * The common HTTP handler + locking lives in src/venc_jpeg.c.
 */

typedef struct {
	uint32_t width;       /* 0 = inherit from main stream */
	uint32_t height;      /* 0 = inherit from main stream */
	uint32_t quality;     /* 1–100, MJPEG q-factor; clamped */
	int      channel;     /* VENC channel ID (default 7; well clear of dual) */
	bool     enabled;     /* If false, init is a no-op, capture returns ENOENT */
} VencJpegConfig;

/* Initialize the JPEG subsystem.  Returns 0 on success.  Idempotent —
 * re-init returns 0 without touching SDK state if already initialized.
 *
 * Must be called after the main VPE/VENC pipeline is up so the backend
 * can bind to the active VPE output port.  Backend implementations grab
 * the VPE port info via star6e_jpeg_set_source() / maruko_jpeg_set_source()
 * which the pipeline calls during its own bring-up. */
int venc_jpeg_init(const VencJpegConfig *cfg);

/* Tear down the JPEG channel.  Idempotent.  Must be called before the
 * main VPE/VENC pipeline tears down its source ports. */
void venc_jpeg_shutdown(void);

/* Capture one JPEG frame.  Allocates *out_buf (caller frees via
 * venc_jpeg_free).  Returns 0 on success, -ENODEV if subsystem disabled,
 * -ETIMEDOUT if no frame within timeout_ms, -EIO on SDK failure.
 *
 * Internally serialized; safe to call from multiple HTTP worker threads. */
int venc_jpeg_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms);

/* What a grayscale capture should ask the scaler for.  Grouped rather than
 * passed positionally: three adjacent uint32_t arguments across two backends
 * and a weak stub is an easy transposition to make and a hard one to see. */
typedef struct {
	/* Centre sub-rect of the scaler input window to capture, as a percentage
	 * of each linear dimension.  0 or >=100 = the whole window.  50 halves
	 * width and height (a quarter of the area) around the centre.
	 *
	 * Cropping is not scaling: it keeps every source pixel it covers, so
	 * pixels-per-module is untouched — and it drops the frame edges, where a
	 * fisheye lens distorts the most and a QR decoder's corner mapping is
	 * least reliable.  Narrower field of view is the whole cost. */
	uint32_t crop_pct;
	/* Long-side cap on the returned frame (0 = no cap).  Applied after the
	 * crop, so it scales whatever the crop selected. */
	uint32_t max_dim;
	/* Frame wait budget; 0 = backend default (1500 ms). */
	uint32_t timeout_ms;
} VencJpegGrayReq;

/* Capture one grayscale frame as a binary P5 PGM: the luma/Y plane of an
 * uncompressed NV12 frame off the scaler.  With a zeroed req this is the FULL
 * resolution of the active sensor mode's scaler input window (see
 * venc_jpeg_gray_tap_dims).  Allocates *out_buf (caller frees via
 * venc_jpeg_free).
 *
 * Returns 0 on success, -EINVAL on a NULL argument, -ENODEV if the snapshot
 * subsystem is disabled, -ETIMEDOUT if no frame arrives in time, -ENOSYS if
 * the backend has no grayscale hook, -EBUSY if another feature owns the tap,
 * -EIO on SDK failure.
 *
 * Shares the snapshot subsystem's enable gate and mutex with
 * venc_jpeg_capture(): the two never run concurrently, and both are off
 * when snapshot.enabled is false. */
int venc_jpeg_capture_gray(uint8_t **out_buf, size_t *out_len,
	const VencJpegGrayReq *req);

/* Free a buffer returned by venc_jpeg_capture() / venc_jpeg_capture_gray(). */
void venc_jpeg_free(uint8_t *buf);

/* Live-update MJPEG quality factor (1..99) on the running channel.
 * Internally serialized via the same mutex as venc_jpeg_capture, so the
 * SDK Set/Get sequence cannot interleave with a capture in progress.
 * Returns 0 on success, -ENODEV if the snapshot subsystem is disabled,
 * -ENOSYS if the backend has no set-quality hook, -EIO on SDK failure. */
int venc_jpeg_set_quality(uint32_t q);

/* HTTP handler for GET /api/v1/snapshot.jpg.  Returns image/jpeg on
 * success, application/json {ok:false,error:{...}} on failure. */
int handle_snapshot_jpeg(int client_fd, const HttpRequest *req, void *ctx);

/* HTTP handler for GET /api/v1/snapshot.pgm.  Returns a binary P5 PGM
 * (image/x-portable-graymap) on success, application/json
 * {ok:false,error:{...}} on failure.  Intended for on-device consumers
 * (e.g. a boot-time QR scan) that want raw grayscale without a JPEG
 * decode step.  The whole scaler input window; honours an optional
 * `?maxDim=<px>` query parameter. */
int handle_snapshot_pgm(int client_fd, const HttpRequest *req, void *ctx);

/* HTTP handler for GET /api/v1/snapshot-center.pgm.  Same format and error
 * surface as handle_snapshot_pgm, but captures only the centre
 * VENC_JPEG_GRAY_CENTER_PCT% of the window — a fixed, parameterless variant
 * for QR scanning on a high-resolution sensor or behind a fisheye lens, where
 * the frame edges cost bytes and decode time and carry the worst distortion.
 * A separate route rather than a flag on the one above: the caller picks a
 * behaviour, not a geometry. */
int handle_snapshot_pgm_center(int client_fd, const HttpRequest *req,
	void *ctx);

/* ── Grayscale tap geometry (shared by both backends) ────────────────── */

/* Long-side cap the backends fall back to when the scaler refuses to
 * program the tap at the preferred (full-resolution) geometry.  The vendor
 * port limits are undocumented, so a rejection must degrade to a frame size
 * known to work rather than fail the request. */
#define VENC_JPEG_GRAY_SAFE_DIM  1280u

/* Smallest tap the SCL is asked for, whatever maxDim says. */
#define VENC_JPEG_GRAY_MIN_DIM   64u

/* Centre fraction (per linear dimension) served by snapshot-center.pgm. */
#define VENC_JPEG_GRAY_CENTER_PCT  50u

/* Derive the grayscale tap geometry from the scaler's input window.
 *
 * src_w/src_h is the window the tap is cut from — on both backends the
 * post-precrop frame the active sensor mode delivers to the scaler, i.e. the
 * largest grayscale frame obtainable without upscaling.  QR/vision consumers
 * are resolution-limited (~4 px per QR module at the frame edge), so the
 * default is that full window; max_dim (0 = uncapped) scales it down,
 * aspect-preserved, for callers that want a cheaper frame.
 *
 * The result is width 16-aligned (sane SCL stride) and height even (NV12
 * chroma), floored at VENC_JPEG_GRAY_MIN_DIM.  PGM is self-describing, so
 * consumers read the real dims from the header rather than assuming any of
 * this.  Returns 0 on success, -EINVAL on a NULL out pointer or empty
 * source window. */
int venc_jpeg_gray_tap_dims(uint32_t src_w, uint32_t src_h, uint32_t max_dim,
	uint32_t *out_w, uint32_t *out_h);

/* Shrink a scaler source rect to its centre `pct`% in each linear dimension.
 * pct 0 or >=100 copies the rect through unchanged.
 *
 * The in/out rect is in whichever coordinate domain the caller's crop
 * primitive uses — Star6E passes (0, 0, w, h) because MI_VPE_SetPortCrop is
 * relative to the VPE channel input, Maruko passes the published SCL crop
 * window because fnSetPortConfig takes ISP-plane coordinates.  Origin and
 * size are aligned even (both scalers reject odd crop rects) and the size is
 * floored at 2 px so a degenerate pct cannot produce an empty rect.
 *
 * Returns 0 on success, -EINVAL on a NULL out pointer or empty source. */
int venc_jpeg_gray_center_rect(uint32_t src_x, uint32_t src_y,
	uint32_t src_w, uint32_t src_h, uint32_t pct,
	uint32_t *out_x, uint32_t *out_y, uint32_t *out_w, uint32_t *out_h);

/* ── Backend interface (implemented per-SOC) ─────────────────────────── */

/* Backend-private: register VPE/SCL output port info with the JPEG
 * module.  The pipeline calls this during init right after the main
 * VPE port is configured.  After this call, venc_jpeg_init() can bind
 * the MJPEG channel to the same source. */
struct MI_SYS_ChnPort_t_;
void venc_jpeg_set_source(const void *vpe_port_opaque);

/* Backend-private: create + bind MJPEG channel.  Channel stays idle
 * (StartRecvPic off) after this returns.  Called from venc_jpeg_init. */
int venc_jpeg_backend_init(const VencJpegConfig *cfg);

/* Backend-private: capture one JPEG.  Called from venc_jpeg_capture
 * under the module lock.  Implementation does StartRecvPic → wait for
 * frame → GetStream → memcpy → ReleaseStream → StopRecvPic. */
int venc_jpeg_backend_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms);

/* Backend-private: register the scaler input window the grayscale tap is cut
 * from — the post-precrop frame of the active sensor mode.  The pipeline
 * calls this during bring-up (and with 0,0,0,0 on teardown).  Both backends
 * take the size as the tap's full-resolution default; Maruko additionally
 * programs x/y as an explicit SCL crop, while the Star6E VPE tap inherits the
 * window from its channel.  Weak no-op default so a backend that implements
 * no grayscale capture links unchanged. */
void venc_jpeg_set_gray_source(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Backend-private: capture one grayscale frame as a complete binary P5
 * PGM blob (header + tightly-packed Y-plane rows).  Called from
 * venc_jpeg_capture_gray under the module lock.  Programs a short-lived tap
 * on the scaler, grabs one uncompressed NV12 frame and copies its luma
 * plane.  Geometry is venc_jpeg_gray_center_rect(source window, req->crop_pct)
 * fed through venc_jpeg_gray_tap_dims(.., req->max_dim).  `req` is never NULL.
 * Returns 0 on success (allocating *out_buf), -ENOSYS when the backend does
 * not implement grayscale capture, -ENODEV when no source window has been
 * registered, -EBUSY when another feature owns the tap, -ETIMEDOUT if no
 * frame arrives, -EIO on SDK failure. */
int venc_jpeg_backend_capture_gray(uint8_t **out_buf, size_t *out_len,
	const VencJpegGrayReq *req);

/* Backend-private: destroy MJPEG channel.  Called from venc_jpeg_shutdown. */
void venc_jpeg_backend_shutdown(void);

/* Backend-private: Get→modify→Set MJPEG channel quality on the running
 * channel.  Called from venc_jpeg_set_quality under the module lock.
 * Returns 0 on success, -EIO on SDK error, -ENOSYS when the backend
 * does not implement live quality (host-test fallback). */
int venc_jpeg_backend_set_quality(uint32_t q);

#ifdef __cplusplus
}
#endif

#endif /* VENC_JPEG_H */
