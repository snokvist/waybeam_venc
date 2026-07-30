/* test_venc_jpeg.c — unit tests for src/venc_jpeg.c
 *
 * Covers the common (HTTP/lock) layer.  Backend MJPEG plumbing is
 * hardware-dependent and validated on-device via
 * scripts/maruko_sensor_init_diff.sh.
 *
 * The host test_runner links src/venc_jpeg.c with no backend file,
 * so the weak fallback stubs in venc_jpeg.c resolve every backend
 * symbol — meaning every code path here exercises the public API in
 * the "no backend present" state (init fails → endpoint disabled).
 */

#include "test_helpers.h"
#include "venc_jpeg.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int test_venc_jpeg(void)
{
	int failures = 0;

	/* Reset to a clean state in case a previous test suite left the
	 * static `g_initialized` flag set. */
	venc_jpeg_shutdown();

	/* Capture without init must refuse. */
	{
		uint8_t *buf = NULL;
		size_t   len = 0;
		int rc = venc_jpeg_capture(&buf, &len, 100);
		CHECK("capture before init returns -ENODEV", rc == -ENODEV);
		CHECK("capture before init leaves buf NULL", buf == NULL);
		CHECK("capture before init leaves len 0",    len == 0);
	}

	/* NULL args rejected with -EINVAL. */
	{
		size_t len = 0;
		CHECK("capture rejects NULL buf",
			venc_jpeg_capture(NULL, &len, 0) == -EINVAL);
		uint8_t *buf = NULL;
		CHECK("capture rejects NULL len",
			venc_jpeg_capture(&buf, NULL, 0) == -EINVAL);
	}

	/* Grayscale tap geometry.  The default (max_dim=0) must be the FULL
	 * source window — the active sensor mode's scaler input — because QR
	 * decoding is limited by pixels per module, not by frame count. */
	{
		uint32_t w = 0, h = 0;

		CHECK("tap dims reject an empty source",
			venc_jpeg_gray_tap_dims(0, 1080, 0, &w, &h) == -EINVAL);
		CHECK("tap dims reject NULL out",
			venc_jpeg_gray_tap_dims(1920, 1080, 0, &w, NULL) == -EINVAL);

		/* imx335 2592x1944 at a 16:9 precrop: uncapped = the whole window. */
		CHECK("uncapped keeps the full source",
			venc_jpeg_gray_tap_dims(2592, 1458, 0, &w, &h) == 0 &&
			w == 2592 && h == 1458);

		/* Explicit cap scales down, aspect preserved. */
		CHECK("cap scales the long side down",
			venc_jpeg_gray_tap_dims(2592, 1458, 1280, &w, &h) == 0 &&
			w == 1280 && h == 720);

		/* A cap above the source never upscales. */
		CHECK("cap above source does not upscale",
			venc_jpeg_gray_tap_dims(1280, 720, 4096, &w, &h) == 0 &&
			w == 1280 && h == 720);

		/* Portrait source: the cap applies to the long side (height). */
		CHECK("portrait caps on height",
			venc_jpeg_gray_tap_dims(1080, 1920, 960, &w, &h) == 0 &&
			h == 960 && w == 528);

		/* Width 16-aligned (SCL stride) and height even (NV12 chroma). */
		CHECK("odd source is aligned down",
			venc_jpeg_gray_tap_dims(2593, 1459, 0, &w, &h) == 0 &&
			w == 2592 && h == 1458);

		/* Floor so a tiny cap cannot ask the SCL for a degenerate frame. */
		CHECK("tiny cap floors at the minimum",
			venc_jpeg_gray_tap_dims(1920, 1080, 8, &w, &h) == 0 &&
			w == VENC_JPEG_GRAY_MIN_DIM && h == VENC_JPEG_GRAY_MIN_DIM);
	}

	/* Init with NULL cfg rejected. */
	CHECK("init rejects NULL cfg", venc_jpeg_init(NULL) == -EINVAL);

	/* Init with enabled=false is a no-op success.  Subsequent capture
	 * returns -ENODEV because the subsystem is marked disabled. */
	{
		VencJpegConfig cfg = {
			.width = 1920, .height = 1080, .quality = 80,
			.channel = 7, .enabled = false,
		};
		CHECK("init(enabled=false) returns 0", venc_jpeg_init(&cfg) == 0);

		uint8_t *buf = NULL;
		size_t   len = 0;
		CHECK("capture after init(enabled=false) returns -ENODEV",
			venc_jpeg_capture(&buf, &len, 100) == -ENODEV);
	}

	venc_jpeg_shutdown();

	/* Init with enabled=true, no backend linked → backend_init returns
	 * -ENOSYS via the weak stub; module marks itself disabled, so
	 * capture still returns -ENODEV (clean degradation). */
	{
		VencJpegConfig cfg = {
			.width = 1920, .height = 1080, .quality = 80,
			.channel = 7, .enabled = true,
		};
		int rc = venc_jpeg_init(&cfg);
		CHECK("init(enabled=true,no backend) returns backend err",
			rc == -ENOSYS);

		uint8_t *buf = NULL;
		size_t   len = 0;
		CHECK("capture after failed backend init still -ENODEV",
			venc_jpeg_capture(&buf, &len, 100) == -ENODEV);
	}

	venc_jpeg_shutdown();

	/* venc_jpeg_free with NULL is a no-op (matches free(NULL)). */
	venc_jpeg_free(NULL);
	CHECK("free(NULL) does not crash", 1);

	/* Shutdown is idempotent. */
	venc_jpeg_shutdown();
	venc_jpeg_shutdown();
	CHECK("double shutdown does not crash", 1);

	/* Re-init after shutdown should work again. */
	{
		VencJpegConfig cfg = {
			.width = 1280, .height = 720, .quality = 80,
			.channel = 7, .enabled = false,
		};
		CHECK("re-init after shutdown returns 0",
			venc_jpeg_init(&cfg) == 0);
	}

	venc_jpeg_shutdown();
	return failures;
}
