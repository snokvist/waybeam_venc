/* venc_jpeg.c — common JPEG snapshot logic shared across backends.
 *
 * Owns the public API surface (init/capture/shutdown), the module-wide
 * mutex, and the HTTP handler.  The per-backend file (star6e_jpeg.c or
 * maruko_jpeg.c) implements the actual SDK VENC channel lifecycle via
 * the venc_jpeg_backend_* hooks declared in include/venc_jpeg.h.
 */

#include "venc_jpeg.h"
#include "venc_httpd.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized;
static VencJpegConfig g_cfg;

int venc_jpeg_init(const VencJpegConfig *cfg)
{
	if (!cfg)
		return -EINVAL;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (g_initialized) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return 0;
	}

	g_cfg = *cfg;
	/* Clamp quality to a sane range; SDK MJPEG quality is roughly
	 * MinQfactor/MaxQfactor on Star6E and a quality int on Maruko. */
	if (g_cfg.quality == 0) g_cfg.quality = 80;
	if (g_cfg.quality > 99) g_cfg.quality = 99;
	if (g_cfg.channel <= 0) g_cfg.channel = 7;

	if (!g_cfg.enabled) {
		g_initialized = true;
		pthread_mutex_unlock(&g_jpeg_mutex);
		return 0;
	}

	int rc = venc_jpeg_backend_init(&g_cfg);
	if (rc != 0) {
		fprintf(stderr, "[jpeg] backend_init failed %d (snapshot endpoint disabled)\n", rc);
		g_cfg.enabled = false;  /* Mark disabled; capture will return -ENODEV */
	}
	g_initialized = true;
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

void venc_jpeg_shutdown(void)
{
	pthread_mutex_lock(&g_jpeg_mutex);
	if (g_initialized && g_cfg.enabled) {
		venc_jpeg_backend_shutdown();
	}
	g_initialized = false;
	g_cfg.enabled = false;
	pthread_mutex_unlock(&g_jpeg_mutex);
}

int venc_jpeg_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (!g_initialized || !g_cfg.enabled) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return -ENODEV;
	}
	int rc = venc_jpeg_backend_capture(out_buf, out_len, timeout_ms);
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

int venc_jpeg_capture_gray(uint8_t **out_buf, size_t *out_len,
	const VencJpegGrayReq *req)
{
	if (!out_buf || !out_len || !req)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;

	/* Apply the default frame budget once, here, so every backend can treat
	 * req->timeout_ms as already non-zero. */
	VencJpegGrayReq eff = *req;
	if (eff.timeout_ms == 0)
		eff.timeout_ms = VENC_JPEG_GRAY_TIMEOUT_MS;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (!g_initialized || !g_cfg.enabled) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return -ENODEV;
	}
	int rc = venc_jpeg_backend_capture_gray(out_buf, out_len, &eff);
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

int venc_jpeg_gray_tap_dims(uint32_t src_w, uint32_t src_h, uint32_t max_dim,
	uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w = src_w, h = src_h;
	uint32_t longest = (w > h) ? w : h;

	if (!out_w || !out_h)
		return -EINVAL;
	if (w == 0 || h == 0)
		return -EINVAL;

	/* Scale down only — asking the scaler to upscale buys no detail. */
	if (max_dim != 0 && longest > max_dim) {
		w = (uint32_t)((uint64_t)w * max_dim / longest);
		h = (uint32_t)((uint64_t)h * max_dim / longest);
	}
	w &= ~15u;   /* 16-align the width so the scaler stride is sane */
	h &= ~1u;    /* NV12 needs an even height */
	if (w < VENC_JPEG_GRAY_MIN_DIM) w = VENC_JPEG_GRAY_MIN_DIM;
	if (h < VENC_JPEG_GRAY_MIN_DIM) h = VENC_JPEG_GRAY_MIN_DIM;
	*out_w = w;
	*out_h = h;
	return 0;
}

int venc_jpeg_gray_center_rect(uint32_t src_x, uint32_t src_y,
	uint32_t src_w, uint32_t src_h, uint32_t pct,
	uint32_t *out_x, uint32_t *out_y, uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w, h;

	if (!out_x || !out_y || !out_w || !out_h)
		return -EINVAL;
	if (src_w == 0 || src_h == 0)
		return -EINVAL;

	if (pct == 0 || pct >= 100) {
		w = src_w;
		h = src_h;
	} else {
		w = src_w * pct / 100;
		h = src_h * pct / 100;
	}
	w &= ~1u;            /* both scalers reject an odd crop rect */
	h &= ~1u;
	if (w < 2) w = 2;    /* never hand the scaler an empty rect */
	if (h < 2) h = 2;
	*out_w = w;
	*out_h = h;
	*out_x = src_x + (((src_w - w) / 2) & ~1u);
	*out_y = src_y + (((src_h - h) / 2) & ~1u);
	return 0;
}

int venc_jpeg_gray_tap_geoms(uint32_t src_w, uint32_t src_h, uint32_t max_dim,
	VencJpegGrayGeom out[2])
{
	if (!out)
		return -EINVAL;
	if (venc_jpeg_gray_tap_dims(src_w, src_h, max_dim, &out[0].w,
	    &out[0].h) != 0)
		return -EINVAL;
	if (out[0].w <= VENC_JPEG_GRAY_SAFE_DIM &&
	    out[0].h <= VENC_JPEG_GRAY_SAFE_DIM)
		return 1;
	/* Cannot fail: the source window is non-empty and the cap is non-zero. */
	(void)venc_jpeg_gray_tap_dims(src_w, src_h, VENC_JPEG_GRAY_SAFE_DIM,
		&out[1].w, &out[1].h);
	return 2;
}

void venc_jpeg_free(uint8_t *buf)
{
	free(buf);
}

int venc_jpeg_set_quality(uint32_t q)
{
	if (q == 0) q = 1;
	if (q > 99) q = 99;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (!g_initialized || !g_cfg.enabled) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return -ENODEV;
	}
	int rc = venc_jpeg_backend_set_quality(q);
	if (rc == 0)
		g_cfg.quality = q;
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

int handle_snapshot_jpeg(int client_fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;

	uint8_t *buf = NULL;
	size_t   len = 0;
	int rc = venc_jpeg_capture(&buf, &len, 1500);
	if (rc == -ENODEV) {
		return httpd_send_error(client_fd, 503, "snapshot_disabled",
			"snapshot endpoint not available (subsystem disabled or "
			"pipeline not running)");
	}
	if (rc == -ETIMEDOUT) {
		return httpd_send_error(client_fd, 504, "snapshot_timeout",
			"timed out waiting for a JPEG frame from the MJPEG channel");
	}
	if (rc != 0 || !buf || len == 0) {
		venc_jpeg_free(buf);
		return httpd_send_error(client_fd, 500, "snapshot_failed",
			"backend MJPEG capture failed");
	}

	int sent = httpd_send_binary(client_fd, 200, "image/jpeg", buf, (int)len);
	venc_jpeg_free(buf);
	return sent;
}

/* Parse one optional positive-integer query parameter.  Absent → 0, which
 * every geometry knob reads as "unset, use the default".  Returns -1 on
 * anything that is not a plain positive integer <= max, so a typo fails
 * loudly rather than silently capturing at the default geometry. */
static int pgm_parse_uint(const HttpRequest *req, const char *key,
	unsigned long max, uint32_t *out)
{
	char raw[16];
	char *end = NULL;
	unsigned long v;

	*out = 0;
	if (!req || httpd_query_param(req, key, raw, sizeof(raw)) != 0)
		return 0;
	if (raw[0] < '1' || raw[0] > '9')   /* also rejects "", "0…", "-1", " 1" */
		return -1;
	v = strtoul(raw, &end, 10);
	if (*end != '\0' || v > max)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

int handle_snapshot_pgm(int client_fd, const HttpRequest *req, void *ctx)
{
	uint8_t *buf = NULL;
	size_t   len = 0;
	VencJpegGrayReq greq = { 0 };

	(void)ctx;
	if (pgm_parse_uint(req, "crop", 100, &greq.crop_pct) != 0) {
		return httpd_send_error(client_fd, 400, "bad_crop",
			"crop must be a percentage from 1 to 100 (omit it, or pass 100, "
			"for the whole scaler window)");
	}
	if (pgm_parse_uint(req, "maxDim", VENC_JPEG_GRAY_MAX_DIM,
	    &greq.max_dim) != 0) {
		return httpd_send_error(client_fd, 400, "bad_max_dim",
			"maxDim must be a positive integer up to 8192 (omit it for the "
			"full sensor-mode resolution)");
	}

	int rc = venc_jpeg_capture_gray(&buf, &len, &greq);
	if (rc == -ENODEV) {
		return httpd_send_error(client_fd, 503, "snapshot_disabled",
			"snapshot endpoint not available (subsystem disabled or "
			"pipeline not running)");
	}
	if (rc == -ENOSYS) {
		return httpd_send_error(client_fd, 501, "snapshot_gray_unsupported",
			"grayscale snapshot not implemented on this backend");
	}
	if (rc == -EBUSY) {
		return httpd_send_error(client_fd, 409, "snapshot_gray_busy",
			"the VPE tap this capture needs is owned by another feature "
			"(stab framing or NPU detection); disable it or capture before "
			"it starts");
	}
	if (rc == -ETIMEDOUT) {
		return httpd_send_error(client_fd, 504, "snapshot_timeout",
			"timed out waiting for a frame from the VPE port");
	}
	if (rc != 0 || !buf || len == 0) {
		venc_jpeg_free(buf);
		return httpd_send_error(client_fd, 500, "snapshot_failed",
			"backend grayscale capture failed");
	}

	int sent = httpd_send_binary(client_fd, 200, "image/x-portable-graymap",
		buf, (int)len);
	venc_jpeg_free(buf);
	return sent;
}

/* Default fallback for builds that don't link a backend (e.g. host-native
 * test runner).  Per-backend files override these with strong symbols.  */
__attribute__((weak)) void venc_jpeg_set_source(const void *vpe_port_opaque)
{
	(void)vpe_port_opaque;
}

__attribute__((weak)) int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	(void)cfg;
	return -ENOSYS;
}

__attribute__((weak)) int venc_jpeg_backend_capture(uint8_t **out_buf,
	size_t *out_len, uint32_t timeout_ms)
{
	(void)out_buf; (void)out_len; (void)timeout_ms;
	return -ENOSYS;
}

__attribute__((weak)) int venc_jpeg_backend_capture_gray(uint8_t **out_buf,
	size_t *out_len, const VencJpegGrayReq *req)
{
	(void)out_buf; (void)out_len; (void)req;
	return -ENOSYS;
}

__attribute__((weak)) void venc_jpeg_set_gray_source(uint32_t x, uint32_t y,
	uint32_t w, uint32_t h)
{
	(void)x; (void)y; (void)w; (void)h;
}

__attribute__((weak)) void venc_jpeg_backend_shutdown(void) { }

__attribute__((weak)) int venc_jpeg_backend_set_quality(uint32_t q)
{
	(void)q;
	return -ENOSYS;
}
