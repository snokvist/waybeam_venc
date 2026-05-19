#ifndef STAR6E_EIS_H
#define STAR6E_EIS_H

/*
 * Star6E gyro-assisted EIS via SigmaStar VPE LDC engine.
 *
 * Phase 3a (this header): load a precompiled LDC view-config blob +
 *   sensor calibration polynomial, populate channel_attr.lensInit
 *   with WORKMODE_DIS_GYRO, and let the kernel engine warp frames
 *   using its internal identity matrix.  No gyro feed yet.
 *
 * Phase 3b — gyro→3×3 matrix loop                          (TBD)
 * Phase 3c — rolling-shutter slicing + spring recentering   (TBD)
 */

#include "venc_config.h"
#include "star6e.h"

/* SigmaStar LDC mode values.  Defined locally so we don't pull in
 * SDK headers that aren't already in-tree. */
typedef enum {
	LDC_WORKMODE_LDC      = 0x01,
	LDC_WORKMODE_LUT      = 0x02,
	LDC_WORKMODE_DIS_GYRO = 0x04,
} LdcWorkMode;

typedef enum {
	LDC_MAPINFOTYPE_DISPMAP     = 0,
	LDC_MAPINFOTYPE_SENSORCALIB = 1,
} LdcMapInfoType;

/* Opaque state owning the blob buffers that lensInit points into. */
typedef struct Star6eEis Star6eEis;

/* True if EIS is enabled in config.  Single source of truth so
 * callers don't have to know whether `eis` may be NULL. */
int star6e_eis_should_enable(const VencConfigEis *eis);

/* Load LDC config + calibration blobs into a new Star6eEis context
 * and populate attr->lensInit.  attr->lensAdjOn is set by the caller.
 *
 * The blob pointers in attr->lensInit reference memory owned by the
 * returned context — keep it alive until after MI_VPE_CreateChannel
 * returns (the SDK copies the blobs internally during CreateChannel).
 *
 * Returns a Star6eEis* on success, NULL on any I/O or validation
 * failure (with stderr error logged).  When NULL, the caller MUST
 * leave lensInit zero-initialised and lensAdjOn=0. */
Star6eEis *star6e_eis_attach(MI_VPE_ChannelAttr_t *attr,
	const VencConfigEis *eis);

/* Push the loaded LDC blob to the kernel via the view-config family.
 * Must be called AFTER MI_VPE_CreateChannel + MI_VPE_SetChannelParam,
 * BEFORE MI_VPE_StartChannel.  Without this, SCL queries to the LDC
 * engine return "view config not validated" and the pipeline stalls.
 *
 * Returns 0 on success, -1 on push failure or missing vtable fn ptrs. */
int star6e_eis_push_view_config(Star6eEis *eis, int chn);

/* Free blob buffers.  Safe to call with NULL.  Call AFTER
 * MI_VPE_CreateChannel + view-config push — earlier calls would
 * dangle the pointers inside lensInit / the kernel's view state. */
void star6e_eis_release(Star6eEis *eis);

#endif /* STAR6E_EIS_H */
