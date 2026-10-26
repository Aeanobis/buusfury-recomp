// What the mods know about Buu's Fury: the same engine as Legacy of Goku II,
// with its own addresses and layouts. Numbers are game addresses unless they
// say ROM offset. See also games/lgoku2/mods/lg2.h for the engine's ways.
#ifndef BF_H
#define BF_H
#include "mod.h"

// ---- the characters ----------------------------------------------------------------
// The save data starts with the current character's number (a byte); the
// character records are 0x4F8 further on, 0x68 bytes each, nine of them
// (the game's own menu code reaches record k at save + 0x4F8 + 0x68 k):
enum { BF_GOKU, BF_GOHAN, BF_GOTEN, BF_TRUNKS, BF_VEGETA, BF_GOTENKS, BF_VEGITO, BF_GOGETA, BF_NINTH, BF_CHARACTERS };
#define BF_RAM_SAVE 0x03001068      // (the game reaches it through its table of addresses at ROM 0x54FBC, entry +0x14)
#define BF_RAM_CHARS 0x03001560
#define BF_IN_PARTY 1               // flags bit 0: the Status page lists him and he can be switched to
struct BfChar {
	uint32_t flags;             // bit 0: in the party (the Status page lists the first eight records with it set)
	uint32_t unknown4;
	uint8_t unknown8[4];        // ff ff ff ff
	uint8_t attacks;            // how many attack slots are filled
	uint8_t attack[7];          // the attacks, by number
	uint8_t form;               // index into BF_ROM_FORMS (what he looks like)
	uint8_t unknown21;
	uint16_t hp, maxHp;
	uint16_t ep, maxEp;
	uint8_t unknown30[6];
	uint32_t xp;
	uint8_t str, pow, end, spd; // the stats as shown (the level-up code adds to these)
	uint8_t unknown44[20];
	uint32_t unknown64[3];      // 44, 77, 112 in every record
	uint16_t base[5];           // the stats again (str, pow, end, end?, spd): before gear?
	uint8_t gear[14];
	uint32_t unknown100;        // (the record is 0x68 bytes)
};
static inline struct BfChar* bfChar(int who) {
	return (struct BfChar*) modRam(BF_RAM_CHARS) + who;
}
static inline int bfCurrentChar(void) {
	return *(uint8_t*) modRam(BF_RAM_SAVE);
}

// ---- the player object -------------------------------------------------------------
#define BF_RAM_PLAYER_PTR 0x03001C34  // the player's object (0 when there is none)
#define BF_ACTOR_SPRITE_DEF 0x58      // game address of the sprite definition it is drawn with
#define BF_ACTOR_WORLD_X 0x1A0        // 24.8 fixed point
#define BF_ACTOR_WORLD_Y 0x1A4

// ---- forms and sprites -------------------------------------------------------------
// A form is 24 bytes: +0 flags, +2 status portrait, +4 dialogue portrait,
// +8 pointer to the form's behaviour record in RAM, +12 which character,
// +14 sprite definition number (16-bit), +16 six bytes, +22 and +23 the forms
// to transform to and back (0xff: none)
// Levels run from 50 to 200: level 50 + n needs the experience in the n-th
// entry of the table (151 entries; 0, 10, 40, 100, 200, 350, ...)
#define BF_ROM_XP_TABLE 0x086B7084
static inline int bfLevel(const struct BfChar* c) {
	int n = 0;
	while (n < 150 && modRead32(BF_ROM_XP_TABLE + 4 * (n + 1)) <= c->xp) {
		++n;
	}
	return 50 + n;
}

#define BF_ROM_FORMS 0x086B82EC
#define BF_ROM_SPRITE_DEFS 0x086B6BB0 // 308 pointers to sprite definitions (tools/sprites numbers them from here;
#define BF_ROM_SPRITE_TABLE 0x086B6BB4 // the game's own numbers start one entry later: definition n is at +4n from here)
#define BF_FN_GET_SPRITE_DEF 0x0801713C // (number) -> the definition's address

// ---- areas -------------------------------------------------------------------------
#define BF_RAM_ZONE 0x0300197C        // byte 0: zone, byte 1: area (the world state record the area loader writes)
#define BF_FN_LOAD_AREA 0x08008D44    // (world state, zone, area, &int16 x,y; on the stack: flag, 1): unloads the area, loads the new one, rebuilds the player
#define BF_FN_TAKE_EXIT 0x080075AC    // (exit object, player): acts if a direction the exit's mask allows is held
#define BF_ROM_EXIT_CLASS 0x0804A09C  // the exit object's class (+0x10 direction, +0x11 held mask, +0x14 record)
#define BF_ROM_SCRATCH 0x7F0000       // ROM offset: free space (the game's data ends at 0x7B89A5)

#define BF_RAM_MENU_OPEN 0x03000F98   // 1 while the Start menu (Journal/Status/Inventory/Equip/Options) is up
enum { BF_BTN_A = 1, BF_BTN_B = 2, BF_BTN_SELECT = 4, BF_BTN_START = 8, BF_BTN_RIGHT = 16, BF_BTN_LEFT = 32, BF_BTN_UP = 64, BF_BTN_DOWN = 128, BF_BTN_R = 256, BF_BTN_L = 512 };

void bfGoTo(int zone, int area, int x, int y); // switch.c: load that area with the player there (the game's exit routine)
int bfNetplayActive(void);  // netplay.c: 1 from the online lobby to the end of an online match (other mods keep their hands off the game then)

#endif
