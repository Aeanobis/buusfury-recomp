# Buu's Fury online battles: the relay

Two players fight one-on-one over the internet (see the netplay.c entry in the
main README). They never connect to each other directly: both connect to this
small relay, which pairs them (by a four-digit room code for a battle with
a friend, or with whoever else is waiting for a random battle) and passes
bytes between them. It needs only Python 3 and one open TCP port.

## No server of your own: run it on your PC (free)

The relay only has to be up while the two of you fight, so when you are one
of the players it can run right here. `host-here.sh` starts it and opens a
tunnel through bore.pub, a free public TCP relay (no account, no card:
https://github.com/ekzhang/bore), so the other player can reach it from
anywhere:

    cargo install bore-cli          # once; needs Rust (https://rustup.rs)
    tools/netplay/host-here.sh

It prints the line the other player puts in their `buusfury.netplay`
(`server bore.pub:NNNNN`; the port is random each time unless `--port N`
gets you a fixed one that nobody else holds), points your own game straight
at the relay (`server 127.0.0.1:7777`), and shows players coming and going.
Keep the window open while you play; Ctrl-C stops it. `--no-tunnel` skips
bore for a LAN game or a router that already forwards TCP 7777 to this PC,
and `--to HOST` uses a bore server of your own. bore.pub is one volunteer's
machine: when it is down, try again later or use a server of your own.

## Running it on a server (always on, free)

For random battles with people you do not know, or an address that works
while your PC is off, run the relay on a machine with a public address. It
needs next to nothing (a few hundred bytes a second per match, plus 65 KB at
the start), so a free tier that stays free for good is enough. As of October
2026 two of them give a real machine with an open TCP port; both ask for a
card at sign-up for identity only (not charged inside the free tier; prepaid
and virtual cards are refused):

- Oracle Cloud Always Free (oracle.com/cloud/free): a VM.Standard.E2.1.Micro
  (1 GB) or an Ampere A1 VM within 2 OCPUs and 12 GB, public IPv4 included,
  for good.
- Google Cloud Always Free: one e2-micro in us-west1, us-central1 or
  us-east1 with a 30 GB standard disk and 1 GB of outgoing traffic a month
  (hundreds of hours of matches). Not free there: an external IPv4 address
  in use, $0.005 an hour past one free hour a month, about $3.65 a month
  (cloud.google.com/vpc/network-pricing). An IPv6 address costs nothing,
  but then every player needs IPv6 at home.

### Google

    ~/google-cloud-sdk/bin/gcloud auth login     # once, after signing up
    tools/netplay/gcloud-up.sh                   # VM with IPv4 (~$3.65 a month)
    tools/netplay/gcloud-up.sh --ipv6-only       # VM with IPv6 only ($0)

gcloud-up.sh needs the Google Cloud CLI (unpacked into ~/google-cloud-sdk,
or on the PATH) logged in to the account, and the project with billing set
(`gcloud config set project ID`); it then enables Compute Engine, makes a
small network of its own called netplay with an IPv4+IPv6 subnet (the
default network cannot carry IPv6; both are free), opens TCP 7777 and 22 in
its firewall, creates the VM inside the free limits (e2-micro, standard
disk, us-central1; the console's defaults would pick a balanced disk, which
is not free), puts your SSH key on it, waits for it and hands over to
deploy.sh below. `--down` deletes it all again. The same by hand in the
console: VPC network > Create network, custom subnet in us-central1 with
IPv4 and IPv6 and external IPv6 access; Firewall > Create rule: ingress on
that network, source 0.0.0.0/0 (and ::/0 in a second rule), tcp:22,7777; VM
instances > Create instance, name netplay, region us-central1, machine type
e2-micro, boot disk Ubuntu 24.04 of type "Standard persistent disk", 10 GB,
Networking on that subnet, IP stack type IPv4 and IPv6, external IPv4 None
for the free version; then `tools/netplay/deploy.sh <your user>@<the VM's
address>` after adding your public key under Metadata > SSH keys.

### Oracle, step by step

1. Sign up with your real address and a region near you.
2. Compute > Instances > Create instance: image Canonical Ubuntu 24.04, shape
   VM.Standard.E2.1.Micro (marked Always Free), assign a public IPv4 address,
   and paste your SSH public key (`~/.ssh/id_ed25519.pub`; `ssh-keygen -t
   ed25519` makes one). Note the public IP once it is running.
3. Networking > Virtual Cloud Networks > your VCN > Security Lists > Default
   Security List > Add Ingress Rules: source CIDR 0.0.0.0/0, IP protocol TCP,
   destination port range 7777.
4. From this folder: `tools/netplay/deploy.sh ubuntu@<public IP>` (the login
   is `opc` on an Oracle Linux image).

### Any Linux box with a public address and sudo

deploy.sh copies the relay over, installs it as a service that starts with
the machine, opens the port in the machine's own firewall (ufw, firewalld,
or the iptables rules Oracle's Ubuntu images ship with, which reject
everything else), checks from here that the relay answers through the
public address (an IPv6 address works too), and writes the `server` line
into your buusfury.netplay. `tools/netplay/deploy.sh --check HOST` tests a
relay from here any time.

By hand instead:

    sudo cp netplay_server.py /usr/local/bin/
    sudo cp netplay.service /etc/systemd/system/
    sudo systemctl enable --now netplay
    sudo ufw allow 7777/tcp        # or whatever your firewall is

`journalctl -u netplay -f` shows rooms being hosted, joined and finished.
Another port: edit `--port` in the service file.

To try it on one machine first: `python3 netplay_server.py --port 7777` in a
terminal, and `server 127.0.0.1:7777` in the config below.

## Pointing the game at it

Each player puts the relay's address in `~/.local/share/buusfury/buusfury.netplay`
(`host-here.sh` writes it for the player running it):

    server example.com:7777

Then, in the game: press Start and go right past Options (R) to the ONLINE
page, which has two ways to find an opponent:

- RANDOM BATTLE: the relay pairs you with whoever else is waiting for one
  (LOOKING FOR AN OPPONENT until someone comes; B cancels). The first to
  arrive is the host of the match.
- HOST A BATTLE and JOIN WITH CODE: one of you hosts (the game shows a room
  code to pass on), the other joins by typing it in.

The fight starts where the host stands, with the host's character and the
guest's; it ends when one is beaten, and both games go back to exactly where
they were.

Both players must run the same build of the game (the same ROM, the same
mods), or the handshake refuses with THE OTHER SIDE RUNS A DIFFERENT VERSION.

## Protocol (for the curious)

Opening exchange, little-endian: client sends `"BFNP"`, version (u8, 1),
command (u8: 1 host, 2 join, 3 random), code (u32; a host may ask for a code,
0 = any); server answers `"BFNP"`, status (u8), code (u32): 0 paired (the
relay starts), 1 no such room, 2 room full, 3 bad request, 4 queued for a
random battle (wait for a status 0; you will be the host). A host or a queued
player gets its status 0 as a second answer when the other player arrives.
From then on the relay copies bytes both ways until one side closes. A
waiting player that closes its connection is forgotten at once; one nobody
comes to is dropped after ten minutes.
