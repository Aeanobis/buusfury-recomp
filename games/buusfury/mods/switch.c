// Switching characters, to any of the five the story plays, story or no
// story: their records are kept flagged as in the party, so the game's own
// Switch Characters at the save pads lists all five and switches to them
// itself; the fusions are kept out of it. The looks (outfits) live on that
// screen too, with Up and Down, below. Looks are remembered in the settings.
#include "bf.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Young Gohan from Legacy of Goku II (tools/sprites/convert_lg2.py): his four
// outfits and his Super Saiyan sprite, in the grown part of the ROM; Future
// Trunks (Legacy of Goku II's Trunks: his jacket and his Saiyan armour, each
// with his Super Saiyan) and Vegeta's Saiyan armour (with his), the same way.
// They are converted from that game's ROM (not in the repository); without
// them the game builds and plays the same, minus those outfits.
#if __has_include("generated/youngGohanGi_data.inc")
#define LG2_OUTFITS 1
#include "generated/youngGohanGi_data.inc"
#include "generated/youngGohanNamek_data.inc"
#include "generated/youngGohanArmour_data.inc"
#include "generated/youngGohanArmourB_data.inc"
#include "generated/youngGohanSsj_data.inc"
#include "generated/futureTrunks_data.inc"
#include "generated/futureTrunksSsj_data.inc"
#include "generated/futureTrunksArmour_data.inc"
#include "generated/futureTrunksArmourSsj_data.inc"
#include "generated/vegetaArmour_data.inc"
#include "generated/vegetaArmourSsj_data.inc"
#else
#define LG2_OUTFITS 0
#endif

// The five the story plays (the fusions are not characters to walk about as:
// they are kept out of the party and so off the pads' list)
enum { STORY_CHARACTERS = 5 };
static const struct { const char* name; uint8_t who; } entries[STORY_CHARACTERS] = {
	{ "GOKU", BF_GOKU }, { "GOHAN", BF_GOHAN }, { "GOTEN", BF_GOTEN }, { "TRUNKS", BF_TRUNKS }, { "VEGETA", BF_VEGETA },
};

// The looks, by character: the outfits (untransformed forms) and what to call
// them. An outfit whose form the table gives no transformation is marked in
// the list, since the game cannot go Super Saiyan from it.
//
// Young Gohan's looks are Legacy of Goku II's sprites standing in for one of
// adult Gohan's outfits: the record is put in that outfit's form and the
// sprite table entries of the outfit (and of its Super Saiyan form, when it
// has one) point at the young sprites instead. So the game's own
// transformation still works, and shows young Gohan's Super Saiyan.
struct Look { uint8_t form; const char* name; uint32_t drawAs, ssjDrawAs; }; // game addresses of the stand-in sprites, or 0
#define YOUNG(name) (0x08000000 + name##RomOffset)
static const struct Look gokuLooks[] = { { 2, "GOKU" }, { 1, "CASUAL" } };
// (named by what the sprites show: 15 is the Saiyaman tunic and gloves without
// the helmet and cape, 11 the Supreme Kai's blue clothes, 12 those with the Z Sword)
static const struct Look gohanLooks[] = { { 10, "SCHOOL" }, { 15, "SAIYAMAN, NO HELMET" }, { 13, "GI" }, { 9, "BLACK GI" }, { 11, "KAI CLOTHES" },
                                          { 14, "GREAT SAIYAMAN" }, // (12, the Kai clothes with the Z Sword, left out at the user's request)
#if LG2_OUTFITS
                                          // (all four on the school outfit, the one with a transformation: each goes
                                          // Super Saiyan as the young gi, the one Super Saiyan sprite that game has)
                                          { 10, "YOUNG GI", 0x08000000 + 0x800000, 0x08000000 + 0x900000 },
                                          { 10, "YOUNG NAMEK", 0x08000000 + 0x840000, 0x08000000 + 0x900000 },
                                          { 10, "YOUNG ARMOUR", 0x08000000 + 0x880000, 0x08000000 + 0x900000 },
                                          { 10, "YOUNG ARMOUR B", 0x08000000 + 0x8C0000, 0x08000000 + 0x900000 },
#endif
                                        };
static const struct Look gotenLooks[] = { { 19, "GOTEN" } };
// (Legacy of Goku II's def 129 is the jacket and 128 the armour, each with its own Super Saiyan)
static const struct Look trunksLooks[] = { { 21, "TRUNKS" }, { 23, "CASUAL" },
#if LG2_OUTFITS
                                           { 21, "FUTURE TRUNKS", 0x08000000 + 0x9C0000, 0x08000000 + 0xA00000 },
                                           { 21, "FUTURE ARMOUR", 0x08000000 + 0x940000, 0x08000000 + 0x980000 },
#endif
                                         };
static const struct Look vegetaLooks[] = { { 26, "BUU'S FURY" }, { 27, "HALO" },
#if LG2_OUTFITS
                                           { 26, "SAIYAN ARMOUR", 0x08000000 + 0xA40000, 0x08000000 + 0xA80000 },
#endif
                                         };
static const struct Look gotenksLooks[] = { { 31, "GOTENKS" } };
static const struct Look vegitoLooks[] = { { 39, "VEGITO" } };
static const struct Look gogetaLooks[] = { { 42, "GOGETA" } };
#define N(a) (int) (sizeof(a) / sizeof(*(a)))
static const struct { const struct Look* looks; int count; } looksOf[BF_CHARACTERS] = {
	{ gokuLooks, N(gokuLooks) }, { gohanLooks, N(gohanLooks) }, { gotenLooks, N(gotenLooks) }, { trunksLooks, N(trunksLooks) },
	{ vegetaLooks, N(vegetaLooks) }, { gotenksLooks, N(gotenksLooks) }, { vegitoLooks, N(vegitoLooks) }, { gogetaLooks, N(gogetaLooks) }, { 0, 0 },
};
static int look[BF_CHARACTERS]; // which of his looks each character has chosen
static struct { uint16_t def, ssjDef; uint32_t own, ssjOwn; } swapped[BF_CHARACTERS]; // the sprite table entries a look replaced

static uint32_t* spriteTable(void) {
	return (uint32_t*) modRom(BF_ROM_SPRITE_TABLE - 0x08000000);
}

static uint16_t formDef(int form) {
	return modRead16(BF_ROM_FORMS + form * 24 + 14);
}

// Put the sprite table back for a character, then swap for his look if it stands in for the outfit
static void applySwap(int who) {
	uint32_t* table = spriteTable();
	const struct Look* l;
	if (swapped[who].def) {
		table[swapped[who].def] = swapped[who].own;
	}
	if (swapped[who].ssjDef) {
		table[swapped[who].ssjDef] = swapped[who].ssjOwn;
	}
	swapped[who].def = swapped[who].ssjDef = 0;
	if (!looksOf[who].count) {
		return;
	}
	l = &looksOf[who].looks[look[who]];
	if (l->drawAs) {
		swapped[who].def = formDef(l->form);
		swapped[who].own = table[swapped[who].def];
		table[swapped[who].def] = l->drawAs;
	}
	if (l->ssjDrawAs && modRead8(BF_ROM_FORMS + l->form * 24 + 22) != 0xFF) {
		swapped[who].ssjDef = formDef(modRead8(BF_ROM_FORMS + l->form * 24 + 22));
		swapped[who].ssjOwn = table[swapped[who].ssjDef];
		table[swapped[who].ssjDef] = l->ssjDrawAs;
	}
}
static int recheck; // a state was loaded: the player may be drawn with the sprite his look replaced
static int rebuild; // the player's record changed form under him: rebuilt whatever he is drawn with
static uint32_t lastKeys;

static const char* settingName(int who) {
	static char name[24];
	snprintf(name, sizeof(name), "bf_look_%d", who);
	return name;
}

// The look the record is in now, if it is one of the listed ones
static int lookOfForm(int who, int form) {
	int i;
	for (i = 0; i < looksOf[who].count; ++i) {
		if (looksOf[who].looks[i].form == form && !looksOf[who].looks[i].drawAs) {
			return i;
		}
	}
	return -1;
}

static void loadLooks(void) {
	int who;
	for (who = 0; who < BF_CHARACTERS; ++who) {
		int fromRecord = lookOfForm(who, bfChar(who)->form);
		look[who] = modGetSetting(settingName(who), fromRecord >= 0 ? fromRecord : 0);
		if (look[who] < 0 || look[who] >= looksOf[who].count) {
			look[who] = 0;
		}
		applySwap(who);
	}
}

// The exit routine wants an exit object and its record (laid out as the
// game's), and a direction held
void bfGoTo(int zone, int area, int x, int y) {
	uint32_t player = modRead32(BF_RAM_PLAYER_PTR);
	uint8_t* exit = modRom(BF_ROM_SCRATCH);
	uint8_t* record = exit + 32;
	if ((player >> 24) != 2 || !modCanCall()) {
		return;
	}
	memset(exit, 0, 64);
	*(uint32_t*) exit = BF_ROM_EXIT_CLASS;
	exit[0x11] = 0xFF;
	*(uint32_t*) (exit + 0x14) = 0x08000000 + BF_ROM_SCRATCH + 32;
	record[0x11] = (uint8_t) zone;
	record[0x12] = (uint8_t) area;
	record[0x13] = 1;
	*(int16_t*) (record + 0x14) = (int16_t) x;
	*(int16_t*) (record + 0x16) = (int16_t) y;
	{
		uint32_t cls = modRead32(player), method = cls + modRead32(cls + 0x84);
		uint32_t input = modCall(method | 1, player, 0, 0, 0);
		uint16_t was = modRead16(input + 2);
		modWrite16(input + 2, 0xF0); // every direction held (not every button: 0xFF would be Start too, and the menu would open after the move)
		modCall(BF_FN_TAKE_EXIT | 1, 0x08000000 + BF_ROM_SCRATCH, player, 0, 0);
		modWrite16(input + 2, was);
	}
}

// Everyone in the party: the game's own Status page then lists all eight
// (its Left/Right go through them) and can switch to them itself
static void everyoneInTheParty(void) {
	int who, current = bfCurrentChar();
	for (who = 0; who < STORY_CHARACTERS; ++who) {
		bfChar(who)->flags |= BF_IN_PARTY | 2; // (Goku's record holds 3; what bit 1 means is not known)
	}
	for (who = STORY_CHARACTERS; who < BF_CHARACTERS; ++who) {
		if (who != current) {
			bfChar(who)->flags &= ~(uint32_t) BF_IN_PARTY; // the fusions, and the spare ninth record (an online match's guest): not on the pads' list
		}
	}
}

// A record's form must be one of its own character's (the form table says
// whose each is). Gohan was once found in Goku's forms, drawn as Goku and
// transforming as Goku; a wrong form is put back to the look's own, a
// transformed one to that look's transformation, and the player rebuilt.
static int formOwner(int form) {
	return modRead8(BF_ROM_FORMS + form * 24 + 12);
}

static void keepOwnForms(void) {
	int who;
	for (who = 0; who < STORY_CHARACTERS; ++who) {
		struct BfChar* c = bfChar(who);
		int form = c->form, transformed = modRead8(BF_ROM_FORMS + form * 24) & 0x40;
		int base, next;
		if (form < 48 && formOwner(form) == who) {
			continue;
		}
		if (form >= 48) {
			transformed = 0; // 255: a transformation from an outfit without one; back to the look's outfit
		}
		base = looksOf[who].count ? looksOf[who].looks[look[who]].form : form;
		next = modRead8(BF_ROM_FORMS + base * 24 + 22);
		c->form = (uint8_t) (transformed && next != 0xFF ? next : base);
		modLog("switch: %s's record was in form %d, not his: now %d", entries[who].name, form, c->form);
		if (who == bfCurrentChar()) {
			recheck = 1; // (the player is drawn with the wrong sprite: rebuilt below)
			rebuild = 1;
		}
	}
}

// ---- looks on the game's own Switch Character screen (the save pads) ------------
// The screen keeps a figure object of class 0x0804AC20 per party member and,
// in its own object, a table of {index, figure} pairs followed (0x48 bytes
// after the table's first word) by the cursor's index and the figure chosen.
// Up and Down over a figure change that character's look, as in Legacy of
// Goku II: the record is put in the look's form and the figure pointed at
// its sprite, so what is chosen is what the game then builds.
#define FIGURE_CLASS 0x0804AC20
#define PAD_FIGURES 8

static int padFigures(uint32_t* figures, int most) {
	const uint32_t* ram = modRam(0x02000000);
	uint32_t i;
	int n = 0;
	for (i = 0; i + 0x240 / 4 < 0x40000 / 4 && n < most; ++i) {
		if (ram[i] == FIGURE_CLASS && (ram[i + 0x58 / 4] >> 24) == 8) {
			figures[n++] = 0x02000000 + i * 4;
		}
	}
	return n;
}

// The character a figure shows: the one whose form's sprite (the table entry, stand-in or not) it is drawn with
static int figureCharacter(uint32_t figure) {
	uint32_t def = modRead32(figure + BF_ACTOR_SPRITE_DEF);
	int who;
	for (who = 0; who < STORY_CHARACTERS; ++who) {
		if (spriteTable()[formDef(bfChar(who)->form)] == def) {
			return who;
		}
	}
	return -1;
}

// The screen's pair table: {0, a figure} ... ; returns the address of its first word, or 0
static uint32_t padTable(const uint32_t* figures, int n) {
	const uint32_t* ram = modRam(0x02000000);
	uint32_t i;
	int k;
	if (n < 2) {
		return 0;
	}
	for (i = 0; i + 4 < 0x40000 / 4; ++i) {
		if (ram[i] != 0 || ram[i + 2] != 1) {
			continue;
		}
		for (k = 0; k < n && ram[i + 1] != figures[k]; ++k) {
		}
		if (k == n) {
			continue;
		}
		for (k = 0; k < n && ram[i + 3] != figures[k]; ++k) {
		}
		if (k < n) {
			return 0x02000000 + i * 4;
		}
	}
	return 0;
}

// A figure keeps, besides its sprite definition (+0x58), the animation it is
// in (+0x50: a pointer into the definition's animation table) and the frame
// it shows (+0x30 and +0x54: a frame record of that animation); the game only
// re-reads the definition when the animation wraps, so a new definition is
// given the same animation and frame of its own, else the old sprite stays
static void repointFigure(uint32_t figure, uint32_t from, uint32_t to) {
	uint32_t anim = modRead32(figure + 0x50), frame = modRead32(figure + 0x54), anim2, frame2;
	int count = modRead8(from), dirs = modRead8(from + 3), count2 = modRead8(to), dirs2 = modRead8(to + 3);
	int a, i, n, n2, which = -1, index = -1;
	for (a = 0; a < count && a < 40; ++a) {
		if (modRead32(from + 0x54 + 4 * a) == anim) {
			which = a;
		}
	}
	modWrite32(figure + BF_ACTOR_SPRITE_DEF, to);
	if (count2 == 0 || dirs2 == 0) {
		return;
	}
	if (which < 0 || which >= count2) {
		which = which >= 0 && count2 > 1 ? 1 : 0; // (the figures walk on the spot: animation 1 where there is one)
	}
	n = (int) modRead32(anim);
	for (i = 0; dirs && i < n * dirs && i < 1024; ++i) {
		if (modRead32(anim + 4 + 4 * i) == frame) {
			index = i;
		}
	}
	anim2 = modRead32(to + 0x54 + 4 * which);
	n2 = (int) modRead32(anim2);
	if (index < 0 || dirs2 != dirs) {
		index = 0; // (kid Trunks's sprite has two directions, the grown ones four: the first frame, facing down)
	} else if (index >= n2 * dirs2) {
		index %= dirs2; // the same way faced, the first frame
	}
	frame2 = modRead32(anim2 + 4 + 4 * index);
	modWrite32(figure + 0x50, anim2);
	modWrite32(figure + 0x54, frame2);
	modWrite32(figure + 0x30, frame2);
}

static unsigned padNoteUntil;
static int padNoteWho;

static void padLooks(uint32_t pressed) {
	uint32_t figures[PAD_FIGURES], table, figure;
	int n = padFigures(figures, PAD_FIGURES), cursor, who;
	if (n < 2) {
		return;
	}
	table = padTable(figures, n);
	if (!table) {
		return;
	}
	cursor = (int) modRead32(table + 0x48);
	if (cursor < 0 || cursor >= n) {
		return;
	}
	figure = modRead32(table + 4 + cursor * 8);
	who = figureCharacter(figure);
	if (who < 0 || !looksOf[who].count) {
		return;
	}
	{
		const struct Look* l = &looksOf[who].looks[look[who]];
		modText(48, 30, 0xC0C0C0, "UP / DOWN: LOOK  %s: %s", entries[who].name, l->name);
	}
	if (pressed & (BF_BTN_UP | BF_BTN_DOWN)) {
		int count = looksOf[who].count;
		look[who] = (look[who] + ((pressed & BF_BTN_DOWN) ? 1 : count - 1)) % count;
		modSetSetting(settingName(who), look[who]);
		{
			uint32_t was = modRead32(figure + BF_ACTOR_SPRITE_DEF);
			applySwap(who);
			bfChar(who)->form = looksOf[who].looks[look[who]].form;
			repointFigure(figure, was, spriteTable()[formDef(bfChar(who)->form)]);
		}
		padNoteUntil = modFrame() + 90;
		padNoteWho = who;
		modLog("switch: on the pad screen, %s's look is now %s (form %d)", entries[who].name, looksOf[who].looks[look[who]].name, bfChar(who)->form);
	}
}

// Gohan's transformation skill: at Orange Star High the story takes his
// Super Saiyan (skill 20) out of his list and puts the Great Saiyaman change
// (21) in its place, which the story then needs. While a young look is on,
// Super Saiyan is added as a skill of its own at the end of his list (the
// outfit transforms into the young Super Saiyan); an adult look takes the
// added one away again. An earlier build put it in the Saiyaman slot
// instead, which stranded the story: such a record is put right.
#define SKILL_SUPER_SAIYAN 20
#define SKILL_SAIYAMAN 21

static int hasSkill(const struct BfChar* c, int skill) {
	int i, n = c->attacks < 7 ? c->attacks : 7;
	for (i = 0; i < n; ++i) {
		if (c->attack[i] == skill) {
			return i;
		}
	}
	return -1;
}

static void youngGohanSkill(void) {
	struct BfChar* c = bfChar(BF_GOHAN);
	int young = looksOf[BF_GOHAN].count && looksOf[BF_GOHAN].looks[look[BF_GOHAN]].drawAs != 0;
	int saiyaman = hasSkill(c, SKILL_SAIYAMAN), ssj = hasSkill(c, SKILL_SUPER_SAIYAN);
	// (A rule that put Saiyaman back over a Super Saiyan found in his list, meant to undo an older build's own
	// swap, is gone: the story takes Super Saiyan away for the Saiyaman stretch and gives it back later, and
	// the rule kept taking it away again.)
	if (saiyaman < 0) {
		return; // before the story's swap he has Super Saiyan himself
	}
	// The added skill is only ever there while his outfit can transform: the
	// game follows the outfit's "next" blindly, and an outfit without one
	// (the gi, Great Saiyaman) took him to form 255 and the game to nowhere
	young = young && c->form < 48 && modRead8(BF_ROM_FORMS + c->form * 24 + 22) != 0xFF;
	if (young && ssj < 0 && c->attacks < 7) {
		c->attack[c->attacks++] = SKILL_SUPER_SAIYAN;
		modSetSetting("bf_gohan_ssj_added", 1);
		modLog("switch: young Gohan: Super Saiyan added to his skills (slot %d)", c->attacks - 1);
	} else if (!young && ssj >= 0 && modGetSetting("bf_gohan_ssj_added", 0)) {
		int i;
		for (i = ssj; i + 1 < c->attacks; ++i) {
			c->attack[i] = c->attack[i + 1];
		}
		c->attack[--c->attacks] = 0;
		if (c->unknown21 >= c->attacks) {
			c->unknown21 = 0; // (the selected skill)
		}
		modSetSetting("bf_gohan_ssj_added", 0);
		modLog("switch: adult Gohan: the added Super Saiyan taken out of his skills");
	}
}

// A move on request from outside (support, tests): <save folder>/warp.request holding
// "zone area x y" takes the player there as soon as the game can be asked, and the file goes.
static void warpOnRequest(void) {
	char path[1100];
	FILE* f;
	int z, a, x, y;
	if (modFrame() % 10 || !modSaveDir()[0]) {
		return;
	}
	snprintf(path, sizeof(path), "%s/warp.request", modSaveDir());
	f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (fscanf(f, "%d %d %d %d", &z, &a, &x, &y) == 4 && modCanCall() && (modRead32(BF_RAM_PLAYER_PTR) >> 24) == 2 && modRead32(BF_RAM_MENU_OPEN) != 1) {
		fclose(f);
		unlink(path);
		modLog("switch: moved on request to zone %d area %d (%d,%d)", z, a, x, y);
		bfGoTo(z, a, x, y);
		return;
	}
	fclose(f);
}

static void frame(void) {
	warpOnRequest();
	uint32_t keys = modKeys(), pressed = keys & ~lastKeys;
	if (bfNetplayActive()) {
		lastKeys = keys;
		return; // an online match: both machines must run exactly the same game
	}
	everyoneInTheParty(); // (the save data comes back from every state and save: kept so each frame)
	keepOwnForms();
	youngGohanSkill();
	int menu = modRead8(BF_RAM_MENU_OPEN) == 1;
	lastKeys = keys;
	padLooks(pressed);
	// A player made before the look was applied (a loaded state, the resume)
	// still points at the sprite the look replaced: reload the area once so
	// the game rebuilds him with the look
	if (recheck && !menu) {
		uint32_t player = modRead32(BF_RAM_PLAYER_PTR);
		int who = bfCurrentChar();
		if ((player >> 24) == 2 && modCanCall() && who < BF_CHARACTERS) {
			uint32_t drawn = modRead32(player + BF_ACTOR_SPRITE_DEF);
			uint32_t wanted = modCall(BF_FN_GET_SPRITE_DEF | 1, BF_ROM_FORMS + bfChar(who)->form * 24, 0, 0, 0);
			recheck = 0;
			if ((drawn != wanted && swapped[who].def) || rebuild) {
				rebuild = 0;
				int zone = modRead8(BF_RAM_ZONE), area = modRead8(BF_RAM_ZONE + 1);
				bfGoTo(zone, area, (int) modRead32(player + BF_ACTOR_WORLD_X) >> 8, (int) modRead32(player + BF_ACTOR_WORLD_Y) >> 8);
				modLog("switch: the player was drawn without his look: area reloaded");
			}
		}
	}
}

static void onStateRead(void) {
	if (bfNetplayActive()) {
		return;
	}
	loadLooks();
	everyoneInTheParty();
	recheck = 1;
}

MOD_INIT(switch) {
#if LG2_OUTFITS
	memcpy(modRom(youngGohanGiRomOffset), youngGohanGiData, youngGohanGiSize);
	memcpy(modRom(youngGohanNamekRomOffset), youngGohanNamekData, youngGohanNamekSize);
	memcpy(modRom(youngGohanArmourRomOffset), youngGohanArmourData, youngGohanArmourSize);
	memcpy(modRom(youngGohanArmourBRomOffset), youngGohanArmourBData, youngGohanArmourBSize);
	memcpy(modRom(youngGohanSsjRomOffset), youngGohanSsjData, youngGohanSsjSize);
	memcpy(modRom(futureTrunksRomOffset), futureTrunksData, futureTrunksSize);
	memcpy(modRom(futureTrunksSsjRomOffset), futureTrunksSsjData, futureTrunksSsjSize);
	memcpy(modRom(futureTrunksArmourRomOffset), futureTrunksArmourData, futureTrunksArmourSize);
	memcpy(modRom(futureTrunksArmourSsjRomOffset), futureTrunksArmourSsjData, futureTrunksArmourSsjSize);
	memcpy(modRom(vegetaArmourRomOffset), vegetaArmourData, vegetaArmourSize);
	memcpy(modRom(vegetaArmourSsjRomOffset), vegetaArmourSsjData, vegetaArmourSsjSize);
#endif
	loadLooks();
	everyoneInTheParty();
	recheck = 1;
	modOnStateRead(onStateRead);
	modOnFrame(frame);
}
