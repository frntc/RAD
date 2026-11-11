#!/usr/bin/env bash
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
#
# testprg.sh - assemble the gpu64 API conformance suite and desk-check it.
#
# For each tests/gpu64_test_*.a:
#   1. assemble it with 64tass into the matching .prg
#   2. run it under tools/prgsim against a reference model of the API built
#      from docs/README.md and the files it indexes, and require VERDICT PASS
#
# Step 2 is the point. A test that encodes a misreading of the reference
# fails here, on a PC, in a second -- rather than at the bench, where the
# same red line would be indistinguishable from a firmware defect and where
# the time to investigate it is expensive. Green here means "the suite
# asserts what the document says"; red at the bench then means "the
# firmware does not".
#
# Note that 64tass is invoked WITHOUT --nostart: that option strips the
# 2-byte load address and produces a PRG that loads to the wrong place and
# silently never runs. See dev/progress_tracker.md.
#
# Usage:
#   tools/testprg.sh              # assemble + desk-check everything
#   tools/testprg.sh system draw  # just these
#   tools/testprg.sh -v system    # and print the screen it produced

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRGDIR="$REPO_ROOT/tests"
SIM="$REPO_ROOT/tools/prgsim/runsim.py"

verbose=0
if [ "${1:-}" = "-v" ]; then verbose=1; shift; fi

if [ $# -gt 0 ]; then
	names=("$@")
else
	names=()
	for f in "$PRGDIR"/gpu64_test_*.a; do
		b="$(basename "$f" .a)"
		names+=("${b#gpu64_test_}")
	done
fi

fail=0
for n in "${names[@]}"; do
	src="$PRGDIR/gpu64_test_$n.a"
	prg="$PRGDIR/gpu64_test_$n.prg"
	if [ ! -f "$src" ]; then
		echo "no such test: $n"
		fail=1
		continue
	fi

	if ! out=$( cd "$PRGDIR" && 64tass --cbm-prg -o "$prg" "$src" 2>&1 ); then
		echo "ASSEMBLE FAIL  $n"
		echo "$out" | grep -v '^$' | tail -20
		fail=1
		continue
	fi

	# Both display cases: a frame clock that calibrated at boot and one
	# that did not. The suite has to reach the same verdict either way --
	# a display gpu64 could not measure is not a conformance failure, and
	# a test that forgets to allow for it would only be caught on the one
	# display nobody has.
	for mode in "" "--no-vblank"; do
		if simout=$( python3 "$SIM" "$prg" $mode 2>&1 ); then
			[ $verbose -eq 1 ] && echo "$simout"
		else
			echo "SIM FAIL  $n ${mode:-(vblank)}"
			echo "$simout"
			fail=1
		fi
	done

	if [ $fail -eq 0 ] || [ $verbose -eq 1 ]; then
		size=$(stat -c%s "$prg")
		printf 'ok  %-12s %5d bytes\n' "$n" "$size"
	fi
done

exit $fail
