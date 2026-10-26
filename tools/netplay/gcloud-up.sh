#!/bin/bash
# Make the Buu's Fury netplay relay's server on Google Cloud: the e2-micro VM
# of the Always Free tier, set up inside the free limits (one e2-micro in
# us-central1, a standard 10 GB disk), the firewall opened, and the relay
# installed by deploy.sh. One thing there is not free: an external IPv4
# address in use costs $0.005 an hour past one free hour a month, about
# $3.65 a month. An external IPv6 address costs nothing, but then every
# player's internet connection needs IPv6.
#
#   tools/netplay/gcloud-up.sh              VM with an IPv4 address (~$3.65 a month)
#   tools/netplay/gcloud-up.sh --ipv6-only  VM with only an IPv6 address ($0)
#   tools/netplay/gcloud-up.sh --down       delete the VM and its firewall rules
#   options: --port N (default 7777), --zone us-west1-b (us-west1, us-central1
#   or us-east1 only: the free e2-micro lives nowhere else)
#
# Before the first run: sign up at cloud.google.com/free (a card is asked for
# identity; nothing inside the free tier is charged), then log the CLI in:
#   ~/google-cloud-sdk/bin/gcloud auth login
# The CLI is used from the PATH or from ~/google-cloud-sdk/bin.
set -e
here=$(cd "$(dirname "$0")" && pwd)
gcloud=$(command -v gcloud || true)
[ -z "$gcloud" ] && [ -x "$HOME/google-cloud-sdk/bin/gcloud" ] && gcloud=$HOME/google-cloud-sdk/bin/gcloud
if [ -z "$gcloud" ]; then
	echo "gcloud is not installed: unpack the Google Cloud CLI into ~/google-cloud-sdk" >&2
	echo "(https://cloud.google.com/sdk/docs/install)" >&2
	exit 1
fi
name=netplay
zone=us-central1-a
port=7777
user=$USER
ipv6=0
down=0
while [ $# -gt 0 ]; do
	case "$1" in
		--ipv6-only) ipv6=1 ;;
		--down) down=1 ;;
		--port) port=$2; shift ;;
		--zone) zone=$2; shift ;;
		-h|--help) sed -n '2,20p' "$0" | cut -c3-; exit 0 ;;
		*) echo "unknown option $1 (try --help)" >&2; exit 2 ;;
	esac
	shift
done
region=${zone%-*}

account=$("$gcloud" auth list --filter=status:ACTIVE --format='value(account)' 2> /dev/null | head -1)
if [ -z "$account" ]; then
	echo "the CLI is not logged in. Run:  $gcloud auth login" >&2
	exit 1
fi
project=$("$gcloud" config get-value project 2> /dev/null || true)
if [ -z "$project" ]; then
	projects=$("$gcloud" projects list --format='value(projectId)' 2> /dev/null || true)
	if [ "$(echo "$projects" | grep -c .)" = 1 ]; then
		project=$projects
		"$gcloud" config set project "$project" > /dev/null 2>&1
	else
		echo "pick the project first:  $gcloud config set project ID" >&2
		echo "(yours: $(echo $projects))" >&2
		exit 1
	fi
fi
echo "account $account, project $project, zone $zone"

if [ $down = 1 ]; then
	"$gcloud" compute instances delete "$name" --zone "$zone" --quiet 2> /dev/null || true
	"$gcloud" compute firewall-rules delete "$name-v4" "$name-v6" --quiet 2> /dev/null || true
	"$gcloud" compute networks subnets delete "$name" --region "$region" --quiet 2> /dev/null || true
	"$gcloud" compute networks delete "$name" --quiet 2> /dev/null || true
	echo "gone. (Your buusfury.netplay still names it.)"
	exit 0
fi

case "$region" in
	us-west1|us-central1|us-east1) ;;
	*) echo "the free e2-micro only lives in us-west1, us-central1 or us-east1; zone $zone is elsewhere" >&2; exit 1 ;;
esac

echo "== enabling Compute Engine (the first time takes a minute)"
"$gcloud" services enable compute.googleapis.com
# a network of its own: the default one is auto-mode, and only a custom-mode
# network may have the IPv4+IPv6 subnet the IPv6 address needs (both are free)
echo "== network $name with an IPv4+IPv6 subnet in $region"
"$gcloud" compute networks describe "$name" > /dev/null 2>&1 \
	|| "$gcloud" compute networks create "$name" --subnet-mode custom --quiet
"$gcloud" compute networks subnets describe "$name" --region "$region" > /dev/null 2>&1 \
	|| "$gcloud" compute networks subnets create "$name" --network "$name" --region "$region" --range 10.77.0.0/24 --stack-type IPV4_IPV6 --ipv6-access-type EXTERNAL --quiet
echo "== firewall: TCP $port (the relay) and 22 (ssh) in"
"$gcloud" compute firewall-rules describe "$name-v4" > /dev/null 2>&1 \
	|| "$gcloud" compute firewall-rules create "$name-v4" --network "$name" --direction INGRESS --allow "tcp:22,tcp:$port" --source-ranges 0.0.0.0/0 --target-tags "$name" --quiet
"$gcloud" compute firewall-rules describe "$name-v6" > /dev/null 2>&1 \
	|| "$gcloud" compute firewall-rules create "$name-v6" --network "$name" --direction INGRESS --allow "tcp:22,tcp:$port" --source-ranges ::/0 --target-tags "$name" --quiet
[ -f ~/.ssh/id_ed25519.pub ] || ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/id_ed25519
if "$gcloud" compute instances describe "$name" --zone "$zone" > /dev/null 2>&1; then
	echo "== the VM $name exists already"
else
	if [ $ipv6 = 1 ]; then
		echo "== creating the VM (e2-micro, standard disk, IPv6 only: nothing to pay)"
		addr=(--stack-type IPV4_IPV6 --no-address --ipv6-network-tier PREMIUM)
	else
		echo "== creating the VM (e2-micro, standard disk; its IPv4 address is the one paid thing, about \$3.65 a month)"
		addr=()
	fi
	"$gcloud" compute instances create "$name" --zone "$zone" --machine-type e2-micro \
		--image-family ubuntu-2404-lts-amd64 --image-project ubuntu-os-cloud \
		--boot-disk-type pd-standard --boot-disk-size 10GB --tags "$name" \
		--network "$name" --subnet "$name" \
		--metadata "enable-oslogin=false,ssh-keys=$user:$(cat ~/.ssh/id_ed25519.pub)" "${addr[@]}" --quiet
fi
if [ $ipv6 = 1 ]; then
	ip=$("$gcloud" compute instances describe "$name" --zone "$zone" --format='value(networkInterfaces[0].ipv6AccessConfigs[0].externalIpv6)')
else
	ip=$("$gcloud" compute instances describe "$name" --zone "$zone" --format='value(networkInterfaces[0].accessConfigs[0].natIP)')
fi
if [ -z "$ip" ]; then
	echo "the VM has no external address; look at it in the console" >&2
	exit 1
fi
echo "== the VM is at $ip; waiting for its ssh"
up=0
for i in $(seq 1 36); do
	if ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$user@$ip" true 2> /dev/null; then
		up=1
		break
	fi
	sleep 5
done
if [ $up = 0 ]; then
	echo "no ssh answer from $ip after three minutes. Try later:  tools/netplay/deploy.sh $user@$ip $port" >&2
	exit 1
fi
exec "$here/deploy.sh" "$user@$ip" "$port"
