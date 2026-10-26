#!/bin/sh
# Disassemble part of a game's ROM as Thumb code: tools/dis.sh lgoku2 0x08008A4E 0x08008B3C [arm]
D=$(dirname "$0")/..
MODE=-Mforce-thumb; [ "$4" = arm ] && MODE=
"$D/deps/armtools/usr/bin/arm-none-eabi-objdump" -D -b binary -marm $MODE --adjust-vma=0x08000000 \
	--start-address="$2" --stop-address="$3" "$D/build/$1/rom.gba" | sed -n '/^ *[0-9a-f]*:/p'
