/* qr_decode.c — decode a QR code from a binary P5 PGM (grayscale) image.
 *
 * Reads a P5 PGM from a file argument (or stdin when the argument is "-" or
 * absent), runs quirc over the luma pixels, and prints the payload of the
 * first successfully decoded QR code to stdout.
 *
 * Robustness for real captures (small codes in a large frame, rotation,
 * mirroring):
 *   - each detected code is retried through quirc_flip() so mirrored/flipped
 *     codes decode (ISO 18004:2015 mirror feature);
 *   - decoding is attempted over the full frame, then a grid of overlapping
 *     tiles (so a small code occupies a larger fraction of each sub-image and
 *     gets local contrast), then a half-scale pass (helps large/soft codes).
 *   The first success wins.
 *
 * Exit codes:
 *   0  a QR code was decoded; payload written to stdout
 *   1  no QR code found / not decodable
 *   2  usage or input error (bad PGM, allocation failure)
 *
 * Pairs with waybeam's GET /api/v1/snapshot.pgm endpoint:
 *   curl -s http://127.0.0.1/api/v1/snapshot.pgm | qr_decode
 *
 * quirc (ISC) is vendored under tools/qr/quirc/.
 */

#include "quirc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_REGION   40   /* skip regions too small to hold a findable code */

/* Read the next unsigned integer from a P5 header, skipping whitespace and
 * '#' comment lines.  Returns 0 on success, -1 on EOF/format error. */
static int pgm_read_uint(FILE *f, unsigned *out)
{
	int c;

	for (;;) {
		c = fgetc(f);
		if (c == EOF)
			return -1;
		if (c == '#') {
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		break;
	}
	if (c < '0' || c > '9')
		return -1;

	unsigned v = 0;
	while (c >= '0' && c <= '9') {
		v = v * 10u + (unsigned)(c - '0');
		c = fgetc(f);
	}
	*out = v;
	return 0;
}

/* Load a binary P5 PGM into a freshly malloc'd 8-bit grayscale buffer.
 * Returns the pixel buffer (caller frees) and writes w/h, or NULL on error. */
static uint8_t *pgm_load(FILE *f, unsigned *w, unsigned *h)
{
	char magic[2] = {0};
	unsigned maxval = 0;

	if (fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '5') {
		fprintf(stderr, "qr_decode: not a P5 PGM\n");
		return NULL;
	}
	if (pgm_read_uint(f, w) != 0 || pgm_read_uint(f, h) != 0 ||
	    pgm_read_uint(f, &maxval) != 0) {
		fprintf(stderr, "qr_decode: malformed PGM header\n");
		return NULL;
	}
	if (*w == 0 || *h == 0 || maxval == 0 || maxval > 255) {
		fprintf(stderr, "qr_decode: unsupported PGM (w=%u h=%u max=%u; "
			"need 8-bit)\n", *w, *h, maxval);
		return NULL;
	}
	/* Exactly one whitespace byte separates the header from the raster;
	 * pgm_read_uint already consumed it while scanning past maxval. */

	size_t npix = (size_t)*w * *h;
	uint8_t *pix = malloc(npix);
	if (!pix) {
		fprintf(stderr, "qr_decode: out of memory\n");
		return NULL;
	}
	if (fread(pix, 1, npix, f) != npix) {
		fprintf(stderr, "qr_decode: short pixel data\n");
		free(pix);
		return NULL;
	}
	return pix;
}

/* Run quirc over one w×h grayscale region copied out of src at (x0,y0), trying
 * every detected code both as-is and mirror-flipped.  Prints the first decoded
 * payload and returns 1; returns 0 if nothing decoded here. */
static int decode_region(struct quirc *q, const uint8_t *src, int sstride,
	int x0, int y0, int w, int h)
{
	if (w < MIN_REGION || h < MIN_REGION)
		return 0;
	if (quirc_resize(q, w, h) != 0)
		return 0;

	int bw = 0, bh = 0;
	uint8_t *buf = quirc_begin(q, &bw, &bh);
	for (int r = 0; r < h; r++)
		memcpy(buf + (size_t)r * w,
		       src + (size_t)(y0 + r) * sstride + x0, (size_t)w);
	quirc_end(q);

	int n = quirc_count(q);
	for (int i = 0; i < n; i++) {
		struct quirc_code code;
		struct quirc_data data;

		quirc_extract(q, i, &code);
		if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
			quirc_flip(&code);
			if (quirc_decode(&code, &data) != QUIRC_SUCCESS)
				continue;
		}
		fwrite(data.payload, 1, (size_t)data.payload_len, stdout);
		fputc('\n', stdout);
		return 1;
	}
	return 0;
}

/* 2×2 box-average downscale into a freshly malloc'd buffer (odd tail dropped).
 * Returns NULL if the result would be too small to bother. */
static uint8_t *downscale2(const uint8_t *src, int w, int h, int *ow, int *oh)
{
	int dw = w / 2, dh = h / 2;
	if (dw < MIN_REGION || dh < MIN_REGION)
		return NULL;

	uint8_t *dst = malloc((size_t)dw * dh);
	if (!dst)
		return NULL;
	for (int y = 0; y < dh; y++) {
		const uint8_t *r0 = src + (size_t)(2 * y) * w;
		const uint8_t *r1 = r0 + w;
		uint8_t *o = dst + (size_t)y * dw;
		for (int x = 0; x < dw; x++) {
			int s = r0[2 * x] + r0[2 * x + 1] +
				r1[2 * x] + r1[2 * x + 1];
			o[x] = (uint8_t)((s + 2) >> 2);
		}
	}
	*ow = dw;
	*oh = dh;
	return dst;
}

/* Three overlapping start offsets (0, centre, end) for one axis, or a single
 * offset when the axis is not worth tiling.  Returns the count (1 or up to 3,
 * de-duplicated). */
static int tile_offsets(int dim, int tile, int *out)
{
	int n = 0, cand[3], i, j;

	cand[0] = 0;
	cand[1] = (dim - tile) / 2;
	cand[2] = dim - tile;
	for (i = 0; i < 3; i++) {
		if (cand[i] < 0)
			cand[i] = 0;
		for (j = 0; j < n; j++)
			if (out[j] == cand[i])
				break;
		if (j == n)
			out[n++] = cand[i];
	}
	return n;
}

/* 3×3 box blur into a freshly malloc'd buffer (edges copied through).  A light
 * denoise lets a noisy capture's modules threshold cleanly — often the
 * difference between decode and no-decode on a small code (measured); over-
 * blurring merges modules, so exactly one pass is applied. */
static uint8_t *box_blur3(const uint8_t *src, int w, int h)
{
	uint8_t *dst = malloc((size_t)w * h);
	if (!dst)
		return NULL;
	memcpy(dst, src, (size_t)w * h);
	for (int y = 1; y < h - 1; y++) {
		for (int x = 1; x < w - 1; x++) {
			int s = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					s += src[(size_t)(y + dy) * w + (x + dx)];
			dst[(size_t)y * w + x] = (uint8_t)(s / 9);
		}
	}
	return dst;
}

/* Full frame → overlapping tiles → half scale.  Returns 1 on first decode. */
static int decode_passes(struct quirc *q, const uint8_t *pix, int w, int h)
{
	if (decode_region(q, pix, w, 0, 0, w, h))
		return 1;

	/* Tile only when the frame is big enough that a small code benefits from
	 * a local, higher-fraction view.  Half-size tiles at 3 offsets per axis
	 * (50% overlap) so a code near a seam still lands whole in some tile. */
	if (w >= 2 * MIN_REGION && h >= 2 * MIN_REGION &&
	    (w > 640 || h > 640)) {
		int tw = (w + 1) / 2, th = (h + 1) / 2;
		int xs[3], ys[3];
		int nx = tile_offsets(w, tw, xs);
		int ny = tile_offsets(h, th, ys);
		for (int yi = 0; yi < ny; yi++)
			for (int xi = 0; xi < nx; xi++)
				if (decode_region(q, pix, w, xs[xi], ys[yi], tw, th))
					return 1;
	}

	int hw = 0, hh = 0;
	uint8_t *half = downscale2(pix, w, h, &hw, &hh);
	if (half) {
		int ok = decode_region(q, half, hw, 0, 0, hw, hh);
		free(half);
		if (ok)
			return 1;
	}
	return 0;
}

/* One inverted full-frame pass.  Some codes are rendered light-on-dark (an
 * inverted display, a reversed print); quirc assumes dark-on-light and never
 * tries inversion itself, so a single 255-p pass covers them.  Kept to one
 * full-frame attempt — an inverted code that is also small/noisy is rare
 * enough not to warrant the full multi-pass strategy.  quirc already
 * normalises contrast/brightness via its adaptive threshold, so no
 * stretch/gamma pass is needed (measured: it decodes delta-10 contrast and
 * near-black frames unaided). */
static int decode_inverted(struct quirc *q, const uint8_t *pix, int w, int h)
{
	size_t n = (size_t)w * h;
	uint8_t *inv = malloc(n);
	if (!inv)
		return 0;
	for (size_t i = 0; i < n; i++)
		inv[i] = (uint8_t)(255 - pix[i]);
	int ok = decode_region(q, inv, w, 0, 0, w, h);
	free(inv);
	return ok;
}

/* Try the sharp image first; if nothing decodes, retry a light-denoised copy
 * (rescues noisy captures), then one inverted pass.  Returns 1 on first
 * decode. */
static int decode_image(struct quirc *q, const uint8_t *pix, int w, int h)
{
	if (decode_passes(q, pix, w, h))
		return 1;

	uint8_t *blur = box_blur3(pix, w, h);
	if (blur) {
		int ok = decode_passes(q, blur, w, h);
		free(blur);
		if (ok)
			return 1;
	}

	return decode_inverted(q, pix, w, h);
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1 && strcmp(argv[1], "-") != 0) ? argv[1] : NULL;
	FILE *f = path ? fopen(path, "rb") : stdin;
	if (!f) {
		fprintf(stderr, "qr_decode: cannot open %s\n", path);
		return 2;
	}

	unsigned w = 0, h = 0;
	uint8_t *pix = pgm_load(f, &w, &h);
	if (path)
		fclose(f);
	if (!pix)
		return 2;

	struct quirc *q = quirc_new();
	if (!q) {
		fprintf(stderr, "qr_decode: quirc_new failed\n");
		free(pix);
		return 2;
	}

	int rc = decode_image(q, pix, (int)w, (int)h) ? 0 : 1;

	quirc_destroy(q);
	free(pix);
	return rc;
}
