#ifndef STAR6E_FRAMING_H
#define STAR6E_FRAMING_H

/* Framing-module registry.  A "framing" module owns the VPE port0 → VENC path
 * for one video0.framing preset (e.g. "stab").  Modules are statically linked
 * and self-register; the pipeline selects one per run by preset name and
 * drives it through this vtable.  Removing a module = drop its source file
 * from the build (see the STAB flag in the Makefile). */

#include <stdint.h>
#include "venc_config.h"
#include "star6e_pipeline.h"

typedef struct FramingModule {
	const char *preset_name;            /* matched against video0.framing */

	int  (*enabled)(const VencConfig *);

	/* pipeline_start, before VENC create: compute geometry from vcfg + the
	 * source dims; return the encode dims the pipeline sizes VENC to. */
	int  (*prepare)(const VencConfig *vcfg, uint32_t src_w, uint32_t src_h,
	                uint32_t *enc_w, uint32_t *enc_h);

	/* bind_and_finalize, after VENC create: bind the VPE ports. */
	int  (*setup_ports)(Star6ePipelineState *state, uint32_t src_fps,
	                    uint32_t dst_fps);
	int  (*start)(void);
	void (*stop)(void);
	void (*apply_ae_crop)(void);

	void (*set_pan)(double x, double y);              /* optional (NULL ok) */
	int  (*active)(void);                             /* detector running? */
	int  (*set_live)(const char *key, const char *val); /* optional live tuning */

	/* Optional (NULL ok).  For manual-compose presets whose OSD sits on the
	 * pre-compose source (stab-fill): return 1 and the live encode-domain
	 * offset the compose applies, so the OSD draw cycle can counter-shift the
	 * panel and keep it screen-fixed.  Return 0 (or be NULL) when the OSD is
	 * already 1:1 with the encoded frame (HW-crop "stab", zoom, off). */
	int  (*osd_anchor)(int *off_x, int *off_y);
} FramingModule;

/* Append a module to the registry (called from register_builtins). */
void star6e_framing_register(const FramingModule *m);

/* Return the registered module whose enabled() matches vcfg, else NULL. */
const FramingModule *star6e_framing_select(const VencConfig *vcfg);

/* Register all compiled-in framing modules.  Called once at pipeline init. */
void star6e_framing_register_builtins(void);

#endif /* STAR6E_FRAMING_H */
