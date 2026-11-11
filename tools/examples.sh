#!/usr/bin/env bash
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
#
# examples.sh - assemble the gpu64 examples and games and check them on a PC.
#
# For each gpu64_demo_*.a in examples/ and games/:
#   1. assemble it with 64tass into the matching .prg
#   2. run it under tools/prgsim, twice: once modelling a display with a
#      frame clock and once modelling one without, because a demo that only
#      works on the first is broken on hardware nobody can predict. The
#      first run is 400 frames -- see the comment on --stop-after below
#   3. render what the HDMI output was showing when it finished, into
#      out/examples/<name>.ppm
#
# A class-1 demo is handled differently at step 3. The model does not
# rasterise class 1 -- see tools/prgsim/gpu64class1.py -- so there is no page
# for --ppm to photograph. Instead the run writes a frame stream, and
# tools/hostsim/scenesim renders EVERY frame of it through the firmware's own
# gpu64_3dSceneRender(). out/<name>.ppm is the last of those, and
# out/<name>.c1/ holds the stream, its blobs and the per-frame PPMs.
#
# The demos are not the conformance suite: they assert nothing and reach no
# verdict. What this script establishes is that each one assembles, runs to
# completion in both display cases, and produces a picture -- which is the
# part that would otherwise cost bench time to find out. Look at the PPMs
# before deploying; that is the whole point of rendering them.
#
# Note that 64tass is invoked WITHOUT --nostart: that option strips the
# 2-byte load address and produces a PRG that loads to the wrong place and
# silently never runs.
#
# Usage:
#   tools/examples.sh                # assemble + run + render everything
#   tools/examples.sh hello rotate   # just these
#   tools/examples.sh -v hello       # and print the C64 screen it produced
#   tools/examples.sh --chain        # also check every class-0 demo renders the
#                                 # same when launched on top of another
#                                 # demo's leftover gpu64 state

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMODIR="$REPO_ROOT/examples"
SDKDIR="$REPO_ROOT/sdk"
# The games live in games/ and .include the SDK from sdk/.
GAMEDIRS=("$REPO_ROOT/games/quake" "$REPO_ROOT/games/stunts" "$REPO_ROOT/games/nighthawk")
OUTDIR="$REPO_ROOT/out/examples"
SIM="$REPO_ROOT/tools/prgsim/runsim.py"
SCENESIM="$REPO_ROOT/tools/hostsim/scenesim"

verbose=0
if [ "${1:-}" = "-v" ]; then verbose=1; shift; fi

# --chain: also render every class-0 demo a second time, launched on top of
# another demo's leftover gpu64 state, and require the two pictures to be
# identical.
#
# The Pi is not reset when the C64 loads a new .prg. Only a C64 /RESET
# re-baselines gpu64, so a demo started from the RAD menu inherits the
# previous one's palette, draw/visible page pairing, border, display mode
# and framebuffer contents. That produced two bench reports on 2026-09-09 --
# bounce in the wrong colours, and palette recolouring bounce's leftover
# rectangles instead of showing its own rings -- and dmInit's FULL_RESET is
# what closes it. This is the regression that keeps it closed.
#
# Off by default because it doubles the run: the default pass is the one
# that has to stay fast enough to run before every commit.
chain=0
if [ "${1:-}" = "--chain" ]; then chain=1; shift; fi

if [ $# -gt 0 ]; then
	names=("$@")
else
	names=()
	for f in "$DEMODIR"/gpu64_demo_*.a; do
		b="$(basename "$f" .a)"
		names+=("${b#gpu64_demo_}")
	done
	for d in "${GAMEDIRS[@]}"; do
		for f in "$d"/gpu64_demo_*.a; do
			b="$(basename "$f" .a)"
			names+=("${b#gpu64_demo_}")
		done
	done
fi

mkdir -p "$OUTDIR"

fail=0
for n in "${names[@]}"; do
	srcdir="$DEMODIR"
	for d in "${GAMEDIRS[@]}"; do
		[ -f "$d/gpu64_demo_$n.a" ] && srcdir="$d"
	done
	src="$srcdir/gpu64_demo_$n.a"
	prg="$srcdir/gpu64_demo_$n.prg"
	if [ ! -f "$src" ]; then
		echo "no such demo: $n"
		fail=1
		continue
	fi

	# A game's title tune is the user's own: games/*/gpu64_tune.bin is not
	# in the repository. Without one the game builds with a silent stub.
	if grep -q 'gpu64_tune.bin' "$src" && [ ! -f "$srcdir/gpu64_tune.bin" ]; then
		cp "$SDKDIR/tune_silent.bin" "$srcdir/gpu64_tune.bin"
	fi
	# stunts.reu is generated, not stored.
	if [ "$n" = "stunt" ] && [ ! -f "$srcdir/stunts.reu" ]; then
		python3 "$srcdir/gen_stunt.py" > /dev/null || { echo "gen_stunt.py failed"; fail=1; continue; }
	fi

	lst=$(mktemp)
	if ! out=$( cd "$srcdir" && 64tass --cbm-prg -I "$SDKDIR" -o "$prg" -L "$lst" "$src" 2>&1 ); then
		echo "ASSEMBLE FAIL  $n"
		echo "$out" | grep -v '^$' | tail -20
		rm -f "$lst"
		fail=1
		continue
	fi

	# A `.for a = ...` loop leaves a symbol named `a` behind, and 64tass
	# then assembles every later `lsr a` as `lsr $03` -- a zero-page shift
	# that leaves A alone, with no warning. The game's rnd and mul8 ran
	# like that through stage D. Any accumulator shift that did not come
	# out as the one-byte opcode is that trap.
	bad=$(grep -P "\t(lsr|asl|ror|rol) a\s*(;.*)?$" "$lst" |
	      grep -vP "^\.[0-9a-f]+\t(4a|0a|6a|2a)\t")
	rm -f "$lst"
	if [ -n "$bad" ]; then
		echo "ASSEMBLE FAIL  $n: 'a' is a symbol -- rename a .for a loop"
		echo "$bad" | head -5
		fail=1
		continue
	fi

	# Per-demo extra arguments. A demo steered by the keyboard reads every
	# key as released without --key, so it stands still for four hundred
	# frames and proves almost nothing; the schedule below walks it around
	# its room, looks up, and fires.
	extra=()
	c1=0
	case "$n" in
	level)
		# The whole level comes off the Pi's SD card, so the model needs
		# the same file -- without it LOAD_LEVEL answers BAD_ARGS and the
		# run proves only that the refusal path works. Built by
		# tools/gen_quakelevel.py; skipped rather than failed, because a
		# tree without a Quake BSP to convert is a normal state.
		c1=1
		if [ ! -f "$REPO_ROOT/build/e1m1.g64lev" ]; then
			echo "skip  level      (no build/e1m1.g64lev -- run tools/gen_quakelevel.py)"
			continue
		fi
		# The schedule used to have to be a tour that RETURNS --
		# every walk paired with its own reverse, because nothing
		# stopped the camera leaving the map and the first schedule
		# tried here flew out through a wall by frame 500 and spent
		# the remaining thousand frames photographing the black
		# outside of it. CLIP_MOVE ($15) is what changed that: the
		# walls hold the player in now, so this is an ordinary walk
		# and a wrong turn costs a dull frame rather than an empty
		# one.
		#
		# It is still worth keeping the walks at the front. Forward
		# at the start is +z and the corridor runs that way, so the
		# first few hundred frames are the ones a reader can check
		# against the level; the later stretches walk into walls on
		# purpose, which is now a test rather than an escape.
		extra=(--level="$REPO_ROOT/build/e1m1.g64lev"
		       --key=W:20-260
		       --key=A:280-340 --key=W:360-520
		       --key=D:540-660 --key=W:680-840
		       --key=Q:860-920
		       --key=D:940-1000 --key=W:1020-1180
		       --key=SPACE:1020-1180
		       --key=S:1200-1300
		       --key=A:1320-1450 --key=W:1320-1450)
		;;
	game)
		# The doors demo needs the same level file the level demo
		# does, and for the same reason.
		c1=1
		if [ ! -f "$REPO_ROOT/build/e1m1.g64lev" ]; then
			echo "skip  game       (no build/e1m1.g64lev -- run tools/gen_quakelevel.py)"
			continue
		fi
		# This schedule is not a tour: it is a ROUTE, and it was not
		# written by hand. Walking to a door means getting past the
		# geometry in between, and the three key ranges below were
		# found by flooding E1M1's reachable space with the firmware's
		# own trace (tools/hostsim/libclipmove.so, the library prgsim
		# already calls) and taking the shortest path to each of three
		# things worth touching:
		#
		#   the func_door_secret at 21.5 2.5 1.75, which opens on
		#   being walked into;
		#   the double door at 7.25 2 17.6, the level's first proper
		#   door; and
		#   the func_button at -2 1.5 18, whose target is a door on
		#   the far side of the room -- the one touch in the run that
		#   proves the target/targetname wiring rather than a
		#   proximity test.
		#
		# It walks with left SHIFT (run) held throughout, so a frame
		# is 0.53 world units and the whole route fits in four hundred
		# of them. Each leg ends with ninety frames of standing still,
		# which is what the assertions below are timed against. SPACE
		# is the jump.
		#
		# The last leg presses the button and then steps back out with
		# E. The door the button opens is the floor under the button
		# and it lowers; a player left standing on it rides it down, is
		# lifted out when it shuts (the demo climbs a player out of a
		# shut mover since run 39), lands back inside the button and
		# presses it again. The route used to stay, and every earlier
		# build then sat inside the shut door for the rest of the run.
		#
		# --notarget: the route is about doors, and since stage E the
		# grunts by the button would otherwise shoot the player off it.
		# tools/check_ai.py is the run where the monsters fight.
		#
		# --frame-ms=32: the game scales its motion by the measured
		# frame time, so the route only walks where it was flooded on a
		# clock that reads the 32ms the constants were tuned at.
		extra=(--level="$REPO_ROOT/build/e1m1.g64lev" --notarget
		       --frame-ms=32
		       --key=LSHIFT:20-426
		       --key=W:20-43 --key=E:44-55
		       --key=W:154-168 --key=Q:169-172
		       --key=W:173-187 --key=Q:188-207
		       --key=Q:308-324 --key=E:326-340)
		;;
	cube)
		c1=1
		;;
	quake3d)
		c1=1
		extra=(--key=W:20-260 --key=A:120-200 --key=D:300-420
		       --key=E:430-500 --key=W:520-760 --key=F1:640-700
		       --key=F7:780-800 --key=SPACE:820-900 --key=S:950-1150
		       --key=Q:1000-1080 --key=F3:1200-1260)
		;;
	stunt)
		# stunts.reu is the selected REU image: the game's meshes, fonts
		# and track blocks come out of it. The grid countdown is 180
		# frames. The autopilot (F3) then drives a lap and a half, and
		# the last stretch is driven by hand: throttle held, a touch of
		# steering, enough to put a car off the edge and prove the crane.
		c1=1
		extra=(--reu="$REPO_ROOT/games/stunts/stunts.reu"
		       --level-slot="${STUNT_TRACK:-0}"
		       --key=F3:190-195
		       --key=F3:1000-1005 --key=W:1000-1500
		       --key=A:1100-1130 --key=D:1250-1320)
		;;
	nighthawk)
		# The autopilot (F3) takes off and flies the circuit; the
		# cockpit view (F1) is switched in over the climb-out and
		# back to the chase camera before the first turn.
		# tools/check_nighthawk.py flies the rest of the circuit.
		c1=1
		extra=(--key=F3:30-35 --key=F1:600-605 --key=F1:1000-1005)
		;;
	esac

	bad=0
	for mode in "" "--no-vblank"; do
		ppm=""
		frames="--stop-after=60"
		if [ -z "$mode" ]; then
			ppm="--ppm=$OUTDIR/$n.ppm"
			# Several hundred frames, not the handful the simulator
			# used to run: a defect whose onset was frame 98 got
			# through a 96-frame check once already on this project,
			# and the raycast demo looked clean here for the same
			# reason. The no-vblank pass only has to prove the demo
			# does not hang, so it stays short.
			frames="--stop-after=400"
			if [ $c1 -eq 1 ]; then
				# One commit per frame clock tick, so the
				# stop-poll count that buys 400 rendered
				# frames is the same 400 -- but the class-1
				# demos poll STOP once per attempted commit,
				# and a run worth looking at wants more than
				# the first few seconds of it.
				frames="--stop-after=1500"
				rm -rf "$OUTDIR/$n.c1"
				mkdir -p "$OUTDIR/$n.c1"
				ppm="--c1-stream=$OUTDIR/$n.c1/frames.txt"
			fi
		fi
		if simout=$( python3 "$SIM" "$prg" --demo $mode $frames $ppm "${extra[@]+"${extra[@]}"}" 2>&1 ); then
			[ $verbose -eq 1 ] && echo "$simout"
		else
			echo "SIM FAIL  $n ${mode:-(vblank)}"
			echo "$simout"
			bad=1
			fail=1
		fi
	done

	# The picture, for a class-1 demo, comes out of the firmware renderer
	# rather than the model: every frame of the stream, so a defect that
	# only shows up once the player has walked somewhere is visible here
	# and not only at the bench.
	if [ $bad -eq 0 ] && [ $c1 -eq 1 ]; then
		if [ ! -x "$SCENESIM" ]; then
			echo "SCENESIM MISSING  $n  (make -C tools/hostsim)"
			bad=1
			fail=1
		elif ! scout=$( "$SCENESIM" "$OUTDIR/$n.c1/frames.txt" \
				 "$OUTDIR/$n.c1" --ppm-every=25 2>&1 ); then
			echo "RENDER FAIL  $n"
			echo "$scout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$scout"
			last=$(ls "$OUTDIR/$n.c1"/frame*.ppm 2>/dev/null | tail -1)
			[ -n "$last" ] && cp "$last" "$OUTDIR/$n.ppm"
		fi
	fi

	# The one demo that asserts something. Everything else here is
	# checked by looking at the PPM, which cannot distinguish a door
	# that opens from a door painted open -- see tools/check_game.py.
	# The frames are the route's own: 110 is while the player stands in
	# the secret door, 230 in the double doorway, 340 just after the
	# button was pressed, and by 400 the door it opened has to have
	# closed again.
	if [ $bad -eq 0 ] && [ "$n" = "game" ]; then
		for check in "110 310" "230 14" "340 29" "400 29 --shut"; do
			set -- $check
			if ! gout=$( python3 "$REPO_ROOT/tools/check_game.py" 					--prg="$prg" 					--level="$REPO_ROOT/build/e1m1.g64lev" 					--frame="$1" --ent="$2" ${3:-} 					"${extra[@]}" 2>&1 ); then
				echo "GAME CHECK FAIL  $n"
				echo "$gout"
				bad=1
				fail=1
			else
				[ $verbose -eq 1 ] && echo "$gout"
			fi
		done
	fi

	# Stage D's claim is a chain -- fire key, cone search, a trace in the
	# frame's WORLD_TICK, pellets, pain, death, a backpack, the player's
	# own death and restart -- whose every link ends in a scene node or a
	# number on rows 21-22. tools/check_combat.py drives its own two
	# scripted runs, so it does not use the door route above.
	if [ $bad -eq 0 ] && [ "$n" = "game" ]; then
		if ! gout=$( python3 "$REPO_ROOT/tools/check_combat.py" \
				--prg="$prg" \
				--level="$REPO_ROOT/build/e1m1.g64lev" 2>&1 ); then
			echo "COMBAT CHECK FAIL  $n"
			echo "$gout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$gout"
		fi
	fi

	# And stage E, the monsters' half, which everything above keeps asleep
	# with --notarget: a grunt and a dog wake, run at the player on CLIP_MOVE
	# answers and hurt them, and a restart puts them back, asleep.
	if [ $bad -eq 0 ] && [ "$n" = "game" ]; then
		if ! gout=$( python3 "$REPO_ROOT/tools/check_ai.py" \
				--prg="$prg" \
				--level="$REPO_ROOT/build/e1m1.g64lev" 2>&1 ); then
			echo "AI CHECK FAIL  $n"
			echo "$gout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$gout"
		fi
	fi

	# Water, slime and the suit: CLIP_MOVE's waterlevel turned into
	# swimming, drowning and liquid damage (gpu64_game_env.inc).
	if [ $bad -eq 0 ] && [ "$n" = "game" ]; then
		if ! gout=$( python3 "$REPO_ROOT/tools/check_env.py" \
				--prg="$prg" \
				--level="$REPO_ROOT/build/e1m1.g64lev" 2>&1 ); then
			echo "ENV CHECK FAIL  $n"
			echo "$gout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$gout"
		fi
	fi

	# The SID: runsim's --sid-log decoded back into effect names, so the
	# scripted shots, pains, doors, splashes and the jump each made theirs
	# (gpu64_game_sfx.inc).
	if [ $bad -eq 0 ] && [ "$n" = "game" ]; then
		if ! gout=$( python3 "$REPO_ROOT/tools/check_sfx.py" \
				--prg="$prg" \
				--level="$REPO_ROOT/build/e1m1.g64lev" 2>&1 ); then
			echo "SFX CHECK FAIL  $n"
			echo "$gout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$gout"
		fi
	fi

	# Stage F: the three-button counter and its wait -1 door, the exit's
	# intermission and the way back out of it, and the muzzle flash
	# (gpu64_game_exit.inc).
	if [ $bad -eq 0 ] && [ "$n" = "game" ]; then
		if ! gout=$( python3 "$REPO_ROOT/tools/check_exit.py" \
				--prg="$prg" \
				--level="$REPO_ROOT/build/e1m1.g64lev" 2>&1 ); then
			echo "EXIT CHECK FAIL  $n"
			echo "$gout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$gout"
		fi
	fi

	# Nighthawk's claim is that the C64 flies the aeroplane, which a
	# picture of a jet over a field cannot show: the checkride flies a
	# whole autopilot circuit and requires one landing and no crash.
	if [ $bad -eq 0 ] && [ "$n" = "nighthawk" ]; then
		if ! gout=$( python3 "$REPO_ROOT/tools/check_nighthawk.py" \
				--prg="$prg" 2>&1 ); then
			echo "CHECKRIDE FAIL  $n"
			echo "$gout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$gout"
		fi
	fi

	# The launch-order check. The chained-after program is whichever
	# class-0 demo is not this one and disturbs the most state: palette
	# loads a 256-entry ramp and never flips, bounce ends with the draw
	# page away from the visible one, and between them they cover both
	# halves of what leaks.
	if [ $bad -eq 0 ] && [ $chain -eq 1 ] && [ $c1 -eq 0 ]; then
		prev="$DEMODIR/gpu64_demo_palette.prg"
		[ "$n" = "palette" ] && prev="$DEMODIR/gpu64_demo_bounce.prg"
		if chout=$( python3 "$SIM" "$prg" --demo --stop-after=400 \
				--chain="$prev" --ppm="$OUTDIR/$n.chain.ppm" \
				"${extra[@]+"${extra[@]}"}" 2>&1 ); then
			if ! cmp -s "$OUTDIR/$n.ppm" "$OUTDIR/$n.chain.ppm"; then
				echo "CHAIN DIFF  $n  (differs when launched after $(basename "$prev" .prg))"
				echo "            out/$n.ppm vs out/$n.chain.ppm"
				bad=1
				fail=1
			fi
		else
			echo "CHAIN FAIL  $n"
			echo "$chout"
			bad=1
			fail=1
		fi
	fi

	if [ $bad -eq 0 ]; then
		size=$(stat -c%s "$prg")
		printf 'ok  %-10s %5d bytes  ->  out/%s.ppm\n' "$n" "$size" "$n"
	fi
done

exit $fail
