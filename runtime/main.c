// Native frontend for the statically recompiled game.
//
// Game code:   gen/rc_blocks_*.c (produced by tools/recomp.py)
// Hardware:    mGBA core (video, audio, timers, DMA, BIOS calls, EEPROM)
// This file:   window, audio output, input, save file, test/verification modes
#include <SDL2/SDL.h>

#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/overrides.h>
#include <mgba/internal/defines.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba-util/vfs.h>

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>

#define RC_NO_ISA
#include "rc_rt.h"
#include "mod.h"
#include "romload.h"

#define W 240
#define H 160
#define SAMPLE_RATE 48000
#define GBA_FPS (16777216.0 / 280896.0)

static uint8_t* rcRomImage;   // the player's ROM as loaded (runtime/romload.c), before the core copies it
static const char* romPathArg; // --rom

typedef void (*RecompBlock)(struct ARMCore*);
struct RecompRegion {
	uint32_t base;
	uint32_t size;
	RecompBlock* table[2];
	const uint8_t* image;
	const uint16_t* guardLen[2];
};
extern struct RecompRegion recompRegions[3]; // ROM, BIOS, IWRAM
extern int recompEnabled;
extern uint64_t recompBlocksRun, recompInterpreted;
extern void (*recompMiss)(uint32_t pc, int thumb);

extern const uint32_t rcRamBase, rcRamSize;
extern const uint8_t rcRamImage[];

enum { RC_ROM, RC_BIOS, RC_IWRAM, RC_REGIONS };
static const char* const regionName[RC_REGIONS + 1] = { "ROM", "BIOS", "RAM", "other" };

// Per region and CPU mode: which addresses are inside a compiled block, and
// which addresses the interpreter was asked to start executing at.
static uint8_t* covered[RC_REGIONS][2];
static uint8_t* missed[RC_REGIONS][2];
static uint64_t interpCount[RC_REGIONS + 1];
static uint64_t interpUncovered[RC_REGIONS + 1];
static uint16_t* guardLen[2];
static uint8_t ramImage[SIZE_WORKING_IRAM];
static unsigned nBlocks;
static uint8_t* leader[RC_REGIONS][2];

struct mCore* rcCoreForDebug;

// Some ROM dumps were altered to keep their saves in battery RAM instead of
// the EEPROM chip the original cartridge had (games/<game>/game.mk says so with
// SAVE_MEMORY := SRAM). The hardware layer goes by the game's ID and would
// provide an EEPROM, so the game's saves would vanish. Resetting and loading a
// state both put the EEPROM back, so this is reapplied after each.
static void _applySaveMemory(struct mCore* core) {
#ifdef RC_FORCE_SRAM
	struct GBA* gba = core->board;
	if (gba->memory.savedata.type != SAVEDATA_SRAM) {
		struct GBACartridgeOverride override = { "", SAVEDATA_SRAM, HW_NO_OVERRIDE, IDLE_LOOP_NONE, false };
		GBAOverrideApply(gba, &override);
	}
#else
	UNUSED(core);
#endif
}

static int _regionOf(uint32_t pc, uint32_t* off) {
	int i;
	for (i = 0; i < RC_REGIONS; ++i) {
		if (pc - recompRegions[i].base < recompRegions[i].size) {
			*off = pc - recompRegions[i].base;
			return i;
		}
	}
	return RC_REGIONS;
}

static void _register(const struct RcEntry* e, unsigned n) {
	unsigned i;
	for (i = 0; i < n; ++i) {
		uint32_t off;
		int r = _regionOf(e[i].addr, &off);
		if (r == RC_REGIONS) {
			continue;
		}
		int width = e[i].thumb ? 2 : 4;
		leader[r][e[i].thumb][off >> 4] |= 1 << ((off >> 1) & 7);
		uint32_t end = off + e[i].size;
		uint32_t o;
		// Every instruction of the block is an entry point (see emit_resume in recomp.py)
		for (o = off; o < end && o < recompRegions[r].size; o += width) {
			recompRegions[r].table[e[i].thumb][o >> 1] = e[i].fn;
			covered[r][e[i].thumb][o >> 4] |= 1 << ((o >> 1) & 7);
			if (r == RC_IWRAM) {
				// The block also baked in the two opcodes after it (pipeline prefetch)
				uint32_t len = end - o + 2 * width;
				if (o + len > recompRegions[r].size) {
					len = recompRegions[r].size - o;
				}
				guardLen[e[i].thumb][o >> 1] = len;
			}
		}
		++nBlocks;
	}
}

static void _setupRegions(size_t romSize) {
	recompRegions[RC_ROM].base = 0x08000000U;
	recompRegions[RC_ROM].size = romSize;
	recompRegions[RC_BIOS].base = 0;
	recompRegions[RC_BIOS].size = SIZE_BIOS;
	recompRegions[RC_IWRAM].base = 0x03000000U;
	recompRegions[RC_IWRAM].size = SIZE_WORKING_IRAM;
	int r, m;
	for (r = 0; r < RC_REGIONS; ++r) {
		for (m = 0; m < 2; ++m) {
			recompRegions[r].table[m] = calloc(recompRegions[r].size / 2, sizeof(RecompBlock));
			covered[r][m] = calloc(recompRegions[r].size / 16 + 1, 1);
			leader[r][m] = calloc(recompRegions[r].size / 16 + 1, 1);
			missed[r][m] = calloc(recompRegions[r].size / 16 + 1, 1);
		}
	}
	for (m = 0; m < 2; ++m) {
		guardLen[m] = calloc(SIZE_WORKING_IRAM / 2, sizeof(uint16_t));
		recompRegions[RC_IWRAM].guardLen[m] = guardLen[m];
	}
	if (rcRamSize) {
		memcpy(&ramImage[rcRamBase - 0x03000000U], rcRamImage, rcRamSize);
	}
	recompRegions[RC_IWRAM].image = ramImage;
}

// ---- profile: where the interpreter had to take over ---------------------------
static void _miss(uint32_t pc, int thumb) {
	uint32_t off = 0;
	int r = _regionOf(pc, &off);
	++interpCount[r];
	if (r == RC_REGIONS) {
		static int reported;
		if (!reported && getenv("RC_DEBUG_WILD")) { // execution left ROM, RAM code and BIOS: say how it got there
			extern struct mCore* rcCoreForDebug;
			struct ARMCore* cpu = rcCoreForDebug->cpu;
			reported = 1;
			fprintf(stderr, "wild jump to %08X (%s), lr %08X, r0-r7 %X %X %X %X %X %X %X %X, sp %08X\n", pc, thumb ? "thumb" : "arm", cpu->gprs[14],
			        cpu->gprs[0], cpu->gprs[1], cpu->gprs[2], cpu->gprs[3], cpu->gprs[4], cpu->gprs[5], cpu->gprs[6], cpu->gprs[7], cpu->gprs[13]);
		}
		return;
	}
	// A compiled block exists but was not used: RAM no longer holds the code it was built from
	if (covered[r][thumb][off >> 4] & (1 << ((off >> 1) & 7))) {
		if (getenv("RC_GUARD_DEBUG")) {
			static unsigned shown;
			if (shown < 40) {
				++shown;
				fprintf(stderr, "guard: stale block at %08X (%s)\n", pc, thumb ? "thumb" : "arm");
			}
		}
		return;
	}
	++interpUncovered[r];
	missed[r][thumb][off >> 4] |= 1 << ((off >> 1) & 7);
}

static void _writeProfile(const char* path) {
	FILE* f = fopen(path, "a");
	if (!f) {
		return;
	}
	unsigned n = 0;
	int r, m;
	for (r = 0; r < RC_REGIONS; ++r) {
		for (m = 0; m < 2; ++m) {
			uint32_t off;
			for (off = 0; off < recompRegions[r].size; off += 2) {
				if (missed[r][m][off >> 4] & (1 << ((off >> 1) & 7))) {
					fprintf(f, "%c %08X\n", m ? 'T' : 'A', off + recompRegions[r].base);
					++n;
				}
			}
		}
	}
	fclose(f);
	fprintf(stderr, "profile: %u new block entry points appended to %s\n", n, path);
}

// ---- mods (see mod.h) ------------------------------------------------------------
#define MAX_MODS 64
#define MAX_HOOKS 256
static struct { const char* name; void (*fn)(void); } modInits[MAX_MODS];
static unsigned nModInits;
static void (*modFrameFns[MAX_MODS])(void);
static unsigned nModFrameFns;
static struct ModHook { uint32_t addr; ModHookFn fn; RecompBlock orig; } modHooks[MAX_HOOKS];
static unsigned nModHooks;
static struct mCore* modCore;
static color_t* modVideo;
static unsigned modFrameNo;
static uint8_t modPcKeys[2][512];

void modAddInit(const char* name, void (*fn)(void)) {
	if (nModInits < MAX_MODS) {
		modInits[nModInits].name = name;
		modInits[nModInits++].fn = fn;
	}
}

void modOnFrame(void (*fn)(void)) {
	if (nModFrameFns < MAX_MODS) {
		modFrameFns[nModFrameFns++] = fn;
	}
}

static void (*modStateWriteFns[MAX_MODS])(void);
static void (*modStateReadFns[MAX_MODS])(void);
static int nModStateWriteFns, nModStateReadFns;
void modOnStateWrite(void (*fn)(void)) {
	if (nModStateWriteFns < MAX_MODS) {
		modStateWriteFns[nModStateWriteFns++] = fn;
	}
}
void modOnStateRead(void (*fn)(void)) {
	if (nModStateReadFns < MAX_MODS) {
		modStateReadFns[nModStateReadFns++] = fn;
	}
}
static void _stateWriting(void) {
	int i;
	for (i = 0; i < nModStateWriteFns; ++i) {
		modStateWriteFns[i]();
	}
}
static void _stateRead(void) {
	int i;
	for (i = 0; i < nModStateReadFns; ++i) {
		modStateReadFns[i]();
	}
}

static void _hookTrampoline(struct ARMCore* cpu) {
	int thumb = cpu->executionMode == MODE_THUMB;
	uint32_t pc = cpu->gprs[ARM_PC] - (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
	unsigned i;
	for (i = 0; i < nModHooks; ++i) {
		if (modHooks[i].addr == pc) {
			break;
		}
	}
	if (modHooks[i].fn((uint32_t*) cpu->gprs) == MOD_RETURN) {
		// Same effect as the function executing `bx lr` straight away
		uint32_t lr = cpu->gprs[ARM_LR];
		_ARMSetMode(cpu, lr & 1);
		cpu->gprs[ARM_PC] = lr & ~1U;
		cpu->cycles += (lr & 1) ? ThumbWritePC(cpu) : ARMWritePC(cpu);
		return;
	}
	modHooks[i].orig(cpu);
}

void modHook(uint32_t addr, ModHookFn fn) {
	addr &= ~1U;
	uint32_t off;
	int r = _regionOf(addr, &off);
	int thumb = r < RC_REGIONS && (leader[r][1][off >> 4] & (1 << ((off >> 1) & 7)));
	if (r == RC_REGIONS || !recompEnabled || (!thumb && !(leader[r][0][off >> 4] & (1 << ((off >> 1) & 7))))) {
		fprintf(stderr, "mod: cannot hook %08X: no compiled block starts there.\n"
		                "     Add the line \"T %08X\" (or \"A %08X\" for ARM code) to games/" GAME_ID "/entries.txt and run make.\n", addr, addr, addr);
		exit(1);
	}
	if (nModHooks == MAX_HOOKS) {
		fputs("mod: too many hooks\n", stderr);
		exit(1);
	}
	RecompBlock* slot = &recompRegions[r].table[thumb][off >> 1];
	if (*slot == _hookTrampoline) {
		fprintf(stderr, "mod: %08X is hooked twice\n", addr);
		exit(1);
	}
	modHooks[nModHooks].addr = addr;
	modHooks[nModHooks].fn = fn;
	modHooks[nModHooks++].orig = *slot;
	*slot = _hookTrampoline;
}

// Calling a game function: point the CPU at it with a return address that we
// trap (the ROM's entry point, which nothing returns to), and run until it gets
// there.
#define CALL_RETURN_TRAP 0x08000000U
static void ThumbStepForMods(struct ARMCore* cpu) {
	ARMRun(cpu); // one interpreted instruction
}
static int modCallReturned;
static RecompBlock modCallTrapOrig;

static void _callReturnTrap(struct ARMCore* cpu) {
	if (modCallReturned) { // not our call: this really is the game booting
		if (cpu->executionMode == MODE_ARM && modCallTrapOrig) {
			modCallTrapOrig(cpu);
		} else {
			ThumbStepForMods(cpu);
		}
		return;
	}
	modCallReturned = 1;
	cpu->nextEvent = cpu->cycles; // leave the run loop
}

int modCanCall(void) {
	struct ARMCore* cpu = modCore->cpu;
	return recompEnabled && cpu->cpsr.priv == MODE_SYSTEM && modCallReturned;
}

uint32_t modCall(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
	struct ARMCore* cpu = modCore->cpu;
	if (!modCanCall()) {
		return 0;
	}
	struct ARMCore saved = *cpu;
	cpu->gprs[0] = a0;
	cpu->gprs[1] = a1;
	cpu->gprs[2] = a2;
	cpu->gprs[3] = a3;
	cpu->gprs[ARM_SP] &= ~7;
	// A Thumb function may return with "pop {pc}", which stays in Thumb state,
	// so its trap is the same address reached in Thumb state
	cpu->gprs[ARM_LR] = CALL_RETURN_TRAP | (addr & 1);
	cpu->halted = 0;
	_ARMSetMode(cpu, addr & 1 ? MODE_THUMB : MODE_ARM);
	cpu->gprs[ARM_PC] = addr & ~1U;
	if (addr & 1) {
		ThumbWritePC(cpu);
	} else {
		ARMWritePC(cpu);
	}
	modCallReturned = 0;
	int guard = 0;
	while (!modCallReturned && ++guard < 2000000) {
		if (cpu->gprs[ARM_PC] - (cpu->executionMode == MODE_ARM ? WORD_SIZE_ARM : WORD_SIZE_THUMB) == CALL_RETURN_TRAP) {
			modCallReturned = 1; // arrived by a path that skipped the block table
			break;
		}
		ARMRunLoop(cpu);
	}
	if (!modCallReturned) {
		fprintf(stderr, "mod: call to %08X never returned\n", addr);
		modCallReturned = 1;
	}
	uint32_t result = cpu->gprs[0];
	// Back to where the game was, keeping the time that passed
	int32_t cycles = cpu->cycles, nextEvent = cpu->nextEvent;
	*cpu = saved;
	cpu->cycles = cycles;
	cpu->nextEvent = nextEvent < cycles ? nextEvent : cycles;
	GBATestIRQ(modCore->board, 0); // an interrupt that arrived at the last moment is still owed
	return result;
}

uint32_t* modRegisters(void) {
	struct ARMCore* cpu = modCore->cpu;
	return (uint32_t*) cpu->gprs;
}

uint8_t modRead8(uint32_t a) { return modCore->busRead8(modCore, a); }
uint16_t modRead16(uint32_t a) { return modCore->busRead16(modCore, a); }
uint32_t modRead32(uint32_t a) { return modCore->busRead32(modCore, a); }

// The ROM the CPU reads. mGBA maps the embedded image at first, but the
// first patch-write into the ROM region makes it copy the whole cartridge
// and read from the copy from then on; anything written to the image after
// that would go unseen. So the mods' ROM pointer follows mGBA's, and the
// mods' writes to ROM addresses go straight into that memory instead of
// through mGBA's patch path, which is what would trigger the copy.
uint8_t* modRom(uint32_t offset) {
	struct GBA* gba = modCore ? modCore->board : NULL;
	return gba && gba->memory.rom ? (uint8_t*) gba->memory.rom + offset : rcRomImage + offset;
}
static int _isRom(uint32_t a) { return a - 0x08000000U < 0x02000000U; }
void modWrite8(uint32_t a, uint8_t v) {
	if (_isRom(a)) { *modRom(a - 0x08000000U) = v; return; }
	modCore->rawWrite8(modCore, a, -1, v);
}
void modWrite16(uint32_t a, uint16_t v) {
	if (_isRom(a)) { memcpy(modRom(a - 0x08000000U), &v, 2); return; }
	modCore->rawWrite16(modCore, a, -1, v);
}
void modWrite32(uint32_t a, uint32_t v) {
	if (_isRom(a)) { memcpy(modRom(a - 0x08000000U), &v, 4); return; }
	modCore->rawWrite32(modCore, a, -1, v);
}
void* modRam(uint32_t a) {
	struct GBA* gba = modCore->board;
	if (a - 0x03000000U < SIZE_WORKING_IRAM) {
		return (uint8_t*) gba->memory.iwram + (a - 0x03000000U);
	}
	if (a - 0x02000000U < SIZE_WORKING_RAM) {
		return (uint8_t*) gba->memory.wram + (a - 0x02000000U);
	}
	return NULL;
}
static int modIsPaused;
static uint32_t modKeyState;
static uint32_t modBlockKeys; // buttons held when a mod menu closed: ignored until released
void modSetPaused(int paused) {
	if (modIsPaused && !paused) {
		modBlockKeys = modKeyState;
	}
	modIsPaused = paused;
}
int modPaused(void) { return modIsPaused; }
static int modScale = 1;
void modTextScale(int scale) { modScale = scale < 1 ? 1 : scale; }
static int modTextPlainOn;
void modTextPlain(int on) { modTextPlainOn = on; }
uint32_t modRandom(uint32_t n) {
	static uint32_t state = 0x2545F491;
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return n ? (uint32_t) (((uint64_t) state * n) >> 32) : 0;
}
unsigned modFrame(void) { return modFrameNo; }
uint32_t modKeys(void) { return modKeyState; }
int modKeyDown(int sc) { return sc >= 0 && sc < 512 && modPcKeys[0][sc]; }
int modKeyPressed(int sc) { return sc >= 0 && sc < 512 && modPcKeys[0][sc] && !modPcKeys[1][sc]; }
uint32_t* modScreen(void) { return modVideo; }

void modRect(int x, int y, int w, int h, uint32_t color) {
	int i, j;
	for (j = y < 0 ? 0 : y; j < y + h && j < H; ++j) {
		for (i = x < 0 ? 0 : x; i < x + w && i < W; ++i) {
			modVideo[j * W + i] = color;
		}
	}
}

// 3x5 pixel font, ASCII 32-90, five rows of three bits per glyph
static const char modFont[59][6] = {
	"00000", "22202", "55000", "57575", "36736", "41241", "25253", "22000", "12221", "42224", "05250", "02720",
	"00024", "00700", "00002", "11244", "75557", "26227", "71747", "71717", "55711", "74717", "74757", "71111",
	"75757", "75717", "02020", "02024", "12421", "07070", "42124", "71202", "75747", "25755", "65656", "34443",
	"65556", "74747", "74744", "34553", "55755", "72227", "11156", "55655", "44447", "57555", "65555", "25552",
	"65644", "25563", "65655", "34216", "72222", "55557", "55552", "55575", "55255", "55222", "71247",
};

void modText(int x, int y, uint32_t color, const char* fmt, ...) {
	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	const char* p;
	int k = modScale;
	if (!modTextPlainOn) {
		modRect(x - k, y - k, (int) strlen(buf) * 4 * k + k, 7 * k, 0x000000);
	}
	for (p = buf; *p; ++p, x += 4 * k) {
		int c = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
		if (c < 32 || c > 90) {
			c = '?';
		}
		int row, col;
		for (row = 0; row < 5; ++row) {
			int bits = modFont[c - 32][row] - '0';
			for (col = 0; col < 3; ++col) {
				if (bits & (4 >> col)) {
					if (modTextPlainOn) { // a dark edge to the lower right instead of the box
						modRect(x + col * k + 1, y + row * k + 1, k, k, 0x000000);
					}
					modRect(x + col * k, y + row * k, k, k, color);
				}
			}
		}
	}
}

// Write watches: wrap the CPU's store functions
#define MAX_WATCHES 16
static struct { uint32_t addr, size; ModWatchFn fn; } modWatches[MAX_WATCHES];
static unsigned nModWatches;
static void (*origStore32)(struct ARMCore*, uint32_t, int32_t, int*);
static void (*origStore16)(struct ARMCore*, uint32_t, int16_t, int*);
static void (*origStore8)(struct ARMCore*, uint32_t, int8_t, int*);

static void _watchHit(struct ARMCore* cpu, uint32_t addr, uint32_t value, int bytes) {
	unsigned i;
	for (i = 0; i < nModWatches; ++i) {
		if (addr + bytes > modWatches[i].addr && addr < modWatches[i].addr + modWatches[i].size) {
			uint32_t pc = cpu->gprs[ARM_PC] - (cpu->executionMode == MODE_THUMB ? 4 : 8);
			modWatches[i].fn(addr, value, bytes, pc, (const uint32_t*) cpu->gprs);
		}
	}
}
static void _watchStore32(struct ARMCore* cpu, uint32_t a, int32_t v, int* c) { _watchHit(cpu, a & ~3U, v, 4); origStore32(cpu, a, v, c); }
static void _watchStore16(struct ARMCore* cpu, uint32_t a, int16_t v, int* c) { _watchHit(cpu, a & ~1U, (uint16_t) v, 2); origStore16(cpu, a, v, c); }
static void _watchStore8(struct ARMCore* cpu, uint32_t a, int8_t v, int* c) { _watchHit(cpu, a, (uint8_t) v, 1); origStore8(cpu, a, v, c); }

void modWatchWrites(uint32_t addr, uint32_t size, ModWatchFn fn) {
	if (nModWatches < MAX_WATCHES) {
		modWatches[nModWatches].addr = addr;
		modWatches[nModWatches].size = size;
		modWatches[nModWatches++].fn = fn;
	}
}

static void _installWatches(struct ARMCore* cpu) {
	if (!nModWatches) {
		return;
	}
	origStore32 = cpu->memory.store32;
	origStore16 = cpu->memory.store16;
	origStore8 = cpu->memory.store8;
	cpu->memory.store32 = _watchStore32;
	cpu->memory.store16 = _watchStore16;
	cpu->memory.store8 = _watchStore8;
}

static char modStateDir[1024] = ".";

// Rolling snapshots for working out a crash afterwards: every 5 seconds of
// play the state goes to <id>.auto1 (the one before to .auto2), and the moment
// the core reports a jump to nowhere the rolling stops, the broken state goes
// to <id>.crash, and .auto1/.auto2 are the last good states (5 and 10 seconds
// before). They load with --load-state like any other.
static int crashed;
static int recoverPending;      // a jump to nowhere was seen: go back to the last good snapshot
static int recoverEnabled;      // only the windowed game recovers; a headless run shows its crash
unsigned modRecoveredFrame;     // the frame of the last recovery, for the mods to tell the player

static void _writeState(const char* path) {
	size_t size = modCore->stateSize(modCore);
	void* buf = malloc(size);
	FILE* f = fopen(path, "wb");
	if (f && modCore->saveState(modCore, buf)) {
		fwrite(buf, 1, size, f);
	}
	if (f) {
		fclose(f);
	}
	free(buf);
}

static void _autoSnapshot(void) {
	char a1[1100], a2[1100];
	if (crashed || !modStateDir[0] || modFrameNo % 300) {
		return;
	}
	snprintf(a1, sizeof(a1), "%s/" SAVE_ID ".auto1", modStateDir);
	snprintf(a2, sizeof(a2), "%s/" SAVE_ID ".auto2", modStateDir);
	rename(a1, a2);
	_stateWriting();
	_writeState(a1);
}

static void _noteCrash(void) {
	char path[1100];
	if (crashed || !modStateDir[0] || !modCore) {
		return;
	}
	crashed = 1;
	snprintf(path, sizeof(path), "%s/" SAVE_ID ".crash", modStateDir);
	_writeState(path);
	modLog("the game jumped to nowhere: state kept in %s; the last good states are in .auto1 and .auto2", path);
	recoverPending = recoverEnabled;
	fprintf(stderr, "lgoku2: the game jumped to nowhere at frame %u (state kept in %s)%s\n", modFrameNo, path, recoverEnabled ? "; going back to the last snapshot" : "");
}

// The game went off the rails: back to the snapshot of ten seconds before
// (five, if that is all there is), at the end of the frame, with the mods
// told as for any loaded state. Snapshots carry on from there.
static void _recover(void) {
	static const char* const names[] = { ".auto2", ".auto1" };
	size_t size = modCore->stateSize(modCore);
	void* buf = malloc(size);
	int k, ok = 0;
	char path[1100];
	recoverPending = 0;
	for (k = 0; k < 2 && !ok; ++k) {
		FILE* f;
		snprintf(path, sizeof(path), "%s/" SAVE_ID "%s", modStateDir, names[k]);
		f = fopen(path, "rb");
		if (f) {
			ok = fread(buf, 1, size, f) == size && modCore->loadState(modCore, buf);
			fclose(f);
		}
	}
	free(buf);
	if (ok) {
		_applySaveMemory(modCore);
		_stateRead();
		crashed = 0;
		modRecoveredFrame = modFrameNo;
		modLog("recovered: back to the state in %s", path);
		fprintf(stderr, "lgoku2: the game jumped to nowhere; back to %s\n", path);
	} else {
		fprintf(stderr, "lgoku2: the game jumped to nowhere and no snapshot could be loaded\n");
	}
}

static int _stateFile(int slot, int save) {
	char path[1100];
	snprintf(path, sizeof(path), "%s/" SAVE_ID ".state%d", modStateDir, slot);
	size_t size = modCore->stateSize(modCore);
	void* buf = malloc(size);
	int ok = 0;
	FILE* f = fopen(path, save ? "wb" : "rb");
	if (f && save) {
		_stateWriting();
		ok = modCore->saveState(modCore, buf) && fwrite(buf, 1, size, f) == size;
	} else if (f) {
		ok = fread(buf, 1, size, f) == size && modCore->loadState(modCore, buf);
		_applySaveMemory(modCore);
		if (ok) {
			_stateRead();
		}
	}
	if (f) {
		fclose(f);
	}
	free(buf);
	return ok;
}

static void _settingsPath(char* path, size_t size) {
	snprintf(path, size, "%s/" SAVE_ID ".modsettings", modStateDir);
}

int modGetSetting(const char* name, int fallback) {
	char path[1100], key[64];
	int value;
	_settingsPath(path, sizeof(path));
	FILE* f = fopen(path, "r");
	if (!f) {
		return fallback;
	}
	while (fscanf(f, "%63s %d", key, &value) == 2) {
		if (!strcmp(key, name)) {
			fallback = value;
		}
	}
	fclose(f);
	return fallback;
}

void modSetSetting(const char* name, int value) {
	char path[1100], key[64];
	char kept[4096] = "";
	int old;
	_settingsPath(path, sizeof(path));
	FILE* f = fopen(path, "r");
	while (f && fscanf(f, "%63s %d", key, &old) == 2) {
		if (strcmp(key, name) && strlen(kept) + 96 < sizeof(kept)) {
			snprintf(kept + strlen(kept), 96, "%s %d\n", key, old);
		}
	}
	if (f) {
		fclose(f);
	}
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s%s %d\n", kept, name, value);
		fclose(f);
	}
}

void modLog(const char* fmt, ...) {
	static FILE* f;
	static int tried;
	va_list ap;
	if (!tried) {
		char path[1100];
		tried = 1;
		if (modStateDir[0]) {
			snprintf(path, sizeof(path), "%s/" SAVE_ID ".modlog", modStateDir);
			f = fopen(path, "w");
		}
	}
	if (!f) {
		return;
	}
	fprintf(f, "%u: ", modFrameNo);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

int modSaveState(int slot) { return _stateFile(slot, 1); }
int modLoadState(int slot) { return _stateFile(slot, 0); }

// The same snapshots kept in memory (a netplay mod sends one to the other side)
size_t modStateSize(void) { return modCore->stateSize(modCore); }
int modStateSave(void* buf) {
	_stateWriting();
	return modCore->saveState(modCore, buf) ? 1 : 0;
}
int modStateLoad(const void* buf) {
	int ok = modCore->loadState(modCore, buf) ? 1 : 0;
	_applySaveMemory(modCore);
	if (ok) {
		_stateRead();
	}
	return ok;
}

static color_t modCleanFrame[W * H];
static int modHaveCleanFrame;
static uint32_t (*modKeyFilters[MAX_MODS])(uint32_t);
static const char* modKeyFilterNames[MAX_MODS];
static unsigned nModKeyFilters;
static const char* modInitName = "?"; // the mod whose MOD_INIT is running
void modFilterKeys(uint32_t (*fn)(uint32_t keys)) {
	if (nModKeyFilters < MAX_MODS) {
		modKeyFilterNames[nModKeyFilters] = modInitName;
		modKeyFilters[nModKeyFilters++] = fn;
	}
}

// Buttons held while nothing reaches the game: a mod's page may be taking them
// (fine), or a mod may be stuck swallowing them (a "frozen" game). The diary
// says which filter it was, once it has gone on for 5 seconds.
static void _swallowWatch(uint32_t in, uint32_t out, unsigned culprit) {
	static unsigned frames;
	if ((in & 0x3FF) && !(out & 0x3FF)) {
		++frames;
		if (frames == 300 || frames % 1800 == 0) {
			modLog("input swallowed for %u s: buttons %03X held, nothing reaches the game; first filter to drop them: %s",
			       frames / 60, in & 0x3FF, culprit < nModKeyFilters ? modKeyFilterNames[culprit] : "(none: the game itself?)");
		}
	} else {
		frames = 0;
	}
}

// Runs one game frame unless a mod has the game paused. Returns 1 if it ran.
static int (*modFrameGateFn)(uint32_t* keys);
static unsigned modGameFrameNo;
void modSetFrameGate(int (*fn)(uint32_t* keys)) { modFrameGateFn = fn; }
unsigned modGameFrame(void) { return modGameFrameNo; }
const char* modSaveDir(void) { return modStateDir; }

static int _runGameFrame(struct mCore* core, uint32_t keys, int mods) {
	modKeyState = keys;
	modBlockKeys &= keys;
	keys &= ~modBlockKeys;
	if (mods && modIsPaused) {
		memcpy(modVideo, modCleanFrame, sizeof(modCleanFrame));
		return 0;
	}
	if (mods && modFrameGateFn && !modFrameGateFn(&keys)) {
		// held back (a lockstep mod waiting for the other side): the picture stays
		if (modHaveCleanFrame) {
			memcpy(modVideo, modCleanFrame, sizeof(modCleanFrame));
		}
		return 0;
	}
	if (mods) {
		unsigned i, culprit = ~0u;
		uint32_t in = keys;
		for (i = 0; i < nModKeyFilters; ++i) {
			uint32_t out = modKeyFilters[i](keys);
			if ((keys & 0x3FF) && !(out & 0x3FF) && culprit == ~0u) {
				culprit = i;
			}
			keys = out;
		}
		_swallowWatch(in, keys, culprit);
	}
	// The video hardware only redraws the lines of the picture that changed.
	// So whatever the mods painted over last frame's picture has to come off
	// first, or it would stay on every line the game leaves alone (a moving
	// overlay over a still scene left a trail).
	if (mods && modHaveCleanFrame) {
		memcpy(modVideo, modCleanFrame, sizeof(modCleanFrame));
	}
	core->setKeys(core, keys);
	core->runFrame(core);
	++modGameFrameNo;
	if (mods) {
		memcpy(modCleanFrame, modVideo, sizeof(modCleanFrame));
		modHaveCleanFrame = 1;
	}
	return 1;
}

static uint32_t modTestKeys[64][2]; // --pc-keys: frame, scancode
static unsigned nModTestKeys;

// Notice the game writing its battery save by watching the save memory itself
static int modGameSavedNow;
int modGameSaved(void) { return modGameSavedNow; }

static void _watchBatterySave(void) {
	// The hardware layer marks the save memory dirty when the game writes it and
	// clears the mark once the data has been flushed to the file.
	static int wasDirty;
	struct GBA* gba = modCore->board;
	struct GBASavedata* sd = &gba->memory.savedata;
	int dirty = sd->dirty != 0;
	const char* test = getenv("RC_TEST_TOUCH_SAVE");
	if (test && (unsigned) atoi(test) == modFrameNo) {
		sd->dirty |= mSAVEDATA_DIRT_NEW; // test aid: pretend the game saved
		dirty = 1;
	}
	modGameSavedNow = 0;
	if (dirty && !wasDirty) {
		fprintf(stderr, "battery save: the game started writing (frame %u, save type %d)\n", modFrameNo, sd->type);
	} else if (!dirty && wasDirty) {
		fprintf(stderr, "battery save: written to file (frame %u)\n", modFrameNo);
		modGameSavedNow = 1;
	}
	wasDirty = dirty;
}

static void _modsFrame(void) {
	unsigned i;
	++modFrameNo;
	_watchBatterySave();
	for (i = 0; i < nModTestKeys; ++i) {
		modPcKeys[0][modTestKeys[i][1]] = 0;
	}
	for (i = 0; i < nModTestKeys; ++i) {
		if (modTestKeys[i][0] == modFrameNo) {
			modPcKeys[0][modTestKeys[i][1]] = 1;
		}
	}
	for (i = 0; i < nModFrameFns; ++i) {
		modFrameFns[i]();
	}
}

// ---- logging -------------------------------------------------------------------
static int logVerbose;
static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (strstr(format, "Jumped to invalid address")) {
		_noteCrash();
	}
	if (!logVerbose && !(level & (mLOG_FATAL | mLOG_ERROR))) {
		return;
	}
	fprintf(stderr, "[%s] ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}
static struct mLogger logger = { .log = _log };

// ---- helpers -------------------------------------------------------------------
static uint64_t fnv(uint64_t h, const void* data, size_t n) {
	const uint8_t* p = data;
	size_t i;
	for (i = 0; i < n; ++i) {
		h = (h ^ p[i]) * 0x100000001B3ULL;
	}
	return h;
}

static void _put32(uint8_t* p, uint32_t v) {
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void _pngChunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
	uint8_t b[4];
	_put32(b, n);
	fwrite(b, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (n) {
		fwrite(data, 1, n, f);
	}
	uint32_t crc = crc32(crc32(0, (const Bytef*) type, 4), data, n);
	_put32(b, crc);
	fwrite(b, 1, 4, f);
}

#define UPSCALE 4
void rcUpscale(const uint32_t* src, uint32_t* dst, int width, int height, int factor);
static uint32_t bigFrame[W * UPSCALE * H * UPSCALE]; // upscaled picture, 0x00RRGGBB

// redFirst: pixels are 0x00BBGGRR (the game's frame) rather than 0x00RRGGBB
static void _writePNGSized(const char* path, const uint32_t* px, int w, int h, int redFirst) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		return;
	}
	size_t rawSize = (size_t) h * (w * 3 + 1);
	uint8_t* raw = malloc(rawSize);
	uLongf clen = compressBound(rawSize);
	uint8_t* comp = malloc(clen);
	int x, y;
	for (y = 0; y < h; ++y) {
		uint8_t* row = &raw[y * (w * 3 + 1)];
		*row++ = 0;
		for (x = 0; x < w; ++x) {
			uint32_t c = px[y * w + x];
			if (redFirst) {
				*row++ = c; *row++ = c >> 8; *row++ = c >> 16;
			} else {
				*row++ = c >> 16; *row++ = c >> 8; *row++ = c;
			}
		}
	}
	compress(comp, &clen, raw, rawSize);
	uint8_t ihdr[13] = { 0 };
	_put32(ihdr, w);
	_put32(ihdr + 4, h);
	ihdr[8] = 8; ihdr[9] = 2;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	_pngChunk(f, "IHDR", ihdr, 13);
	_pngChunk(f, "IDAT", comp, clen);
	_pngChunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(comp);
}

// ---- picture options ---------------------------------------------------------------
enum { FX_NONE, FX_SCANLINES, FX_GRID, FX_COUNT };
enum { PIC_ORIGINAL, PIC_SMOOTH, PIC_HD, PIC_COUNT };
static int optSmooth = PIC_SMOOTH; // F2: original pixels, xBRZ upscaling, or redrawn by a neural network
int rcAiInit(const char* param, const char* bin);
void rcAiShutdown(void);
int rcAiPending(void);
int rcAiCoverage(void);
void rcAiStats(unsigned* hits, unsigned* misses, unsigned* inferred, unsigned* cached);
void rcAiCompose(const uint32_t* frame, const uint32_t* fallback, uint32_t* out, int scrollX, int scrollY);
static int aiState; // 0 = not tried, 1 = running, -1 = unavailable

// The map's scroll position: the scroll of the backmost enabled background
static void _mapScroll(int* x, int* y) {
	struct GBA* gba = rcCoreForDebug->board;
	uint16_t dispcnt = gba->memory.io[REG_DISPCNT >> 1];
	int bg, best = -1, bestPriority = -1;
	for (bg = 0; bg < 4; ++bg) {
		int priority = gba->memory.io[(REG_BG0CNT >> 1) + bg] & 3;
		if ((dispcnt & (0x100 << bg)) && priority >= bestPriority) {
			best = bg;
			bestPriority = priority;
		}
	}
	*x = best < 0 ? 0 : gba->memory.io[(REG_BG0HOFS >> 1) + best * 2] & 0x1FF;
	*y = best < 0 ? 0 : gba->memory.io[(REG_BG0VOFS >> 1) + best * 2] & 0x1FF;
}
static int optFx = FX_SCANLINES; // F3: none, scanlines, LCD grid
static int optColor = 1;         // F4: GBA screen colour correction

// GBA games were drawn over-bright and over-saturated to compensate for the
// handheld's dark, washed-out LCD. This is the usual correction (the "gba-color"
// shader's matrix), worked in linear light.
static void _correctColors(const color_t* src, color_t* dst) {
	static float toLinear[256];
	static uint8_t fromLinear[4096];
	static int ready;
	int i;
	if (!ready) {
		for (i = 0; i < 256; ++i) {
			toLinear[i] = powf(i / 255.0f, 2.2f);
		}
		for (i = 0; i < 4096; ++i) {
			fromLinear[i] = (uint8_t) (powf(i / 4095.0f, 1 / 2.2f) * 255.0f + 0.5f);
		}
		ready = 1;
	}
	for (i = 0; i < W * H; ++i) {
		float r = toLinear[src[i] & 0xFF], g = toLinear[(src[i] >> 8) & 0xFF], b = toLinear[(src[i] >> 16) & 0xFF];
		float r2 = 0.82f * r + 0.24f * g - 0.06f * b;
		float g2 = 0.125f * r + 0.665f * g + 0.21f * b;
		float b2 = 0.195f * r + 0.075f * g + 0.73f * b;
		// Full strength looks flat on a bright monitor: go half way
		r2 = (r2 + r) * 0.5f;
		g2 = (g2 + g) * 0.5f;
		b2 = (b2 + b) * 0.5f;
		int ri = r2 < 0 ? 0 : r2 > 1 ? 4095 : (int) (r2 * 4095), gi = g2 < 0 ? 0 : g2 > 1 ? 4095 : (int) (g2 * 4095), bi = b2 < 0 ? 0 : b2 > 1 ? 4095 : (int) (b2 * 4095);
		dst[i] = fromLinear[ri] | (fromLinear[gi] << 8) | (fromLinear[bi] << 16);
	}
}

static inline uint32_t _dim(uint32_t c, unsigned factor) { // factor out of 256
	return ((((c & 0xFF00FF) * factor) >> 8) & 0xFF00FF) | ((((c & 0x00FF00) * factor) >> 8) & 0x00FF00);
}

// Builds the picture that goes on screen: game frame -> bigFrame (0x00RRGGBB)
static void _composePicture(const color_t* frame) {
	static color_t corrected[W * H];
	int x, y;
	if (optColor) {
		_correctColors(frame, corrected);
		frame = corrected;
	}
	if (optSmooth == PIC_HD && !aiState) {
		aiState = rcAiInit(AI_MODEL ".param", AI_MODEL ".bin") ? -1 : 1;
	}
	if (optSmooth == PIC_HD && aiState > 0) {
		static uint32_t fallback[W * UPSCALE * H * UPSCALE];
		int sx, sy;
		_mapScroll(&sx, &sy);
		rcUpscale(frame, fallback, W, H, UPSCALE);
		rcAiCompose(frame, fallback, bigFrame, sx, sy);
		if (getenv("RC_AI_DEBUG")) {
			static unsigned n;
			if (++n % 60 == 0) {
				unsigned hits, misses, inferred, cached;
				rcAiStats(&hits, &misses, &inferred, &cached);
				fprintf(stderr, "HD mode: last second %d%% of the picture in HD, %u tiles computed, %u in cache, scroll %d,%d\n", rcAiCoverage(), inferred, cached, sx, sy);
			}
		}
	} else if (optSmooth) {
		rcUpscale(frame, bigFrame, W, H, UPSCALE);
	} else {
		for (y = 0; y < H * UPSCALE; ++y) {
			uint32_t* out = &bigFrame[y * W * UPSCALE];
			const color_t* in = &frame[(y / UPSCALE) * W];
			for (x = 0; x < W * UPSCALE; ++x) {
				uint32_t c = in[x / UPSCALE];
				out[x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
			}
		}
	}
	if (optFx == FX_NONE) {
		return;
	}
	// Each game pixel is a UPSCALE x UPSCALE block: darken its last row
	// (scanlines), or its last row and column (LCD grid)
	for (y = 0; y < H * UPSCALE; ++y) {
		uint32_t* out = &bigFrame[y * W * UPSCALE];
		if (y % UPSCALE == UPSCALE - 1) {
			for (x = 0; x < W * UPSCALE; ++x) {
				out[x] = _dim(out[x], 176);
			}
		} else if (optFx == FX_GRID) {
			for (x = UPSCALE - 1; x < W * UPSCALE; x += UPSCALE) {
				out[x] = _dim(out[x], 176);
			}
		} else if (y % UPSCALE == UPSCALE - 2) {
			for (x = 0; x < W * UPSCALE; ++x) {
				out[x] = _dim(out[x], 232); // soften the edge of the scanline
			}
		}
	}
}

static void _writePNG(const char* path, const color_t* px) {
	if (getenv("RC_AI_BENCH")) { // test aid: the HD picture exactly as it stood on this frame
		_writePNGSized(path, bigFrame, W * UPSCALE, H * UPSCALE, 0);
		return;
	}
	const char* fx = getenv("RC_SHOT_FX"); // test aid: "smooth,effect,colour" digits, e.g. 111
	if (fx && strlen(fx) == 3) {
		optSmooth = (fx[0] - '0') % PIC_COUNT;
		optFx = (fx[1] - '0') % FX_COUNT;
		optColor = fx[2] != '0';
		_composePicture(px);
		if (optSmooth == PIC_HD) { // wait for the network to finish every tile
			int tries;
			for (tries = 0; tries < 400 && rcAiPending(); ++tries) {
				SDL_Delay(25);
			}
			_composePicture(px);
		}
		_writePNGSized(path, bigFrame, W * UPSCALE, H * UPSCALE, 0);
		return;
	}
	_writePNGSized(path, px, W, H, 1);
}

static void _mkdirs(char* path) {
	char* p;
	for (p = path + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			mkdir(path, 0755);
			*p = '/';
		}
	}
}

// Deterministic button mashing used by the automated tests
static unsigned autoSeed; // --autoplay-seed: wander around instead of the fixed pattern
static const char* inputScript; // --input "U:120,A:4,N:30,UA:10": buttons held for a number of frames

static uint32_t _scriptKeys(unsigned frame) {
	const char* p = inputScript;
	unsigned start = 0;
	while (*p) {
		uint32_t keys = 0;
		for (; *p && *p != ':'; ++p) {
			switch (*p) {
			case 'A': keys |= 1 << GBA_KEY_A; break;
			case 'B': keys |= 1 << GBA_KEY_B; break;
			case 'S': keys |= 1 << GBA_KEY_START; break;
			case 'E': keys |= 1 << GBA_KEY_SELECT; break;
			case 'U': keys |= 1 << GBA_KEY_UP; break;
			case 'D': keys |= 1 << GBA_KEY_DOWN; break;
			case 'L': keys |= 1 << GBA_KEY_LEFT; break;
			case 'R': keys |= 1 << GBA_KEY_RIGHT; break;
			case 'l': keys |= 1 << GBA_KEY_L; break;
			case 'r': keys |= 1 << GBA_KEY_R; break;
			}
		}
		unsigned n = *p == ':' ? strtoul(p + 1, (char**) &p, 10) : 0;
		if (frame < start + n) {
			return keys;
		}
		start += n;
		if (*p == ',') {
			++p;
		}
	}
	return 0;
}

static uint32_t _autoKeys(unsigned frame) {
	uint32_t keys = 0;
	if (inputScript) {
		return _scriptKeys(frame);
	}
	if (autoSeed && frame > 2400) {
		// Hold a random direction for a second or two, mash A, close any dialogue
		static const int dirs[4] = { GBA_KEY_RIGHT, GBA_KEY_DOWN, GBA_KEY_LEFT, GBA_KEY_UP };
		uint32_t h = (frame / 90 + 1) * 2654435761U * autoSeed;
		h ^= h >> 15;
		keys |= 1 << dirs[h & 3];
		if (frame % 12 < 3) {
			keys |= 1 << GBA_KEY_A;
		}
		return keys;
	}
	if (frame % 40 < 4) {
		keys |= 1 << GBA_KEY_A;
	}
	if (frame % 97 < 4) {
		keys |= 1 << GBA_KEY_START;
	}
	if (frame > 1500) {
		static const int dirs[4] = { GBA_KEY_RIGHT, GBA_KEY_DOWN, GBA_KEY_LEFT, GBA_KEY_UP };
		keys |= 1 << dirs[(frame / 48) & 3];
		if (frame % 23 < 3) {
			keys |= 1 << GBA_KEY_B;
		}
	}
	return keys;
}

static uint32_t _keyboardKeys(void) {
	const uint8_t* k = SDL_GetKeyboardState(NULL);
	uint32_t keys = 0;
	keys |= (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K]) << GBA_KEY_A;
	keys |= (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) << GBA_KEY_B;
	keys |= (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) << GBA_KEY_SELECT;
	keys |= k[SDL_SCANCODE_RETURN] << GBA_KEY_START;
	keys |= k[SDL_SCANCODE_RIGHT] << GBA_KEY_RIGHT;
	keys |= k[SDL_SCANCODE_LEFT] << GBA_KEY_LEFT;
	keys |= k[SDL_SCANCODE_UP] << GBA_KEY_UP;
	keys |= k[SDL_SCANCODE_DOWN] << GBA_KEY_DOWN;
	keys |= (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_E]) << GBA_KEY_R;
	keys |= (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_Q]) << GBA_KEY_L;
	return keys;
}

static uint32_t _padKeys(SDL_GameController* pad) {
	if (!pad) {
		return 0;
	}
	uint32_t keys = 0;
#define BTN(B) SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_##B)
	keys |= BTN(B) << GBA_KEY_A;   // east face button, as on the GBA
	keys |= BTN(A) << GBA_KEY_B;
	keys |= BTN(BACK) << GBA_KEY_SELECT;
	keys |= BTN(START) << GBA_KEY_START;
	keys |= BTN(DPAD_RIGHT) << GBA_KEY_RIGHT;
	keys |= BTN(DPAD_LEFT) << GBA_KEY_LEFT;
	keys |= BTN(DPAD_UP) << GBA_KEY_UP;
	keys |= BTN(DPAD_DOWN) << GBA_KEY_DOWN;
	keys |= BTN(RIGHTSHOULDER) << GBA_KEY_R;
	keys |= BTN(LEFTSHOULDER) << GBA_KEY_L;
#undef BTN
	int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
	int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
	keys |= (x > 14000) << GBA_KEY_RIGHT;
	keys |= (x < -14000) << GBA_KEY_LEFT;
	keys |= (y > 14000) << GBA_KEY_DOWN;
	keys |= (y < -14000) << GBA_KEY_UP;
	return keys;
}

static void _usage(void) {
	puts(GAME_TITLE " - native build\n"
	     "\n"
	     "  --scale N         window scale (default 4)\n"
	     "  --fullscreen      start fullscreen (F11 toggles)\n"
	     "  --save PATH       save file (default ~/.local/share/" GAME_ID "/" GAME_ID ".sav)\n"
	     "  --rom FILE        your ROM of the game (default: ~/.local/share/" SAVE_ID "/" SAVE_ID ".gba,\n"
	     "                    or " GAME_ID ".gba next to the program; the program holds none of the game)\n"
	     "  --mute            no audio\n"
	     "  --fresh           boot from the title screen instead of resuming where you quit\n"
	     "  --no-mods         run the unmodified game\n"
	     "  --load-state FILE start from a save state (as written by the savestates mod)\n"
	     "  --save-state FILE with --headless: write a save state at the end\n"
	     "  --obj-only        draw sprites only, no backgrounds (for capturing graphics)\n"
	     "  --input SCRIPT    with --headless: scripted buttons, e.g. U:120,A:4,N:30 (UDLR A B S=Start E=Select l r)\n"
	     "\n"
	     "Keys: arrows move, X = A, Z = B, A = L, S = R, Enter = Start,\n"
	     "      Backspace = Select, Tab = fast forward, Space = skip scenes (fast forward\n"
	     "      and advance text), F11 fullscreen, Esc quit.\n"
	     "      Picture: F2 original pixels/smooth/HD redraw, F3 scanlines/LCD grid/off, F4 colour correction.\n"
	     "      Quitting saves; starting resumes.\n"
	     "\n"
	     "Development:\n"
	     "  --interp          ignore recompiled code, interpret everything\n"
	     "  --headless N      run N frames without a window, then exit\n"
	     "  --autoplay        scripted button presses (implied by --headless)\n"
	     "  --hash FILE       write a state hash per frame (to diff two runs)\n"
	     "  --shot FILE.png   screenshot of the last frame (use %d for every 300th frame)\n"
	     "  --profile FILE    append addresses that had no compiled block, for the next recompile\n"
	     "  --dump-iwram FILE write internal work RAM at exit (source image for RAM-resident code)\n"
	     "  --stats           print native/interpreted execution counts\n"
	     "  --verbose         show hardware-layer log messages");
}

int main(int argc, char** argv) {
	int scale = 4, fullscreen = 0, mute = 0, interp = 0, headless = 0, autoplay = 0, stats = 0, noMods = 0;
	const char* savePath = NULL;
	const char* hashPath = NULL;
	const char* shotPath = NULL;
	const char* profilePath = NULL;
	const char* ramDumpPath = NULL;
	const char* loadStatePath = NULL;
	int fresh = 0;
	int objOnly = 0;
	const char* saveStatePath = NULL;
	int i;
	for (i = 1; i < argc; ++i) {
		const char* a = argv[i];
		const char* next = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--scale") && next) { scale = atoi(next); ++i; }
		else if (!strcmp(a, "--fullscreen")) { fullscreen = 1; }
		else if (!strcmp(a, "--save") && next) { savePath = next; ++i; }
		else if (!strcmp(a, "--mute")) { mute = 1; }
		else if (!strcmp(a, "--interp")) { interp = 1; }
		else if (!strcmp(a, "--no-mods")) { noMods = 1; }
		else if (!strcmp(a, "--pc-keys") && next) {
			// test aid: "FRAME:SCANCODE,..." presses PC keys for mods in scripted runs
			const char* p = next;
			while (*p && nModTestKeys < 64) {
				modTestKeys[nModTestKeys][0] = strtoul(p, (char**) &p, 10);
				modTestKeys[nModTestKeys++][1] = *p == ':' ? strtoul(p + 1, (char**) &p, 10) & 511 : 0;
				if (*p == ',') {
					++p;
				}
			}
			++i;
		}
		else if (!strcmp(a, "--headless") && next) { headless = atoi(next); ++i; }
		else if (!strcmp(a, "--autoplay")) { autoplay = 1; }
		else if (!strcmp(a, "--load-state") && next) { loadStatePath = next; ++i; }
		else if (!strcmp(a, "--fresh")) { fresh = 1; }
		else if (!strcmp(a, "--obj-only")) { objOnly = 1; }
		else if (!strcmp(a, "--input") && next) { inputScript = next; ++i; }
		else if (!strcmp(a, "--save-state") && next) { saveStatePath = next; ++i; }
		else if (!strcmp(a, "--autoplay-seed") && next) { autoSeed = atoi(next); ++i; }
		else if (!strcmp(a, "--hash") && next) { hashPath = next; ++i; }
		else if (!strcmp(a, "--shot") && next) { shotPath = next; ++i; }
		else if (!strcmp(a, "--rom") && next) { romPathArg = next; ++i; }
		else if (!strcmp(a, "--profile") && next) { profilePath = next; ++i; }
		else if (!strcmp(a, "--dump-iwram") && next) { ramDumpPath = next; ++i; }
		else if (!strcmp(a, "--stats")) { stats = 1; }
		else if (!strcmp(a, "--verbose")) { logVerbose = 1; }
		else { _usage(); return strcmp(a, "--help") ? 1 : 0; }
	}
	if (scale < 1) {
		scale = 1;
	}

	mLogSetDefaultLogger(&logger);

	size_t romSize;
	rcRomImage = rcLoadRom(romPathArg, headless, &romSize);
	if (!rcRomImage) {
		return 1;
	}
	_setupRegions(romSize);
	if (!interp) {
		rcRegisterAll(_register);
		recompEnabled = 1;
		modCallTrapOrig = recompRegions[RC_ROM].table[0][0];
		recompRegions[RC_ROM].table[0][0] = _callReturnTrap;
		recompRegions[RC_ROM].table[1][0] = _callReturnTrap;
		modCallReturned = 1;
	}
	if (profilePath || stats) {
		recompMiss = _miss;
	}

	struct mCore* core = GBACoreCreate();
	if (!core || !core->init(core)) {
		fputs("core init failed\n", stderr);
		return 1;
	}
	mCoreInitConfig(core, NULL);
	static color_t video[W * H];
	core->setVideoBuffer(core, video, W);
	core->setAudioBufferSize(core, 2048);

	modCore = core;
	recoverEnabled = !headless || getenv("RC_RECOVER") != NULL; // (RC_RECOVER=1: a headless run recovers too, to test it)
	rcCoreForDebug = core;
	modVideo = video;
	if (interp) {
		noMods = 1;
	}

	if (!core->loadROM(core, VFileFromMemory(rcRomImage, romSize))) {
		fputs("the core rejected the ROM\n", stderr);
		return 1;
	}

	// Battery save. Test runs get a throwaway in-memory save so they are repeatable.
	char saveBuf[1024];
	if (headless && !savePath) {
		core->loadSave(core, VFileMemChunk(NULL, 0));
		modStateDir[0] = 0; // and no snapshots, states or diary dropped into the current folder
	} else {
		if (!savePath) {
			const char* xdg = getenv("XDG_DATA_HOME");
			const char* home = getenv("HOME");
			if (xdg && *xdg) {
				snprintf(saveBuf, sizeof(saveBuf), "%s/" SAVE_ID "/" SAVE_ID ".sav", xdg);
			} else {
				snprintf(saveBuf, sizeof(saveBuf), "%s/.local/share/" SAVE_ID "/" SAVE_ID ".sav", home ? home : ".");
			}
			_mkdirs(saveBuf);
			savePath = saveBuf;
		}
		snprintf(modStateDir, sizeof(modStateDir), "%s", savePath);
		char* slash = strrchr(modStateDir, '/');
		if (slash) {
			*slash = '\0';
		} else {
			strcpy(modStateDir, ".");
		}
		struct VFile* sav = VFileOpen(savePath, O_CREAT | O_RDWR);
		if (!sav || !core->loadSave(core, sav)) {
			fprintf(stderr, "warning: cannot open save file %s: %s\n", savePath, strerror(errno));
		}
	}

	// Mods start here, once the save folder is known, so they can read their settings
	if (!noMods) {
		unsigned m;
		for (m = 0; m < nModInits; ++m) {
			modInitName = modInits[m].name;
			modInits[m].fn();
			fprintf(stderr, "mod: %s\n", modInits[m].name);
		}
	}

	blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SAMPLE_RATE);
	blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SAMPLE_RATE);
	core->reset(core);
	_applySaveMemory(core);

	struct GBA* gba = core->board;
	struct ARMCore* cpu = core->cpu;
	if (!noMods) {
		_installWatches(cpu);
	}
	if (objOnly) {
		int layer;
		for (layer = 0; layer < 4; ++layer) {
			core->enableVideoLayer(core, layer, false);
		}
	}
	// Save anytime: quitting writes a snapshot, starting resumes from it
	char resumePath[1100];
	snprintf(resumePath, sizeof(resumePath), "%s/" SAVE_ID ".resume", modStateDir);
	int resumed = 0;
	if (!headless && !fresh && !loadStatePath && !interp) {
		FILE* f = fopen(resumePath, "rb");
		if (f) {
			size_t size = core->stateSize(core);
			void* buf = malloc(size);
			resumed = fread(buf, 1, size, f) == size && core->loadState(core, buf);
			fclose(f);
			free(buf);
			if (!resumed) {
				core->reset(core);
			}
			_applySaveMemory(core);
		}
	}
	if (loadStatePath) {
		FILE* f = fopen(loadStatePath, "rb");
		size_t size = core->stateSize(core);
		void* buf = malloc(size);
		if (!f || fread(buf, 1, size, f) != size || !core->loadState(core, buf)) {
			fprintf(stderr, "cannot load state %s\n", loadStatePath);
			return 1;
		}
		_applySaveMemory(core);
		fclose(f);
		free(buf);
	}
	static int16_t audio[8192 * 2];
	FILE* hashFile = hashPath ? fopen(hashPath, "w") : NULL;
	unsigned frame = 0;

	if (headless) {
		Uint64 t0 = SDL_GetPerformanceCounter();
		unsigned played = 0; // frames the game ran (a mod's frame gate may hold some): the --input script and the frame count are these
		unsigned maxLoops = (unsigned) headless * 20 + 6000; // a mod holding the game forever must not hold the test forever
		int paceFps = getenv("RC_HEADLESS_FPS") ? atoi(getenv("RC_HEADLESS_FPS")) : 0; // RC_HEADLESS_FPS=60: run at that pace (a test with timing between instances) instead of flat out
		Uint32 paceStart = SDL_GetTicks();
		for (frame = 0; played < (unsigned) headless && frame < maxLoops; ++frame) {
			if (paceFps > 0) {
				Uint32 due = paceStart + (Uint32) ((uint64_t) frame * 1000 / (unsigned) paceFps), now = SDL_GetTicks();
				if (now < due) {
					SDL_Delay(due - now);
				}
			}
			memcpy(modPcKeys[1], modPcKeys[0], 512);
			if (_runGameFrame(core, _autoKeys(getenv("RC_INPUT_BY_LOOP") ? frame : played), !noMods)) { // (RC_INPUT_BY_LOOP=1: a test of a mod's own screens, which hold the game)
				++played;
			} else {
				SDL_Delay(1); // held by a mod (waiting for the other side of a network match): don't spin
			}
			if (!noMods) {
				_modsFrame();
			}
			if (recoverPending) {
				_recover();
			}
			int n = blip_samples_avail(core->getAudioChannel(core, 0));
			if (n > 8192) {
				n = 8192;
			}
			blip_read_samples(core->getAudioChannel(core, 0), audio, n, 1);
			blip_read_samples(core->getAudioChannel(core, 1), audio + 1, n, 1);
			if (hashFile) {
				uint64_t h = fnv(0xCBF29CE484222325ULL, video, sizeof(video));
				h = fnv(h, audio, n * 4);
				h = fnv(h, cpu->gprs, sizeof(cpu->gprs));
				h = fnv(h, &cpu->cpsr, sizeof(cpu->cpsr));
				h = fnv(h, &cpu->cycles, sizeof(cpu->cycles));
				h = fnv(h, gba->memory.wram, SIZE_WORKING_RAM);
				h = fnv(h, gba->memory.iwram, SIZE_WORKING_IRAM);
				h = fnv(h, gba->memory.io, sizeof(gba->memory.io));
				fprintf(hashFile, "%u %016llx pc=%08x\n", frame, (unsigned long long) h, cpu->gprs[15]);
			}
			if (getenv("RC_AI_BENCH")) { // test aid: run the HD mode at real-time pace, as the window would
				optSmooth = PIC_HD;
				optFx = FX_NONE;
				_composePicture(video);
				SDL_Delay(15);
			}
			if (shotPath && strstr(shotPath, "%d") && frame % 300 == 299) {
				char name[1024];
				snprintf(name, sizeof(name), shotPath, frame + 1);
				_writePNG(name, video);
			}
		}
		double secs = (double) (SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
		fprintf(stderr, "%u frames in %.2fs = %.0f fps (%.1fx real time)\n", frame, secs, frame / secs, frame / secs / GBA_FPS);
		if (shotPath && !strstr(shotPath, "%d")) {
			_writePNG(shotPath, video);
		}
		if (saveStatePath) {
			size_t size = core->stateSize(core);
			void* buf = malloc(size);
			FILE* f = fopen(saveStatePath, "wb");
			if (f && core->saveState(core, buf)) {
				fwrite(buf, 1, size, f);
			}
			if (f) {
				fclose(f);
			}
			free(buf);
		}
	} else {
		if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
			fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
			return 1;
		}
		SDL_Window* win = SDL_CreateWindow(GAME_TITLE,
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * scale, H * scale,
			SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
		SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
		if (!ren && win) {
			ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
		}
		if (!ren) {
			fprintf(stderr, "cannot create window: %s\n", SDL_GetError());
			return 1;
		}
		SDL_RenderSetLogicalSize(ren, W * UPSCALE, H * UPSCALE);
		SDL_RenderSetIntegerScale(ren, SDL_FALSE);
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
		SDL_Texture* texBig = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W * UPSCALE, H * UPSCALE);
		char settingsPath[1100];
		snprintf(settingsPath, sizeof(settingsPath), "%s/" SAVE_ID ".settings", modStateDir);
		FILE* sf = fopen(settingsPath, "r");
		if (sf) {
			char line[8] = "";
			if (fgets(line, sizeof(line), sf) && strlen(line) >= 3) {
				optSmooth = (line[0] - '0') % PIC_COUNT;
				optFx = (line[1] - '0') % FX_COUNT;
				optColor = line[2] != '0';
			}
			fclose(sf);
		}
		unsigned filterNote = 0;
		const char* filterText = "";

		SDL_AudioDeviceID dev = 0;
		if (!mute) {
			SDL_AudioSpec want = { 0 }, have;
			want.freq = SAMPLE_RATE;
			want.format = AUDIO_S16SYS;
			want.channels = 2;
			want.samples = 1024;
			dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
			if (dev) {
				SDL_PauseAudioDevice(dev, 0);
			} else {
				fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
			}
		}

		SDL_GameController* pad = NULL;
		int running = 1;
		unsigned resumeNote = resumed ? 240 : 0;
		double freq = (double) SDL_GetPerformanceFrequency();
		double nextFrame = SDL_GetPerformanceCounter() / freq;
		while (running) {
			SDL_Event ev;
			while (SDL_PollEvent(&ev)) {
				if (ev.type == SDL_QUIT) {
					running = 0;
				} else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
					if (ev.key.keysym.sym == SDLK_ESCAPE) {
						running = 0;
					} else if (ev.key.keysym.sym == SDLK_F2 || ev.key.keysym.sym == SDLK_F3 || ev.key.keysym.sym == SDLK_F4) {
						static const char* const fxName[FX_COUNT] = { "SCREEN EFFECT: OFF", "SCREEN EFFECT: SCANLINES", "SCREEN EFFECT: LCD GRID" };
						if (ev.key.keysym.sym == SDLK_F2) {
							static const char* const picName[PIC_COUNT] = { "PICTURE: ORIGINAL PIXELS", "PICTURE: SMOOTH", "PICTURE: HD REDRAW" };
							optSmooth = (optSmooth + 1) % PIC_COUNT;
							if (optSmooth == PIC_HD && aiState < 0) {
								optSmooth = PIC_ORIGINAL; // no usable GPU
							}
							filterText = picName[optSmooth];
						} else if (ev.key.keysym.sym == SDLK_F3) {
							optFx = (optFx + 1) % FX_COUNT;
							filterText = fxName[optFx];
						} else {
							optColor = !optColor;
							filterText = optColor ? "COLOURS: CORRECTED" : "COLOURS: ORIGINAL";
						}
						filterNote = 120;
						sf = fopen(settingsPath, "w");
						if (sf) {
							fprintf(sf, "%d%d%d\n", optSmooth, optFx, optColor);
							fclose(sf);
						}
					} else if (ev.key.keysym.sym == SDLK_F11) {
						fullscreen = !fullscreen;
						SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
					}
				} else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
					pad = SDL_GameControllerOpen(ev.cdevice.which);
				} else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad &&
				           ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
					SDL_GameControllerClose(pad);
					pad = NULL;
				}
			}
			// Space: skip scenes (fast forward and tap A to advance the text)
			int skip = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE] && !modIsPaused;
			int fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB] || skip;
			memcpy(modPcKeys[1], modPcKeys[0], 512);
			memcpy(modPcKeys[0], SDL_GetKeyboardState(NULL), 512);
			uint32_t held = autoplay ? _autoKeys(frame) : (_keyboardKeys() | _padKeys(pad));
			if (skip && (frame & 4)) {
				held |= 1 << GBA_KEY_A;
			}
			int ran = _runGameFrame(core, held, !noMods);
			++frame;
			if (ran && !interp) {
				if (recoverPending) {
		_recover();
	}
	_autoSnapshot();
			}
			if (!noMods) {
				_modsFrame();
			}
			if (resumeNote) {
				--resumeNote;
				modTextScale(2);
				modText(8, 30, 0x40FF40, "RESUMED WHERE YOU QUIT");
				modTextScale(1);
			}

			int n = ran ? blip_samples_avail(core->getAudioChannel(core, 0)) : 0;
			if (n > 8192) {
				n = 8192;
			}
			blip_read_samples(core->getAudioChannel(core, 0), audio, n, 1);
			blip_read_samples(core->getAudioChannel(core, 1), audio + 1, n, 1);
			if (dev && !fast) {
				// Keep at most ~100 ms queued so sound never drifts behind the picture
				if (SDL_GetQueuedAudioSize(dev) > SAMPLE_RATE * 4 / 10) {
					SDL_ClearQueuedAudio(dev);
				}
				SDL_QueueAudio(dev, audio, n * 4);
			}

			if (filterNote) {
				--filterNote;
				modTextScale(2);
				modText(8, 44, 0x40FF40, "%s", filterText);
				modTextScale(1);
			}
			if (!fast || frame % 8 == 0) {
				SDL_RenderClear(ren);
				_composePicture(video);
				SDL_UpdateTexture(texBig, NULL, bigFrame, W * UPSCALE * sizeof(uint32_t));
				SDL_RenderCopy(ren, texBig, NULL, NULL);
				SDL_RenderPresent(ren);
			}

			double now = SDL_GetPerformanceCounter() / freq;
			if (fast) {
				nextFrame = now;
			} else {
				nextFrame += 1.0 / GBA_FPS;
				if (nextFrame < now - 0.1) {
					nextFrame = now;   // fell far behind (window dragged, etc): don't sprint to catch up
				}
				while (now < nextFrame) {
					double left = nextFrame - now;
					if (left > 0.002) {
						SDL_Delay((Uint32) ((left - 0.001) * 1000));
					}
					now = SDL_GetPerformanceCounter() / freq;
				}
			}
		}
		if (!interp) {
			size_t size = core->stateSize(core);
			void* buf = malloc(size);
			FILE* f = fopen(resumePath, "wb");
			_stateWriting();
			if (f && core->saveState(core, buf)) {
				fwrite(buf, 1, size, f);
			}
			if (f) {
				fclose(f);
			}
			free(buf);
		}
		if (dev) {
			SDL_CloseAudioDevice(dev);
		}
		SDL_Quit();
	}

	if (hashFile) {
		fclose(hashFile);
	}
	if (stats) {
		fprintf(stderr, "blocks compiled in: %u\n", nBlocks);
		fprintf(stderr, "native block runs: %llu, interpreted instructions: %llu\n",
		        (unsigned long long) recompBlocksRun, (unsigned long long) recompInterpreted);
		int r;
		for (r = 0; r <= RC_REGIONS; ++r) {
			fprintf(stderr, "  interpreter entries in %-5s: %10llu  (%llu at addresses with no compiled block)\n", regionName[r],
			        (unsigned long long) interpCount[r], (unsigned long long) interpUncovered[r]);
		}
	}
	if (ramDumpPath) {
		FILE* f = fopen(ramDumpPath, "wb");
		if (f) {
			fwrite(gba->memory.iwram, 1, SIZE_WORKING_IRAM, f);
			fclose(f);
		}
	}
	if (profilePath) {
		_writeProfile(profilePath);
	}
	rcAiShutdown();
	core->deinit(core);
	return 0;
}
