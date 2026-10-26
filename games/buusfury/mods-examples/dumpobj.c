// Development aid: dump the sprites near the middle of the screen, decoded to
// RGBA, straight from the emulated video hardware. Works for any of the games.
// Copy into ../mods/, run make, then:  DUMPOBJ=some/dir ./buusfury --load-state ...
// Each distinct image is written once as <hash>_<w>x<h>.rgba; the log on stderr
// says which sprite showed which image on which frame.
#include "mod.h"
#include <stdio.h>
#include <stdlib.h>

static const uint8_t sizes[4][4][2] = {
	{ { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
	{ { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
	{ { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
	{ { 8, 8 }, { 8, 8 }, { 8, 8 }, { 8, 8 } },
};

static void frame(void) {
	const char* dir = getenv("DUMPOBJ");
	if (!dir) {
		return;
	}
	int i;
	for (i = 0; i < 128; ++i) {
		uint16_t a0 = modRead16(0x07000000 + i * 8), a1 = modRead16(0x07000002 + i * 8), a2 = modRead16(0x07000004 + i * 8);
		if ((a0 & 0x300) == 0x200) {
			continue; // hidden
		}
		int w = sizes[a0 >> 14][a1 >> 14][0], h = sizes[a0 >> 14][a1 >> 14][1];
		int y = a0 & 0xFF, x = a1 & 0x1FF;
		if (y >= 160) y -= 256;
		if (x >= 240) x -= 512;
		if (x + w < 80 || x > 160 || y + h < 70 || y > 150) {
			continue;
		}
		int bpp8 = (a0 >> 13) & 1, tile = a2 & 0x3FF, pal = a2 >> 12;
		int step = bpp8 ? 2 : 1;
		int oneD = modRead16(0x04000000) & 0x40; // DISPCNT: 1D or 2D sprite tile mapping
		static uint8_t px[64 * 64 * 4];
		uint32_t hash = 2166136261u;
		int tx, ty, k;
		for (ty = 0; ty < h; ++ty) {
			for (tx = 0; tx < w; ++tx) {
				int sx = (a1 & 0x1000) ? w - 1 - tx : tx, sy = (a1 & 0x2000) ? h - 1 - ty : ty;
				int t = oneD ? tile + ((sy / 8) * (w / 8) + sx / 8) * step : tile + (sy / 8) * 32 + (sx / 8) * step;
				int c;
				if (bpp8) {
					c = modRead8(0x06010000 + t * 32 + (sy & 7) * 8 + (sx & 7));
				} else {
					c = modRead8(0x06010000 + t * 32 + (sy & 7) * 4 + (sx & 7) / 2);
					c = (sx & 1) ? c >> 4 : c & 15;
				}
				uint16_t rgb = c ? modRead16(0x05000200 + ((bpp8 ? 0 : pal * 16) + c) * 2) : 0;
				uint8_t* o = &px[(ty * w + tx) * 4];
				o[0] = (rgb & 31) << 3;
				o[1] = ((rgb >> 5) & 31) << 3;
				o[2] = ((rgb >> 10) & 31) << 3;
				o[3] = c ? 255 : 0;
				for (k = 0; k < 4; ++k) {
					hash = (hash ^ o[k]) * 16777619u;
				}
			}
		}
		fprintf(stderr, "f%u obj%d x%d y%d %dx%d tile%d %s img%08X\n", modFrame(), i, x, y, w, h, tile, bpp8 ? "8bpp" : "4bpp", hash);
		char path[512];
		snprintf(path, sizeof(path), "%s/%08X_%dx%d.rgba", dir, hash, w, h);
		FILE* f = fopen(path, "rb");
		if (f) {
			fclose(f);
		} else if ((f = fopen(path, "wb"))) {
			fwrite(px, 1, w * h * 4, f);
			fclose(f);
		}
	}
}

MOD_INIT(dumpobj) {
	modOnFrame(frame);
}
