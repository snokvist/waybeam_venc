# Goal — Retire the resilience reboot gate

## TL;DR

`process_restart_set_query()` in `src/venc_api.c:1819` currently refuses every
live `video0.resilience` change on both backends and asks for a reboot. The
gate was added under time pressure with empirical-but-narrow evidence:
*"two backends crashed under rapid SET cycling, so block all of it."*

The adversarial question: **is the resilience preset itself the problem, or
did we generalize from a rate-limit / teardown-order bug?**

Goal, in priority order:

1. **Live change** of `video0.resilience` with no pipeline interruption (best
   case). Probably not achievable for `ref_base/ref_enhance` — the MI VENC
   reference-pyramid is rebuilt only at channel creation — but is plausible
   for `intra_refresh_mode` and `gop_size` alone.
2. **Soft reinit** (in-process pipeline teardown + reconfigure, no exec) as
   the current Maruko path does for every other RESTART knob.
3. **SIGHUP-respawn** (fork+exec) on Maruko, matching Star6E. This is what
   Star6E already does for every *other* RESTART change and it works
   reliably — there is no architectural reason resilience should be the
   exception.

Any of these three is strictly better than the current "persist to disk,
return `reboot_required:true`, leave g_cfg untouched" behaviour.

## What the gate looks like today

`src/venc_api.c:1799-1870`:

```c
if (strcmp(g_cfg->video0.resilience, new_cfg.video0.resilience) != 0)
    resilience_change = 1;

if (resilience_change) {
    /* persist to disk only; do not commit g_cfg; do not call
     * venc_api_request_reinit(); return reboot_required:true */
}
```

The gate is **cross-backend**: same code path on Star6E and Maruko, same
"physical reboot required" message. That's the part that smells.

## Empirical evidence we *do* have

Both observations are from the 2026-05-15 bench session.

### Star6E (192.168.1.13)

- Single resilience SET via SIGHUP-respawn: **works**.
- Two consecutive resilience SETs back-to-back: **works**.
- Third+ resilience SET in a tight loop (no operator gate between):
  **stream wedged, ICMP dies, power-cycle required**.

The Star6E path is fork+exec — the SDK kernel state is reset by parent
process death (`star6e_runtime.c:692-705`). The 12-round cross-mode-sensor
SIGHUP-respawn test passed cleanly with no degradation. That same path
handles every other RESTART-tier change (sensor mode, encoder geometry,
codec params) without trouble.

So Star6E's failure is **rate-dependent, not change-content-dependent**.
The conclusion "resilience needs reboot" did not follow from this evidence.

### Maruko (192.168.2.12)

7-transition sweep across all resilience presets, in-process reinit:

- Transitions 1-6: clean.
- Transition 7 (range→fpv): kernel page fault in `MI_SYS_IMPL_FlushInputPortTasks`
  inside `[mi]`. waybeam ends in `State: Z (zombie)`. System stayed alive,
  reboot required to recover venc.

Kernel stack (extracted from `/proc/<pid>/stack` post-crash):
```
do_task_dead ← do_exit ← die ← __do_kernel_fault
  ← MI_SYS_IMPL_FlushInputPortTasks [mi]
```

This is fired from teardown — specifically one of the `MI_SYS_UnBindChnPort`
calls in `maruko_pipeline_teardown_graph()` (`src/maruko_pipeline.c:3274`).
Once the page fault hits, Maruko's process is dead but `mi` kmod state is
inconsistent — the only reliable recovery is `reboot`.

Maruko has **no fork+exec respawn path**. Every reinit is in-process. So a
Maruko crash under live resilience cycling tells us about the in-process
teardown order, not about the resilience preset.

## Adversarial hypotheses

H1. **The gate is overscoped on Star6E.**
    Star6E SIGHUP-respawn handles every other RESTART knob. The same path
    can handle resilience if we (a) rate-limit consecutive SETs, or
    (b) serialize through the existing `pipeline_lifetime_rwlock`. We
    never tried either before adding the gate.

H2. **The Maruko crash is a teardown-order bug, not a resilience bug.**
    Any in-process reinit on Maruko has *some* probability of tripping
    `MI_SYS_IMPL_FlushInputPortTasks`. We've just been lucky on the other
    RESTART knobs because they were tested less. The bench memory
    `venc_resilience_reboot_required.md` says "7-transition sweep proved
    6/7 worked + 7th kernel-page-faulted" — that's a 14% failure rate per
    cycle, which is *not* zero for the other RESTART knobs either; we
    haven't run 50 sensor-mode-switch cycles on Maruko to know.

H3. **`intra_refresh_mode` can be changed live.**
    `MI_VENC_SetH264IntraRefresh` / its H.265 equivalent is documented as
    runtime-settable in the MI SDK. `maruko_apply_intra_refresh()`
    (`src/maruko_pipeline.c:1122`) is called at pipeline_start but the
    underlying call takes (`MI_VENC_DEV`, `MI_VENC_CHN`, ir_struct) — no
    teardown needed. We have never tested calling it while VENC is
    actively encoding.

H4. **`gop_size` is already live-changeable.**
    The user reminded us mid-session that `gopSize` has been live-mutable
    "since the beginning". If `rescue`/`sprint` differ from `off`/`racing`
    *only* in `gop_size`, those four can be transitioned live with zero
    SDK risk — `intra_refresh_mode` stays the same.

H5. **`ref_base`/`ref_enhance` is the only field that genuinely needs a
    channel rebuild.** `MI_VENC_SetRefParam` (per
    `venc_refpred_silent_noop.md`) silently no-ops on Star6E for HEVC
    anyway, so the live-change path is "set the field, log it, do not
    rebuild" — semantically identical to the current behaviour without
    the gate, just honest about it.

## Concrete plan of attack (in order)

### Phase 0 — instrumentation (no behaviour change)

- Add a `[waybeam] resilience-change diff:` log line in
  `process_restart_set_query()` that prints the field-level delta
  (intra_refresh_mode, ref_base, ref_enhance, gop_size).
- Add a counter for in-process Maruko reinits with cause-of-restart
  (`debug` / `RESTART knob X`). Currently we don't even know how often
  the *other* RESTART knobs trip the same teardown path on Maruko.

### Phase 1 — minimal-change live transitions

Field-level live-change matrix:

| Field                | Live API call                              | Plausible? |
|----------------------|--------------------------------------------|------------|
| `gop_size`           | already wired (debug knob)                 | yes        |
| `intra_refresh_mode` | `MI_VENC_SetH265IntraRefresh()` mid-stream | likely     |
| `ref_base`/`enhance` | none — channel rebuild                     | no         |
| `ref_pred`           | `MI_VENC_SetRefParam` (no-op on Star6E)    | trivial    |

Implementation: split `process_restart_set_query` into two paths.

1. **Live-safe delta** (only `gop_size` and/or `intra_refresh_mode`
   changed): apply via existing live-update hooks, no reinit.
2. **Full reconfigure** (any `ref_*` changed): fall through to the
   normal RESTART path (in-process reinit on Maruko, SIGHUP-respawn on
   Star6E).

A direct preset rename like `racing → endurance` only flips
`intra_refresh_mode` (fast → balanced) — that should be a live change,
not a reboot.

### Phase 2 — Maruko fork+exec respawn parity

Mirror `star6e_runtime_respawn_after_exit()` for Maruko:

- After `maruko_pipeline_teardown()` returns cleanly, fork; parent exits,
  child waits for parent-PID death (same `kill(parent_pid, 0) == ESRCH`
  pattern with a long timeout), then `execv` a fresh venc.
- This eliminates the entire `MI_SYS_IMPL_FlushInputPortTasks` page-fault
  surface for *any* RESTART knob, not just resilience.

This is independently valuable — even if we can never live-change
resilience, this turns the Maruko reboot-required path into a
self-healing 2-second process replacement.

### Phase 3 — drop the gate

Once Phase 1 + Phase 2 land:

- `gop_size`-only changes apply live (already true today, just unblocked).
- `intra_refresh_mode`-only changes apply live.
- `ref_*` changes trigger SIGHUP-respawn on both backends.
- The "reboot_required" sentinel in `make_single_set_reboot_required_json`
  is deleted along with its callsite.

Validation: 100-cycle automated sweep cycling through all 10 resilience
presets on both bench devices, with a 250 ms gate between SETs to start
(then tightened to zero once the SIGHUP-respawn path is rate-limit-clean).

## Don't-do list

- **Don't** rip out the gate before Phase 1 lands — the empirical bench
  evidence is "the current code crashed both backends", and that has not
  been disproven yet, only re-framed.
- **Don't** skip Phase 2. Without Maruko fork+exec parity, in-process
  reinit on Maruko remains the single biggest stability liability in the
  project, and live resilience changes would only make it worse.
- **Don't** rely on `MI_VENC_SetRefParam` doing anything observable on
  Star6E HEVC — it silently no-ops per `venc_refpred_silent_noop.md`.

## Files in scope

Primary:
- `src/venc_api.c` — gate sits in `process_restart_set_query` (1799-1870)
- `src/venc_config.c` — resilience preset table (`apply_resilience_preset`)
- `src/maruko_runtime.c` — in-process reinit loop
- `src/maruko_pipeline.c` — teardown order, intra_refresh apply
- `src/star6e_runtime.c` — SIGHUP-respawn template to mirror for Maruko

Reference / memory:
- `venc_resilience_reboot_required.md` (cross-backend bench evidence)
- `venc_star6e_reinit_fragility.md` (Star6E rate-cycling failure)
- `venc_refpred_silent_noop.md` (refPred no-op on Star6E HEVC)
- `feedback_no_chained_resilience_sets.md` (manual-gate rule)

## Success criteria

1. A direct `racing → endurance` SET applies live with zero observable
   video glitch on the decoder side.
2. A direct `racing → fpv` SET (forces ref_pred change) triggers a
   SIGHUP-respawn on Maruko, completes in under 3 s, and is invisible to
   the operator beyond a brief stream pause.
3. 100-cycle automated preset sweep on both 192.168.1.13 (Star6E) and
   192.168.2.12 (Maruko) finishes with the daemon still alive, ICMP
   green throughout, no kernel-fault dmesg lines.
4. The `reboot_required` JSON response is no longer reachable from any
   user-visible SET path.
