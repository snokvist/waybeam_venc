/* qr_decode.c — decode a QR code from a binary P5 PGM (grayscale) image.
 *
 * Reads a P5 PGM from a file argument (or stdin when the argument is "-" or
 * absent), runs quirc over the luma pixels, and prints the payload of the
 * first successfully decoded QR code to stdout.
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
	if (!q || quirc_resize(q, (int)w, (int)h) != 0) {
		fprintf(stderr, "qr_decode: quirc init failed\n");
		free(pix);
		if (q) quirc_destroy(q);
		return 2;
	}

	int bw = 0, bh = 0;
	uint8_t *buf = quirc_begin(q, &bw, &bh);
	memcpy(buf, pix, (size_t)w * h);
	quirc_end(q);
	free(pix);

	int found = 0, rc = 1;
	int count = quirc_count(q);
	for (int i = 0; i < count && !found; i++) {
		struct quirc_code code;
		struct quirc_data data;

		quirc_extract(q, i, &code);
		if (quirc_decode(&code, &data) != QUIRC_SUCCESS)
			continue;
		/* Payload may be binary; write exactly payload_len bytes. */
		fwrite(data.payload, 1, (size_t)data.payload_len, stdout);
		fputc('\n', stdout);
		found = 1;
		rc = 0;
	}

	quirc_destroy(q);
	return rc;
}
