#!/bin/sh
# qr_watch.sh — poll the waybeam grayscale snapshot until a QR code decodes.
#
# The interactive counterpart to qr_boot_action.sh: no boot window, no action
# dispatch, no payload grammar.  It grabs GET /api/v1/snapshot.pgm every
# INTERVAL seconds, runs qr_decode over it, and exits as soon as one decodes —
# printing the payload on stdout and every status line on stderr, so it is safe
# to capture:
#
#   msg="$(qr_watch.sh)"
#
# Use it to check what a code actually carries, to confirm a print is legible
# at working distance, or to test the endpoint without rebooting into the
# pairing window.
#
# Exit: 0 payload decoded (on stdout) | 1 gave up after QR_MAX_TRIES | 2 setup error
#
# Env overrides: QR_INTERVAL_S QR_MAX_TRIES QR_ENDPOINT QR_DECODE_BIN QR_TMP_PGM

set -u

INTERVAL_S="${QR_INTERVAL_S:-15}"
MAX_TRIES="${QR_MAX_TRIES:-0}"          # 0 = keep going until it decodes
ENDPOINT="${QR_ENDPOINT:-http://127.0.0.1/api/v1/snapshot.pgm}"
QR_DECODE="${QR_DECODE_BIN:-/usr/bin/qr_decode}"
TMP_PGM="${QR_TMP_PGM:-/tmp/qr_watch.pgm}"

log() { echo "[qr-watch] $*" >&2; }

[ -x "$QR_DECODE" ] || { log "decoder $QR_DECODE not found/executable"; exit 2; }

log "polling $ENDPOINT every ${INTERVAL_S}s (ctrl-c to stop)"

try=0
while :; do
	try=$((try + 1))

	# Keep the body on an HTTP error so the reason is visible rather than
	# looking like an empty frame: 409 = the scaler tap this capture needs
	# is owned by stab framing or NPU detection, 503 = snapshot disabled.
	code="$(curl -sS -m 5 -o "$TMP_PGM" -w '%{http_code}' "$ENDPOINT" 2>/dev/null)"

	if [ "$code" = "200" ]; then
		payload="$("$QR_DECODE" "$TMP_PGM" 2>/dev/null)"
		if [ -n "$payload" ]; then
			log "decoded on try $try"
			printf '%s\n' "$payload"
			rm -f "$TMP_PGM"
			exit 0
		fi
		log "try $try: snapshot ok, no QR in frame"
	elif [ -n "$code" ] && [ "$code" != "000" ]; then
		log "try $try: HTTP $code — $(head -c 200 "$TMP_PGM" 2>/dev/null)"
	else
		log "try $try: no response from $ENDPOINT"
	fi

	if [ "$MAX_TRIES" -gt 0 ] && [ "$try" -ge "$MAX_TRIES" ]; then
		rm -f "$TMP_PGM"
		log "giving up after $try tries"
		exit 1
	fi

	sleep "$INTERVAL_S"
done
