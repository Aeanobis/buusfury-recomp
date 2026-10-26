#!/bin/bash
# Put the Buu's Fury netplay relay on a server you can ssh into, as a service
# that starts with the machine, open its port, check it from here, and point
# this PC's game at it. One command once the machine exists:
#
#   tools/netplay/deploy.sh ubuntu@203.0.113.5          # relay on port 7777
#   tools/netplay/deploy.sh ubuntu@203.0.113.5 7778     # another port
#   tools/netplay/deploy.sh --check 203.0.113.5 [port]  # only test a relay from here
#
# The server needs python3 (every Ubuntu, Debian and Oracle Linux image has
# it) and a login that can sudo without a password (the default user of a
# cloud VM: ubuntu, opc, debian...). The cloud's own firewall must already let
# TCP <port> in (Oracle: an ingress rule in the VCN's security list; Google: a
# VPC firewall rule), see README.md; the machine's own firewall (ufw,
# firewalld, or the iptables rules Oracle's Ubuntu images ship with) is opened
# by this script.

here=$(cd "$(dirname "$0")" && pwd)
check=0
if [ "$1" = "--check" ]; then
	check=1
	shift
fi
target=$1
port=${2:-7777}
if [ -z "$target" ] || [ "$target" = "-h" ] || [ "$target" = "--help" ]; then
	sed -n '2,17p' "$0" | cut -c3-
	exit 2
fi
host=${target#*@}
case "$host" in *:*) host=$(python3 -c "import ipaddress,sys; print(ipaddress.ip_address(sys.argv[1]).compressed)" "$host" 2> /dev/null || echo "$host");; esac # IPv6 in its short form
scptarget=$target
case "$host" in *:*) scptarget="[$host]"; [ "$target" != "$host" ] && scptarget="${target%%@*}@[$host]";; esac # an IPv6 address
ssh="ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15"
scp="scp -q -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15"

probe() { # host port: does a netplay relay answer there? (a bad request gets status 3)
	python3 - "$1" "$2" <<'PY'
import socket, struct, sys
host, port = sys.argv[1], int(sys.argv[2])
try:
    s = socket.create_connection((host, port), timeout=6)
    s.sendall(struct.pack("<4sBBI", b"BFNP", 1, 9, 0))
    s.settimeout(6)
    d = s.recv(9)
except OSError as e:
    print("cannot reach %s:%d: %s" % (host, port, e)); sys.exit(1)
if len(d) == 9 and d[:4] == b"BFNP" and d[4] == 3:
    print("the relay answers at %s:%d" % (host, port)); sys.exit(0)
print("something answers at %s:%d but it is not the relay (%r)" % (host, port, d)); sys.exit(1)
PY
}

if [ $check = 0 ]; then
	echo "== copying the relay to $target"
	$scp "$here/netplay_server.py" "$here/netplay.service" "$scptarget:/tmp/" || exit 1
	echo "== installing it as a service"
	$ssh "$target" "sudo bash -s $port" <<'REMOTE' || exit 1
set -e
port=$1
command -v python3 > /dev/null || { echo "python3 is missing on the server (apt install python3)"; exit 1; }
install -m 755 /tmp/netplay_server.py /usr/local/bin/netplay_server.py
sed "s/--port 7777/--port $port/" /tmp/netplay.service > /etc/systemd/system/netplay.service
systemctl daemon-reload
systemctl enable --now netplay > /dev/null 2>&1
systemctl restart netplay
sleep 1
if ! systemctl is-active --quiet netplay; then
	echo "the relay did not start:"
	journalctl -u netplay -n 20 --no-pager
	exit 1
fi
if command -v ufw > /dev/null && ufw status 2> /dev/null | grep -q '^Status: active'; then
	ufw allow "$port/tcp" > /dev/null
	echo "ufw: port $port opened"
elif command -v firewall-cmd > /dev/null && firewall-cmd --state > /dev/null 2>&1; then
	firewall-cmd --permanent --add-port="$port/tcp" > /dev/null
	firewall-cmd --reload > /dev/null
	echo "firewalld: port $port opened"
elif command -v iptables > /dev/null && iptables -S INPUT 2> /dev/null | grep -q -- '-j REJECT'; then
	# Oracle's Ubuntu images: INPUT ends in a REJECT rule, so the ACCEPT goes first
	iptables -C INPUT -p tcp --dport "$port" -j ACCEPT 2> /dev/null || iptables -I INPUT 1 -p tcp --dport "$port" -j ACCEPT
	if command -v netfilter-persistent > /dev/null; then
		netfilter-persistent save > /dev/null 2>&1
	elif [ -d /etc/iptables ]; then
		iptables-save > /etc/iptables/rules.v4
	fi
	echo "iptables: port $port opened"
else
	echo "no firewall on the machine itself"
fi
echo "relay: $(systemctl is-active netplay), starts with the machine"
REMOTE
fi

echo "== checking $host:$port from here"
if ! probe "$host" "$port"; then
	if [ $check = 0 ]; then
		echo "The service is up on the machine but nothing gets through from outside:"
		echo "the cloud's own firewall is still shut. Oracle: Networking > Virtual Cloud"
		echo "Networks > your VCN > Security Lists > Default > Add Ingress Rule"
		echo "(source 0.0.0.0/0, protocol TCP, destination port $port). Google: VPC"
		echo "network > Firewall > Create rule (ingress, 0.0.0.0/0, tcp:$port)."
		echo "Then:  tools/netplay/deploy.sh --check $host $port"
	fi
	exit 1
fi

cfg=${XDG_DATA_HOME:-$HOME/.local/share}/buusfury/buusfury.netplay
mine="server $host:$port"
mkdir -p "$(dirname "$cfg")"
old=$(grep '^server ' "$cfg" 2> /dev/null | head -1)
if [ -n "$old" ] && [ "$old" != "$mine" ]; then
	cp "$cfg" "$cfg.bak"
	echo "your buusfury.netplay pointed at '${old#server }'; kept a copy as buusfury.netplay.bak"
fi
{ grep -v '^server ' "$cfg" 2> /dev/null; echo "$mine"; } > "$cfg.new" && mv "$cfg.new" "$cfg"
echo
echo "Done. Your game now uses:  $mine"
echo "Everyone else puts that same line in their buusfury.netplay"
echo "(~/.local/share/buusfury/ on Linux), then: Start, R past Options, ONLINE page."
