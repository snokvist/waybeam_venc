#ifndef MARUKO_STABFILL_PROBE_H
#define MARUKO_STABFILL_PROBE_H

#include <stdint.h>

/* Phase 5a go/no-go bench for Maruko (i6c) stab-fill.
 *
 * VERDICT HISTORY: the original 5a run concluded "the i6c VENC does not encode
 * manual pushes".  That device result was an ABI ARTIFACT — the probe's
 * BufConf used eBufType=0 (BUFDATA_RAW on i6c, not FRAME) and a Star6E-shaped
 * union offset, so every push was silently degenerate.  With the corrected
 * i6c MI_SYS_BufConf_t (now fixed in this probe too) the direct manual push
 * DOES encode at the full sensor rate — it is the shipped stab-fill feed path
 * (see src/maruko_framing_stab.c fill mode).  Also note: only VENC channels
 * 0..2 exist (MI_VENC_MAX_CHN_NUM_PER_DC=3), and a sibling frame-base channel
 * cannot coexist with a RING-fed chn 0 on the single H26x device (SYS/BUSY),
 * so this sibling-channel probe understates what the real single-channel
 * frame-base graph can do.
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
