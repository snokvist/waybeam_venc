# wfb_tx NAL-Aware Link Protection — Implementation Spec

**Status:** Phase 1 spec (design, pre-implementation).
**Target codebase:** `wfb-ng` (`svpcom/wfb-ng`, `src/`). This is the
companion-sender contract; the encoder side already satisfies it.
**waybeam changes required:** none. The in-band NAL contract is already in
place — HEVC output is single-NAL + FU-A only (commit `9cad65e`, #142), the
runtime already rewrites droppable enhancement frames to `TRAIL_N` and keeps
reference frames as `TRAIL_R` (README → refPred notes), and the RTP packetizer
already sets the marker bit on the last packet of every access unit
(`src/rtp_packetizer.c:92`).

**Transport (verified in-repo, scoped):** this spec targets waybeam's default
`rtp` stream mode (`src/rtp_packetizer.c`), which sets the RTP marker on each
access unit's last packet (`:92`, `:25`) — the frame-boundary signal keys on
that marker. The `compact` stream mode is **not used and is out of scope**
(scoped out by design decision). waybeam emits **no AUDs** and has **no
Annex-B / start-code egress**, so RTP is the only transport considered.

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

The same peek that selects MCS also (a) enables an optional **drop** action
(live bandwidth shedding of droppable frames), replacing the need for a second
transport channel, and (b) emits a **frame-boundary signal** that closes the
FEC block at each access-unit boundary — deterministic single-class blocks and
lower latency, for the price of a marker bit the peek already reads.

### Why not the alternatives (settled design decisions)

- **Per-layer FEC + second port (`enhancePort`/`thinEnhance` + wfb_tx
  rewrite):** heaviest path. Splits one stream into two, forces shared-SSRC
  reconstruction and a multi-stream FEC engine, gates on encoder presets,
  and does not generalize to normal-mode IDR protection. Rejected as the
  default; the only thing it uniquely buys is *tunable FEC code-rate per
  layer* and protection *below the MCS floor* (see §11 non-goals).
- **`-Q` qdisc + fwmark:** good for the *congestion* regime (shed enhancement
  under air-queue pressure) but it is transmit **prioritization**, not error
  protection — it does nothing for SNR-limited loss, which is our actual
  threat. Complementary, not a substitute.
- **Per-packet MCS (this spec):** smallest change that attacks on-air loss at
  the source; one mechanism, mode-agnostic, predicate-driven.

---

## 2. Core idea

Single stream, single FEC block, unchanged transport. At ingestion
(plaintext, before FEC/encryption), `wfb_tx` runs each datagram through one
shared **byte-matcher** whose matches drive two independent outcome kinds:

- a per-packet **transmit action** — first match wins:

  | Action | Effect |
  |--------|--------|
  | `PASS` | Default. Transmit at base MCS. |
  | `PROTECT` | Transmit at `base_mcs − mcs_delta` (clamped to 0). More robust. |
  | `DROP` | Discard before it enters the FEC block (gated by a live toggle). |

- zero or more **control signals** — accumulate-all — currently `FEC_CLOSE`
  (close the FEC block at this packet, for frame-boundary alignment).

The matcher is one generic vocabulary — NAL-unit-type **and** raw
offset/mask/value — so new peek targets, whether they drive an action or a
signal, are **added as data**, not new code paths. The frame-boundary FEC
close is a **mandatory** signal expressed in this same vocabulary (the RTP
marker bit is just a byte test), so the engine stays protocol-agnostic; nothing
about RTP is hard-wired into it.

---

## 3. The match table (the struct)

```c
/* src/peek.hpp — data-only, control-protocol serialisable */

/* ---- generic matcher, shared by action rules and signal rules ---- */
typedef enum {
    PEEK_MATCH_NAL_TYPE = 0, /* decode HEVC/H.264 NAL type, test type_mask */
    PEEK_MATCH_BYTE_MASK,    /* raw test: (base[off] & mask) == val */
} peek_match_kind_t;

typedef enum { PEEK_PROTO_HEVC = 0, PEEK_PROTO_H264 = 1 } peek_proto_t;

typedef enum {
    PEEK_ANCHOR_DATAGRAM = 0, /* off measured from datagram byte 0 (RTP header) */
    PEEK_ANCHOR_PAYLOAD,      /* off measured from start of codec payload (post-RTP) */
} peek_anchor_t;

typedef struct {
    peek_match_kind_t kind;
    union {
        struct {                 /* PEEK_MATCH_NAL_TYPE */
            uint8_t  proto;      /* peek_proto_t */
            uint64_t type_mask;  /* bit i set => NAL type i matches */
        } nal;
        struct {                 /* PEEK_MATCH_BYTE_MASK */
            uint8_t  anchor;     /* peek_anchor_t */
            uint16_t off;
            uint8_t  mask;
            uint8_t  val;        /* match iff (byte & mask) == val */
        } byte;
    } u;
} peek_match_t;

/* ---- outcome 1: per-packet transmit action (first match wins) ---- */
typedef enum {
    PEEK_ACT_PASS = 0,   /* transmit at base MCS */
    PEEK_ACT_PROTECT,    /* transmit at base_mcs - mcs_delta */
    PEEK_ACT_DROP,       /* discard before FEC (if drop_enabled) */
} peek_action_t;

typedef struct {
    peek_match_t  match;
    peek_action_t action;
    uint8_t       mcs_delta;     /* PROTECT only; steps below base MCS */
} peek_rule_t;

/* ---- outcome 2: control signals (accumulate all matches) ---- */
typedef enum {
    PEEK_SIG_FEC_CLOSE = 1u << 0, /* close the FEC block at this packet */
    /* future: PEEK_SIG_* ... */
} peek_signal_t;

typedef struct {
    peek_match_t match;
    uint16_t     signal;         /* bitmask of peek_signal_t */
} peek_sig_rule_t;

#define PEEK_MAX_RULES     8
#define PEEK_MAX_SIG_RULES 4

typedef struct {
    bool            enabled;       /* master pipeline toggle (live) */
    bool            drop_enabled;  /* arm DROP-action rules (live) */
    uint8_t         base_mcs;      /* mirror of the configured MCS */
    peek_rule_t     rules[PEEK_MAX_RULES];
    uint8_t         n_rules;
    peek_sig_rule_t sig_rules[PEEK_MAX_SIG_RULES]; /* seeded with mandatory FEC_CLOSE */
    uint8_t         n_sig_rules;
} peek_cfg_t;
```

**Evaluation (per packet, only while `enabled`):** *action* rules are scanned
in order, **first match wins**, default `PASS`. *signal* rules are scanned
independently and **all** matches OR together (a packet can be both `PROTECT`
and `FEC_CLOSE`). When `enabled == false` the whole pipeline is skipped
(zero-cost passthrough; FEC blocks then close on `-T` alone, exactly as today).
The signal table is **never empty** — it is seeded with the mandatory
`FEC_CLOSE` rule (§6).

---

## 4. Profiles (startup presets)

A profile expands to an *action* rule table so operators don't hand-author
masks. The predicate is the *only* thing that differs between modes — proof
that the mechanism is universal.

| Profile | Action rules (in order) |
|---------|-------------------------|
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

**Mandatory signal seed (independent of profile, including `off`-but-enabled):**
the config always carries the frame-boundary signal rule (§6, *Frame-boundary
FEC close*): `BYTE_MASK{anchor=DATAGRAM, off=1, mask=0x80, val=0x80}` →
`FEC_CLOSE` (the RTP marker bit). **Verified against waybeam:** the RTP
packetizer sets the marker on the last packet of every access unit
(`src/rtp_packetizer.c:92`, `:25`). It is not part of any profile and cannot be
configured away.

---

## 5. Classification (the peek)

Runs in `send_packet()` on the plaintext datagram, returning a transmit action
and a signal set:

```
peek(payload, len, cfg) -> {action, signals}:
    if not cfg.enabled: return {PASS, 0}
    nal  = locate_payload(payload, len)          # skip the RTP header
    t    = nal_type(nal, len-(nal-payload), cfg) # HEVC/H.264 + FU-A indirection
    action = PASS
    for r in cfg.rules[0..n_rules):              # first match wins
        if match(r.match, payload, len, nal, t): action = r.action; break
    signals = 0
    for s in cfg.sig_rules[0..n_sig_rules):      # accumulate all
        if match(s.match, payload, len, nal, t): signals |= s.signal
    return {action, signals}

match(m, payload, len, nal, t):
    if m.kind == NAL_TYPE:
        return (m.u.nal.type_mask >> t) & 1
    if m.kind == BYTE_MASK:
        base = (m.u.byte.anchor == DATAGRAM) ? payload : nal
        i    = (base - payload) + m.u.byte.off
        return i < len and (payload[i] & m.u.byte.mask) == m.u.byte.val
```

`locate_payload()` (RTP): require `len ≥ 12`; `off = 12 + 4*(payload[0]&0x0F)`;
if `payload[0]&0x10` (extension) add `4 + 4*be16(payload+off+2)`; fail-open to
the datagram start if anything is short.

`nal_type()` extraction:
- **HEVC:** need ≥2 bytes. `t = (nal[0] >> 1) & 0x3F`. If `t == 49` (FU),
  need ≥3 bytes, real type `= nal[2] & 0x3F`.
- **H.264:** need ≥1 byte. `t = nal[0] & 0x1F`. If `t == 28` (FU-A), need
  ≥2 bytes, real type `= nal[1] & 0x1F`.

All fragments of one access unit carry the same effective type (FU headers
echo the original type), so a frame is classified **consistently across all
its fragments** — essential for DROP correctness (§6). Short/un-parseable
packets fall through to `PASS` with no signals — fail-open, never drop or
mis-protect on a malformed peek.

---

## 6. Actions & signals

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
  - **Why this is near-free here:** the mandatory `FEC_CLOSE` signal closes the
    block at every access-unit boundary (below), so **each frame is its own FEC
    block** → blocks are **single-class by construction** and parity simply
    matches that class. No mostly-enhancement block is ever forced to robust
    parity; the mixed-block airtime cost evaporates.
  - **Residual mixes, both handled by A:** (i) param sets (Δ2) bundled with their
    IDR (Δ1) in one access unit share a block — A picks the deepest (Δ2);
    (ii) an IDR larger than `k` packets spills into a second block — still all
    IDR class. There is no protect-vs-`PASS` mix within a single frame.
  - **Determinism:** with marker-close the single-class property no longer
    depends on inter-frame gap > `fec_timeout`; it holds at any fps and under
    pacing/jitter. The gap argument applies only to the `-T` fallback path
    (Option B, parity at base MCS, is therefore unnecessary and omitted).

### Frame-boundary FEC close (mandatory signal)
- The seeded `FEC_CLOSE` rule (§4) fires on the **last packet of each access
  unit** — for RTP the marker bit (`payload[1] & 0x80`), expressed as a
  `BYTE_MASK` rule so the engine stays protocol-agnostic.
- On a **transmitted** packet whose signals include `FEC_CLOSE`, `data_source`
  flushes the block **immediately after the packet is enqueued** — the same
  path the `-T` branch already uses (`send_packet(NULL, 0, WFB_PACKET_FEC_ONLY)`;
  then reset `fec_close_ts`). Order matters: enqueue the marked fragment first
  so parity covers it, *then* close.
- **Why mandatory, not optional:** it makes the single-class block boundary
  **deterministic** (block = exactly one AU) instead of timing-dependent, and
  it emits parity at frame end instead of after up to `fec_timeout` of silence
  — ~2–4 ms lower latency on the recovery path per frame (data fragments
  already inject as they arrive; only parity waited on `-T`).
- **`-T` retained as fallback:** close on **marker OR timeout, whichever
  first**. Covers a missing marker, partial frames, or non-RTP ports.
- **DROP precedence:** signals are evaluated only for transmitted packets; an
  armed-DROP packet returns before signal handling. A fully-dropped enhancement
  frame therefore raises no `FEC_CLOSE` — correct, because the previous
  transmitted frame already closed on its own marker.

### DROP (live shedding)
- If `action == DROP` **and** `cfg.drop_enabled`, return from `send_packet()`
  **before** the datagram is copied into the FEC block (and before signal
  handling). The frame never enters the stream.
- Because classification is consistent per access unit (§5), **all** fragments
  of a dropped NAL are dropped — no half-transmitted frames.
- No IDR-on-resume handshake is needed (unlike the PR's transport-layer
  `thinEnhance`): only **non-reference** frames are ever DROP-tagged, so the
  decoder loses nothing it was depending on. Re-enabling `drop_enabled` simply
  lets the next enhancement frames through.

### PASS
- Unchanged transmit path: base-MCS header, normal FEC/encrypt/inject. (Still
  subject to signal handling — a `PASS` packet can carry `FEC_CLOSE`.)

### Coexistence
- Orthogonal to the existing data/FEC `fwmark` (`set_mark 0/1`) and to `-Q`.
  PROTECT changes the radiotap rate; fwmark changes the kernel qdisc class.
  Both can be active.

---

## 7. Control surface

### 7.1 Startup args (`wfb_tx`)
```
--peek-profile {off|idr|refpred|idr+refpred}   # default: off
--peek-rule <proto>:<action>:<types>[:Δ<n>]    # repeatable; granular action override
        # e.g. --peek-rule hevc:protect:idr,cra:Δ1
        #      --peek-rule hevc:drop:trail_n
```
A profile seeds the action table; `--peek-rule` entries append/override. The
mandatory frame-boundary signal rule (§4) is always seeded (RTP marker).

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
independently. Frame-boundary `FEC_CLOSE` is **not** a toggle — it is always
active while `enabled=1`; `peek status` reports the seeded signal rule(s).

### 7.3 Backward compatibility
New `cmd_id`s only; the control switch already returns `ENOTSUP` for unknown
commands, so an old `wfb_tx_cmd` against a new `wfb_tx` (and vice-versa)
degrades cleanly. Existing `CMD_SET_RADIO`/`CMD_SET_FEC` are untouched.

---

## 8. Integration points (file-by-file)

| File | Change |
|------|--------|
| `src/peek.hpp` (new) | Table types (§3), profile + `--peek-rule` parsing, mandatory RTP-marker signal seeding, `peek()`. |
| `src/peek.cpp` (new) | RTP-header locate, `nal_type()`, generic byte matcher, action + signal eval. Self-contained, host-testable. |
| `src/tx.cpp` | `send_packet()`: call `peek()`; early-return on armed DROP; stash action in `block_action[]`; on a transmitted packet with `FEC_CLOSE`, flush the block (FEC-only) after enqueue. `inject_packet()`: pick radiotap header by action/level. Init: build per-MCS header set. Control switch: `CMD_SET_PEEK`/`CMD_GET_PEEK`. Arg parse: `--peek-*`. |
| `src/tx_cmd.c` / `src/tx_cmd.h` | New verbs + `cmd_peek_req_t` + `CMD_*_PEEK` ids. |
| `Makefile` / `CMakeLists` | Add `peek.cpp`; add a `test_peek` host unit. |

No change to `rx.cpp`, the FEC core, the wire/crypto format, or waybeam.

---

## 9. Edge cases & guards

- **Fail-open peek:** any short/un-parseable packet → `PASS`, no signals. Never
  drop, mis-rate, or spuriously close on ambiguity.
- **MCS floor:** `Δ` clamped at MCS0.
- **Mixed-block parity:** Option A (most-robust-in-block); blocks are
  single-class **by construction** via marker-close (§6).
- **Frame-close fallback:** marker **or** `-T`, whichever first — a missing or
  unparsed marker still closes the block via the idle timeout.
- **Drop atomicity:** per-AU classification consistency guarantees all-or-none
  per frame; dropped frames raise no `FEC_CLOSE`.
- **Non-video ports:** peek is opt-in per `wfb_tx` instance; a port carrying
  MSP/MAVLink simply runs `off`.
- **Zero-cost when off:** `enabled=false` skips the whole pipeline (action
  *and* signal eval); identical to today's hot path, FEC closes on `-T`.
- **Airtime budget:** PROTECT stretches on-air time for matched frames
  (~1.3–1.5× at Δ1). Steady + small for `refpred` base; **bursty** for `idr`
  (whole keyframe). Tune `Δ` and which subset is protected per GOP length.

---

## 10. Test plan

1. **Host unit (`test_peek`):** table of crafted RTP+NAL byte sequences
   (single-NAL and FU-A, HEVC + H.264, truncated, marker set/clear) → assert
   expected `{action, signals}` for each profile. Pure function, no hardware.
2. **Loopback inject capture:** run `wfb_tx` against a pcap/monitor capture;
   assert each frame's radiotap MCS matches its NAL class, that the FEC block
   closes on the marker packet (one block per AU), and that armed DROP removes
   exactly the `TRAIL_N` AUs.
3. **Bench (weak link):** on a link tuned to ~5–15% raw loss, compare base-
   frame vs enhancement-frame delivery ratio with profile `refpred` on/off;
   expect base delivery to rise and visible corruption/stall events to fall.
4. **Drop toggle:** `peek drop on` reduces measured bitrate by the enhancement
   fraction with no decoder errors; `peek drop off` restores full rate.
5. **Latency:** confirm parity emission moves from `+fec_timeout` (after last
   data packet) to immediately on the marker (capture timestamps).
6. **A/B vs baseline:** confirm `peek off` is byte-identical on the wire to the
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
- **`compact` stream mode & non-RTP transports.** Out of scope — `compact` is
  unused, and RTP is waybeam's only video egress. The peek assumes RTP framing.

---

## 12. Future extensions (the table already accommodates)

- **New signals.** The signal table accepts more `PEEK_SIG_*` outcomes (e.g. a
  key-frame boundary hook for an external recorder, a stats tap) using the same
  byte-matcher — add an enum bit and one handler; matcher and actions unchanged.
- **More matcher kinds.** Beyond `NAL_TYPE` / `BYTE_MASK` (e.g. a range or
  multi-byte compare), added as a union arm + one `case`.
- **`CMD_SET_PEEK_RULE`** — install/replace action *and* signal rules live (both
  rule structs are already control-protocol serialisable), for tuning without
  restart.
- **Per-rule drop arming** — promote the single `drop_enabled` to a per-rule
  flag once more than one droppable target exists.
- **PHY-diversity dimensions** — extend PROTECT to also set LDPC/STBC for the
  most critical NALs (same radiotap-template selection mechanism).
