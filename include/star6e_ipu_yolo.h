#ifndef STAR6E_IPU_YOLO_H
#define STAR6E_IPU_YOLO_H

/*
 * Star6E IPU YOLO detection — host side.
 *
 * Adds a VPE port1 tap (NV12, model input size) feeding a detector backend
 * (see detect_plugin.h) on a dedicated reader thread.  The backend owns the
 * IPU device + decode; in Phase A the "worker" backend also draws its own
 * MI_RGN overlay, so enabling detection puts boxes on the encoded stream with
 * no host-side overlay work.
 *
 * Port1 is a single physical VPE output shared with the stab framing tap, so
 * detection and framing=stab are mutually exclusive — the caller must only
 * start detection when no port1-owning framing module is active.
 */

#include "star6e_pipeline.h"   /* Star6ePipelineState */
#include "venc_config.h"       /* VencConfig */

/* Start the detector: resolve + init the configured backend, create the
 * port1 tap, and launch the reader thread.  No-op returning 0 when
 * detect.enabled is false.  On any failure the tap is torn down and the
 * pipeline continues without detection (returns 0 — detection is best-effort,
 * never fatal to the stream).  state->detect is set when active. */
int star6e_ipu_yolo_start(Star6ePipelineState *state, const VencConfig *vcfg);

/* Stop the detector: join the reader (before DisablePort — MMU-safe), deinit
 * the backend, disable port1, free the context.  Safe when inactive. */
void star6e_ipu_yolo_stop(Star6ePipelineState *state);

#endif /* STAR6E_IPU_YOLO_H */
