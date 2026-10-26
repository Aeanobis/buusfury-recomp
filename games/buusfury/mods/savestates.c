// Save states. F5 or 9 saves, F8 or 0 loads; hold Shift for a second slot.
// On a controller: Select+R saves, Select+L loads.
// Saving at an in-game save point also saves state 1.
// To use it, copy this file into ../mods/ and run make.
#include "bf.h"

#include <stdio.h>

enum { KEY_9 = 38, KEY_0 = 39, KEY_F5 = 62, KEY_F8 = 65, KEY_LSHIFT = 225, KEY_RSHIFT = 229 }; // SDL scancodes
enum { BTN_SELECT = 4, BTN_R = 256, BTN_L = 512 };

static uint32_t lastKeys;

static unsigned messageUntil;
static const char* message;

static void frame(void) {
	if (bfNetplayActive()) {
		return; // no saving or loading in the middle of an online match
	}
	int slot = modKeyDown(KEY_LSHIFT) || modKeyDown(KEY_RSHIFT) ? 2 : 1;
	uint32_t keys = modKeys();
	uint32_t pressed = keys & ~lastKeys;
	lastKeys = keys;
	int save = modKeyPressed(KEY_F5) || modKeyPressed(KEY_9) || ((keys & BTN_SELECT) && (pressed & BTN_R));
	if (modGameSaved()) {
		save = 1;
		slot = 1;
	}
	int load = modKeyPressed(KEY_F8) || modKeyPressed(KEY_0) || ((keys & BTN_SELECT) && (pressed & BTN_L));
	if (save) {
		int ok = modSaveState(slot);
		message = ok ? (slot == 1 ? "STATE 1 SAVED" : "STATE 2 SAVED") : "SAVE FAILED";
		messageUntil = modFrame() + 120;
		fprintf(stderr, "savestates: save slot %d %s (frame %u)\n", slot, ok ? "ok" : "FAILED", modFrame());
	}
	if (load) {
		int ok = modLoadState(slot);
		message = ok ? "STATE LOADED" : "NO STATE IN THIS SLOT";
		messageUntil = modFrame() + 120;
		fprintf(stderr, "savestates: load slot %d %s\n", slot, ok ? "ok" : "failed");
	}
	if (modFrame() < messageUntil) {
		modTextScale(2);
		modText(8, 30, 0x40FF40, "%s", message);
		modTextScale(1);
	}
}

MOD_INIT(savestates) {
	modOnFrame(frame);
}
