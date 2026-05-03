/*
 * Pipeline-lifetime guard.
 *
 * Synchronises the HTTP worker thread (venc_httpd) with the runner
 * thread during pipeline teardown / re-init / shutdown windows.
 *
 * Without this guard, every `apply_*` and `query_*` callback registered
 * through VencApplyCallbacks dereferences vendor SDK handles (VENC
 * channels, ISP/SCL/VPE channels, audio handles) that the runner thread
 * destroys and recreates around `MI_*_DestroyChannel` / `MI_*_CreateChannel`
 * pairs — racing on flags such as `g_mi_isp_chn_created` and on the
 * static control contexts (`g_star6e_control_ctx`, `g_ctx`).
 *
 * Lock model: a single rwlock owned by venc_api.
 *   - HTTP side: take the rdlock around any callback dispatch.  Multiple
 *     concurrent reads are allowed in principle (the httpd is single-
 *     threaded today, so contention is low).
 *   - Runner side: take the wrlock around any teardown/re-init window
 *     while the httpd is still up.  Includes:
 *       * Maruko in-process reinit (teardown_graph + reinit_pipeline).
 *       * Star6E shutdown teardown (controls_reset + pipeline_stop)
 *         until venc_httpd_stop() returns.
 *
 * The rwlock guarantees that an in-flight HTTP callback either runs
 * fully against a live pipeline or is not called at all — once the
 * runner holds the wrlock, every new HTTP dispatch attempt blocks until
 * the runner releases it.
 *
 * Lock ordering (avoid deadlock):
 *   pipeline_lifetime_rdlock() OUTSIDE g_cfg_mutex.
 *   The runner never touches g_cfg under wrlock, so there is no inversion.
 *
 * No init/destroy required: backed by a static
 * PTHREAD_RWLOCK_INITIALIZER.  Helpers may be called from any thread at
 * any point in the process lifetime, including before main().
 */

#ifndef PIPELINE_LIFETIME_H
#define PIPELINE_LIFETIME_H

#ifdef __cplusplus
extern "C" {
#endif

void pipeline_lifetime_rdlock(void);
void pipeline_lifetime_rdunlock(void);

void pipeline_lifetime_wrlock(void);
void pipeline_lifetime_wrunlock(void);

#ifdef __cplusplus
}
#endif

#endif /* PIPELINE_LIFETIME_H */
