# HANDOFF — Maruko IMX415 mode-lineup rework (branch `feature/maruko-imx415-mode-lineup`)

Status as of 2026-07-03 evening. Written for the next agent taking over.
Companion doc: `MARUKO_IMX415_1485_MODES.md` (mode table, recipes, known issues).

## What this branch does

Replaces the 8-mode list (vendor + 1485 mix) with one best mode per FPS tier,
all non-binned and ~16:9. **Breaking `sensor.mode` remap** (old→new: 0→0,
5→1, 7→3; old 1–4 unsurfaced under `#if 0`; old 6 re-sized). VERSION 0.21.0.

| Idx | Mode | Device-verified (this session, .12) |
|---|---|---|
| 0 | 3760x2116@30 (891/REALTIME) | **FAILS — pre-existing master bug, see below** |
| 1 | 2952x1656@50_1485 | 50.0 fps exact, ~16.5 Mbps — **`video0.size` must be `auto`** (see /16 note) |
| 2 | 2688x1512@60_1485 (NEW) | 60.0 fps exact, ~14.6 Mbps, dmesg clean |
| 3 | 2112x1184@90_1485 | table byte-identical to #155-verified old mode 7; set as final bench state |
| 4 | 1920x1080@100_1485 (NEW) | 100.0 fps exact (shutter 9994 µs), ~16.2 Mbps |

fps confirmed via `[verbose]` frame-counter deltas (the per-second fps field
truncates: shows 59/99/49 while deltas are exactly 60/100/50 per second).

Gates: `make verify` green both backends, `make test` 1699/1699. The .ko on
.12 flash is this branch's build (md5 `18899d6b…`); flash `/usr/bin/waybeam`
is still master v0.20.0 (fine — branch changes are driver-only).

## OPEN ITEM 1 — mode 0 zero-frames: PRE-EXISTING master regression

**Not caused by this branch.** Reproduced identically with master's .ko
(`cce8971d…`) + master flash binary: bring-up completes, 3A runs, then
"no encoder data received; aborting stream loop" after ~30 s. The aborted
process exits and lingers as an unreaped `[waybeam]` zombie (harmless, no
D-state; cleared by reboot).

Diagnostic snapshot mid-failure (`/proc/mi_modules`, dumps were saved to the
session scratchpad; re-generate live as needed):

- **Sensor + ISP are fine**: ISP dev `FrameDoneCnt` ticks; SCL *input* port
  shows `FPS 30.00, FinishCnt 3429, DropCnt 0` — frames arrive from ISP.
- **SCL channel drops almost everything**: CHN dump `DropCnt=3129,
  EnqOTNull=3119` (enqueue-with-no-output-port), output-port dump section
  EMPTY, chn `pixel` prints `ERR` — yet `MI_SCL_SetOutputPortParam` /
  `EnableOutputPort` returned success (no error in app log).
- **VENC starves and drops what it does get**: input `FrameCnt=184,
  BlockCnt=1304` (~9 fps effective), encodes 183, output `FrameCnt=0,
  DropCnt=183`, `GetStreamCnt=0`, `PollFailCnt=40`.
- **HW_RING never engages**: VENC chn `RingStartLine=0,
  RingRealTotalHeight=0` despite ring pool "size=3760x2112 ring=2112"
  configured; ISP dev also logs `fifofullcnt=310 / DropCnt=310`.

**Working hypothesis**: the Tier-C low-latency SCL→VENC HW_RING + IFC
(compress=6) path fails at 3760-px width. Every mode verified since Tier C
landed is ≤2952 wide; mode 0 (the only 3760-wide surfaced mode) simply has
not been run since. Repro: `sensor.mode 0`, `video0.size 3760x2112` (or
`auto`), fps 30.

Suggested attack: in `src/maruko_pipeline.c` around lines 739–770 (SCL port
0 setup, `compress=(i6_common_compr)6 /* IFC */`, "IFC compress required for
HW_RING"), try mode 0 with (a) compress=0 + FRAME bind instead of HW_RING,
or (b) HW_RING with a width cap that falls back to the plain path when
`out_w > 2952/3072-ish`, and bisect the actual width limit. If REALTIME+
plain-frame works at 3760, gate HW_RING on width.

## OPEN ITEM 2 — encode-width /16 validation vs hardware

`venc_config` rejects explicit `video0.size` widths not divisible by 16
("width must be a multiple of 16"), but `size: auto` happily encodes
2952 (÷16 = 184.5) natively and it works. Consequences:

- Mode 1 **must** run `video0.size auto` for its 1:1 50 fps. Forcing
  2944x1656 (nearest /16) adds a 2952→2944 SCL crop that costs ~7 fps
  (measured 43 vs 50).
- Also: heights not /8 are rejected (mode 0 sensor 2116 → encode 2112).
- Follow-up: either relax the width check to /8 (verify VENC stride rules)
  or document `auto` as the intended path for non-/16 modes.

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
