#ifndef MARUKO_FRAMING_H
#define MARUKO_FRAMING_H

/* Framing-module registry (Maruko/i6c) — the parallel of include/star6e_framing.h.
 * A "framing" module owns the SCL port0 → VENC path for one video0.framing
 * preset (e.g. "stab").  Modules are statically linked and self-register; the
 * pipeline selects one per run by preset name and drives it through this
 * vtable.  Removing a module = drop its source file from the build (the STAB
 * flag in the Makefile).  The only structural difference from the Star6E vtable
 * is setup_ports()'s first arg — MarukoBackendContext* vs Star6ePipelineState*
 * (the SCL/VPE handles differ per SoC). */

#include <stdint.h>
#include "venc_config.h"
#include "maruko_pipeline.h"

typedef struct MarukoFramingModule {
	const char *preset_name;            /* matched against video0.framing */

	int  (*enabled)(const VencConfig *);

	/* pipeline_start, before VENC create: compute geometry from vcfg + the
	 * source dims; return the encode dims the pipeline sizes VENC to. */
	int  (*prepare)(const VencConfig *vcfg, uint32_t src_w, uint32_t src_h,
	                uint32_t *enc_w, uint32_t *enc_h);

	/* after VENC create: enable the detector tap port + stash ctx. */
	int  (*setup_ports)(MarukoBackendContext *ctx, uint32_t src_fps,
	                    uint32_t dst_fps);
	int  (*start)(void);
	void (*stop)(void);
	void (*apply_ae_crop)(void);

	void (*set_pan)(double x, double y);              /* optional (NULL ok) */
	int  (*active)(void);                             /* detector running? */
	int  (*set_live)(const char *key, const char *val); /* optional live tuning */
} MarukoFramingModule;

/* Append a module to the registry (called from register_builtins). */
void maruko_framing_register(const MarukoFramingModule *m);

/* Return the registered module whose enabled() matches vcfg, else NULL. */
const MarukoFramingModule *maruko_framing_select(const VencConfig *vcfg);

/* Register all compiled-in framing modules.  Called once at pipeline init. */
void maruko_framing_register_builtins(void);

/* Debug-OSD snapshot from the stab/stab-fill module: last detector measurement
 * + Kalman correction (stab pixels), pause state, and whether the fill preset
 * is active.  Returns 1 when a stab thread is running, else 0 (hide the row).
 * Defined in maruko_framing_stab.c — only call under HAVE_FRAMING_STAB. */
int maruko_framing_stab_osd_status(int *acc_x, int *acc_y,
	int *meas_x, int *meas_y, int *paused, int *fill);

#endif /* MARUKO_FRAMING_H */
