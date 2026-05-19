#ifndef STAR6E_EIS_EPTZ_H
#define STAR6E_EIS_EPTZ_H

/*
 * Thin wrapper over vendored libeptz.a (SigmaStar EPTZ library).
 * Produces a binary LDC config blob acceptable to
 * MI_VPE_LDCSetViewConfig from a small set of runtime parameters,
 * with no .cfg/.json input file dependency.
 */

/* Compile an LDC_MODE_1O (bypass / pass-through) view config for the
 * given input and output dimensions.  A loaded sensor calibration
 * poly buffer is required — libeptz uses it for the world-to-camera
 * transform.  *out_size receives the bin length on success.
 *
 * Returns a heap pointer owned by libeptz on success; caller MUST
 * release it via star6e_eis_eptz_bin_free() after the view config
 * push completes.  Returns NULL on any failure (logged to stderr). */
void *star6e_eis_eptz_compile_bypass(int in_w, int in_h,
	int out_w, int out_h,
	void *calib_buf, unsigned int calib_size,
	int *out_size);

/* Free the bin returned from star6e_eis_eptz_compile_bypass.
 * Safe to call with NULL. */
void star6e_eis_eptz_bin_free(void *bin);

#endif /* STAR6E_EIS_EPTZ_H */
