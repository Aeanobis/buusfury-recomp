#!/bin/sh
# Regenerates the sprites converted from Buu's Fury for Legacy of Goku II.
# Needs both games built and their sprites extracted into build/sprites/ (see
# extract.c), and the games' palettes in sprites/palettes/ (not in the
# repository, being the games' data): <game>-obj.bin is the 512-byte object
# palette extract.c writes as objpalette.bin, taken while the playable
# characters are on screen; lgoku2-status-bg.bin is the status screen's
# background palette (palette RAM 0x05000000, 512 bytes, on that screen).
# The list pairs each Legacy of Goku II animation number with the Buu's Fury
# animation that plays in its place:
#   0 stand  1 walk  2 dizzy  3 knocked down  4 arms raised  5 fire pose
#   6-8 punches  9 transform  11 cheer  12 get up  13 run  14 celebrate
#   16 Masenko (overhead throw)  18 Kamehameha  20 flying
cd "$(dirname "$0")/../.."
#   19 Goku's and Piccolo's second special  21 Super Saiyan forms only  22 Piccolo only
MAP="0:0,1:1,2:9,3:17,4:13,5:3,6:4,7:5,8:6,9:14,10:0,11:12,12:15,13:2,14:13,15:0,16:19,17:0,18:18,19:19,20:16,21:14,22:0"
# For Buu's Fury sprites that have no flying animation (16): running stands in
MAP2="0:0,1:1,2:9,3:17,4:13,5:3,6:4,7:5,8:6,9:14,10:0,11:13,12:17,13:2,14:13,15:0,16:19,17:0,18:18,19:19,20:2,21:14,22:0"
# The transformation (9) and, in Super Saiyan sprites, the change back (21)
# depend on what the Buu's Fury sprite has. Buu's Fury keeps "turn Super
# Saiyan" in animation 10 and "turn back" in the Super Saiyan sprite's
# animation 11. (14, which looked right by its length, is a jump for joy.)
BASE=$(echo "$MAP" | sed 's/9:14/9:10/')                 # ordinary sprite that can turn Super Saiyan
PLAIN=$(echo "$MAP" | sed 's/9:14/9:13/')                # no transformation of its own: arms raised
PLAIN2=$(echo "$MAP2" | sed 's/9:14/9:13/')
SUPER=$(echo "$MAP" | sed 's/9:14/9:13/; s/21:14/21:11/')   # Super Saiyan sprite
SUPER2=$(echo "$MAP2" | sed 's/9:14/9:13/; s/21:14/21:11/')
C="python3 -I tools/sprites/convert_bf.py"
G=games/lgoku2/mods/generated
# name, Buu's Fury sprite, Legacy of Goku II sprite to take the collision boxes from, ROM offset
# Adult Gohan's transformation is the Mystic form (adult_gohan.c): the power-up
# pose (the second frame of 13) throughout, as the Super Saiyan transformation
# holds its pose; powering down relaxes out of it.
MYSTIC=$(echo "$PLAIN" | sed 's/9:13/9:13+1/; s/21:14/21:13-/')
$C adultGohan   111 0x38 0x800000 "$MYSTIC" $G/adult_gohan_data.inc
$C saiyaman     112 0x38 0x840000 "$PLAIN"  $G/saiyaman_data.inc
$C gogeta       103 60   0x880000 "$PLAIN"  $G/gogeta_data.inc
$C vegito       285 60   0x900000 "$PLAIN2" $G/vegito_data.inc
$C gokuSsj3     121 61   0x940000 "$SUPER2" $G/goku_ssj3_data.inc
# Buu's Fury's Super Saiyan Goku, only for the Super Saiyan 3 transformation
# scene (looks.c): its animation 9 is that game's turning Super Saiyan 3
# (animation 10, eleven frames of hair growing). The scene shows the first
# two frames once and then the last two turn about, so four are chosen.
SSJ3SCENE=$(echo "$MAP" | sed 's/9:14/9:10[0.3.8.10]/')
$C gokuSsjBf    119 61   0xFD0000 "$SSJ3SCENE" $G/goku_ssj_bf_data.inc
$C vegetaBuu    280 137  0x980000 "$BASE"   $G/vegeta_buu_data.inc
$C vegetaMajin  284 139  0x9C0000 "$SUPER"  $G/vegeta_majin_data.inc
# Gohan in the Great Saiyaman's clothes without the helmet and cape, and the same as a Super Saiyan
$C saiyamanBare    113 0x38 0xA40000 "$BASE"  $G/saiyaman_bare_data.inc
$C saiyamanBareSsj 115 0x3A 0xA80000 "$SUPER" $G/saiyaman_bare_ssj_data.inc
# Kid Trunks, and as a Super Saiyan
$C kidTrunks    270 128  0xAC0000 "$BASE"   $G/kid_trunks_data.inc
$C kidTrunksSsj 271 130  0xB00000 "$SUPER"  $G/kid_trunks_ssj_data.inc
# Gotenks (kid Trunks fused with Goten), and as a Super Saiyan
$C gotenks      127 128  0xB40000 "$BASE"   $G/gotenks_data.inc
$C gotenksSsj   130 130  0xB80000 "$SUPER2" $G/gotenks_ssj_data.inc

# Adult Gohan's portraits: status screen (normal, Super Saiyan), then dialogue (normal, Super Saiyan).
# Needs the portraits of both games extracted into build/sprites/lg2portraits and bfportraits.
P="python3 -I tools/sprites/convert_portrait.py"
$P adultGohanPortraits 0x8C0000 $G/adult_gohan_portraits.inc /dev/null 38:1:status 43:4:status 38:50:master 43:53:master
# The other looks: each as a status-screen portrait then a dialogue one.
# Great Saiyaman; Gogeta; Vegito; Super Saiyan 3 Goku; Vegeta (Buu saga); Majin Vegeta;
# Gohan in the Saiyaman clothes without the helmet; the same as a Super Saiyan;
# kid Trunks; kid Trunks as a Super Saiyan; Gotenks; Gotenks as a Super Saiyan
$P lookPortraits 0xA00000 $G/look_portraits.inc sprites/converted-portraits-preview.png \
	39:1:status 39:50:master  26:7:status 26:57:master  143:7:status 143:57:master \
	53:7:status 53:57:master  139:16:status 139:98:master  138:17:status 138:99:master \
	42:1:status 42:50:master  46:4:status 46:53:master \
	130:11:status 130:90:master  132:13:status 132:93:master \
	60:11:status 60:90:master  63:13:status 63:93:master

# The fusion dance, drawn for the partner who appears when Goku or Vegeta fuses
python3 -I tools/sprites/make_dance.py gokuDance 119 21 $G/goku_dance.inc
python3 -I tools/sprites/make_dance.py vegetaDance 280 21 $G/vegeta_dance.inc
python3 -I tools/sprites/make_dance.py kidTrunksDance 270 21 $G/kid_trunks_dance.inc
python3 -I tools/sprites/make_dance.py gotenDance 124 21 $G/goten_dance.inc

# ---- the other way: Legacy of Goku II sprites for Buu's Fury ---------------------
# Young Gohan's four outfits and his Super Saiyan sprite (games/buusfury/mods/switch.c),
# into the grown part of that ROM. Buu's Fury animation <- Legacy of Goku II animation.
L="python3 -I tools/sprites/convert_lg2.py"
GB=games/buusfury/mods/generated
mkdir -p $GB
YOUNG="0:0,1:1,2:13,3:5,4:6,5:7,6:8,7:0,8:0,9:2,10:9,11:21,12:11,13:4,14:14,15:12,16:20,17:3,18:18,19:16,20:0,21:0"
$L youngGohanGi      56 107 0x800000 "$YOUNG" $GB/youngGohanGi_data.inc
$L youngGohanNamek   53 107 0x840000 "$YOUNG" $GB/youngGohanNamek_data.inc
$L youngGohanArmour  55 107 0x880000 "$YOUNG" $GB/youngGohanArmour_data.inc
$L youngGohanArmourB 57 107 0x8C0000 "$YOUNG" $GB/youngGohanArmourB_data.inc
$L youngGohanSsj     59 107 0x900000 "$YOUNG" $GB/youngGohanSsj_data.inc
# Future Trunks (his jacket, his armour, each with his Super Saiyan) and Vegeta's
# Saiyan armour (with his), adult sprites on Vegeta's template (switch.c looks)
$L futureTrunks          128 279 0x940000 "$YOUNG" $GB/futureTrunks_data.inc
$L futureTrunksSsj       134 279 0x980000 "$YOUNG" $GB/futureTrunksSsj_data.inc
$L futureTrunksArmour    129 279 0x9C0000 "$YOUNG" $GB/futureTrunksArmour_data.inc
$L futureTrunksArmourSsj 130 279 0xA00000 "$YOUNG" $GB/futureTrunksArmourSsj_data.inc
$L vegetaArmour          137 279 0xA40000 "$YOUNG" $GB/vegetaArmour_data.inc
$L vegetaArmourSsj       140 279 0xA80000 "$YOUNG" $GB/vegetaArmourSsj_data.inc
