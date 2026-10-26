// Loading the game. This program holds none of it: the player supplies their
// own dump of the cartridge, and only the dump the code was recompiled from
// will do (the compiled blocks mirror its code byte for byte), so the file is
// checked by SHA-1 before anything runs. The project's own patches
// (games/<game>/patches/*.ips, carried inside the executable by the Makefile)
// are then applied and the image padded, the same steps tools/ips.py takes
// when the build prepares $(BUILD)/rom.gba for the recompiler.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "romload.h"

struct RcPatch { const uint8_t* start; const uint8_t* end; };
extern const struct RcPatch rcPatchTable[]; // ends with { 0, 0 }

// ---- SHA-1 (FIPS 180-4) ----------------------------------------------------------------
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1Block(uint32_t h[5], const uint8_t* p) {
	uint32_t w[80], a, b, c, d, e, f, k, t;
	int i;
	for (i = 0; i < 16; ++i) {
		w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (; i < 80; ++i) {
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; ++i) {
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else { f = b ^ c ^ d; k = 0xCA62C1D6; }
		t = rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void sha1(const uint8_t* data, size_t len, char out[41]) {
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	uint64_t bits = (uint64_t) len * 8;
	uint8_t block[64];
	size_t done = 0;
	int i, last = 0;
	while (!last) {
		size_t n = len - done < 64 ? len - done : 64;
		memcpy(block, data + done, n);
		done += 64;
		if (n < 64) {
			block[n++] = 0x80;
			if (n > 56) {
				memset(block + n, 0, 64 - n);
				sha1Block(h, block);
				n = 0;
			}
			memset(block + n, 0, 56 - n);
			for (i = 0; i < 8; ++i) {
				block[56 + i] = (uint8_t) (bits >> (56 - 8 * i));
			}
			last = 1;
		}
		sha1Block(h, block);
	}
	for (i = 0; i < 5; ++i) {
		snprintf(out + 8 * i, 9, "%08x", h[i]);
	}
}

// ---- IPS, as tools/ips.py applies it ------------------------------------------------
static int growTo(uint8_t** rom, size_t* size, size_t* cap, size_t need) {
	if (need > *cap) {
		size_t c = *cap ? *cap : 1;
		while (c < need) {
			c *= 2;
		}
		uint8_t* n = realloc(*rom, c);
		if (!n) {
			return 0;
		}
		*rom = n;
		*cap = c;
	}
	if (need > *size) {
		memset(*rom + *size, 0, need - *size);
		*size = need;
	}
	return 1;
}
static int applyIps(uint8_t** rom, size_t* size, size_t* cap, const uint8_t* p, const uint8_t* end) {
	if (end - p < 8 || memcmp(p, "PATCH", 5)) {
		return 0;
	}
	p += 5;
	while (p + 3 <= end) {
		if (!memcmp(p, "EOF", 3) && end - p <= 6) { // the end (an offset that reads "EOF" is still a record)
			return 1;
		}
		if (end - p < 5) {
			return 0;
		}
		size_t off = (size_t) p[0] << 16 | (size_t) p[1] << 8 | p[2];
		size_t n = (size_t) p[3] << 8 | p[4];
		p += 5;
		if (n) {
			if ((size_t) (end - p) < n || !growTo(rom, size, cap, off + n)) {
				return 0;
			}
			memcpy(*rom + off, p, n);
			p += n;
		} else { // run of one byte
			if (end - p < 3) {
				return 0;
			}
			n = (size_t) p[0] << 8 | p[1];
			if (!growTo(rom, size, cap, off + n)) {
				return 0;
			}
			memset(*rom + off, p[2], n);
			p += 3;
		}
	}
	return 1;
}

// ---- finding and checking the file ----------------------------------------------------
static int matchesExpected(const char* hash) {
	const char* list = ROM_SHA1;
	while (*list) {
		while (*list == ' ') {
			++list;
		}
		if (!strncasecmp(list, hash, 40) && (list[40] == 0 || list[40] == ' ')) {
			return 1;
		}
		while (*list && *list != ' ') {
			++list;
		}
	}
	return 0;
}
static void dataDirPath(char* out, size_t n) {
	const char* xdg = getenv("XDG_DATA_HOME");
	const char* home = getenv("HOME");
	if (xdg && *xdg) {
		snprintf(out, n, "%s/" SAVE_ID "/" SAVE_ID ".gba", xdg);
	} else {
		snprintf(out, n, "%s/.local/share/" SAVE_ID "/" SAVE_ID ".gba", home ? home : ".");
	}
}
static void exeDirPath(char* out, size_t n) {
	char exe[1024];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len <= 0) {
		snprintf(out, n, GAME_ID ".gba");
		return;
	}
	exe[len] = 0;
	char* slash = strrchr(exe, '/');
	if (slash) {
		*slash = 0;
	}
	snprintf(out, n, "%s/" GAME_ID ".gba", exe);
}
static void complain(int quiet, const char* text) {
	fputs(text, stderr);
	fputc('\n', stderr);
	if (!quiet) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, text, NULL);
	}
}

uint8_t* rcLoadRom(const char* explicitPath, int quiet, size_t* sizeOut) {
	char dataPath[1200], exePath[1200], text[2400];
	const char* env = getenv("RC_ROM");
	dataDirPath(dataPath, sizeof(dataPath));
	exeDirPath(exePath, sizeof(exePath));
	const char* candidates[3] = { dataPath, exePath, GAME_ID ".gba" };
	int n = 3;
	if (explicitPath || (env && *env)) { // a named file is the only one tried
		candidates[0] = explicitPath ? explicitPath : env;
		n = 1;
	}
	const char* path = NULL;
	FILE* f = NULL;
	int i;
	for (i = 0; i < n && !f; ++i) {
		if ((f = fopen(candidates[i], "rb"))) {
			path = candidates[i];
		}
	}
	if (!f) {
		if (n == 1) {
			snprintf(text, sizeof(text), "Cannot read the ROM %s.", candidates[0]);
		} else {
			snprintf(text, sizeof(text),
			         GAME_TITLE " needs your own copy of the game.\n\n"
			         "This program holds none of the cartridge. Put your dump of it\n"
			         "(the one with SHA-1 %s) at\n"
			         "  %s\n"
			         "or next to the program as %s, or start it with --rom FILE.",
			         ROM_SHA1, dataPath, GAME_ID ".gba");
		}
		complain(quiet, text);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fileSize <= 0 || fileSize > (64 << 20)) {
		fclose(f);
		snprintf(text, sizeof(text), "%s is not a GBA ROM (size %ld).", path, fileSize);
		complain(quiet, text);
		return NULL;
	}
	size_t size = (size_t) fileSize, cap = size;
	if (cap < (size_t) ROM_MB << 20) {
		cap = (size_t) ROM_MB << 20;
	}
	uint8_t* rom = malloc(cap);
	if (!rom || fread(rom, 1, size, f) != size) {
		fclose(f);
		snprintf(text, sizeof(text), "Cannot read %s.", path);
		complain(quiet, text);
		return NULL;
	}
	fclose(f);
	char hash[41];
	sha1(rom, size, hash);
	if (!matchesExpected(hash)) {
		snprintf(text, sizeof(text),
		         "%s is not the dump this build of " GAME_TITLE " was made from.\n"
		         "  its SHA-1: %s\n"
		         "  needed:    %s\n"
		         "The compiled code mirrors that dump exactly, so no other will run.", path, hash, ROM_SHA1);
		complain(quiet, text);
		free(rom);
		return NULL;
	}
	for (i = 0; rcPatchTable[i].start; ++i) {
		if (!applyIps(&rom, &size, &cap, rcPatchTable[i].start, rcPatchTable[i].end)) {
			snprintf(text, sizeof(text), "The build's patch %d is malformed.", i + 1);
			complain(quiet, text);
			free(rom);
			return NULL;
		}
	}
	if (size < (size_t) ROM_MB << 20) { // room for the mods' added data, as the build left it
		memset(rom + size, 0xFF, ((size_t) ROM_MB << 20) - size);
		size = (size_t) ROM_MB << 20;
	}
	fprintf(stderr, "ROM: %s\n", path);
	*sizeOut = size;
	return rom;
}
