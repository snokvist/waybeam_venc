# Boot-time QR scan (waybeam-link pairing)

On-device consumer for waybeam's grayscale snapshot endpoint. For the first
seconds of the camera's life it looks for a QR code held in front of the lens
and, on a valid payload, applies an action — the MVP being **RF-link pairing**:
the QR carries a ground station's pre-shared key, the vehicle scans it, and the
two share a link. Security is by proximity — whoever can put a QR in front of
the camera during the boot window can pair.

No NPU / detector is involved: a QR code carries its own finder patterns, so
quirc locates and decodes it directly from the full grayscale frame.

## Pieces

| File | Role |
|------|------|
| `qr_decode.c` | Reads a P5 PGM, decodes the first QR code, prints its payload. Exit 0 = decoded, 1 = none, 2 = input error. |
| `quirc/` | Vendored quirc QR library (ISC; see `quirc/LICENSE`). |
| `qr_boot_action.sh` | rc.local scanner: polls `snapshot.pgm`, decodes, dispatches. |
| `qr_watch.sh` | Interactive scanner: polls until a QR decodes, prints the payload, exits. |

`qr_decode` is tuned for real captures: each candidate is retried mirror-flipped
(flipped codes), decoding is attempted over the full frame, overlapping tiles,
and a half-scale copy (small codes in a large frame), a light-denoised copy is
tried as a fallback (noisy captures), and finally one inverted pass (light-on-
dark codes). The first success wins, so a clean code still returns on the first
pass.

No contrast-stretch / gamma / extra-binarization pass is applied: quirc's own
adaptive threshold already normalises local contrast and brightness — measured,
it decodes delta-10 low-contrast and near-black frames unaided — so such a pass
adds cost with no gain. Don't add one.

**Resolution floor (important):** these passes fix orientation, threshold, and
noise — they do **not** add resolution. A QR whose modules are only ~2–3 px in
the captured frame is at quirc's detection limit and may not decode regardless.
The real levers there are more pixels on the code: present the QR larger/closer,
or capture at a higher `snapshot`/main-stream resolution. If small-code
reliability is still short after that, a heavier decoder (zbar) detects small/
noisy codes better than quirc but is a larger dependency to cross-compile.

The firmware half is just `GET /api/v1/snapshot.pgm` (the Y plane of the same
NV12 source the JPEG snapshot comes from; gated by `snapshot.enabled`). See
`documentation/HTTP_API_CONTRACT.md`.

## Build

```bash
make qr-decode          # -> out/<soc>/qr_decode (cross-compiled for the target)
```

## Deploy

```bash
scp out/star6e/qr_decode        root@<dev>:/usr/bin/qr_decode
scp tools/qr/qr_boot_action.sh  root@<dev>:/usr/bin/qr_boot_action.sh
scp tools/qr/qr_watch.sh        root@<dev>:/usr/bin/qr_watch.sh
```

Then invoke it early from `/etc/rc.local` (after waybeam starts), e.g.:

```sh
/usr/bin/qr_boot_action.sh >> /tmp/qr_boot.log 2>&1 &
```

`snapshot.enabled` must be true in `/etc/waybeam.json`. The script tolerates
the endpoint returning `503` while the pipeline is still coming up.

## Interactive scanning

`qr_watch.sh` is the bench counterpart — no boot window, no payload grammar, no
action. It polls until something decodes, prints the payload, and exits:

```sh
qr_watch.sh                          # every 15 s until a QR decodes
msg="$(qr_watch.sh)"                 # payload on stdout, status on stderr
qr_watch.sh -c -i 2                  # continuous: stream every decode
qr_watch.sh -c -i 2 | while read -r p; do echo "saw: $p"; done
```

`-c` keeps scanning past the first hit and prints one payload per line, for
leaving a scanner running while you present codes, tune focus, or check how
reliably one decodes. Without it the first decode exits.

Flags: `-c` continuous, `-i` interval seconds, `-n` stop after N snapshots,
`-e` endpoint, `-h` usage. Exit 0 = decoded at least once, 1 = hit `-n` with no
decode (default 0 = never give up), 2 = setup or usage error.

It reports the HTTP status rather than retrying blind, which matters because
`409` (the scaler tap is owned by stab framing or NPU detection) and `503`
(`snapshot.enabled` false) would otherwise look like an empty frame.

A miss costs more than a hit — `qr_decode` runs its full pass chain before
giving up (~0.7 s vs ~0.1 s on a 1280x720 frame on an SSC338Q), so keep the
interval well clear of that.

Tunables (env, flags win): `QR_ENDPOINT`, `QR_INTERVAL_S` (15), `QR_MAX_TRIES`
(0), `QR_CONTINUOUS` (0), `QR_DECODE_BIN`, `QR_TMP_PGM`.

## Payload format

```
cmd=pair;gs=<ground-station-id>;psk=<link-key>
```

`cmd` is the only whitelisted verb (`pair` in the MVP); `psk` must be 4–32
alphanumeric chars; `gs` is an optional label. Unknown commands and malformed
payloads are logged and ignored, and scanning continues until the window closes.

## Integration point

`apply_pairing()` in `qr_boot_action.sh` is where the key meets waybeam-link.
The link's key store lives outside this repo, so the default just writes the key
to `$QR_PSK_OUT` and logs. Either edit `apply_pairing()` to call your link
tooling (write key + restart the wfb/link service), or drop an executable
`/etc/waybeam/qr_pair_hook.sh <gs> <psk>` — if present it owns the apply.

Tunables (env): `QR_ENDPOINT`, `QR_WINDOW_S` (15), `QR_INTERVAL_S` (1),
`QR_DECODE_BIN`, `QR_PSK_OUT`, `QR_PAIR_HOOK`.

## Extending

The dispatcher is a `case "$cmd"` — add verbs (e.g. a preset/reset) as new
branches with their own validation and action. Pairing is only the first.

Note on the trust model: a static PSK in a QR anyone can photograph is not auth
against someone who has *seen* it — proximity to the boot-window feed is the
boundary. For replay-resistance, extend the payload to an HMAC-over-timestamp
scheme; the vehicle side parses the same `key=value` format.
