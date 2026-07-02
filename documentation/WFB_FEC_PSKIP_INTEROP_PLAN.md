# WFB FEC × PSKIP Interop Plan

Audience: the wfb_ng link-controller agent (FEC block sizing / M-bit
closure logic).  Purpose: evaluate and implement the controller-side
changes needed so waybeam's new PSKIP frame-lost mode (venc v0.19.0)
does not degrade FEC efficiency or poison block-sizing state.

Status: proposal — venc side is merged on the venc branch; PSKIP is not
yet hardware-verified (see Open Questions).

## 1. What the encoder now emits

waybeam v0.19.0 exposes the SigmaStar VENC frame-lost strategy live:

| Field (live via `/api/v1/set`) | Meaning |
|---|---|
| `video0.frameLost` | enable the strategy |
| `video0.frameLostMode` | `normal` (frame not encoded — cadence gap) or `pskip` (all-skip placeholder P-frame) |
| `video0.frameLostThreshold` | trigger in kbps; `0` = auto (150% of bitrate, 512 kbps floor) |
| `video0.frameLostGap` | SDK `u32EncFrmGaps`: encode 1, skip N while tripped |

In `pskip` mode an over-threshold frame is emitted as a valid coded
picture whose macroblocks are all "skip": roughly 100–200 bytes at
1080p (~1 bit/CTU + slice header).  The H.265 reference chain and the
frame cadence stay intact — the decoder repeats the last picture.

## 2. Wire-level contract (what your controller sees)

- Every PSKIP placeholder is a complete access unit.  The waybeam RTP
  packetizer sets the **marker bit on the last packet of every access
  unit** (`src/rtp_packetizer.c`, `src/star6e_hevc_rtp.c`), so each
  placeholder arrives as **one small RTP packet with M=1** and its own
  RTP timestamp.
- During a pskip episode at N fps, the link therefore carries N tiny
  M=1 packets per second interleaved with (or replacing) real frames.
- A real 1080p P-frame at FPV bitrates essentially never fits one
  packet, so `single packet AND size < ~300 B` is a reliable
  placeholder discriminator even without explicit signalling.
- The waybeam sidecar (UDP, `outgoing.sidecarPort`) already reports per
  frame: `seq_first`, `seq_count`, RTP timestamp, and (ENC_INFO
  trailer) `frame_size_bytes` + `frame_type` — enough to map exact RTP
  seq ranges to placeholder frames with zero heuristics.

## 3. The problem for M-bit-driven FEC

With "close block on M-bit" + "size K from recent frame size":

1. **Estimator poisoning.**  A pskip storm collapses the frame-size
   average toward 1 packet; when real frames resume (or interleave via
   `frameLostGap`'s encode-1-skip-N pattern) the K-sizing loop
   oscillates between K=1 and K=real-frame every few frames.
2. **Degenerate K=1 blocks.**  Each ~150 B placeholder closes its own
   block and drags its own parity packet(s).  Byte overhead is trivial;
   **packet-rate** overhead is not — per-packet airtime dominates for
   small packets, and pskip fires exactly when the link is congested
   (at 120 fps fully tripped: ~240 extra pkt/s of data+parity).
3. **Dropping placeholders at the transport is NOT a fix.**  Suppressing
   pskip packets sender-side is semantically `normal` mode: the decoder
   is missing a picture the next P-frame lists as a reference.  Content
   is bit-identical to the previous picture so most decoders conceal it
   invisibly, but that reintroduces decoder-dependent missing-ref
   behavior — exactly what pskip exists to avoid.  Placeholders must be
   transmitted.

## 4. Proposed controller changes

### 4.1 Occupancy-gated M-bit closure (required)

Do not close the FEC block on M-bit unless the block already holds at
least `min_packets` (suggested: 2) or `min_bytes` (suggested: ~1 KB).
A placeholder AU rides in the open block and the block closes on the
next real frame's M-bit.  Keep the existing block timeout as backstop
(covers a pskip-only tail with no real frame following).

Latency argument: a placeholder is a repeat-frame — displaying it late
is imperceptible — and it is delivered in-order in the same block as
the real frame that references it, so decode order is preserved.

### 4.2 Gate the K-sizing estimator (required)

Exclude frames with `seq_count == 1 AND bytes < ~300` from the
frame-size statistic that sizes K.  FEC geometry then keeps tracking
real frames through a pskip episode and needs no re-convergence when it
ends.

### 4.3 Telemetry (recommended)

Count and expose: placeholder frames seen, blocks whose closure was
deferred by the occupancy gate, blocks closed by timeout instead of
M-bit.  These are the acceptance signals for §6.

## 5. Optional venc-side assist (available on request)

The sidecar `RtpSidecarFrame.flags` byte has unused bits (0x01
keyframe, 0x02 enc-info, 0x04 transport-info are taken).  waybeam can
set a `RTP_SIDECAR_FLAG_PSKIP` (0x08) on frames emitted while the
frame-lost strategy is active, giving the controller a zero-heuristic
placeholder signal that old probes ignore (no version bump).  Not yet
implemented — say so if wanted; the size heuristic in §2 is expected to
be sufficient.

## 6. Acceptance criteria (bench, encoder + controller together)

Setup: waybeam streaming 8192 kbps / 60 fps over wfb_ng; force pskip
with `/api/v1/set?video0.frameLostMode=pskip&video0.frameLostThreshold=2000`.

1. Placeholder frames arrive as single M=1 packets ≤ ~300 B (confirms
   the §2 contract on real hardware).
2. With §4.1+§4.2 active: FEC block rate during the pskip episode stays
   within ~10% of the pre-episode rate (no K=1 block storm), and K
   sizing returns to steady state within one block of the episode
   ending (no oscillation).
3. Decoded video stays artifact-free with frozen/slow motion during the
   episode; recovery to full motion is seamless on
   `frameLostThreshold=0`.
4. End-to-end latency of real frames is unchanged (placeholders may be
   delivered up to one frame period late by design).

## 7. Open questions (venc bench items, block §6)

- **PSKIP on hardware**: never exercised on either SoC (i6/i6c); the
  enum is SDK-declared but the emitted size/NAL layout needs
  confirmation.  Expected: one NAL, single RTP packet.
- **`frameLostGap` semantics in PSKIP mode**: whether gapped frames are
  emitted as placeholders (cadence preserved — one M=1 packet each) or
  not emitted at all (cadence gap, like `normal`).  This decides
  whether the gap knob multiplies or reduces the M-bit rate the
  controller sees.  SDK docs are ambiguous; bench first.
