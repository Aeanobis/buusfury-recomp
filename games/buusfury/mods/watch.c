// The diary: what the player is doing, for working out what went wrong later
// (the runtime keeps the last lines next to the save).
#include "bf.h"
#include <stdio.h>

static void frame(void) {
	static int lastWho = -1, lastForm = -1;
	int who = bfCurrentChar();
	uint32_t player = modRead32(BF_RAM_PLAYER_PTR);
	if (who < 0 || who >= BF_CHARACTERS) {
		return;
	}
	struct BfChar* c = bfChar(who);
	if (who != lastWho || c->form != lastForm || modFrame() % 600 == 0) {
		modLog("watch: character %d form %d hp %d/%d ep %d/%d xp %u stats %d/%d/%d/%d at (%d,%d) def %08X", who, c->form, c->hp, c->maxHp, c->ep, c->maxEp, c->xp,
		       c->str, c->pow, c->end, c->spd, (player >> 24) == 2 ? (int) modRead32(player + BF_ACTOR_WORLD_X) >> 8 : -1,
		       (player >> 24) == 2 ? (int) modRead32(player + BF_ACTOR_WORLD_Y) >> 8 : -1, (player >> 24) == 2 ? modRead32(player + BF_ACTOR_SPRITE_DEF) : 0);
		lastWho = who;
		lastForm = c->form;
	}
	if (modFrame() == 10 && modCanCall()) {
		int n;
		for (n = 0; n < BF_CHARACTERS; ++n) {
			fprintf(stderr, "record %d: flags %08X form %d hp %d/%d xp %u\n", n, bfChar(n)->flags, bfChar(n)->form, bfChar(n)->hp, bfChar(n)->maxHp, bfChar(n)->xp);
		}
	}
}

MOD_INIT(watch) { modOnFrame(frame); }
