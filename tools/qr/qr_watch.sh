#!/bin/sh
# qr_watch.sh — poll the waybeam grayscale snapshot and decode QR codes.
#
# The interactive counterpart to qr_boot_action.sh: no boot window, no action
# dispatch, no payload grammar.  It grabs GET /api/v1/snapshot.pgm every
# INTERVAL seconds and runs qr_decode over it, printing payloads on stdout and
# every status line on stderr, so it is safe to capture:
#
#   msg="$(qr_watch.sh)"                 # one-shot: exits on the first decode
#   qr_watch.sh -c                       # continuous: keeps scanning, streams
#
# Use one-shot to read a single code; use continuous to leave a scanner running
# while you present codes, tune focus, or check how reliably one decodes.
#
# Exit: 0 payload decoded (on stdout) | 1 no decode within the try limit
#       2 setup or usage error.  Continuous mode exits 0 if it decoded at least
#       once, and only stops on the try limit or ctrl-c.
#
# Env overrides (flags win): QR_INTERVAL_S QR_MAX_TRIES QR_ENDPOINT
#                            QR_CONTINUOUS QR_DECODE_BIN QR_TMP_PGM

set -u

INTERVAL_S="${QR_INTERVAL_S:-15}"
MAX_TRIES="${QR_MAX_TRIES:-0}"          # 0 = no limit
ENDPOINT="${QR_ENDPOINT:-http://127.0.0.1/api/v1/snapshot.pgm}"
CONTINUOUS="${QR_CONTINUOUS:-0}"        # 1 = keep scanning past the first hit
QR_DECODE="${QR_DECODE_BIN:-/usr/bin/qr_decode}"
TMP_PGM="${QR_TMP_PGM:-/tmp/qr_watch.pgm}"

log() { echo "[qr-watch] $*" >&2; }

usage() {
	cat >&2 <<EOF
usage: qr_watch.sh [-c] [-i SECONDS] [-n TRIES] [-e URL]

  -c  continuous — keep scanning and stream every decode, instead of
      exiting on the first one (ctrl-c to stop)
  -i  seconds between snapshots (default $INTERVAL_S)
  -n  stop after this many snapshots (default ${MAX_TRIES}, 0 = no limit)
  -e  snapshot endpoint (default $ENDPOINT)

Payloads go to stdout, status to stderr.
EOF
}

while getopts ':ci:n:e:h' opt; do
	case "$opt" in
	c) CONTINUOUS=1 ;;
	i) INTERVAL_S="$OPTARG" ;;
	n) MAX_TRIES="$OPTARG" ;;
	e) ENDPOINT="$OPTARG" ;;
	h) usage; exit 0 ;;
	:) log "option -$OPTARG needs a value"; usage; exit 2 ;;
	?) log "unknown option -$OPTARG"; usage; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || { log "unexpected argument: $1"; usage; exit 2; }

# Numeric gates, so a typo fails now instead of inside the arithmetic below.
for v in "$INTERVAL_S" "$MAX_TRIES"; do
	case "$v" in
	''|*[!0-9]*) log "-i and -n take non-negative integers (got '$v')"; exit 2 ;;
	esac
done

[ -x "$QR_DECODE" ] || { log "decoder $QR_DECODE not found/executable"; exit 2; }

if [ "$CONTINUOUS" = "1" ]; then
	log "continuous scan of $ENDPOINT every ${INTERVAL_S}s (ctrl-c to stop)"
else
	log "polling $ENDPOINT every ${INTERVAL_S}s (ctrl-c to stop)"
fi

try=0
hits=0
while :; do
	try=$((try + 1))

	# Keep the body on an HTTP error so the reason is visible rather than
	# looking like an empty frame: 409 = the scaler tap this capture needs
	# is owned by stab framing or NPU detection, 503 = snapshot disabled.
	code="$(curl -sS -m 5 -o "$TMP_PGM" -w '%{http_code}' "$ENDPOINT" 2>/dev/null)"

	if [ "$code" = "200" ]; then
		payload="$("$QR_DECODE" "$TMP_PGM" 2>/dev/null)"
		if [ -n "$payload" ]; then
			hits=$((hits + 1))
			log "decoded on try $try (hit $hits)"
			printf '%s\n' "$payload"
			if [ "$CONTINUOUS" != "1" ]; then
				rm -f "$TMP_PGM"
				exit 0
			fi
		else
			log "try $try: snapshot ok, no QR in frame"
		fi
	elif [ -n "$code" ] && [ "$code" != "000" ]; then
		log "try $try: HTTP $code — $(head -c 200 "$TMP_PGM" 2>/dev/null)"
	else
		log "try $try: no response from $ENDPOINT"
	fi

	if [ "$MAX_TRIES" -gt 0 ] && [ "$try" -ge "$MAX_TRIES" ]; then
		rm -f "$TMP_PGM"
		if [ "$hits" -gt 0 ]; then
			log "stopping after $try tries ($hits decoded)"
			exit 0
		fi
		log "giving up after $try tries"
		exit 1
	fi

	sleep "$INTERVAL_S"
done
