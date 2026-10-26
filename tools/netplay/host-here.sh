#!/bin/bash
# Buu's Fury online battles without a server of your own: run the relay on this
# PC and make it reachable from the internet through bore.pub, a free public TCP
# tunnel (no account, no card; https://github.com/ekzhang/bore). Prints the
# address to give the other player, points this PC's own game at the relay, and
# stops everything when you press Ctrl-C or close the window.
#
#   tools/netplay/host-here.sh              relay on port 7777, tunnel through bore.pub
#   tools/netplay/host-here.sh --port N     ask bore.pub for public port N (the same
#                                           address every time, if nobody else holds it)
#   tools/netplay/host-here.sh --to HOST    another bore server (your own, or 127.0.0.1
#                                           with "bore server" running, for a local test)
#   tools/netplay/host-here.sh --no-tunnel  relay only: for a LAN, or when the router
#                                           forwards TCP 7777 to this PC
#
# Needs python3 and bore (cargo install bore-cli; it lands in ~/.cargo/bin).
# The relay has to be running only while the two of you fight.

here=$(cd "$(dirname "$0")" && pwd)
local_port=7777
public_port=0
to=bore.pub
tunnel=1
while [ $# -gt 0 ]; do
	case "$1" in
		--port) public_port=$2; shift 2 ;;
		--local-port) local_port=$2; shift 2 ;;
		--to) to=$2; shift 2 ;;
		--no-tunnel) tunnel=0; shift ;;
		-h|--help) sed -n '2,19p' "$0" | cut -c3-; exit 0 ;;
		*) echo "unknown option $1 (try --help)" >&2; exit 2 ;;
	esac
done

listening() { # host port: true when something accepts connections there
	timeout 1 bash -c "exec 3<>/dev/tcp/$1/$2" 2>/dev/null
}
relay_pid=
bore_pid=
cleanup() { # rc: exit status
	trap - EXIT INT TERM
	[ -n "$bore_pid" ] && kill "$bore_pid" 2>/dev/null
	[ -n "$relay_pid" ] && kill "$relay_pid" 2>/dev/null
	echo
	echo "relay stopped."
	exit "$1"
}
trap 'cleanup $?' EXIT
trap 'cleanup 0' INT TERM
plain() { sed 's/\x1b\[[0-9;]*m//g; s/^/  /' "$1"; } # bore's log without its colours

bore=
if [ $tunnel = 1 ]; then
	bore=$(command -v bore || true)
	[ -z "$bore" ] && [ -x "$HOME/.cargo/bin/bore" ] && bore=$HOME/.cargo/bin/bore
	if [ -z "$bore" ]; then
		echo "bore is not installed. Install it with:  cargo install bore-cli" >&2
		echo "(no cargo? see https://rustup.rs, or run this with --no-tunnel)" >&2
		exit 1
	fi
fi

if listening 127.0.0.1 "$local_port"; then
	echo "something already listens on port $local_port on this PC (a relay from before?)." >&2
	echo "Stop it, or pick another port with --local-port N." >&2
	exit 1
fi
logdir=${XDG_RUNTIME_DIR:-/tmp}/buusfury-netplay
mkdir -p "$logdir"

# 1. the relay, in this window so you can see players come and go
python3 "$here/netplay_server.py" --port "$local_port" &
relay_pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
	listening 127.0.0.1 "$local_port" && break
	kill -0 $relay_pid 2>/dev/null || { echo "the relay did not start." >&2; exit 1; }
	sleep 0.3
done

# 2. the tunnel
public=
if [ $tunnel = 1 ]; then
	for attempt in 1 2; do
		log=$logdir/bore.$attempt.log
		if [ "$public_port" != 0 ]; then
			"$bore" local "$local_port" --to "$to" --port "$public_port" > "$log" 2>&1 &
		else
			"$bore" local "$local_port" --to "$to" > "$log" 2>&1 &
		fi
		bore_pid=$!
		for i in $(seq 1 50); do
			public=$(grep -o 'listening at [^ ]*' "$log" | head -1 | cut -d' ' -f3)
			[ -n "$public" ] && break
			kill -0 $bore_pid 2>/dev/null || break
			sleep 0.3
		done
		[ -n "$public" ] && break
		if [ "$public_port" != 0 ] && grep -q 'port already in use' "$log"; then
			echo "port $public_port on $to is taken by someone else; taking a random one instead."
			public_port=0
			continue
		fi
		echo "could not open the tunnel through $to:" >&2
		plain "$log" >&2
		echo "(is $to up? https://bore.pub shows nothing itself; try again in a minute, or --no-tunnel)" >&2
		exit 1
	done
	public=${public%$'\r'}
fi

# 3. this PC's own game talks to the relay directly
cfg=${XDG_DATA_HOME:-$HOME/.local/share}/buusfury/buusfury.netplay
mine="server 127.0.0.1:$local_port"
mkdir -p "$(dirname "$cfg")"
old=$(grep '^server ' "$cfg" 2>/dev/null | head -1)
if [ -n "$old" ] && [ "$old" != "$mine" ]; then
	cp "$cfg" "$cfg.bak"
	echo "your buusfury.netplay pointed at '${old#server }'; kept a copy as buusfury.netplay.bak"
fi
{ grep -v '^server ' "$cfg" 2>/dev/null; echo "$mine"; } > "$cfg.new" && mv "$cfg.new" "$cfg"

echo
echo "=================================================================="
if [ -n "$public" ]; then
	echo "  The other player puts this line in their buusfury.netplay:"
	echo
	echo "      server $public"
	echo
	echo "  (the file lives in ~/.local/share/buusfury/ on Linux)"
else
	lan=$(hostname -I 2>/dev/null | awk '{print $1}')
	echo "  Relay on port $local_port, no tunnel. The other player uses:"
	echo "      server ${lan:-this-PC}:$local_port    on your LAN"
	echo "      server <your public address>:$local_port   if the router forwards it"
fi
echo "  Your own game is set to:  $mine"
echo "  Then both: Start, R past Options, ONLINE page."
echo "  Keep this window open while you play. Ctrl-C stops it."
echo "=================================================================="
echo

# 4. stay here until something ends
while kill -0 $relay_pid 2>/dev/null && { [ -z "$bore_pid" ] || kill -0 $bore_pid 2>/dev/null; }; do
	sleep 1
done
if [ -n "$bore_pid" ] && ! kill -0 $bore_pid 2>/dev/null; then
	echo "the tunnel through $to dropped (the other player can no longer reach you):" >&2
	tail -3 "$log" | plain /dev/stdin >&2
fi
exit 1
