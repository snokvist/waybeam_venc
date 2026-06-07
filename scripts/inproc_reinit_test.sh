#!/usr/bin/env bash
set -uo pipefail

# In-process reinit vs. fork+exec respawn — on-device test driver.
#
# Companion to documentation/INPROC_REINIT_TEST_PLAN.md.  Runs the
# validation matrix against a live Star6E bench (default root@192.168.1.13).
# This script runs on the host that has SSH access to the device; it does
# NOT run on the device itself.
#
# Default deploy starts waybeam with VENC_INPROC_REINIT=1 (the experiment);
# pass `--respawn` to start the proven fork+exec control instead.

HOST="${HOST:-root@192.168.1.13}"
PORT="${PORT:-80}"
LOCAL_BIN="${LOCAL_BIN:-out/star6e/waybeam}"
REMOTE_BIN="/usr/bin/waybeam"
CONFIG_PATH="/etc/waybeam.json"
LOG_PATH="/tmp/waybeam.log"
HTTP_WAIT_SECS="${HTTP_WAIT_SECS:-40}"   # cold init can take ~13s+
INPROC=1                                  # 1=experiment, 0=respawn control

# dmesg keywords that mean the reinit wedged the SoC / driver.
DMESG_BAD='MMU|not sync|CamOsMutexLock|vpe0_P0_MAIN|fault|oops|panic|watchdog|segmentation'

# ── ssh / device helpers ──────────────────────────────────────────────────

dev() { ssh -o ConnectTimeout=5 -o BatchMode=yes "$HOST" "$@"; }

dev_alive() { dev true >/dev/null 2>&1; }

api() { dev "wget -q -T 8 -O- 'http://127.0.0.1:${PORT}$1' 2>/dev/null"; }

# PID of the *main* waybeam (comm exactly "waybeam" — excludes the
# -wd / -resp / -rwd helper forks that share the same exe).
main_pid() {
	dev 'for p in $(pidof waybeam 2>/dev/null); do
		[ "$(cat /proc/$p/comm 2>/dev/null)" = waybeam ] && { echo "$p"; break; }
	done'
}

http_up() { [ -n "$(api /api/v1/config)" ]; }

# Block until HTTP answers again (after a reinit) or the cap elapses.
# Returns 0 if up, 1 on timeout.
wait_http() {
	local i=0
	while [ "$i" -lt "$HTTP_WAIT_SECS" ]; do
		if ! dev_alive; then return 2; fi   # device unreachable
		if http_up; then return 0; fi
		sleep 1; i=$((i + 1))
	done
	return 1
}

fd_count() { dev "ls /proc/$1/fd 2>/dev/null | wc -l"; }
rss_kb()   { dev "awk '/VmRSS/{print \$2}' /proc/$1/status 2>/dev/null"; }

dmesg_clear() { dev "dmesg -c >/dev/null 2>&1 || true"; }
dmesg_bad()   { dev "dmesg 2>/dev/null" | grep -iE "$DMESG_BAD" || true; }

say()  { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# Abort the run if the device stopped answering SSH — never hammer a
# wedged board (AGENTS device_unresponsive rule).
guard_alive() {
	dev_alive || fail "device unreachable over SSH — likely wedged.
  Recover: power-cycle, or 'echo b > /proc/sysrq-trigger' from a fresh
  SSH session if one is still hot.  Log the transition in CRASH_LOG.md."
}

# ── commands ──────────────────────────────────────────────────────────────

cmd_deploy() {
	[ -f "$LOCAL_BIN" ] || fail "missing $LOCAL_BIN — run 'make build SOC_BUILD=star6e'"
	local envstr=""
	[ "$INPROC" = 1 ] && envstr="VENC_INPROC_REINIT=1"
	say "deploy ($([ "$INPROC" = 1 ] && echo in-process || echo respawn-control)) to $HOST"
	guard_alive
	dev "killall waybeam waybeam-wd waybeam-resp waybeam-rwd 2>/dev/null; sleep 2" || true
	scp -O "$LOCAL_BIN" "$HOST:$REMOTE_BIN" || fail "scp failed"
	dev "$envstr nohup $REMOTE_BIN > $LOG_PATH 2>&1 &" || fail "launch failed"
	if wait_http; then
		printf 'OK  HTTP up, main pid %s\n' "$(main_pid)"
	else
		dev "tail -40 $LOG_PATH" || true
		fail "HTTP did not come up within ${HTTP_WAIT_SECS}s"
	fi
}

# One reinit via /api/v1/restart (pure reload+rebuild, same config).
# Echoes the post-reinit main pid; non-zero exit means wedge/timeout.
one_reinit() {
	api /api/v1/restart >/dev/null 2>&1 || true   # returns before rebuild
	case "$(wait_http; echo $?)" in
		0) main_pid ;;
		2) guard_alive ;;                          # exits
		*) return 1 ;;
	esac
}

# Step 1 + 2: same-mode storm / soak via repeated /api/v1/restart.
# Usage: storm <count>
cmd_storm() {
	local n="${1:-10}" i pid0 pid
	pid0="$(main_pid)"; [ -n "$pid0" ] || fail "waybeam not running — deploy first"
	say "same-mode storm: $n reinits, baseline pid $pid0"
	dmesg_clear
	for i in $(seq 1 "$n"); do
		pid="$(one_reinit)" || fail "reinit #$i: HTTP did not return (wedge?).
  Check 'ssh $HOST tail -60 $LOG_PATH' and dmesg.  Log in CRASH_LOG.md."
		if [ "$INPROC" = 1 ] && [ "$pid" != "$pid0" ]; then
			fail "reinit #$i: pid changed $pid0 -> $pid.
  In-process mode must keep a STATIC pid.  Either VENC_INPROC_REINIT was
  not set, or the process exited and something restarted it."
		fi
		printf '  reinit %3d/%d  pid=%s%s\n' "$i" "$n" "$pid" \
			"$([ "$pid" = "$pid0" ] && echo ' (static)' || echo ' (NEW)')"
	done
	local bad; bad="$(dmesg_bad)"
	[ -z "$bad" ] || fail "dmesg shows fault keywords after storm:
$bad"
	printf 'PASS  %d reinits, pid %s, dmesg clean\n' "$n" \
		"$([ "$INPROC" = 1 ] && echo "static $pid0" || echo "rotated")"
}

# Step 4: fd / RSS soak.  Usage: soak <count>
cmd_soak() {
	local n="${1:-200}" i pid0 pid fd0 rss0 fd rss
	pid0="$(main_pid)"; [ -n "$pid0" ] || fail "waybeam not running — deploy first"
	fd0="$(fd_count "$pid0")"; rss0="$(rss_kb "$pid0")"
	say "fd/RSS soak: $n reinits (pid $pid0, fd0=$fd0, rss0=${rss0}kB)"
	dmesg_clear
	for i in $(seq 1 "$n"); do
		pid="$(one_reinit)" || fail "reinit #$i: HTTP did not return (wedge?)"
		[ "$INPROC" != 1 ] || [ "$pid" = "$pid0" ] || \
			fail "reinit #$i: pid changed $pid0 -> $pid (expected static)"
		if [ $((i % 10)) -eq 0 ]; then
			fd="$(fd_count "$pid")"; rss="$(rss_kb "$pid")"
			printf '  %4d/%d  fd=%s (Δ%+d)  rss=%skB (Δ%+d)\n' \
				"$i" "$n" "$fd" "$((fd - fd0))" "$rss" "$((rss - rss0))"
		fi
	done
	fd="$(fd_count "$pid0")"; rss="$(rss_kb "$pid0")"
	local bad; bad="$(dmesg_bad)"
	[ -z "$bad" ] || fail "dmesg fault keywords after soak:
$bad"
	printf 'DONE  %d reinits.  fd %s->%s (Δ%+d)  rss %s->%s kB (Δ%+d)\n' \
		"$n" "$fd0" "$fd" "$((fd - fd0))" "$rss0" "$rss" "$((rss - rss0))"
	printf 'Interpret: a bounded fd/RSS delta = PASS; steady growth ~1/reinit\n'
	printf '           toward RLIMIT_NOFILE (1024) = the static-PID fd leak.\n'
}

# Step 3: cross-mode (size) rotation.  Usage: crossmode <rounds> <size> [size...]
# Sizes must be valid imx335 sensor-mode resolutions — confirm with
# --list-sensor-modes first.  WxH form, e.g. 1920x1080.
cmd_crossmode() {
	local rounds="${1:-3}"; shift || true
	local sizes=("$@")
	[ "${#sizes[@]}" -ge 2 ] || fail "give >=2 sizes, e.g. crossmode 3 1920x1080 1280x720"
	local pid0; pid0="$(main_pid)"; [ -n "$pid0" ] || fail "waybeam not running"
	say "cross-mode rotation: $rounds rounds over ${sizes[*]} (pid $pid0)"
	dmesg_clear
	local r s pid
	for r in $(seq 1 "$rounds"); do
		for s in "${sizes[@]}"; do
			dev "json_cli -s .video0.size '\"$s\"' -i $CONFIG_PATH" \
				|| fail "json_cli set size=$s failed"
			pid="$(one_reinit)" || fail "round $r size $s: HTTP did not return (wedge?)"
			if [ "$INPROC" = 1 ] && [ "$pid" != "$pid0" ]; then
				fail "round $r size $s: pid changed $pid0 -> $pid"
			fi
			printf '  round %d  size %-11s  pid=%s\n' "$r" "$s" "$pid"
		done
	done
	local bad; bad="$(dmesg_bad)"
	[ -z "$bad" ] || fail "dmesg fault keywords after cross-mode:
$bad"
	printf 'PASS  %d rounds over %d sizes, dmesg clean\n' "$rounds" "${#sizes[@]}"
}

cmd_status() {
	guard_alive
	local pid; pid="$(main_pid)"
	if [ -z "$pid" ]; then echo "waybeam: NOT running"; return 1; fi
	printf 'waybeam main pid: %s\n' "$pid"
	printf 'fd count       : %s\n' "$(fd_count "$pid")"
	printf 'VmRSS          : %s kB\n' "$(rss_kb "$pid")"
	printf 'HTTP           : %s\n' "$(http_up && echo up || echo DOWN)"
	printf 'helper forks   : %s\n' \
		"$(dev 'pidof waybeam-wd waybeam-resp waybeam-rwd 2>/dev/null' || echo none)"
}

usage() {
	cat <<EOF
Usage: HOST=root@192.168.1.13 scripts/inproc_reinit_test.sh [--respawn] <command>

Options:
  --respawn        Deploy/test the fork+exec control (no VENC_INPROC_REINIT)
  HOST, PORT       env overrides (default root@192.168.1.13 : 80)

Commands:
  deploy                       Build must exist; (re)deploy + start + wait HTTP
  status                       Show pid, fd count, RSS, HTTP, helper forks
  storm <n>                    Step 1/2: n same-mode reinits, assert static pid
  soak <n>                     Step 4: n reinits, sample fd/RSS growth
  crossmode <rounds> <s1> <s2>...  Step 3: rotate video0.size across sizes
  all                          deploy + storm 10 + storm 50 + soak 200

Typical session (in-process experiment):
  make build SOC_BUILD=star6e
  scripts/inproc_reinit_test.sh deploy
  scripts/inproc_reinit_test.sh storm 10      # the gate (failure mode #3)
  scripts/inproc_reinit_test.sh soak 200      # fd/RSS leak check
  scripts/inproc_reinit_test.sh crossmode 3 1920x1080 1280x720

Control comparison (same steps, proven path):
  scripts/inproc_reinit_test.sh --respawn deploy
  scripts/inproc_reinit_test.sh --respawn storm 10
EOF
}

main() {
	if [ "${1:-}" = "--respawn" ]; then INPROC=0; shift; fi
	local cmd="${1:-}"; shift || true
	case "$cmd" in
		deploy)    cmd_deploy ;;
		status)    cmd_status ;;
		storm)     cmd_storm "$@" ;;
		soak)      cmd_soak "$@" ;;
		crossmode) cmd_crossmode "$@" ;;
		all)       cmd_deploy; cmd_storm 10; cmd_storm 50; cmd_soak 200 ;;
		""|-h|--help|help) usage ;;
		*) usage; exit 1 ;;
	esac
}

main "$@"
