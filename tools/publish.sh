#!/bin/sh
# Stages the project as two git repositories, one per game: the shared runtime
# and tools with that game's mods. Nothing derived from the ROMs goes in
# (ROMs, recompiled code, extracted/converted sprites and palettes, save
# states, RAM images, reference sprite sheets); whoever builds it needs their
# own ROM, and so does the built game (runtime/romload.c). Without --push it only stages and commits
# under $PUBLISH_DIR (default ~/Desktop/repos); with --push it creates the
# GitHub repositories (private unless --public) or pushes updates to them.
#   tools/publish.sh [--push] [--public]
set -e
cd "$(dirname "$0")/.."
SRC=$(pwd)
OUT=${PUBLISH_DIR:-$HOME/Desktop/repos}
PUSH=0; VIS=--private
for a in "$@"; do
	[ "$a" = "--push" ] && PUSH=1
	[ "$a" = "--public" ] && VIS=--public
done
mkdir -p "$OUT"

publish() {
	game=$1; name=$2; other=$3; message=$4; description=$5
	dst="$OUT/$name"
	mkdir -p "$dst"
	rsync -a --delete --delete-excluded \
		--exclude '/.git' --exclude '/build' --exclude '/gen' --exclude '/deps' --exclude '*.state' --exclude '*.state[0-9]' --exclude '*.sav' --exclude '*.resume' --exclude '*.auto[0-9]' \
		--exclude '/lgoku1' --exclude '/lgoku2' --exclude '/buusfury' \
		--exclude '/games/lgoku1' \
		--exclude "/games/$other/mods" --exclude "/games/$other/mods-examples" --exclude "/games/$other/states" --exclude "/games/$other/profile" --exclude "/games/$other/patches" \
		--exclude '/games/*/states' --exclude '/games/*/mods/generated' --exclude '/games/*/profile/iwram.bin' \
		--exclude '/sprites/*.png' --exclude '/sprites/*.txt' --exclude '/sprites/palettes' --exclude '*.gba' --exclude '*.GBA' --exclude '__pycache__' \
		"$SRC/" "$dst/"
	cat > "$dst/.gitignore" <<'IGN'
# everything derived from the games' ROMs stays out
build/
gen/
deps/
/lgoku1
/lgoku2
/buusfury
games/*/states/
games/*/mods/generated/
games/*/profile/iwram.bin
sprites/*.png
sprites/*.txt
sprites/palettes/
__pycache__/
*.state
*.sav
*.gba
*.GBA
IGN
	cd "$dst"
	[ -d .git ] || git init -q
	git add -A
	if ! git diff --cached --quiet; then
		git commit -q -m "$message

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
	fi
	echo "$name: $(git ls-files | wc -l) files, $(du -sh --exclude=.git . | cut -f1), last commit $(git log -1 --format=%h)"
	if [ $PUSH = 1 ]; then
		if ! git remote get-url origin >/dev/null 2>&1; then
			gh repo create "$name" $VIS --source=. --push --description "$description" >/dev/null
		else
			git push -q origin HEAD
		fi
		echo "  pushed: $(gh repo view "$name" --json url -q .url)"
	fi
	cd "$SRC"
}

publish lgoku2 lgoku2-recomp buusfury "Dragon Ball Z: The Legacy of Goku II, statically recompiled to native Linux, with mods" "Legacy of Goku II recompiled to native Linux, with mods (needs your own ROM)"
publish buusfury buusfury-recomp lgoku2 "Dragon Ball Z: Buu's Fury, statically recompiled to native Linux, with mods" "Buu's Fury recompiled to native Linux, with mods (needs your own ROM)"
