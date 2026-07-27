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
QR_INTERVAL_S=2 QR_MAX_TRIES=5 qr_watch.sh
```

Exit 0 = decoded, 1 = hit `QR_MAX_TRIES` (default 0 = never give up), 2 = setup
error. It reports the HTTP status rather than retrying blind, which matters
because `409` (the scaler tap is owned by stab framing or NPU detection) and
`503` (`snapshot.enabled` false) would otherwise look like an empty frame.

Tunables (env): `QR_ENDPOINT`, `QR_INTERVAL_S` (15), `QR_MAX_TRIES` (0),
`QR_DECODE_BIN`, `QR_TMP_PGM`.

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
