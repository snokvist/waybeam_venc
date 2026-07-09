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

/* Phase F0a go/no-go bench for the module-bind stab-fill path.
 *
 * Phase 5a proved a direct manual push to the i6c H.265 VENC does NOT encode:
 * the VENC is bind-fed by design (SDK-confirmed; UVC injects to SCL, then
 * frame-base-binds SCL->VENC).  F0a stands up that exact topology on a SECOND
 * SCL channel (dev 0, chn 1) with no upstream bind: create + start the SCL
 * channel, frame-base-bind SCL(0,1,0)->VENC(new chn, NORMAL_FRMBASE), then
 * inject hand-composed frames into the SCL input port
 * (MI_SYS_ChnInputPortGetBuf/PutBuf, the SDK's PutStreamToSclInputPort path)
 * and confirm VENC emits.  Also times the per-frame compose+push cost on the
 * single A7 (the pivotal budget question at 50 fps).
 *
 * Env-gated: only runs when MARUKO_STABFILL_F0A is set.  Called once after
 * channel-0 is up; tears its SCL+VENC bridge down cleanly (never SIGKILL).
 * chn0_attr is the live i6c_venc_chn reused as the bridge channel's config. */
void maruko_stabfill_f0a_run(int venc_dev, const void *chn0_attr,
	uint32_t enc_w, uint32_t enc_h);

#endif /* MARUKO_STABFILL_PROBE_H */
