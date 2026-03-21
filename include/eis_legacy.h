#ifndef EIS_LEGACY_H
#define EIS_LEGACY_H

/*
 * EIS legacy backend — original LPF-based crop-window stabilization.
 *
 * This is the former eis_crop.c wrapped in the EisOps vtable interface.
 * Algorithm unchanged: integrate gyro → LPF smooth path → correction =
 * smooth - raw → pixel offset → MI_VPE_SetPortCrop().
 */

#include "eis.h"

/*
 * eis_legacy_create — Allocate and initialize the legacy EIS backend.
 * Called by eis_create() when cfg->mode is "legacy" (or default).
 * Returns NULL on failure.
 */
EisState *eis_legacy_create(const EisConfig *cfg);

#endif /* EIS_LEGACY_H */
