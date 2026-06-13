# wfb_tx NAL-Aware Link Protection — Implementation Spec

**Status:** Phase 1 spec (design, pre-implementation).
**Target codebase:** `wfb-ng` (`svpcom/wfb-ng`, `src/`). This is the
companion-sender contract; the encoder side already satisfies it.
**waybeam changes required:** none. The in-band NAL contract is already in
place — HEVC output is single-NAL + FU-A only (commit `9cad65e`, #142), and
the runtime already rewrites droppable enhancement frames to `TRAIL_N` and
keeps reference frames as `TRAIL_R` (README → refPred notes).

---

## 1. Motivation

waybeam's refPred (SVC-T) pyramid and any normal GOP stream both produce
frames of **unequal importance**: reference frames (`TRAIL_R`) and key
frames (IDR + VPS/SPS/PPS) are far more costly to lose than droppable
enhancement frames (`TRAIL_N`) or individual P-frames. Today `wfb_tx`
transmits every datagram identically, so the link gives the IDR the same
survival odds as a throwaway enhancement frame.

The failure mode we care about is **on-air packet loss in the SNR/fading
regime**, not congestion. The cheapest lever against that is **per-packet
modulation**: send the important frames one MCS step lower (more robust
modulation + coding → ~2–3 dB more link-budget margin) so they simply arrive
more often. This *prevents* the erasure instead of spending FEC bytes to
recover it, leaves the single stream / single FEC block untouched, and is
transparent to `wfb_rx` (802.11 is self-describing — the PHY recovers each
frame's rate from its preamble).

The same peek that selects MCS also enables an optional **drop** action
(live bandwidth shedding of droppable frames), replacing the need for a
second transport channel.

### Why not the alternatives (settled design decisions)

- **Per-layer FEC + second port (`enhancePort`/`thinEnhance` + wfb_tx
  rewrite):** heaviest path. Splits one stream into two, forces shared-SSRC
  reconstruction and a multi-stream FEC engine, gates on encoder presets,
  and does not generalize to normal-mode IDR protection. Rejected as the
  default; the only thing it uniquely buys is *tunable FEC code-rate per
  layer* and protection *below the MCS floor* (see §8 non-goals).
- **`-Q` qdisc + fwmark:** good for the *congestion* regime (shed enhancement
  under air-queue pressure) but it is transmit **prioritization**, not error
  protection — it does nothing for SNR-limited loss, which is our actual
  threat. Complementary, not a substitute.
- **Per-packet MCS (this spec):** smallest change that attacks on-air loss at
  the source; one mechanism, mode-agnostic, predicate-driven.

---

## 2. Core idea

Single stream, single FEC block, unchanged transport. At ingestion
(plaintext, before FEC/encryption), `wfb_tx` classifies each datagram against
an ordered **match→action table** and applies one of three actions:

| Action | Effect |
|--------|--------|
| `PASS` | Default. Transmit at base MCS. |
| `PROTECT` | Transmit at `base_mcs − mcs_delta` (clamped to 0). More robust. |
| `DROP` | Discard before it enters the FEC block (gated by a live toggle). |

The "what to match" half lives in a struct table so new peek targets are
**added as data**, not new code paths.

---

## 3. The match/action table (the struct)

```c
/* src/peek.hpp — data-only, control-protocol serialisable */

typedef enum {
    PEEK_ACT_PASS = 0,   /* transmit at base MCS */
    PEEK_ACT_PROTECT,    /* transmit at base_mcs - mcs_delta */
    PEEK_ACT_DROP,       /* discard before FEC (if drop_enabled) */
} peek_action_t;

/* Tagged matcher. Today one kind; future kinds extend the union without
 * touching the dispatch/action machinery. */
typedef enum {
    PEEK_MATCH_NAL_TYPE = 0, /* match HEVC/H.264 NAL unit type */
    /* future: PEEK_MATCH_BYTE_MASK (offset/mask/value), ... */
} peek_match_kind_t;

typedef enum { PEEK_PROTO_HEVC = 0, PEEK_PROTO_H264 = 1 } peek_proto_t;

typedef struct {
    peek_match_kind_t kind;
    union {
        struct {
            uint8_t  proto;      /* peek_proto_t */
            uint64_t type_mask;  /* bit i set => NAL type i matches */
        } nal;
        /* future: struct { uint16_t off; uint8_t mask, val; } byte; */
    } u;
} peek_match_t;

typedef struct {
    peek_match_t  match;
    peek_action_t action;
    uint8_t       mcs_delta;     /* PROTECT only; steps below base MCS */
} peek_rule_t;

#define PEEK_MAX_RULES 8

typedef struct {
    bool        enabled;         /* master feature toggle (live) */
    bool        drop_enabled;    /* arm DROP-action rules (live) */
    uint8_t     transport;       /* 0=RTP, 1=Annex-B (raw) */
    uint8_t     base_mcs;        /* mirror of the configured MCS */
    peek_rule_t rules[PEEK_MAX_RULES];
    uint8_t     n_rules;
} peek_cfg_t;
```

**Evaluation:** rules are checked in order; **first match wins**; no match →
`PASS`. When `enabled == false`, classification is skipped entirely
(zero-cost passthrough — the existing single-header inject path is unchanged).

---

## 4. Profiles (startup presets)

A profile expands to a rule table so operators don't hand-author masks. The
predicate is the *only* thing that differs between modes — proof that the
mechanism is universal.

| Profile | Rules (in order) |
|---------|------------------|
| `off` | (empty; `enabled=false`) |
| `idr` | PROTECT VPS/SPS/PPS `Δ2`; PROTECT IDR/CRA `Δ1` |
| `refpred` | PROTECT `TRAIL_R` `Δ1`; DROP `TRAIL_N` |
| `idr+refpred` | PROTECT param sets `Δ2`; PROTECT IDR/CRA + `TRAIL_R` `Δ1`; DROP `TRAIL_N` |

HEVC NAL types referenced (`nuh_type`): `TRAIL_N=0`, `TRAIL_R=1`,
`IDR_W_RADL=19`, `IDR_N_LP=20`, `CRA=21`, `VPS=32`, `SPS=33`, `PPS=34`.
H.264 equivalents (`nal_unit_type`): IDR `=5`, SPS `=7`, PPS `=8`
(no temporal-layer marking; `idr` profile only).

`type_mask` for `idr` (HEVC) = bits {19,20,21} (IDR/CRA) and a second rule
with bits {32,33,34} (param sets). `refpred` = bit {1} PROTECT and bit {0}
DROP.

---

## 5. Classification (the peek)

Runs in `send_packet()` on the plaintext datagram. RTP transport (waybeam
default):

```
classify(payload, len, cfg) -> peek_action_t:
    if not cfg.enabled: return PASS
    if cfg.transport == RTP:
        if len < 12: return PASS                 # too short for RTP
        cc  = payload[0] & 0x0F
        off = 12 + 4*cc
        if (payload[0] & 0x10):                  # extension present
            if len < off+4: return PASS
            off += 4 + 4*be16(payload+off+2)
        if off >= len: return PASS
        nal = payload + off
    else:                                        # Annex-B: skip 00 00 01 / 00 00 00 01
        nal = skip_start_code(payload, len)
    t = nal_type(nal, len - (nal-payload), cfg)  # HEVC/H.264 + FU-A indirection
    for r in cfg.rules[0..n_rules):
        if r.match.kind == NAL_TYPE and (r.match.u.nal.type_mask & (1<<t)):
            return r.action
    return PASS
```

`nal_type()` extraction:
- **HEVC:** need ≥2 bytes. `t = (nal[0] >> 1) & 0x3F`. If `t == 49` (FU),
  need ≥3 bytes, real type `= nal[2] & 0x3F`.
- **H.264:** need ≥1 byte. `t = nal[0] & 0x1F`. If `t == 28` (FU-A), need
  ≥2 bytes, real type `= nal[1] & 0x1F`.

All fragments of one access unit carry the same effective type (FU headers
echo the original type), so a frame is classified **consistently across all
its fragments** — essential for DROP correctness (§6).

Short/un-parseable packets fall through to `PASS` — fail-open, never drop or
mis-protect on a malformed peek.

---

## 6. Actions

### PROTECT (per-packet MCS)
- At init, prebuild a small set of radiotap headers — one per distinct MCS in
  use (`base`, `base−1`, `base−2`, …). Each is the existing
  `init_radiotap_header()` output with a different `header[MCS_IDX_OFF]`.
- Thread the per-fragment action from `send_packet()` to `inject_packet()`
  alongside the block buffer (a parallel `uint8_t block_action[RS_N]`,
  mirroring how `set_mark()` already carries per-fragment TX state). At inject,
  select `iovec[0]` = the header for `clamp(base_mcs − mcs_delta, 0)`.
- **MCS floor:** if `base_mcs − mcs_delta < 0`, clamp to 0 and log once. At
  MCS0 PROTECT is a no-op (graceful, finite).
- **Parity packets — Option A (decided):** inject parity at the **most robust
  level present among the block's data fragments**. Parity symbols are the
  recovery substitutes for the data they protect, so they must survive at least
  as well as the most important fragment in the block.
  - **Why this is near-free here:** `fec_timeout` (`-T`) is an *idle/inter-packet*
    timer — `fec_close_ts` resets to `now + fec_timeout` on every received
    datagram, so a block stays open only while packets keep arriving sub-timeout,
    then closes after ~`fec_timeout` ms of silence (or earlier on reaching `k`).
    Video egress is bursty per frame with inter-frame gaps of ~8–16 ms
    (120–60 fps) ≫ a 2–4 ms `-T`, so **each frame closes its own FEC block** →
    blocks are effectively **single-class** and parity simply matches that class.
    No mostly-enhancement block is ever forced to robust parity; the mixed-block
    airtime cost evaporates.
  - **Residual mixes, both handled by A:** (i) param sets (Δ2) bundled with their
    IDR (Δ1) in one access unit share a block — A picks the deepest (Δ2);
    (ii) an IDR larger than `k` packets spills into a second block — still all
    IDR class. There is no protect-vs-`PASS` mix within a single frame.
  - **Dependency:** holds while the inter-frame gap > `fec_timeout`. Keep `-T`
    well below the frame period (2–4 ms is safe through 120 fps). Option B
    (parity at base MCS) is therefore unnecessary and omitted.

### DROP (live shedding)
- If `action == DROP` **and** `cfg.drop_enabled`, return from `send_packet()`
  **before** the datagram is copied into the FEC block. The frame never enters
  the stream.
- Because classification is consistent per access unit (§5), **all** fragments
  of a dropped NAL are dropped — no half-transmitted frames.
- No IDR-on-resume handshake is needed (unlike the PR's transport-layer
  `thinEnhance`): only **non-reference** frames are ever DROP-tagged, so the
  decoder loses nothing it was depending on. Re-enabling `drop_enabled` simply
  lets the next enhancement frames through.

### PASS
- Unchanged from today: base-MCS header, normal FEC/encrypt/inject.

### Coexistence
- Orthogonal to the existing data/FEC `fwmark` (`set_mark 0/1`) and to `-Q`.
  PROTECT changes the radiotap rate; fwmark changes the kernel qdisc class.
  Both can be active.

---

## 7. Control surface

### 7.1 Startup args (`wfb_tx`)
```
--peek-profile {off|idr|refpred|idr+refpred}   # default: off
--peek-transport {rtp|annexb}                  # default: rtp
--peek-rule <proto>:<action>:<types>[:Δ<n>]    # repeatable; granular override
        # e.g. --peek-rule hevc:protect:idr,cra:Δ1
        #      --peek-rule hevc:drop:trail_n
```
A profile seeds the table; `--peek-rule` entries append/override.

### 7.2 Runtime (`wfb_tx_cmd` → control port)
Two new command IDs, parsed in the `tx.cpp` control switch alongside
`CMD_SET_RADIO`/`CMD_SET_FEC`:

```c
/* src/tx_cmd.h */
#define CMD_SET_PEEK 0x10
#define CMD_GET_PEEK 0x11

typedef struct {                 /* payload of CMD_SET_PEEK */
    uint8_t enabled;             /* 0/1, 0xFF = leave unchanged */
    uint8_t drop_enabled;        /* 0/1, 0xFF = leave unchanged */
} cmd_peek_req_t;
```

`wfb_tx_cmd` verbs:
```
wfb_tx_cmd <ctrl_port> peek on            # enabled=1
wfb_tx_cmd <ctrl_port> peek off           # enabled=0
wfb_tx_cmd <ctrl_port> peek drop on       # drop_enabled=1
wfb_tx_cmd <ctrl_port> peek drop off      # drop_enabled=0
wfb_tx_cmd <ctrl_port> peek status        # CMD_GET_PEEK -> prints state + rules
```

These are the two live toggles the feature needs: **feature on/off** and
**drop on/off**. The `0xFF = leave unchanged` sentinel lets each toggle move
independently.

### 7.3 Backward compatibility
New `cmd_id`s only; the control switch already returns `ENOTSUP` for unknown
commands, so an old `wfb_tx_cmd` against a new `wfb_tx` (and vice-versa)
degrades cleanly. Existing `CMD_SET_RADIO`/`CMD_SET_FEC` are untouched.

---

## 8. Integration points (file-by-file)

| File | Change |
|------|--------|
| `src/peek.hpp` (new) | Table types (§3), profile + `--peek-rule` parsing, `peek_classify()`. |
| `src/peek.cpp` (new) | RTP/NAL parse, `nal_type()`, rule eval. Self-contained, host-testable. |
| `src/tx.cpp` | `send_packet()`: call `peek_classify()`, early-return on armed DROP, stash action in `block_action[]`. `inject_packet()`: pick radiotap header by action/level. Init: build per-MCS header set. Control switch: `CMD_SET_PEEK`/`CMD_GET_PEEK`. Arg parse: `--peek-*`. |
| `src/tx_cmd.c` / `src/tx_cmd.h` | New verbs + `cmd_peek_req_t` + `CMD_*_PEEK` ids. |
| `Makefile` / `CMakeLists` | Add `peek.cpp`; add a `test_peek` host unit. |

No change to `rx.cpp`, the FEC core, the wire/crypto format, or waybeam.

---

## 9. Edge cases & guards

- **Fail-open peek:** any short/un-parseable packet → `PASS`. Never drop or
  mis-rate on ambiguity.
- **MCS floor:** `Δ` clamped at MCS0.
- **Mixed-block parity:** Option A (most-robust-in-block); effectively
  single-class under `-T` 2–4 ms since each frame closes its own block (§6).
- **Drop atomicity:** per-AU classification consistency guarantees all-or-none
  per frame.
- **Non-video ports:** peek is opt-in per `wfb_tx` instance; a port carrying
  MSP/MAVLink simply runs `off`.
- **Zero-cost when off:** `enabled=false` skips parse; identical to today's
  hot path.
- **Airtime budget:** PROTECT stretches on-air time for matched frames
  (~1.3–1.5× at Δ1). Steady + small for `refpred` base; **bursty** for `idr`
  (whole keyframe). Tune `Δ` and which subset is protected per GOP length.

---

## 10. Test plan

1. **Host unit (`test_peek`):** table of crafted RTP+NAL byte sequences
   (single-NAL and FU-A, HEVC + H.264, truncated) → assert expected action for
   each profile. Pure function, no hardware.
2. **Loopback inject capture:** run `wfb_tx` against a pcap/monitor capture;
   assert each frame's radiotap MCS matches its NAL class, and that armed DROP
   removes exactly the `TRAIL_N` AUs.
3. **Bench (weak link):** on a link tuned to ~5–15% raw loss, compare base-
   frame vs enhancement-frame delivery ratio with profile `refpred` on/off;
   expect base delivery to rise and visible corruption/stall events to fall.
4. **Drop toggle:** `peek drop on` reduces measured bitrate by the enhancement
   fraction with no decoder errors; `peek drop off` restores full rate.
5. **A/B vs baseline:** confirm `peek off` is byte-identical on the wire to the
   current build (no regression).

---

## 11. Non-goals (explicit)

- **Per-layer FEC code-rate.** Different RS(k,n) per importance class needs
  separate FEC blocks → separate streams. Out of scope; this spec deliberately
  keeps one stream. Revisit only if bench data shows base frames dying from
  *shared-FEC-block exhaustion* rather than raw SNR loss.
- **Protection below the MCS floor.** At MCS0 only FEC/retransmit helps.
- **waybeam / encoder changes.** None.
- **`wfb_rx` changes.** None (mixed-MCS is self-describing).

---

## 12. Future extensions (the table already accommodates)

- **`PEEK_MATCH_BYTE_MASK`** — generic offset/mask/value matcher for non-NAL
  targets (e.g. an OSD/telemetry marker byte), added as a new union arm + one
  `case` in the matcher; dispatch and actions unchanged.
- **`CMD_SET_PEEK_RULE`** — install/replace rules live (the rule struct is
  already control-protocol serialisable), for tuning without restart.
- **Per-rule drop arming** — promote the single `drop_enabled` to a per-rule
  flag once more than one droppable target exists.
- **PHY-diversity dimensions** — extend PROTECT to also set LDPC/STBC for the
  most critical NALs (same radiotap-template selection mechanism).
