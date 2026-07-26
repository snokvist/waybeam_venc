#!/bin/sh
# qr_boot_action.sh — boot-time QR scan + action dispatcher for waybeam.
#
# For the first N seconds of the camera's life, poll the waybeam grayscale
# snapshot endpoint once per second, decode any QR code with qr_decode, and
# act on the first VALID payload.  Intended to be launched from rc.local.
#
# MVP action: pair the waybeam-link RF link.  The QR delivers the ground
# station's pre-shared key; the vehicle applies it so the two share a link.
#
# Trust model: SECURITY BY PROXIMITY.  There is no local allow-list of keys —
# accepting a new ground station's key is the whole point.  The only trust
# boundary is physical: whoever can hold a QR in front of the camera during
# this short boot window can pair.  Keep the window short, and treat access to
# the drone's video feed in its first seconds as equivalent to pairing rights.
#
# Payload format (parsed here; the firmware only serves raw grayscale):
#   cmd=<action>;gs=<ground-station-id>;psk=<link-key>
# e.g.
#   cmd=pair;gs=fpv-groundstation-01;psk=7f3a9c2b5e1d
#
# Unknown commands and malformed payloads are logged and ignored; scanning
# continues until a valid one lands or the window closes.
#
# Exit status: 0 if a pairing was applied, 1 if the window closed with none.

set -u

# ── Tunables (override via environment) ──────────────────────────────────────
ENDPOINT="${QR_ENDPOINT:-http://127.0.0.1/api/v1/snapshot.pgm}"
WINDOW_S="${QR_WINDOW_S:-15}"          # scan for this many seconds
INTERVAL_S="${QR_INTERVAL_S:-1}"       # seconds between snapshots
QR_DECODE="${QR_DECODE_BIN:-/usr/bin/qr_decode}"
TMP_PGM="${QR_TMP_PGM:-/tmp/qr_snap.pgm}"
# Where the pairing hook / default writer put the accepted key:
PSK_OUT="${QR_PSK_OUT:-/tmp/waybeam_link.psk}"
# Optional site integration hook; if present + executable it OWNS the apply and
# is called as:  qr_pair_hook.sh <gs-id> <psk>
PAIR_HOOK="${QR_PAIR_HOOK:-/etc/waybeam/qr_pair_hook.sh}"

log() { echo "[qr-boot] $*" >&2; }

# ── Payload helpers ─────────────────────────────────────────────────────────
# Extract one "key=value" field from a ';'-separated payload.
payload_field() {
	# $1 = key, $2 = payload
	printf '%s\n' "$2" | tr ';' '\n' | grep "^$1=" | head -n1 | cut -d= -f2-
}

# Charset/length gates.  psk: 4-32 chars, [0-9A-Za-z]; gs: 1-32, [0-9A-Za-z_-].
valid_psk() { printf '%s' "$1" | grep -qE '^[0-9A-Za-z]{4,32}$'; }
valid_gs()  { printf '%s' "$1" | grep -qE '^[0-9A-Za-z_-]{1,32}$'; }

# ── The action: apply a link pairing ────────────────────────────────────────
# INTEGRATION POINT.  waybeam-link's key store lives outside this repo, so the
# default here just records the key and hands off to a site hook if one exists.
# Wire apply_pairing() to your link tooling (e.g. write the key, then restart
# the wfb/link service).
apply_pairing() {
	gs="$1"; psk="$2"
	if [ -x "$PAIR_HOOK" ]; then
		log "pairing gs=$gs via hook $PAIR_HOOK"
		"$PAIR_HOOK" "$gs" "$psk"
		return $?
	fi
	# Default: persist the key where the link layer can pick it up + log.
	# --- replace the next two lines with your waybeam-link apply/restart ---
	printf '%s\n' "$psk" > "$PSK_OUT" || return 1
	log "pairing gs=$gs: wrote key to $PSK_OUT (no $PAIR_HOOK; wire this up)"
	return 0
}

# ── Dispatch one decoded payload; return 0 if a valid action was taken ───────
dispatch() {
	payload="$1"
	cmd="$(payload_field cmd "$payload")"
	case "$cmd" in
	pair)
		gs="$(payload_field gs "$payload")"
		psk="$(payload_field psk "$payload")"
		if ! valid_psk "$psk"; then
			log "ignoring pair: bad psk (need 4-32 alnum)"
			return 1
		fi
		if [ -n "$gs" ] && ! valid_gs "$gs"; then
			log "ignoring pair: bad gs id"
			return 1
		fi
		[ -n "$gs" ] || gs="unknown"
		apply_pairing "$gs" "$psk"
		return $?
		;;
	"")
		log "ignoring payload: no cmd= field"
		return 1
		;;
	*)
		log "ignoring unsupported cmd=$cmd"
		return 1
		;;
	esac
}

# ── Scan loop ────────────────────────────────────────────────────────────────
main() {
	if [ ! -x "$QR_DECODE" ]; then
		log "decoder $QR_DECODE not found/executable; aborting"
		return 1
	fi
	log "scanning $ENDPOINT for ${WINDOW_S}s (every ${INTERVAL_S}s)"

	i=0
	while [ "$i" -lt "$WINDOW_S" ]; do
		i=$((i + INTERVAL_S))
		# Fetch a fresh grayscale snapshot.  wget is the busybox default on
		# OpenIPC; a 503 (pipeline still starting) just fails this round.
		if wget -q -T 3 -O "$TMP_PGM" "$ENDPOINT" 2>/dev/null; then
			payload="$("$QR_DECODE" "$TMP_PGM" 2>/dev/null)"
			if [ -n "$payload" ]; then
				log "decoded: $payload"
				if dispatch "$payload"; then
					log "action applied; done"
					rm -f "$TMP_PGM"
					return 0
				fi
			fi
		fi
		sleep "$INTERVAL_S"
	done

	rm -f "$TMP_PGM"
	log "window closed; no valid QR pairing"
	return 1
}

main "$@"
