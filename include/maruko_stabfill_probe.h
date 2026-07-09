#ifndef MARUKO_STABFILL_PROBE_H
#define MARUKO_STABFILL_PROBE_H

#include <stdint.h>

/* Phase 5a go/no-go bench for Maruko (i6c) stab-fill.
 *
 * Star6E's stab-fill composes each frame in software and hands it to VENC via
 * MI_SYS_ChnInputPortGetBuf/PutBuf (an unbound VENC input port).  Maruko's VENC
 * is created with I6C_VENC_SRC_CONF_RING_DMA and fed ONLY by the SCL->VENC RING
 * bind — nothing pushes manually anywhere in the i6c sources.  The pivotal
 * question that decides whether stab-fill is portable to i6c: can an i6c VENC
 * channel be put in a non-RING (manual-push) src-conf and accept pushed frames?
 *
 * This probe creates a SEPARATE VENC channel in each candidate non-RING mode,
 * pushes a handful of hand-filled gray frames, and reports whether the channel
 * emits an encoded bitstream (Query.curPacks > 0 / GetStream succeeds).
 *
 * Env-gated: only runs when MARUKO_STABFILL_PROBE is set.  Called once after
 * channel-0 is up; tears its probe channel down cleanly (never SIGKILL).
 * chn0_attr is the live i6c_venc_chn used for channel 0, reused as the probe
 * channel's encode config (opaque void* to keep this header SDK-free). */
void maruko_stabfill_probe_run(int venc_dev, const void *chn0_attr,
	uint32_t enc_w, uint32_t enc_h);

#endif /* MARUKO_STABFILL_PROBE_H */
