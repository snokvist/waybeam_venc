/*
 * star6e_eis_eptz.c — programmatic LDC view-config blob compiler.
 *
 * Uses the vendored libeptz.a (SigmaStar EPTZ library) to produce a
 * binary LDC config that MI_VPE_LDCSetViewConfig will accept.  No
 * .cfg / .json input file is required — the config struct is built
 * in-memory from the runtime parameters.
 *
 * Caller flow:
 *   bin = star6e_eis_eptz_compile_bypass(in_w, in_h, out_w, out_h, &sz);
 *   MI_VPE_LDCSetViewConfig(chn, bin, sz);
 *   star6e_eis_eptz_bin_free(bin);
 */

#include "star6e_eis_eptz.h"
#include "mi_eptz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *star6e_eis_eptz_compile_bypass(int in_w, int in_h, int out_w, int out_h,
	void *calib_buf, unsigned int calib_size, int *out_size)
{
	if (!out_size || in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0)
		return NULL;
	if (!calib_buf || calib_size == 0) {
		fprintf(stderr, "ERROR: [eptz] calibration buffer required\n");
		return NULL;
	}

	mi_eptz_config_param cfg;
	memset(&cfg, 0, sizeof(cfg));
	/* Documented "happy path": parse a tiny .cfg file giving the
	 * canonical eptz arg layout for LDC_MODE_1O.  Calibration poly
	 * is read by libeptz from <PATH_IN_FOLDER>/Cali_LDCpoly.bin —
	 * star6e_eis.c stages calib.bin there under that name. */
	char cfg_path[] = "/tmp/star6e_eis_bypass.cfg";
	FILE *cf = fopen(cfg_path, "w");
	if (!cf) {
		fprintf(stderr, "ERROR: [eptz] cannot create %s\n", cfg_path);
		return NULL;
	}
	fprintf(cf,
		"-i /etc/waybeam/eis -o /tmp -b /tmp/eptz_bypass.bin "
		"-g unused.jpg -m %d -s %d %d %d %d -c %d %d %d -d 16\n",
		LDC_MODE_1O, in_w, in_h, out_w, out_h,
		in_w / 2, in_h / 2, in_w);
	fclose(cf);

	mi_eptz_err parse_err = mi_eptz_config_parse(cfg_path, &cfg);
	if (parse_err != MI_EPTZ_ERR_NONE) {
		fprintf(stderr, "ERROR: [eptz] config_parse err=0x%x\n", parse_err);
		return NULL;
	}
	/* Override sizes; the .cfg-driven defaults may not match.  These
	 * fields are marked "DO NOT EDIT" but they're what the demos
	 * actually populate from the cmdline. */
	cfg.in_width        = in_w;
	cfg.in_height       = in_h;
	cfg.out_width       = out_w;
	cfg.out_height      = out_h;
	cfg.out_width_tile  = out_w;
	cfg.out_height_tile = out_h;
	cfg.grid_size       = 16;
	/* calib_type=1 = single poly bin (our CalibPoly_new.bin),
	 * vs default 0 which expects rd.bin + ru.bin + a third file. */
	cfg.calib_type      = 1;

	int wb_size = mi_eptz_get_buffer_info(&cfg);
	if (wb_size <= 0) {
		fprintf(stderr, "ERROR: [eptz] get_buffer_info returned %d\n",
			wb_size);
		return NULL;
	}
	printf("[eptz] working buffer: %d B\n", wb_size);

	unsigned char *wb = malloc((size_t)wb_size);
	if (!wb) {
		fprintf(stderr, "ERROR: [eptz] working buffer alloc %d B failed\n",
			wb_size);
		return NULL;
	}

	/* Identity 3x3 perspective matrix.  Many libeptz code paths
	 * dereference para.m33 unconditionally; leaving it NULL crashes
	 * inside runtime_bin_gen.  Static lifetime is fine — libeptz
	 * just reads from it. */
	static float identity_m33[9] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f
	};

	mi_eptz_para para;
	memset(&para, 0, sizeof(para));
	para.ptconfig_para = &cfg;
	para.view_index = 0;
	para.zoom = 1.0f;
	para.zoom_h = 1.0f;
	para.zoom_v = 1.0f;
	para.m33 = identity_m33;

	EPTZ_DEV_HANDLE h = mi_eptz_runtime_init(wb, wb_size, &para);
	if (!h) {
		fprintf(stderr, "ERROR: [eptz] runtime_init failed\n");
		free(wb);
		return NULL;
	}

	int bin_size = 0;
	LDC_BIN_HANDLE bin = NULL;
	mi_eptz_err err = mi_eptz_runtime_map_gen(h, &para, &bin, &bin_size);
	if (err != MI_EPTZ_ERR_NONE || !bin || bin_size <= 0) {
		fprintf(stderr, "ERROR: [eptz] runtime_map_gen err=0x%x\n", err);
		free(wb);
		return NULL;
	}

	/* Working buffer no longer needed; the runtime copies what it
	 * needs into the returned bin handle. */
	free(wb);

	*out_size = bin_size;
	printf("[eptz] compiled LDC bypass: in=%dx%d out=%dx%d bin=%d B\n",
		in_w, in_h, out_w, out_h, bin_size);
	return bin;
}

void star6e_eis_eptz_bin_free(void *bin)
{
	if (!bin)
		return;
	(void)mi_eptz_buffer_free(bin);
}
