#!/usr/bin/env python3
"""HEVC ES bitstream analyzer for refPred verification.

Two subcommands:

  walk  <file>         Histogram NAL types and temporal_id distribution.
  drop  <file> <rate>  Stream NALs to stdout, dropping <rate> fraction of
                       non-IDR / non-parameter-set NALs.  Deterministic via
                       --seed (default 1).

HEVC NAL header (2 bytes after start code):
  byte0: forbidden_zero_bit(1) | nal_unit_type(6) | nuh_layer_id_msb(1)
  byte1: nuh_layer_id_lsb(5)   | nuh_temporal_id_plus1(3)
"""

from __future__ import annotations

import argparse
import os
import random
import sys
from collections import Counter
from typing import Iterator, Tuple


# HEVC NAL types (subset that matters here)
NAL_TRAIL_N    = 0
NAL_TRAIL_R    = 1
NAL_TSA_N      = 2
NAL_TSA_R      = 3
NAL_RASL_R     = 9
NAL_BLA_W_LP   = 16
NAL_IDR_W_RADL = 19
NAL_IDR_N_LP   = 20
NAL_CRA_NUT    = 21
NAL_VPS        = 32
NAL_SPS        = 33
NAL_PPS        = 34
NAL_AUD        = 35
NAL_PREFIX_SEI = 39
NAL_SUFFIX_SEI = 40

NAL_NAMES = {
    0: "TRAIL_N", 1: "TRAIL_R", 2: "TSA_N", 3: "TSA_R",
    4: "STSA_N", 5: "STSA_R", 6: "RADL_N", 7: "RADL_R",
    8: "RASL_N", 9: "RASL_R",
    16: "BLA_W_LP", 17: "BLA_W_RADL", 18: "BLA_N_LP",
    19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA",
    32: "VPS", 33: "SPS", 34: "PPS", 35: "AUD",
    36: "EOS", 37: "EOB", 38: "FD",
    39: "PREFIX_SEI", 40: "SUFFIX_SEI",
}

PARAM_OR_IDR = {NAL_VPS, NAL_SPS, NAL_PPS,
                NAL_IDR_W_RADL, NAL_IDR_N_LP, NAL_CRA_NUT,
                NAL_BLA_W_LP, NAL_AUD}


def iter_nals(data: bytes) -> Iterator[Tuple[int, int, bytes]]:
    """Yield (start_offset, end_offset, nal_bytes) for each NAL.

    Splits on 00 00 00 01 / 00 00 01 start codes.  nal_bytes excludes the
    start code prefix.
    """
    n = len(data)
    i = 0
    starts = []
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, i + 3))
                i += 3
                continue
            if data[i + 2] == 0 and i + 3 < n and data[i + 3] == 1:
                starts.append((i, i + 4))
                i += 4
                continue
        i += 1
    for idx, (sc_begin, payload_begin) in enumerate(starts):
        if idx + 1 < len(starts):
            payload_end = starts[idx + 1][0]
        else:
            payload_end = n
        yield sc_begin, payload_end, data[payload_begin:payload_end]


def parse_header(nal: bytes) -> Tuple[int, int, int]:
    """Return (nal_type, layer_id, temporal_id)."""
    if len(nal) < 2:
        return -1, -1, -1
    b0, b1 = nal[0], nal[1]
    nal_type = (b0 >> 1) & 0x3f
    layer_id = ((b0 & 0x01) << 5) | ((b1 >> 3) & 0x1f)
    tid_p1 = b1 & 0x07
    return nal_type, layer_id, tid_p1 - 1


def cmd_walk(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()
    by_type: Counter[int] = Counter()
    by_tid: Counter[int] = Counter()
    pair: Counter[Tuple[int, int]] = Counter()
    total = 0
    for _, _, nal in iter_nals(data):
        nal_type, _, tid = parse_header(nal)
        if nal_type < 0:
            continue
        total += 1
        by_type[nal_type] += 1
        by_tid[tid] += 1
        pair[(nal_type, tid)] += 1

    print(f"file: {path}  size: {len(data)} B  nals: {total}")
    print()
    print(f"{'nal_type':>3}  {'name':<14}  {'count':>8}")
    for t, c in sorted(by_type.items()):
        print(f"{t:>3}  {NAL_NAMES.get(t, '?'):<14}  {c:>8}")
    print()
    print(f"{'temporal_id':>11}  {'count':>8}")
    for tid in sorted(by_tid):
        print(f"{tid:>11}  {by_tid[tid]:>8}")
    print()
    print(f"{'nal_type':>3}  {'name':<14}  {'tid':>3}  {'count':>8}")
    for (t, tid), c in sorted(pair.items()):
        print(f"{t:>3}  {NAL_NAMES.get(t, '?'):<14}  {tid:>3}  {c:>8}")
    return 0


def cmd_drop(path: str, rate: float, seed: int, warmup: int) -> int:
    rng = random.Random(seed)
    with open(path, "rb") as f:
        data = f.read()
    out = sys.stdout.buffer
    seen_idr = 0
    droppable = 0
    dropped = 0
    for sc_begin, end, nal in iter_nals(data):
        nal_type, _, _ = parse_header(nal)
        is_param_or_idr = nal_type in PARAM_OR_IDR
        if nal_type in (NAL_IDR_W_RADL, NAL_IDR_N_LP, NAL_CRA_NUT):
            seen_idr += 1

        # Reconstruct the on-wire bytes including start code.
        # Look at the start code length: 3 (00 00 01) or 4 (00 00 00 01).
        if sc_begin + 3 < len(data) and data[sc_begin + 2] == 0:
            sc_len = 4
        else:
            sc_len = 3
        unit = data[sc_begin:end]

        if is_param_or_idr or seen_idr == 0:
            out.write(unit)
            continue
        # Warmup: keep the first <warmup> non-param NALs after the first IDR
        # so the decoder reaches steady state.
        if droppable < warmup:
            droppable += 1
            out.write(unit)
            continue
        droppable += 1
        if rng.random() < rate:
            dropped += 1
            continue
        out.write(unit)

    sys.stderr.write(
        f"hevc_analyze drop: rate={rate} seed={seed} warmup={warmup} "
        f"droppable_seen={droppable} dropped={dropped}\n")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    pw = sub.add_parser("walk", help="histogram NAL types/temporal_ids")
    pw.add_argument("file")

    pd = sub.add_parser("drop", help="randomly drop NALs to stdout")
    pd.add_argument("file")
    pd.add_argument("rate", type=float, help="0.0..1.0 drop probability")
    pd.add_argument("--seed", type=int, default=1)
    pd.add_argument("--warmup", type=int, default=60,
        help="keep first N droppable NALs after first IDR (default 60)")

    args = ap.parse_args()
    if args.cmd == "walk":
        return cmd_walk(args.file)
    if args.cmd == "drop":
        return cmd_drop(args.file, args.rate, args.seed, args.warmup)
    return 1


if __name__ == "__main__":
    sys.exit(main())
