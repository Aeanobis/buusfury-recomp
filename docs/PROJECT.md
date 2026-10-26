# Legacy of Goku 1, 2 and Buu's Fury — static recompilation with mod support

Builds native Linux executables of the three GBA games. ROMs, patches and
links to the built games live in `~/Games/<game>/`.

    ./setup.sh            # once: fetch and build dependencies into deps/ (no root)
    make                  # build ./lgoku2  (Legacy of Goku II)
    make GAME=lgoku1      # build ./lgoku1  (The Legacy of Goku)
    make GAME=buusfury    # build ./buusfury (Buu's Fury)
    ./lgoku2              # play; --help lists options

Keys: arrows move, X = A, Z = B, A = L, S = R, Enter = Start, Backspace =
Select, Tab = fast forward, Space = skip scenes, F11 fullscreen, Esc quit
(quitting saves your position; starting resumes it). Game controllers work.
Picture: F2 original pixels / smooth (xBRZ) / HD redraw (neural network on the
GPU, see `runtime/ai_upscale.cpp`), F3 scanlines / LCD grid / off,
F4 colour correction.
Saves: `~/.local/share/lgoku2/lgoku2.sav` and `~/.local/share/lgoku/lgoku.sav`.

Each game has a folder under `games/`: `game.mk` (which ROM), `profile/`
(recorded code-discovery data), and for modding `patches/`, `mods/`,
`entries.txt`.


## Building from this repository

Nothing taken from the games is in the repository: no ROMs, no recompiled
code, no extracted or converted sprites or palettes, no save states. You need
your own ROM of each game, and so does the built game: the executable holds
none of the cartridge. It reads your ROM when it starts (from
`~/.local/share/<game>/<game>.gba`, where `make` leaves a link to the ROM it
was built from, or `<game>.gba` next to the program, or `--rom FILE`), checks
its SHA-1 against the dump the code was recompiled from (the compiled blocks
mirror that dump byte for byte, so no other will run) and applies the
patches itself (`runtime/romload.c`). `games/<game>/game.mk` says where the
build expects the ROM and its SHA-1; `ROM_SHA1` may list several dumps.
Legacy of Goku II's entry is an SRAM-patched dump (`SAVE_MEMORY := SRAM`);
with a clean cartridge dump, put its SHA-1 there and drop that line.

What is shared is this repository, not the built programs: besides the
recompiled code, each game's executable carries the sprites and areas its
mods convert from the *other* game's ROM (`games/<game>/mods/generated/`,
made from your ROMs by `tools/sprites/make_converted_sprites.sh`). Then:

    ./setup.sh                                  # third-party parts under deps/ (mGBA, xBRZ, ncnn, ...)
    make GAME=buusfury                          # Buu's Fury builds on its own (Legacy of Goku II's outfits are optional)
    # Legacy of Goku II's mods need data converted from Buu's Fury (games/lgoku2/mods/generated/, not here);
    # until it is there make builds the bare game and says what is missing. Each game extracts its own graphics:
    make GAME=lgoku2 MODS=tools/mods/extract.c
    EXTRACT_FN=0801F845 EXTRACT_LIST=tools/sprites/graphics-lists/lgoku2.txt EXTRACT_OUT=build/sprites/lg2 ./lgoku2 --headless 200
    make GAME=buusfury MODS=tools/mods/extract.c
    EXTRACT_FN=0803D941 EXTRACT_LIST=tools/sprites/graphics-lists/buusfury.txt EXTRACT_OUT=build/sprites/bf ./buusfury --headless 200
    tools/sprites/make_converted_sprites.sh     # the converted sprites into games/*/mods/generated/ (its header: palettes, captures;
                                                # run the extractors with --load-state of an in-game state so the palette is the in-game one)
    make GAME=lgoku2 && make GAME=buusfury      # with the mods complete

Playing online (`games/buusfury/mods/netplay.c`) needs both players on the
same build: the same repository revision, mods and ROM.

`make profile GAME=<game>` records the code the recompiler missed
(`games/<game>/profile/`); `missed.txt` is kept here, the RAM image it also
writes is not (it holds the game's code) and is remade by that command.

## Modding

There are three ways to change a game, from easiest to most powerful.

**1. ROM patches.** Any `.ips` file in `games/<game>/patches/` is applied to the
ROM at build time, in alphabetical order. `games/lgoku2/patches/enhanced.ips`
is the "Enhanced" hack; delete it and run `make` for the original game.
A patch that changes machine code is fine too, the code is recompiled.

**2. Data edits in C.** A mod can change ROM data (tables, text, graphics)
before the game boots, with names instead of hex offsets. See
`games/lgoku2/mods-examples/starting_stats.c`.

**3. Code hooks in C.** A mod can run native C whenever the game reaches a
given function: inspect or change its arguments, replace it entirely, read and
write game memory, run code every frame, draw on the screen, read the PC
keyboard, save and load states. See `trace.c` and `savestates.c` in the same
folder; the whole API is `runtime/mod.h`.

To enable a mod, put its `.c` file in `games/<game>/mods/` and run `make`.
`--no-mods` runs the game without them.

What this does not give you is the game's source. Hooks attach to addresses,
so each new mod starts with finding the function or variable it needs. Known
so far for Legacy of Goku II (ROM offsets; code address = 0x08000000 + offset):

Everything identified so far is written down as code in
`games/lgoku2/mods/lg2.h`: the character record layout (HP, energy, the three
stats in 8.8 fixed point, experience, flags), the starting-stats, growth and
experience tables, and the functions for levels, experience and levelling up.
The game is C++ built around a bytecode script interpreter; source file names
left in the ROM (`CharacterData.cpp`, `ByteCodeFunctionList.cpp`, ...) help
navigation.

Active mods for Legacy of Goku II (`games/lgoku2/mods/`). None of them has a
keyboard key: everything is reached through the game's own screens, with the
game's buttons.

- `hub.c`: an "Extras" page in the Start menu, between Items and Options (L/R
  as for the other pages). It leads to everything below that is marked (Extras).
- `shop.c` (Extras): Equip (what is worn, on Buu's Fury's Equip screen), buying
  gear, learning any other base character's special (nine in all; a sprite
  that lacks the move's animation gets a stand-in built from its fire pose,
  padded to the length the move steps through, see gohan_attacks.c; it goes in
  a third special slot of the character's own, which L reaches right after
  the specials the story has given him, marked SLOT 3 beside the attack box),
  a full restore. Paid for in zenie.
- `progression.c` (Extras, or L+R together): stat points on level-up; stats go
  to 200 instead of 100 (endurance counts for less past 100); level cap
  The status page's STR/POW/END bars are redrawn to the 200 scale (100 is
  halfway): hooks on the page's number formatter and rectangle fill. The page
  leaves out the NEXT line at exactly level 50 (the old cap), so its one call
  for the level is told 49 then and the "LEVEL: 49" it formats is put back to
  50 (the enemy-killed code, which gives nothing at 50, is told 49 the same way).
  100; +5 levels at the three-year time skip.
- `bosses.c` (Extras, only in Majin Buu's battleground, zone 39): BOSS FIGHT,
  a list of the game's own bosses (Mecha Frieza to Cooler) and Majin Buu's
  forms from Buu's Fury (Majin, Evil, Super, Kid Buu: sprites converted by
  `tools/sprites/convert_bf_boss.py`, driven by the game's Cell Jr/Cooler
  fighting AI with their own stats). The chosen one is built with the game's
  enemy constructor from a spawn record in scratch ROM and put in the world
  list, with a copy of his type record in a spare slot that gives the ordinary
  death and some experience. A sprite short of the animations the AI uses
  (King Cold's) gets a longer copy with stand-in rows, as in gohan_attacks.c.
- Transformations boost STR/POW/END (looks.c, through shop.c's stat
  multiplier, which the status page shows): a Super Saiyan +20%, Gohan's
  Super Saiyan 2 +40%, Super Saiyan 3 and adult Gohan's Mystic form +60%.
  Super Saiyan 3 drains energy about five times as fast as Super Saiyan (12 a
  second more). The game's own
  transformations only add speed (+40%) and drain energy (about 3 a second).
- `halo.c`: Goku's halo from Buu's Fury, drawn above his head once the story
  has him die (he leaves the party during the Cell Games, story flag 191 on);
  also a look of his own on the Switch Characters screen (Up/Down), remembered.
  Dead, he is put back in the party (the game drops him for good; his record
  is untouched, so he is back at his level with his attacks), and from then
  on the halo is not a look that can be taken off: his figure on the Switch
  Characters screen wears it too.
- `nameks.c`: the seven missing Namekians count as found: their story flags
  (the block 288..295, one column of the flag table at ROM 0x2DD40; talking
  to a found Namekian sets one of them) are set once; the quest's return to
  Capsule Corporation is all that is left.
- `heaven.c`: in Heaven (Buu's Fury's, zone 42 area 1, by Instant
  Transmission) Goku is about in his halo, walking a patch of the field, and
  talks when spoken to with A. He is a person of the area the game's own way
  (the area record's people: +0x2C a list {map, then per person a behaviour
  {fn, arg} and an object record {ctor 0x08015E83, 0, x | y << 16, sprite
  def, 1, -> behaviour}}, +0x14 a table of the object records, +4 the count;
  the standing behaviour 0x08003B4B, arg byte 2 the facing). His walking is
  the mod's: place, facing, animation entry (+0x4C) and frame record (+0x20)
  set each frame from the sprite definition's animation table (16 bytes an
  animation: a frame-list pointer per facing, 12 bytes a frame). A near him is
  kept from the game (its own talk would run area scripts Heaven has not
  got); the talk is drawn by the mod while the game runs on without buttons, so
  the music keeps playing.
- `bf_areas.c`: the areas (maps) of Buu's Fury, carried over by
  `tools/maps/convert_bf_area.py` (both games run the same map engine; the
  blocks are copied verbatim with pointers relocated, the tile banks shared in
  one copy at ROM 0xC00000, the areas at 0xD00000; LoG2 zone 20+z is Buu's
  Fury's zone z). They come empty (no people, enemies or scripts) but keep
  their exits, so they join up, and their save points (Capsule discs: stand
  on one and press A, as on the game's own); the Instant Transmission list ends with the
  biggest map of each zone, and leads back out. The game's area lookup and
  its palette-table routine are hooked (Buu's Fury's colour table while one
  of its areas is up). Tile animations and decorations come along; 22 maps
  with a layer kind LoG2 lacks are left out.
- `transmission.c` (Extras): Instant Transmission to any place the world map
  would let you land at.
- `fusion.c`: L goes on past the last attack to the fusions, as in Buu's Fury,
  and B then fuses (the partner appears and they dance): Gogeta (a minute) and
  Vegito (stronger, half a minute) for Goku or Vegeta, Gotenks for Trunks while
  he has his kid look. A Capsule Corp pad ends the wait before the next one.
  Every frame the player object is checked against his character's own sprite
  (the form's entry in the sprite table): a fusion's sprite left on him goes at
  once, any other wrong sprite after half a second (a fusion that ended on a
  switch to someone else once left Gohan drawn as Goku), transformation scenes
  excepted. The stop on L's round (the third special) never touches the
  transformation slot's flag when it is left, and looks.c gives Goku that slot
  back if his record is ever found without it.
- `fusion_moves.c`: while fused, the two special attacks are the fusion's own
  from Buu's Fury, drawn with that game's effects. Gogeta's and Vegito's are
  held: B keeps the beam, dragon or blade out (and hitting) for energy.
- `enemy_hits.c`: shared code for attacks the mods add (finding enemies, and
  hurting them through the game's own routine).
- `combat.c`: enemy health bars, damage numbers, zenie from beaten enemies (the
  total shows at the top of the Start menu); enemy toughness (Extras): normal,
  scaled to your level, tough, brutal. Bosses (an enemy type with 1000 health
  or more) are held to twice their health and 1.4 times their hits whatever
  the mode: scaled, a story boss twenty levels under you was coming out at
  nearly seven times his health.
- `blocking.c`: hold R to guard (no damage); "untouchable" (Extras).
- `adult_gohan.c`, `looks.c`: other looks for the characters, from Buu's Fury,
  chosen on the Switch Characters screen (Up/Down on the highlighted
  character): Gohan (kid, adult, Great Saiyaman, Saiyaman without the helmet),
  Vegeta (armour, Buu saga / Majin), Trunks (future, kid). Portraits follow.
  Super Saiyan 3 for Goku is on the Extras page while he is a Super Saiyan.
  Adult Gohan's transformation slot shows an M (Mystic) in the Z's own style
  (Piccolo's an N), the slot is his as soon as the look is on, the Mystic form
  has the anime's white aura (a flame round him, tongues over his head), and his transformation holds the power-up
  pose like the Super Saiyan one (Buu's Fury animation 13's second frame; the
  conversion's "+1"/"-" mapping syntax). Kid Gohan's transformation goes
  straight to Super Saiyan 2 (form 4, the Cell-fight form; the form table's
  "next" byte of form 2 is pointed at it).
- Super Saiyan 3 is a stop in L's round of the attack slots, right after the
  Z, while Goku is a Super Saiyan (looks.c): the Z's slot visited twice, the
  second time with Buu's Fury's own Z3 icon (its HUD icon bank, ROM 0x718ED8,
  32x16 8bpp icons 1 KB apart with a greyed copy at +0x200; the glyph alone is
  drawn over the box). B there plays the transformation scene with Buu's
  Fury's own turning-Super-Saiyan-3 animation: that game's Super Saiyan Goku
  (sprite 119, converted to ROM 0xFD0000 with its animation 10, eleven frames
  of hair growing, of which four are chosen: the scene shows two frames once
  and then the last two turn about) stands in for the game's Super Saiyan Goku
  (which has no power-up animation) while the game's own scene plays, and at
  its flash the form change (0x08012720, hooked) is turned into the Super
  Saiyan 3 sprite swap (the form "changes" to itself for a frame). Ending it
  (B, or the energy running out) powers him all the way down to his base form
  through the game's power-down scene, played on the Super Saiyan 3 sprite
  (Buu's Fury's turning-back animation). The slot is the only way to it.
- The big attacks grow with the level of whoever fires them (gohan_attacks.c,
  which used to give only Gohan's Kamehameha the gold "electric" look): the
  game's own up to level 24; half as thick again and still blue from 25; from
  50 twice as thick, gold and sparking. Kamehameha, Big Bang and the Spirit
  Bomb (their blue-and-white sprites, found in OAM while the firing pose and
  the 90 frames after it last); the Special Beam Cannon (its yellow, orange
  and pink) only thickens, never changes colour. The plain blast is left alone.
  Their damage grows too (combat.c, by the hit's descriptor: 1.25 times from
  25, 1.5 times from 50; Gohan's Kamehameha keeps its own x3 on top).
- `camp.c`: camps, spots with a Capsule Corp circle (a save pad) of the
  player's choosing: zone 13 area 5 at (252, 163), a snowy clearing, where any
  enemy the game builds is sent far off the map the moment it appears; and
  zone 3 area 3 at (92, 424), the wasteland by the Cell Games arena (its
  enemies stay). Each pad is one more {extra, record} pair in a copy of the
  area's trigger list (the area record is 56 bytes: zone, area, a flag, the
  number of trigger pairs; +0x10 the list; +0x18 the enemy spawn group), the
  pad record {0x0801ED13, 0x0801F1ED, x1, y1, x2, y2} as the game's own,
  whose first word the loader calls to build the pad object. The circle is
  drawn by the mod with the game's own picture of one (generated/pad_image.inc,
  taken off the screen in Capsule Corp), over the floor only: the pixels any
  sprite covers that frame (read from the sprite table and its tiles) are left
  alone, so whoever stands on the circle shows. The first time an area is seen
  after a start it is entered again (a fade) so the loader builds the pad.
- Crash recovery (runtime/main.c): when the core reports a jump to nowhere in
  the windowed game, the frame ends by loading the .auto2 snapshot (ten seconds
  back; .auto1 if there is no other), the mods are told as for any loaded
  state, and looks.c says "CRASH: BACK 10 SECONDS". The .crash state is still
  written first. RC_RECOVER=1 makes a headless run recover too.
- Save pads (the Capsule Corp circles) keep health and energy full while you
  stand on them (savepad.c, from the area's trigger list: records of class
  0x0801ED13 with a rect), and the Super Saiyan 3 drain pauses there.
- Transformations can be turned off: B on the slot again. The game allows
  that for first-level Super Saiyans (the slot's second use goes to the form's
  "next", the base form, through the power-down animation 21); looks.c hooks
  the slot's "usable" check (0x0801749E) so second-level forms (flags bit 1:
  Gohan's Super Saiyan 2, so the Mystic form) can too, and adult_gohan.c gives
  kid Gohan's Super Saiyan 2 sprite (58) a copy with the power-down rows of
  his Super Saiyan sprite (59), which it lacked (copy at ROM 0xFCF000).
- `gohan_attacks.c`: Gohan has a stronger, gold Kamehameha from the start.
- `masenko.c`: Gohan's Masenko works as in Buu's Fury: hold B to raise the
  ball and run a target marker out, let go to lob it there.
- `flight_circle.c`: glowing pads at a few fixed spots; stand on one and press
  A to be carried off.
- `regen.c`: slow health regeneration; the Capsule Corp pad restores health and
  energy.
- `savestates.c`: save states. These, like the picture modes, are functions of
  the program, not of the game, and keep their function keys: F5 saves, F8
  loads (Shift for a second slot).

The sprites and portraits taken from Buu's Fury are converted by
`tools/sprites/make_converted_sprites.sh` into `games/lgoku2/mods/generated/`.

Tools for finding more: `tools/dis.sh` disassembles any range, the `watch.c`
example prints which instruction writes a given memory address, `trace.c`
logs calls to functions, and `--load-state FILE --headless N` replays from a
save state without a window.

Crash forensics: while the window is up the runtime writes a save state to
`<save folder>/lgoku2.auto1` every 5 seconds (the one before to `.auto2`).
The moment the core reports a jump to an invalid address it stops rolling,
writes the broken state to `lgoku2.crash`, and notes it in the mod diary; so
`.auto1`/`.auto2` are the last good states 5 and 10 seconds before a crash
and load with `--load-state` like any other. The mod diary (`lgoku2.modlog`)
also notes every menu, page and scene a mod opens or closes, and when buttons
have been held for 5 seconds with nothing reaching the game, which mod's key
filter is dropping them (a "frozen" game that is still running).

If a hook reports that no compiled block starts at its address, add the
address to `games/<game>/entries.txt` (a line like `T 08012344`) and rebuild.

## How it works

- `tools/recomp.py` finds the game's ARM/Thumb code and emits C
  (`gen/<game>/rc_blocks_*.c`), one function per block of guest instructions.
  Each instruction is the matching instruction body from mGBA's interpreter,
  inlined with a constant opcode, so gcc compiles it to specialised native code.
- The hardware (video, sound, timers, DMA, BIOS calls, EEPROM) is mGBA's core,
  with its CPU loop patched to dispatch into the compiled blocks
  (`runtime/mgba-recomp-hook.patch`).
- `runtime/main.c` is the SDL2 frontend and mod host. Graphics, text and
  music are read from the ROM at run time, so the player's own ROM is loaded
  at start (`runtime/romload.c`): found, checked by SHA-1, patched, padded.

Code the game copies into RAM is compiled too and checked against live memory
before each use. An address with no usable compiled block falls back to mGBA's
interpreter, so a gap costs speed, never correctness. `make profile` records
such addresses; the next `make` compiles them in.

`make verify` runs the same scripted session twice, native and fully
interpreted, without mods, and compares a hash of the picture, sound, CPU
registers, cycle counter and RAM after every frame.

## License

GNU General Public License, version 3 (`LICENSE`); copyright (C) 2026 Aeanobis.
xBRZ, which the program links, is GPLv3 too; mGBA is MPL-2.0 and ncnn BSD.

## Limits

- The generated C is machine-shaped, not readable source.
- It is only as accurate as mGBA 0.10.5 and no faster than it.
- Legacy of Goku II rewrites part of its RAM-resident code while running; those
  parts run in the interpreter (about 1% of instructions in the test session).
- The executables hold no ROM, but they do hold the recompiled code and the
  sprites the mods convert from the other game: share the repository, not them.
- mGBA is MPL-2.0; the instruction bodies in `gen/*/isa-*-inl.h` derive from it.

## Buu's Fury mods (games/buusfury/mods)

Built with `make GAME=buusfury`; run `./buusfury --scale 4`. Saves, snapshots,
the diary and crash recovery work as for Legacy of Goku II (files under
~/.local/share/buusfury/). What the mods know about the game is in `bf.h`:
the save data (0x03001068: byte 0 is the current character), the nine
character records (0x68 bytes from 0x03001568: Goku, Gohan, Goten, Trunks,
Vegeta, Gotenks, Vegito, Gogeta, and a ninth the game uses for guests), the
player object (pointer at 0x03001C34; sprite at +0x58, world x/y at +0x1A0),
the form table (0x086B82EC, 24-byte records) and its sprite resolver
(0x0801713C), the zone/area bytes (0x0300197C), the area loader (0x08008D44,
which rebuilds the player from the current record) and the exit routine
(0x080075AC) the mods use to reload the area with the player in place.

- `watch.c`: the diary line (character, form, health, energy, experience,
  stats, position) on every change of character or form and every ten
  seconds.
- `switch.c`: the five story characters' records are kept flagged as in the
  party (record byte 0 bit 0, which the game's menus test), so the game's own
  Switch Characters at the save pads lists all five and switches natively.
  (A mod list of its own on Select in the Start menu is gone: Select there is
  the game's scouter.) Each character keeps its own record (level, health,
  attacks), so the ones the story has not reached are as the game made them.
  On that screen, Up/Down over a figure changes that character's look
  (outfit), remembered in the settings: the game's own untransformed forms,
  so the record is simply put in them, and transforming stays the game's.
  Gohan also has Legacy of Goku II's young Gohan: his four outfits, converted
  by tools/sprites/convert_lg2.py (the reverse of convert_bf.py) into the
  grown part of the ROM (0x800000 on), standing in for one of adult Gohan's
  outfits in the sprite table (0x086B6BB4 + 4n); all four stand in for the
  school outfit, the one with a transformation, and bring the young Super
  Saiyan sprite (the gi, the one that game has) for its transformed form, so
  the game's transformation shows young Gohan going Super Saiyan from any of
  them. Once the story has swapped Gohan's Super Saiyan skill (20) for the
  Great Saiyaman change (21), a young look puts Super Saiyan back in that
  slot and an adult look the Saiyaman change.
  The looks are also on the game's own Switch Character screen at the pads,
  with Up/Down over a figure as in Legacy of Goku II: the screen keeps a
  figure object (class 0x0804AC20) per party member and a table of {index,
  figure} pairs with the cursor's index 0x48 bytes after it; the record is
  put in the look's form and the figure pointed at its sprite, so the game
  builds what is shown. Trunks has Future Trunks the same way (Legacy of Goku II's jacket and
  Saiyan armour, each with its Super Saiyan, standing in for kid Trunks's
  outfit), and Vegeta his Saiyan armour (with its Super Saiyan, standing in
  for his Buu's Fury clothes); all on Vegeta's sprite as the template, in
  the ROM from 0x940000. The fusions are not on the list and are kept out of
  the party (unless the story is playing one), so the pads' list has the five
  story characters only. A record found in a form that is not its
  character's (Gohan was once left in Goku's forms) is put back in its look's
  own form, or its transformation, and the player rebuilt. A loaded state
  whose player was built without the stand-in gets one area reload. Levels
  are 50 plus the index in the experience table at ROM 0x6B7084.
- `fastsave.c`: saving at a pad takes 0.4 s instead of 2.3. The save is an
  8 KB EEPROM written 8 bytes at a time, and after each write the game's
  routine (0x0804092C) polls the chip's ready bit while counting scanlines,
  up to 0x88; the emulated chip is ready at once, so the poll loop's head
  (0x080409E8) is hooked to set the routine's own stack counters (sp + 0xA4
  polls answered, sp + 0xAC scanlines) as if the wait were over, and it
  leaves by its own exit. The save written is byte for byte the same.
- `savestates.c`: F5/F8 save states, as in the other games.
- `netplay.c`: online one-on-one battles (2026-10-20). The Start menu has one
  more page, ONLINE, between Options and Journal (R from Options, L from
  Journal). The game's menu knows nothing of it: which page the menu is on is
  read off the picture (the heading's yellow name), on Options the press of R
  is kept from the game and the page is drawn over its menu (which runs on
  underneath, given no buttons, so the music plays), and R again hands the
  press on so the game moves to Journal; the same the other way with L, as
  LoG2's Extras page does. On it: RANDOM BATTLE (the relay pairs you with
  whoever else is waiting; the first to arrive hosts), HOST A BATTLE (the relay
  gives a four-digit room code to tell a friend) and JOIN WITH CODE (type the
  code with the D-pad). The host's menu is shut with B before the arena is
  snapshotted, and both players come back to the menu page after the match.
  In a match A is a punch and nothing else: the three ways the game turns A
  into a talk or a save prompt (the player's ask of the area, 0x08011C70 kind
  3; his check of the triggers under his feet, 0x080127AE; and the world's own
  per-frame scan of those triggers, 0x080126D4, which is where a save pad's
  prompt comes from) are hooked to answer "nothing" while a match is on, since
  any box they open freezes both fighters.
  The relay server's address goes in `~/.local/share/buusfury/buusfury.netplay`
  as `server host:port` (the relay is tools/netplay/netplay_server.py, see
  tools/netplay/README.md, which also says where to run it for nothing:
  `tools/netplay/deploy.sh user@host` installs it on any Linux box with a
  public address, such as an Oracle Cloud Always Free VM, as a service and
  checks it from here, `tools/netplay/gcloud-up.sh` first makes that box on
  Google's free e2-micro, and `tools/netplay/host-here.sh` instead runs it
  on your own PC behind a free bore.pub tunnel for as long as you fight).
  Both machines must run the same build.
  How it works: the host's whole machine state (the same snapshot as a save
  state, packed with zlib, about 65 KB) is sent to the guest when the match
  starts, so both start identical; from then on each side sends the other its
  buttons frame by frame and runs a frame only once it holds both sides'
  buttons for it (lockstep; the runtime's frame gate, `modSetFrameGate`,
  holds the game while a frame's buttons are still on their way; the input
  delay is set from the round trip measured during the handshake, 3 to 10
  frames). The host's fighter is the game's own player object; the guest's is
  a second player object built with the game's player constructor
  (0x080109AA) for the guest's character record (sent over; a mirror match
  plays the copy from the ninth record) and added to the area's object list
  (0x0801191A). The player code asks its object for its character record and
  its button record through two virtual getters (class table +0x80 and +0x84,
  0x0804F840 and 0x0804F8C8); those are hooked to answer with the guest's
  record and a second button record for the second object, so the same code
  drives both fighters independently. A punch is broadcast to every object in
  the area (0x08011C0C) as an attack of "type 1", which the player's own
  handler (class +0x24, 0x08010DDE) ignores so he never hits himself; the
  handler is hooked to hand the other fighter a copy typed as an enemy's (0),
  and the game's own overlap test, damage (from the fighters' stats),
  knockback and sound follow. The world state's "fighting allowed" word
  (0x0300197C + 0xC, 0 in towns, where A only talks) is set for the match.
  Neither fighter can leave the other's screen (the camera follows the host's
  fighter on both machines): whoever steps too far is put back. Every 20
  frames both sides compare a hash of the fighters' positions, health and
  energy; a mismatch ends the match as DESYNC. The match ends when a fighter's
  health reaches zero (YOU WIN / YOU LOSE), or on a lost connection, and each
  machine then goes back to the snapshot it took before the match: nothing of
  it stays in anyone's game. In a match only the D-pad, A (attacks and
  combos) and R (charging) reach the fighters: B uses the character's chosen
  skill (in this save Goku's is Instant Transmission, a teleport; others are
  transformations, which rebuild the player), and L, Start and Select open
  things, so they stay out for now. Looks with stand-in sprites are shown as
  their base outfit during a match. Enemies in the area would only go for the
  host's fighter (they target the game's player), so fight where there are
  none (towns are fine). Tests: `NETPLAY_LOCALTEST=<character>` plays a
  match on one machine with the second fighter driven by `NETPLAY_P2INPUT`
  (the runtime's --input letters); `NETPLAY=host|join|random`, `NETPLAY_CODE`,
  `NETPLAY_SERVER` start a networked match without the menu; `NETPLAY_DEBUG=1`
  writes the fighters' state every 20 frames to the diary. A headless run of
  two instances through the menu needs `RC_HEADLESS_FPS=60` (the runtime paces
  the loop; flat out, an instance burns its frame budget while it waits) and
  `RC_INPUT_BY_LOOP=1`.
