#!/bin/sh
# Captures the fusions' attacks out of Buu's Fury and rebuilds
# games/lgoku2/mods/generated/fusion_moves.inc from them.
# Usage: tools/sprites/capture_fusion_moves.sh SCRATCH_DIR
# It builds Buu's Fury with the play_as test aid for the duration.
cd "$(dirname "$0")/../.."
T="${1:?give a scratch directory}"
mkdir -p "$T" "$T/hold"
S=games/buusfury/states/goku-snake-way-full-energy.state
cp games/buusfury/mods-examples/play_as.c games/buusfury/mods/zz_play_as.c
make GAME=buusfury >/dev/null
# record to play as, how many presses of L select the attack, name
for spec in "7 1 bigbang" "7 2 dragon" "6 2 lines" "6 3 sword" "5 2 ghost" "5 3 ball"; do
	set -- $spec
	ls=""
	for k in $(seq $2); do ls="$ls,l:4,N:8"; done
	t0=$((36 + $2 * 12))
	for f in $(seq 0 2 150); do
		AS=$1 ./buusfury --headless $((t0 + f)) --save "$T/bf.sav" --load-state $S --input "N:20,R:6,N:10$ls,B:36,N:300" --obj-only --shot "$T/$3_$f.png" >/dev/null 2>&1
	done
done
for id in 3 5 1 9 13 23; do
	h=$(printf '%x' $id)
	ATT=$h,$h,$h,$h ./buusfury --headless 40 --save "$T/bf.sav" --load-state $S --input "N:40" --shot "$T/icon_$id.png" >/dev/null 2>&1
done
rm games/buusfury/mods/zz_play_as.c
make GAME=buusfury >/dev/null
python3 -I tools/sprites/make_fusion_moves.py "$T" games/lgoku2/mods/generated/fusion_moves.inc
# the held moves, with B down for three seconds
for spec in "7 1 bigbang" "7 2 dragon" "6 2 lines" "6 3 sword"; do
	set -- $spec
	ls=""
	for k in $(seq $2); do ls="$ls,l:4,N:8"; done
	t0=$((36 + $2 * 12))
	for f in $(seq 0 2 238); do
		AS=$1 ./buusfury --headless $((t0 + f)) --save "$T/bf.sav" --load-state $S --input "N:20,R:6,N:10$ls,B:180,N:300" --obj-only --shot "$T/hold/$3_$f.png" >/dev/null 2>&1
	done
done
cp "$T"/icon_*.png "$T/hold/"
python3 -I tools/sprites/make_hold_moves.py "$T/hold" games/lgoku2/mods/generated/hold_moves.inc
