#include "cv610_pq_bin_load.h"

#include <stdio.h>
#include <stdlib.h>

unsigned char *cv610_pq_bin_load(const char *path, size_t max_bytes,
	size_t *out_len)
{
	unsigned char *buf;
	long size;
	FILE *f;

	if (!path || !out_len)
		return NULL;

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "WARNING: cannot open %s\n", path);
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
		fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "WARNING: cannot size %s\n", path);
		fclose(f);
		return NULL;
	}
	if ((unsigned long)size > max_bytes) {
		fprintf(stderr, "WARNING: %s is %ld bytes, over the %zu byte cap\n",
			path, size, max_bytes);
		fclose(f);
		return NULL;
	}

	/* calloc, not malloc: the slack below is deliberate but must never hold
	 * stack contents if the vendor reader reaches into it. */
	buf = calloc(1, (size_t)size + CV610_PQ_NRX_VENDOR_MAX);
	if (!buf) {
		fprintf(stderr, "WARNING: cannot allocate %ld bytes for %s\n",
			size, path);
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "WARNING: short read on %s\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);

	*out_len = (size_t)size;
	return buf;
}
