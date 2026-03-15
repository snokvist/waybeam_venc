#!/bin/bash
# build.sh — automated build script for Waybeam
#
# Downloads the appropriate cross-compilation toolchain and the SigmaStar
# SDK shared libraries from the OpenIPC firmware repository, then compiles
# the streamer for the requested target.
#
# Usage:
#   ./build.sh maruko    — infinity6c  (SigmaStar Maruko)
#   ./build.sh star6e    — infinity6e  (SigmaStar Star6e / Pudding)
#
# Prerequisites: bash, wget, tar, git, make, C compiler (host)

set -euo pipefail

TARGET="${1:-}"

case "$TARGET" in
	maruko)
		FW_PKG="sigmastar-osdrv-infinity6c"
		CC_PKG="toolchain.sigmastar-infinity6c"
		;;
	star6e)
		FW_PKG="sigmastar-osdrv-infinity6e"
		CC_PKG="toolchain.sigmastar-infinity6e"
		;;
	*)
		echo "Usage: $0 [maruko|star6e]"
		echo ""
		echo "  maruko  — SigmaStar infinity6c (Maruko)"
		echo "  star6e  — SigmaStar infinity6e (Star6e / Pudding)"
		exit 1
		;;
esac

REPO_ROOT="$PWD"
TC_DIR="$REPO_ROOT/toolchain/$CC_PKG"
GCC="$TC_DIR/bin/arm-linux-gcc"
TC_URL="https://github.com/openipc/firmware/releases/download/toolchain/$CC_PKG.tgz"

# ── Toolchain ──────────────────────────────────────────────────────────────
if [ ! -x "$GCC" ]; then
	echo "==> Downloading toolchain: $CC_PKG"
	wget -c -q --show-progress "$TC_URL" -P "$REPO_ROOT"
	mkdir -p "$TC_DIR"
	tar -xf "$CC_PKG.tgz" -C "$TC_DIR" --strip-components=1
	rm -f "$CC_PKG.tgz"
fi

# ── OpenIPC firmware (SDK shared libraries) ────────────────────────────────
if [ ! -d "$REPO_ROOT/firmware" ]; then
	echo "==> Cloning OpenIPC firmware (shallow)"
	git clone --depth=1 https://github.com/openipc/firmware "$REPO_ROOT/firmware"
fi

DRV="$REPO_ROOT/firmware/general/package/$FW_PKG/files/lib"

if [ ! -d "$DRV" ]; then
	echo "ERROR: SDK library directory not found: $DRV"
	echo "The firmware clone may be incomplete.  Try removing the 'firmware'"
	echo "directory and re-running this script."
	exit 1
fi

# ── Build ──────────────────────────────────────────────────────────────────
echo "==> Building waybeam for target: $TARGET"
echo "    GCC : $GCC"
echo "    DRV : $DRV"

make -C src -B CC="$GCC" DRV="$DRV" "$TARGET"

echo ""
echo "==> Build complete: src/waybeam"
echo ""
echo "Deploy to camera:"
echo "  scp -O src/waybeam root@<camera-ip>:/tmp/"
echo ""
echo "Run on camera:"
echo "  killall majestic 2>/dev/null; /tmp/waybeam [bitrate_kbps] [host] [port]"
