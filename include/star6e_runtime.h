#ifndef STAR6E_RUNTIME_H
#define STAR6E_RUNTIME_H

#include "backend.h"

/* Comm name for the watchdog fork.  Must differ from "waybeam" —
 * that is what is_another_waybeam_running() in main.c matches
 * against, and a duplicate match would abort the watchdog with the
 * "already running" banner.  (The respawn child's comm name lives
 * in venc_respawn.c.) */
#define VENC_COMM_WATCHDOG  "waybeam-wd"

/* Comm name for the in-process-reinit deadline watchdog fork
 * (VENC_INPROC_REINIT experiment).  Distinct from "waybeam" for the
 * same /proc-walk reason as VENC_COMM_WATCHDOG. */
#define VENC_COMM_REINIT_WD "waybeam-rwd"

/** Return the Star6E backend operations table. */
const BackendOps *star6e_runtime_backend_ops(void);

#endif /* STAR6E_RUNTIME_H */
