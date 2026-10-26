#!/bin/bash
# Two Buu's Fury windows on one PC, to try the online battles alone: player 1
# is your usual game, player 2 a second copy with a save folder of its own
# (a copy of your save the first time) and the same relay. Pick RANDOM BATTLE
# on the ONLINE page in both, or HOST A BATTLE in one and JOIN WITH CODE in
# the other. Keys go to the window that has the focus: click a window to play
# it. A game controller drives player 1 only (player 2 is keyboard only, or
# both would follow the same pad). Esc closes a window as usual;
# Ctrl-C here closes both. Player 2's diary: <its folder>/buusfury.modlog.
#   tools/netplay/two-windows.sh [--scale N]    (default 2: the two fit side by side)
here=$(cd "$(dirname "$0")/../.." && pwd)
scale=2
dir1=
dir2=
while [ $# -gt 0 ]; do
	case "$1" in
		--scale) scale=$2; shift ;;
		--dir1) dir1=$2; shift ;;   # (tests) save folders to use instead of the usual ones
		--dir2) dir2=$2; shift ;;
		-h|--help) sed -n '2,9p' "$0" | cut -c3-; exit 0 ;;
		*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
data=${XDG_DATA_HOME:-$HOME/.local/share}/buusfury
[ -n "$dir1" ] || dir1=$data
[ -n "$dir2" ] || dir2=$data/player2
mkdir -p "$dir2"
[ -e "$dir2/buusfury.sav" ] || cp "$dir1/buusfury.sav" "$dir2/buusfury.sav" 2> /dev/null
[ -e "$dir2/buusfury.netplay" ] || cp "$dir1/buusfury.netplay" "$dir2/buusfury.netplay" 2> /dev/null
[ -e "$dir1/buusfury.netplay" ] || echo "no relay set in $dir1/buusfury.netplay: the ONLINE page will say so"
export NETPLAY_DEBUG=${NETPLAY_DEBUG:-1} # the diaries record every hit and technique, for looking into a report
"$here/buusfury" --scale "$scale" --save "$dir1/buusfury.sav" &
p1=$!
sleep 1
"$here/buusfury" --scale "$scale" --save "$dir2/buusfury.sav" --no-controller &
p2=$!
trap 'kill $p1 $p2 2> /dev/null' INT TERM
echo "player 1: $dir1   player 2: $dir2   (Ctrl-C closes both)"
wait
