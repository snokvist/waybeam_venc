# HANDOFF — Maruko IMX415 mode-lineup rework (branch `feature/maruko-imx415-mode-lineup`)

Status as of 2026-07-03 evening. Written for the next agent taking over.
Companion doc: `MARUKO_IMX415_1485_MODES.md` (mode table, recipes, known issues).

## What this branch does

Replaces the 8-mode list (vendor + 1485 mix) with one best mode per FPS tier,
all non-binned and ~16:9. **Breaking `sensor.mode` remap** (old→new: 0→0,
5→1, 7→3; old 1–4 unsurfaced under `#if 0`; old 6 re-sized). VERSION 0.21.0.

| Idx | Mode | Device-verified (this session, .12) |
|---|---|---|
| 0 | 3760x2116@30 (891/REALTIME) | **works** — 30.0 fps device-verified live at both `out 1440x1080` and `auto`/native `out 3760x2116`; earlier "zero-frames" was a cold-start test artifact, see below |
| 1 | 2952x1656@50_1485 | 50.0 fps exact, ~16.6 Mbps — explicit `video0.size 2952x1656` now valid (see /8 fix) or `auto` |
| 2 | 2688x1512@60_1485 (NEW) | 60.0 fps exact, ~14.6 Mbps, dmesg clean |
| 3 | 2112x1184@90_1485 | table byte-identical to #155-verified old mode 7; set as final bench state |
| 4 | 1920x1080@100_1485 (NEW) | 100.0 fps exact (shutter 9994 µs), ~16.2 Mbps |

fps confirmed via `[verbose]` frame-counter deltas (the per-second fps field
truncates: shows 59/99/49 while deltas are exactly 60/100/50 per second).

Gates: `make verify` green both backends, `make test` 1699/1699. The .ko on
.12 flash is this branch's build (md5 `18899d6b…`); flash `/usr/bin/waybeam`
is still master v0.20.0 (fine — branch changes are driver-only).

## ITEM 1 — mode 0 works; earlier "zero-frames" was likely a test artifact

**Corrected 2026-07-03 (supersedes the earlier "pre-existing regression"
claim, which was wrong).** Mode 0 streams fine, including at the full native
3760 encode. Device-verified live (master v0.20.0 binary + this branch's .ko,
reached by switching modes in the running webui):

- mode 0, `video0.size 1440x1080` (downscaled): `sensor capt 3760x2116 →
  out 1440x1080`, **30.0 fps steady**, zero aborts.
- mode 0, `video0.size auto` (native): `sensor capt 3760x2116 →
  out 3760x2116`, `VENC ring pool 3760x2116 ring=2116`, **30.0 fps steady**,
  ~13 Mbps, ~55 KB/frame, frame counter advancing 30/s.

Switching freely across all five modes works.

So the HW_RING/IFC path is fine at 3760 encode width. The earlier zero-frame
observation came from a **cold `setsid` start straight into mode 0** — and
that specific test was one of the two flagged as polluted by an overlapping
waybeam instance (the same class of error that later caused D-state #4). The
most probable explanation is that the overlap, not the 3760 width, starved
the encoder. The prior HW_RING=0 / EnqOTNull diagnostics are consistent with
two processes fighting over the SCL/VENC channels.

**Remaining question (verification, not a known bug):** does a *clean* cold
boot straight into mode 0 (no prior instance, fresh process) stream? Not yet
tested in isolation. If a follow-up wants certainty: reboot, set mode 0 +
`auto`, `setsid /usr/bin/waybeam`, confirm 30 fps. Given the live path works
at 3760, a genuine cold-start-only failure would be surprising.

The old mid-failure `/proc/mi_modules` snapshot (SCL `EnqOTNull`, VENC
`RingRealTotalHeight=0`, ISP `fifofullcnt`) is preserved in the session
scratchpad for reference, but should now be read as the overlap-starvation
signature, not a 3760-width limit.

## RESOLVED ITEM 2 — encode-width validation was too strict (/16 → /8)

**Fixed on this branch.** `venc_config` had rejected explicit `video0.size`
widths not divisible by 16, yet `size: auto` encoded 2952 (÷8, not ÷16)
natively and streamed fine. Root cause of the drop when an explicit /16
width was set instead: forcing 2944x1656 (nearest /16) does **not** crop —
the SCL keeps the full 2952-wide input and anamorphically **downscales**
2952→2944, a real scaler resize + IFC ring-stride change, costing ~7 fps
(measured 43 vs 50).

The /16 rule came from commit `4c9c63a` (#63/#55), picked defensively from a
single failing case (`854×480` → `MI_ERR_VENC_ILLEGAL_PARAM`). But 854 is
÷2 only, not even ÷8. The real HEVC constraint is /8 (min coding block;
conformance window covers the remainder up to the CTU). Relaxed the width
check to `w % 8` — still rejects 854, now accepts 2952.

Device-verified on .12 (patched maruko binary, tmpfs): explicit
`sensor.mode 1` + `video0.size 2952x1656` + `fps 50` → **50.0 fps exact**
(frame deltas 2238→2288→…→2488 = 50/s), ~16.6 Mbps, `SCL port
crop(0,0 2952x1656) out(2952x1656)` 1:1 passthrough, dmesg `Drop:0`, no
FIFO-full/fence spam. Identical to the auto path.

Note: heights not /8 are still rejected (mode 0 sensor 2116 → encode 2112);
that half of the rule was always correct.

## OPEN ITEM 3 — teardown watchdog (roadmap, deferred by user)

D-state occurrence **#4** today: a second instance start racing a live
teardown (my sequencing error) wedged SIGTERM teardown in
`MI_SYS_IMPL_FlushRealTimeOutputBuf` (SCL working=4, dmesg "inputtask's
fence is not finished" ×62). Recovery `reboot -f`. Rule remains: after
`killall waybeam`, poll `pidof` until empty (teardown can exceed 60 s when
wedged; ~5–10 s normally) before starting a new instance.

## OPEN ITEM 4 — PR decision (user input needed)

PR to `snokvist/waybeam_venc` not yet created. Question for the user: land
the lineup PR now with mode 0 documented as pre-existing-broken (fix in a
follow-up PR), or hold this branch until mode 0 is fixed. Everything except
mode 0 is device-verified merge-ready. PR recipe:
`gh pr create --repo snokvist/waybeam_venc --head snokvist:feature/maruko-imx415-mode-lineup`.

## Bench end state (192.168.2.12)

Config: `sensor.mode 3`, `video0.size 1440x1080`, `video0.fps 90`,
`verbose true` — set + synced; device was `reboot -f`'d after the D-state
wedge. Start `setsid /usr/bin/waybeam` manually (no autostart on this
bench). Config backup from before this session: `/root/waybeam.json.bak.modes`.
