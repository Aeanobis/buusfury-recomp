// Development aid: unpack compressed graphics by having the game do it.
// Works for all three games (they share the routine). Build a game with this
// as its only mod, then run it:
//   make GAME=lgoku2 MODS=tools/mods/extract.c
//   EXTRACT_FN=0801F845 EXTRACT_LIST=tools/sprites/graphics-lists/lgoku2.txt EXTRACT_OUT=build/sprites/lg2 ./lgoku2 --headless 200
// (Buu's Fury: EXTRACT_FN=0803D941, graphics-lists/buusfury.txt, build/sprites/bf; then plain make again.)
// list.txt has one "ROMADDRESS SIZE" (hex address of a graphics block, decimal
// unpacked size) per line; each is written to dir/<address>.bin. The run is
// throwaway: it scribbles on the game's memory.
#include "mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH 0x02030000

static void frame(void) {
	const char* list = getenv("EXTRACT_LIST");
	const char* out = getenv("EXTRACT_OUT");
	const char* fn = getenv("EXTRACT_FN");
	if (!list || !out || !fn || modFrame() != 150) {
		return;
	}
	if (!modCanCall()) {
		fprintf(stderr, "extract: cannot call into the game on this frame\n");
		exit(1);
	}
	FILE* f = fopen(list, "r");
	uint32_t addr, decompress = strtoul(fn, NULL, 16);
	unsigned size, done = 0;
	while (f && fscanf(f, "%x %u", &addr, &size) == 2) {
		if (size > 0x8000) {
			continue;
		}
		memset(modRam(SCRATCH), 0, size);
		modCall(decompress, addr, SCRATCH, 0, 0);
		char path[600];
		snprintf(path, sizeof(path), "%s/%08X.bin", out, addr);
		FILE* o = fopen(path, "wb");
		if (o) {
			fwrite(modRam(SCRATCH), 1, size, o);
			fclose(o);
			++done;
		}
	}
	fprintf(stderr, "extract: %u blocks unpacked\n", done);
	// the object palette as it stands, for a first look at the results
	char path[600];
	snprintf(path, sizeof(path), "%s/objpalette.bin", out);
	FILE* o = fopen(path, "wb");
	if (o) {
		for (int i = 0; i < 256; ++i) {
			uint16_t c = modRead16(0x05000200 + i * 2);
			fwrite(&c, 1, 2, o);
		}
		fclose(o);
	}
	exit(0);
}

MOD_INIT(extract) {
	modOnFrame(frame);
}
