#include "star6e_eis.h"
#include "star6e_eis_eptz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct Star6eEis {
	/* LDC view-config blob — owned by libeptz; freed via eptz API. */
	void *ldc_blob;
	size_t ldc_blob_size;
	/* Calibration polynomial — owned by us (plain malloc). */
	void *calib_blob;
	size_t calib_blob_size;
};

int star6e_eis_should_enable(const VencConfigEis *eis)
{
	return (eis && eis->enabled) ? 1 : 0;
}

static void *load_file(const char *path, size_t *out_size, const char *label)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "ERROR: [eis] %s open failed: '%s'\n", label, path);
		return NULL;
	}
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
		fprintf(stderr, "ERROR: [eis] %s stat failed or empty: '%s'\n",
			label, path);
		fclose(f);
		return NULL;
	}
	size_t sz = (size_t)st.st_size;
	void *buf = malloc(sz);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, sz, f) != sz) {
		fprintf(stderr, "ERROR: [eis] %s short read: '%s'\n", label, path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_size = sz;
	return buf;
}

Star6eEis *star6e_eis_attach(MI_VPE_ChannelAttr_t *attr,
	const VencConfigEis *eis)
{
	if (!attr || !star6e_eis_should_enable(eis))
		return NULL;

	if (!eis->calib_path[0]) {
		fprintf(stderr, "ERROR: [eis] calibPath empty\n");
		return NULL;
	}

	Star6eEis *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	/* Load calibration first — libeptz consumes it during compilation. */
	st->calib_blob = load_file(eis->calib_path, &st->calib_blob_size,
		"calibration");
	if (!st->calib_blob)
		goto fail;

	int compiled_size = 0;
	st->ldc_blob = star6e_eis_eptz_compile_bypass(
		(int)attr->capt.width, (int)attr->capt.height,
		(int)attr->capt.width, (int)attr->capt.height,
		st->calib_blob, (unsigned int)st->calib_blob_size,
		&compiled_size);
	if (!st->ldc_blob)
		goto fail;
	st->ldc_blob_size = (size_t)compiled_size;

	memset(&attr->lensInit, 0, sizeof(attr->lensInit));
	attr->lensInit.mode = LDC_WORKMODE_DIS_GYRO;
	attr->lensInit.bypassOn = 0;
	attr->lensInit.proj3x3On = 0;
	attr->lensInit.userSliceNum = eis->slice_count > 0 ? eis->slice_count : 1;
	attr->lensInit.focalLengthX = eis->focal_length_x;
	attr->lensInit.focalLengthY = eis->focal_length_y;
	attr->lensInit.configAddr = st->ldc_blob;
	attr->lensInit.configSize = (unsigned int)st->ldc_blob_size;
	attr->lensInit.mapType = LDC_MAPINFOTYPE_SENSORCALIB;
	attr->lensInit.calibInfo.calibPolyBinAddr = st->calib_blob;
	attr->lensInit.calibInfo.calibPolyBinSize =
		(unsigned int)st->calib_blob_size;
	/* The lensInit struct has its OWN lensAdjOn at the end of the
	 * struct, separate from the channel-attr-level one.  Without
	 * this the kernel module marks the view config unvalidated
	 * regardless of what the outer flag says. */
	attr->lensInit.lensAdjOn = 1;

	printf("[eis] Phase 3a: mode=DIS_GYRO ldcCfg=%zu B calib=%zu B "
	       "slices=%u focalX=%u focalY=%u\n",
		st->ldc_blob_size, st->calib_blob_size,
		(unsigned)attr->lensInit.userSliceNum,
		(unsigned)attr->lensInit.focalLengthX,
		(unsigned)attr->lensInit.focalLengthY);
	return st;

fail:
	star6e_eis_release(st);
	return NULL;
}

int star6e_eis_push_view_config(Star6eEis *st, int chn)
{
	if (!st || !st->ldc_blob)
		return -1;
	if (!g_mi_vpe.fnLDCBegViewConfig || !g_mi_vpe.fnLDCSetViewConfig ||
	    !g_mi_vpe.fnLDCEndViewConfig) {
		fprintf(stderr, "ERROR: [eis] libmi_vpe.so missing "
			"LDCBeg/Set/End ViewConfig symbols\n");
		return -1;
	}
	int rc = MI_VPE_LDCBegViewConfig(chn);
	if (rc != 0) {
		fprintf(stderr, "ERROR: [eis] LDCBegViewConfig(chn=%d) = %d\n",
			chn, rc);
		return -1;
	}
	rc = MI_VPE_LDCSetViewConfig(chn, st->ldc_blob,
		(unsigned int)st->ldc_blob_size);
	if (rc != 0) {
		fprintf(stderr, "ERROR: [eis] LDCSetViewConfig(chn=%d, "
			"size=%zu) = %d\n", chn, st->ldc_blob_size, rc);
		MI_VPE_LDCEndViewConfig(chn);
		return -1;
	}
	rc = MI_VPE_LDCEndViewConfig(chn);
	if (rc != 0) {
		fprintf(stderr, "ERROR: [eis] LDCEndViewConfig(chn=%d) = %d\n",
			chn, rc);
		return -1;
	}
	printf("[eis] Phase 3a: view config pushed (%zu B)\n",
		st->ldc_blob_size);
	return 0;
}

void star6e_eis_release(Star6eEis *st)
{
	if (!st)
		return;
	star6e_eis_eptz_bin_free(st->ldc_blob);
	free(st->calib_blob);
	free(st);
}
