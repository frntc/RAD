#!/usr/bin/env bash
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
#
# release.sh - build everything a GitHub release contains.
#
#   tools/release.sh v1.0 --kernel /path/to/kernel_rad.img
#
# Output, in out/release/<version>/ (upload these to the release):
#
#   kernel_rad.img                    the firmware (as given with --kernel)
#   gpu64-<ver>-sdcard.zip            unzip onto the RAD SD card: the firmware,
#                                     every program in RAD_PRG/, REU images in REU/
#   gpu64-examples.d64                the examples, simple to a spinning cube
#   gpu64-probes.d64                  hardware probes and the conformance suite
#   gpu64-nighthawk.d64  gpu64-stunts.d64  gpu64-quake.d64
#   SHA256SUMS
#
# Every program is on a .d64 AND in the SD card's RAD_PRG/. Programs that need
# a REU image (stunts, quake) have it in the SD card's REU/ only.
#
# Options:
#   --kernel FILE      firmware image to ship. Without it, if the firmware
#                      tree is present, it is built with tools/build.sh.
#   --sd-base DIR      a RAD SD card base (bootloader, config.txt, ...) to merge
#                      under the overlay, giving a complete card. Optional.
#   --with-quake-data  include build/quake.reu. It is converted from Quake's
#                      game data: only do this if you are sure you may
#                      redistribute it. OFF by default.
#
# Release builds are reproducible from the repo alone: games are assembled in
# a scratch copy so a developer's own title tunes (games/*/gpu64_tune.bin)
# can never leak into a release; a silent stub is used instead.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=""; KERNEL=""; SDBASE=""; WITHQUAKE=0
while [ $# -gt 0 ]; do
	case "$1" in
	--kernel) KERNEL="$2"; shift 2;;
	--sd-base) SDBASE="$2"; shift 2;;
	--with-quake-data) WITHQUAKE=1; shift;;
	-h|--help) sed -n 2,34p "$0"; exit 0;;
	-*) echo "unknown option $1" >&2; exit 2;;
	*) VERSION="$1"; shift;;
	esac
done
[ -n "$VERSION" ] || { echo "usage: tools/release.sh <version> [--kernel FILE] [--sd-base DIR] [--with-quake-data]" >&2; exit 2; }
PY="${PYTHON:-python3}"
for t in 64tass c1541 zip rsync "$PY"; do
	command -v "$t" >/dev/null || { echo "error: $t not found" >&2; exit 1; }
done

"$PY" -c "import numpy" 2>/dev/null || { echo "error: $PY needs numpy (stunts generator). Try: python3 -m venv out/venv && out/venv/bin/pip install numpy && PYTHON=out/venv/bin/python tools/release.sh ..." >&2; exit 1; }

OUT="$REPO/out/release/$VERSION"
WORK="$REPO/out/release-work"
rm -rf "$OUT" "$WORK"
mkdir -p "$OUT" "$WORK/prg" "$WORK/src" "$WORK/sd/RAD_PRG" "$WORK/sd/REU"

# ---- firmware ---------------------------------------------------------
if [ -z "$KERNEL" ]; then
	if [ -x "$REPO/tools/build.sh" ] && [ -d "$REPO/Source/Firmware" ]; then
		echo "==> building firmware"
		"$REPO/tools/build.sh"
		KERNEL="$REPO/external/Circle/app/Firmware/kernel8.img"
	else
		echo "error: no firmware. Pass --kernel /path/to/kernel_rad.img" >&2; exit 1
	fi
fi
[ -f "$KERNEL" ] || { echo "error: $KERNEL not found" >&2; exit 1; }
cp "$KERNEL" "$OUT/kernel_rad.img"

# ---- assemble ---------------------------------------------------------
# asm <srcdir> <name> : assemble srcdir/gpu64_<name>.a into $WORK/prg/<name>.prg
asm() {
	local dir="$1" src="$2" name="$3"
	( cd "$dir" && 64tass --cbm-prg -I "$REPO/sdk" -o "$WORK/prg/$name.prg" "$src" >"$WORK/asm.log" 2>&1 ) \
		|| { echo "ASSEMBLE FAIL $name"; tail -15 "$WORK/asm.log"; exit 1; }
}

echo "==> assembling"
# Examples and tests live in files that only include the SDK.
for f in "$REPO"/examples/gpu64_demo_*.a; do
	b=$(basename "$f" .a); asm "$REPO/examples" "$f" "${b#gpu64_demo_}"
done
for f in "$REPO"/tests/gpu64_*.a; do
	b=$(basename "$f" .a); asm "$REPO/tests" "$f" "${b#gpu64_}"
done
# Games, in a scratch copy: no developer tune, no generated leftovers.
for g in nighthawk quake stunts; do
	mkdir -p "$WORK/src/$g"
	rsync -a --exclude '*.prg' --exclude '*.reu' --exclude 'ID1' --exclude 'gpu64_tune.bin' \
		"$REPO/games/$g/" "$WORK/src/$g/"
	cp "$REPO/sdk/tune_silent.bin" "$WORK/src/$g/gpu64_tune.bin"
done
GPU64_ROOT="$REPO" "$PY" "$WORK/src/stunts/gen_stunt.py" >/dev/null
for f in "$WORK"/src/*/gpu64_demo_*.a; do
	b=$(basename "$f" .a); asm "$(dirname "$f")" "$f" "${b#gpu64_demo_}"
done
cp "$WORK/src/stunts/stunts.reu" "$WORK/sd/REU/stunts.reu"
if [ "$WITHQUAKE" = 1 ]; then
	[ -f "$REPO/build/quake.reu" ] || { echo "error: build/quake.reu missing (tools/gen_quakelevel.py + pack_levels.py)" >&2; exit 1; }
	cp "$REPO/build/quake.reu" "$WORK/sd/REU/quake.reu"
else
	echo "note: quake.reu NOT included (pass --with-quake-data to include it)"
fi

# ---- disk images ------------------------------------------------------
# mkd64 <image> <title> <prg names...> : each program goes on as UPPERCASE name
mkd64() {
	local img="$1" title="$2"; shift 2
	local args=(-format "$title,gp" d64 "$OUT/$img")
	for n in "$@"; do
		[ -f "$WORK/prg/$n.prg" ] || { echo "error: missing $n.prg for $img" >&2; exit 1; }
		local up; up=$(echo "$n" | tr 'a-z' 'A-Z' | tr -c 'A-Z0-9\n' '-' | cut -c1-16)
		args+=(-write "$WORK/prg/$n.prg" "$up")
	done
	c1541 "${args[@]}" >"$WORK/c1541.log" 2>&1 || { cat "$WORK/c1541.log"; echo "error: $img (disk full?)" >&2; exit 1; }
	echo "    $img: $# programs"
}
echo "==> disk images"
EX=(hello text80 palette sprites bounce rotate matrix cube raycast walls sectors)
mkd64 gpu64-examples.d64  "gpu64 examples" "${EX[@]}"
mkd64 gpu64-nighthawk.d64 "gpu64 nighthawk" nighthawk
mkd64 gpu64-stunts.d64    "gpu64 stunts"    stunt
mkd64 gpu64-quake.d64     "gpu64 quake"     quake quake3d level game
PROBES=()
for f in "$WORK"/prg/*.prg; do
	b=$(basename "$f" .prg)
	case " ${EX[*]} nighthawk stunt quake quake3d level game " in *" $b "*) ;; *) PROBES+=("$b");; esac
done
# The probes may not all fit on one disk (664 blocks): split in two.
half=$(( (${#PROBES[@]} + 1) / 2 ))
mkd64 gpu64-probes-1.d64 "gpu64 probes 1" "${PROBES[@]:0:$half}"
mkd64 gpu64-probes-2.d64 "gpu64 probes 2" "${PROBES[@]:$half}"

# ---- SD card overlay --------------------------------------------------
echo "==> SD card"
# RAD_PRG holds everything, under the names the tree uses.
for f in "$WORK"/prg/*.prg; do
	b=$(basename "$f" .prg)
	case "$b" in test_*|probe_*|3d_*|api_demo|ladder_test|loop_test|mirror_probe|trigger_pattern|vblank_demo) n="gpu64_$b";; *) n="gpu64_demo_$b";; esac
	cp "$f" "$WORK/sd/RAD_PRG/$n.prg"
done
cp "$OUT/kernel_rad.img" "$WORK/sd/kernel_rad.img"
cat > "$WORK/sd/GPU64-README.txt" <<TXT
gpu64 $VERSION
Programs are in RAD_PRG/, game data in REU/, firmware is kernel_rad.img.
In the RAD menu press T until it reads REU, then pick a program.
https://github.com/<you>/gpu64
TXT
if [ -n "$SDBASE" ]; then
	rsync -a "$SDBASE"/ "$WORK/merged/" 2>/dev/null || { mkdir -p "$WORK/merged"; rsync -a "$SDBASE"/ "$WORK/merged/"; }
	rsync -a "$WORK/sd"/ "$WORK/merged/"
	ZIPSRC="$WORK/merged"
else
	ZIPSRC="$WORK/sd"
fi
( cd "$ZIPSRC" && zip -qr "$OUT/gpu64-$VERSION-sdcard.zip" . )

# ---- checksums --------------------------------------------------------
( cd "$OUT" && sha256sum * > SHA256SUMS )
echo "==> done: $OUT"
ls -la "$OUT" | awk 'NR>1{printf "    %9d  %s\n",$5,$9}'
