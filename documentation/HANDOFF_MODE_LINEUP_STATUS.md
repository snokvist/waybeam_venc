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
| 3 | 2112x1184@90_1485 | 90.0 fps (`/api/v1/fps/live`=90), table byte-identical to #155 old mode 7 |
| 4 | 1920x1080@100_1485 (NEW) | 100.0 fps exact (shutter 9994 µs), ~16.2 Mbps |

fps confirmed via `[verbose]` frame-counter deltas and the `/api/v1/fps/live`
endpoint (the per-second `[verbose]` fps field truncates: shows 59/99/49
while deltas are exactly 60/100/50 per second).

**Full deploy + switch re-verification (2026-07-03, patched binary on flash).**
After deploying the patched v0.21.0 binary + branch .ko to flash and
rebooting: (a) clean **cold boot into mode 0** native = 30 fps (see Item 1);
(b) **API mode-switch series** `sensor.mode 0→1→2→3→4` via `/api/v1/set`,
each reading its native rate on `/api/v1/fps/live` — mode 1=50, 2=60, 3=90,
4=100, all at the correct native resolution (2952/2688/2112/1920), **zero
aborts, no dmesg wedge, clean respawn-per-switch**. Every `sensor.mode`
change is a full process respawn (new pid; logs to `/tmp/waybeam.log`), and
the daemon serialises them, so hot-swapping never wedges. (`video0.fps` caps
output rate independently of the sensor mode's max — leave it ≥ the mode's
native fps, or a mode will simply be paced lower.)

Gates: `make verify` green both backends, `make test` 1699/1699. Flash
`/usr/bin/waybeam` is now the patched v0.21.0 build (md5 `c385c5fd…`); the
prior v0.20.0 is backed up at `/root/waybeam.bak.v020`. Flash .ko is the
branch build (md5 `18899d6b…`).

## CLOSED ITEM 1 — mode 0 works; earlier "zero-frames" was a test artifact

**Fully resolved 2026-07-03 (supersedes the earlier "pre-existing regression"
claim, which was wrong).** Mode 0 streams fine, including at the full native
3760 encode, from both a live switch AND a clean cold boot.

- **Clean cold boot into mode 0** (patched binary on flash, fresh reboot,
  single instance, `sensor.mode 0` + `video0.size auto` + `fps 30`):
  `sensor capt 3760x2116 → out 3760x2116`, `VENC ring pool 3760x2116
  ring=2116`, REALTIME bind, **30 fps steady** (frame counter +30/s),
  ~15 Mbps, ~64 KB/frame, zero aborts, no FIFO-full / fence spam. This was
  the exact config the flawed earlier test "failed" on — it works.
- Live webui switch into mode 0 at native `out 3760x2116`: 30 fps, same.

So the HW_RING/IFC path is fine at 3760 encode width. The original zero-frame
observation came from a cold `setsid` start into mode 0 that was polluted by
an overlapping waybeam instance (the same class of error that caused
D-state #4) — two processes fighting over the SCL/VENC channels. The
HW_RING=0 / EnqOTNull diagnostics were the overlap-starvation signature, not
a width limit.

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

Flash `/usr/bin/waybeam` = patched v0.21.0 `c385c5fd` (prior v0.20.0 backed
up at `/root/waybeam.bak.v020`); flash .ko = branch `18899d6b`. No autostart
— start `setsid /usr/bin/waybeam` manually; it reads `/etc/waybeam.json`.
Config was actively switched during the deploy re-verification (and by the
user via webui); leave it at any valid mode. `/api/v1/set?sensor.mode=N` +
`?video0.fps=F` drive live switching; `/api/v1/fps/live` reads the true rate.
Config backup from session start: `/root/waybeam.json.bak.modes`.
