#!/usr/bin/env bash
#
# deploy_prg.sh - put this tree's .prg files on a mounted RAD SD card, and
# take off the ones this tree no longer builds.
#
# The card's RAD_PRG directory accumulates: it holds gpu64's own programs
# next to the user's C64 software, and it has collected renamed and
# abandoned copies of gpu64 programs across the campaign (gpu643d0.prg,
# gpu64api.prg, gpu64lad.prg -- 8-character names from before the tree
# settled on gpu64_<kind>_<name>). Picking the current build out of that
# list at the bench is a way to waste a round on a stale program, which has
# already happened once with a stale kernel image.
#
# So this copies every .prg from tests/, examples/ and
# games/*/, and then removes any *gpu64* .prg on the card that this tree does
# not build. It never touches anything that is not gpu64's: the games, the
# monitors, the memory tester and readme.txt are left exactly where they are.
#
# It also puts the games' REU images (build/quake.reu, games/stunts/stunts.reu) in
# the card's REU directory when they exist, because a game ships as its PRG
# plus its REU image and the two are one deploy: the card kept a stale level
# through a whole bench round once, and data that disagrees with the program
# looks like a rendering bug and not like a copy that never happened. The
# firmware reads levels and pictures from the selected REU image only.
#
# It does NOT build anything. Run tools/testprg.sh and tools/examples.sh first
# if you want the .prg files to be current -- they are what assembles them.
# tools/gen_quakelevel.py + pack_levels.py and games/stunts/gen_stunt.py build the images, and the firmware image is
# tools/build.sh's job; neither is this one's.
#
# Usage:
#   SDCARD=/media/you/SIDEKICK tools/deploy_prg.sh          # copy + prune
#   SDCARD=/media/you/SIDEKICK tools/deploy_prg.sh --dry-run
#
# Copyright (c) 2026 Honza Slesinger
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

if [ -z "${SDCARD:-}" ]; then
	echo "error: SDCARD is not set." >&2
	echo "  SDCARD=/media/you/SIDEKICK tools/deploy_prg.sh" >&2
	exit 1
fi
if [ ! -d "$SDCARD" ]; then
	echo "error: $SDCARD is not a directory -- is the card mounted?" >&2
	exit 1
fi

DEST="$SDCARD/RAD_PRG"
if [ ! -d "$DEST" ]; then
	echo "error: $DEST does not exist. That is where RAD looks for .prg" >&2
	echo "files, so this is probably not a RAD card." >&2
	exit 1
fi

# What this tree builds, by basename.
built=()
for f in "$REPO_ROOT"/tests/*.prg "$REPO_ROOT"/examples/*.prg "$REPO_ROOT"/games/*/*.prg; do
	[ -e "$f" ] || continue
	built+=("$(basename "$f")")
done
if [ ${#built[@]} -eq 0 ]; then
	echo "error: no .prg files in tests/, examples/ or games/." >&2
	echo "Run `make examples games tests` first." >&2
	exit 1
fi

echo "==> copying ${#built[@]} programs to $DEST"
for f in "$REPO_ROOT"/tests/*.prg "$REPO_ROOT"/examples/*.prg "$REPO_ROOT"/games/*/*.prg; do
	[ -e "$f" ] || continue
	if [ $DRY -eq 1 ]; then
		echo "    would copy $(basename "$f")"
	else
		cp "$f" "$DEST/"
	fi
done

# Prune: anything on the card whose name contains "gpu64" and that this tree
# does not build. The name test is deliberately the only filter -- a
# gpu64 program that got renamed is exactly what has to go, and nothing
# outside gpu64 is named that way.
echo "==> pruning gpu64 programs this tree no longer builds"
removed=0
for f in "$DEST"/*; do
	[ -f "$f" ] || continue
	b="$(basename "$f")"
	case "$b" in *[Gg][Pp][Uu]64*) ;; *) continue ;; esac
	case "$b" in *.prg|*.PRG) ;; *) continue ;; esac
	keep=0
	for k in "${built[@]}"; do
		[ "$b" = "$k" ] && { keep=1; break; }
	done
	[ $keep -eq 1 ] && continue
	if [ $DRY -eq 1 ]; then
		echo "    would remove $b"
	else
		rm -f "$f"
		echo "    removed $b"
	fi
	removed=$((removed + 1))
done
[ $removed -eq 0 ] && echo "    nothing stale"

# The REU images. Verified by content, not by exit status: these are the
# files whose staleness is invisible at the bench -- the game uses whatever
# image is selected and reports success.
for REU_SRC in "$REPO_ROOT/build/quake.reu" "$REPO_ROOT/games/stunts/stunts.reu"; do
	name="$(basename "$REU_SRC")"
	if [ ! -f "$REU_SRC" ]; then
		echo "==> no $name -- left as it is on the card"
		continue
	fi
	if [ ! -d "$SDCARD/REU" ]; then
		echo "warning: $SDCARD/REU does not exist -- $name not deployed" >&2
	elif [ $DRY -eq 1 ]; then
		echo "==> would copy $name to REU/"
	elif cmp -s "$REU_SRC" "$SDCARD/REU/$name"; then
		echo "==> REU/$name already current"
	else
		echo "==> copying $name to REU/"
		cp "$REU_SRC" "$SDCARD/REU/$name"
		sync
		if cmp -s "$REU_SRC" "$SDCARD/REU/$name"; then
			echo "    $(stat -c%s "$REU_SRC") bytes, verified"
		else
			echo "error: REU/$name does not match the build" >&2
			exit 1
		fi
	fi
done

[ $DRY -eq 0 ] && sync
echo "==> done"
