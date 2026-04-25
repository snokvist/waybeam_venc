#ifndef PIP_COMPOSITOR_H
#define PIP_COMPOSITOR_H

#include "venc_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Picture-in-Picture compositor (Star6E, grayscale I8 path).
 *
 * Hardware path validated by tools/pip_probe.c + src/pip_probe_inproc.c
 * (probes 1–4):
 *   - VPE port-1 with crop produces a zoomed YUV420SP frame
 *   - MI_DIVP_StretchBuf writes directly into an MI_RGN I8 canvas
 *     attached to VPE port-0 channel (Y plane → canvas, UV plane →
 *     small discard scratch)
 *   - kernel composites the canvas onto port-0 frames at output
 *   - per-frame cost ≈ 0.5 ms HW + ~0% CPU
 *
 * Colour PiP is NOT supported in v1.  ARGB888 RGN regions are rejected
 * by the kernel on this BSP, and the ARGB8888→ARGB4444 downconvert
 * path costs CPU.  pip.format="grayscale" is the only supported value;
 * pip.format="color" is rejected at compositor_create time.
 *
 * Palette caveat: I8 mode requires a grayscale palette, which the
 * compositor installs via MI_RGN_Init.  This overrides the 16-entry
 * named-colour palette that debug_osd installs — debug OSD overlays
 * (text, outline rects) will then render as monochrome.  In practice
 * the user keeps debug.showOsd off in production; for engineering the
 * monochrome overlays remain legible.  Documented behaviour, not a bug.
 *
 * Threading: the compositor owns one pthread that drains VPE port-1
 * frames, runs DIVP, calls UpdateCanvas.  The thread is started by
 * pip_compositor_start and stopped (joinable) by pip_compositor_stop.
 *
 * Live updates: pip.zoom.* and pip.position.{x,y} can change via
 * pip_compositor_apply_zoom and apply_position without a restart.
 * pip.position.{w,h} require RGN re-create and are restart-only.
 * pip.refreshEvery is read by the compositor each iteration. */

typedef struct PipCompositor PipCompositor;

/* Create the compositor: dlopens DIVP/SYS/RGN, sets up VPE port-1
 * (SetPortMode + SetPortCrop + SetChnOutputPortDepth + EnablePort),
 * creates I8 RGN region, allocates UV scratch, installs grayscale
 * palette, pre-warms DIVP.  Does NOT start the thread.
 *
 * Returns NULL on failure (logs the specific error to stderr).
 * Returns NULL also when pip.enabled=false or pip.format != "grayscale". */
PipCompositor *pip_compositor_create(const VencConfig *vcfg);

/* Tear down: stop thread (if running), destroy RGN region, free
 * scratch, disable VPE port-1, dlclose libs.  Idempotent. */
void pip_compositor_destroy(PipCompositor *c);

/* Start the compositor thread.  Returns 0 on success. */
int pip_compositor_start(PipCompositor *c);

/* Stop the compositor thread (joinable).  Idempotent. */
void pip_compositor_stop(PipCompositor *c);

/* Live update: zoom rect (calls MI_VPE_SetPortCrop on port-1).
 * Returns 0 on success. */
int pip_compositor_apply_zoom(PipCompositor *c, uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h);

/* Live update: position x/y (calls MI_RGN_SetDisplayAttr).  Returns
 * 0 on success.  Position w/h are not live (RGN re-create needed). */
int pip_compositor_apply_position(PipCompositor *c, uint16_t x, uint16_t y);

/* Live toggle: enable/disable compositor blitting.  When false, the
 * thread keeps running but skips DIVP+UpdateCanvas (RGN canvas keeps
 * its last frame; CPU drops to ~0).  Returns 0 on success. */
int pip_compositor_apply_enabled(PipCompositor *c, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* PIP_COMPOSITOR_H */
