// Test aid for looking at Buu's Fury's attacks (not part of the normal build:
// copy into ../mods/ and rebuild to use it, as tools/sprites/capture_fusion_moves.sh does).
//
//   AS=n         at frame 5, character record n is copied over Goku's (record 0),
//                with plenty of health and energy, so the Snake Way save plays
//                with that character's attacks. 5 Gotenks, 6 and 7 the fusions.
//   ATT=a,b,c,d  (hex) gives Goku those four attacks instead.
//
// Character records are 0x68 bytes from 0x03001568: +4 number of attacks, +5 the
// attacks, +12 form, +14 health, +16 most health, +18 energy, +20 most energy.
#include "mod.h"
#include <stdio.h>
#include <stdlib.h>

static void frame(void) {
	if (modFrame() != 5) {
		return;
	}
	if (getenv("AS")) {
		int n = atoi(getenv("AS")), k;
		for (k = 0; k < 0x68; ++k) {
			modWrite8(0x03001568 + k, modRead8(0x03001568 + n * 0x68 + k));
		}
		modWrite16(0x03001568 + 0x0E, 900);
		modWrite16(0x03001568 + 0x10, 900);
		modWrite16(0x03001568 + 0x12, 400);
		modWrite16(0x03001568 + 0x14, 400);
	}
	if (getenv("ATT")) {
		unsigned a, b, c, d;
		sscanf(getenv("ATT"), "%x,%x,%x,%x", &a, &b, &c, &d);
		modWrite8(0x0300156C, 4);
		modWrite8(0x0300156D, a);
		modWrite8(0x0300156E, b);
		modWrite8(0x0300156F, c);
		modWrite8(0x03001570, d);
	}
}

MOD_INIT(play_as) { modOnFrame(frame); }
