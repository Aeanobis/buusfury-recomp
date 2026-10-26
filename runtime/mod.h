// Mod API. Every .c file in games/<game>/mods/ is compiled into the game.
//
//   #include "mod.h"
//   static int onHeal(uint32_t* r) { r[1] *= 2; return MOD_CONTINUE; }
//   MOD_INIT(my_mod) {
//       modHook(0x08012345, onHeal);       // run onHeal whenever the game calls this function
//       modRom(0x02A86D)[3] = 9;           // edit game data before the game starts
//   }
//
// Addresses are GBA addresses: ROM is 0x08000000 + file offset, work RAM is
// 0x02000000-0x0203FFFF and 0x03000000-0x03007FFF.
#ifndef MOD_H
#define MOD_H

#include <stdint.h>
#include <stddef.h>

// Runs once at startup, before the game boots.
#define MOD_INIT(NAME) \
	static void modInit_##NAME(void); \
	static void __attribute__((constructor)) modRegister_##NAME(void) { modAddInit(#NAME, modInit_##NAME); } \
	static void modInit_##NAME(void)
void modAddInit(const char* name, void (*fn)(void));

// --- hooking game code -------------------------------------------------------
// The hook runs when the game is about to execute the instruction at `addr`,
// normally the first instruction of a function. `r` is the CPU's registers:
// r[0]-r[3] are the function's arguments, r[13] the stack pointer, r[14] the
// return address. They can be changed.
#define MOD_CONTINUE 0 // then run the game's own code as usual
#define MOD_RETURN 1   // skip the function: return to its caller, r[0] is the result
typedef int (*ModHookFn)(uint32_t* r);
void modHook(uint32_t addr, ModHookFn fn);

// --- calling game code -------------------------------------------------------------
// Runs one of the game's own functions with up to four arguments and returns
// its result. Use it from an on-frame callback, not from inside a hook. The
// function runs on the game's stack exactly as if the game had called it.
// modCanCall() is false at moments when that is not safe (the game is inside
// an interrupt); try again next frame.
int modCanCall(void);
uint32_t modCall(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

// The CPU's registers r0-r15 as they stand between frames. The game is
// normally parked inside a "wait for next frame" call at that point, so the
// callee-saved registers (r4-r11) hold whatever loop it is in the middle of.
uint32_t* modRegisters(void);

// --- game memory -------------------------------------------------------------
uint8_t modRead8(uint32_t addr);
uint16_t modRead16(uint32_t addr);
uint32_t modRead32(uint32_t addr);
void modWrite8(uint32_t addr, uint8_t value);
void modWrite16(uint32_t addr, uint16_t value);
void modWrite32(uint32_t addr, uint32_t value);
// Direct pointer to ROM data by file offset: the memory the CPU reads, so a
// change is seen at once. Changing data (tables, text, graphics) works;
// changing machine code does not, it was compiled ahead of time.
uint8_t* modRom(uint32_t offset);
// Direct pointer into work RAM (0x02000000-0x0203FFFF or 0x03000000-0x03007FFF),
// for overlaying a struct on game data. NULL for any other address.
void* modRam(uint32_t addr);

// --- a diary -----------------------------------------------------------------------
// Appends a line to <save folder>/<id>.modlog (emptied when the game starts):
// for working out afterwards what a mod did during play.
void modLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// --- settings ---------------------------------------------------------------------
// Small numbers a mod wants remembered between runs (kept in a text file next to
// the save). Not tied to save files or save states.
int modGetSetting(const char* name, int fallback);
const char* modSaveDir(void); // the folder the save, the settings and the log live in
void modSetSetting(const char* name, int value);

// --- finding things ------------------------------------------------------------
// Calls fn whenever the game writes to [addr, addr+size). `pc` is the address of
// the instruction doing the write, which is how you find the code responsible.
typedef void (*ModWatchFn)(uint32_t addr, uint32_t value, int bytes, uint32_t pc, const uint32_t* r);
void modWatchWrites(uint32_t addr, uint32_t size, ModWatchFn fn);

// --- once per frame ----------------------------------------------------------
// Called after the game has drawn a frame, before it is shown.
void modOnFrame(void (*fn)(void));
// Called just before any save state is written (F5, the resume on exit, the
// rolling snapshots) and just after one is loaded (F8): a mod that keeps
// something of its own in the game's RAM can take it out, and put it back.
void modOnStateWrite(void (*fn)(void));
void modAutoSnapshots(int on);        // the rolling crash snapshots every five seconds: off while a mod has the machine in a state not worth keeping (an online match)
void modOnStateRead(void (*fn)(void));
unsigned modFrame(void);       // frames since boot
extern unsigned modRecoveredFrame; // the frame the game last recovered from a crash by going back to a snapshot (0: never)
uint32_t modKeys(void);        // GBA buttons held: bit 0 A, 1 B, 2 Select, 3 Start, 4 Right, 5 Left, 6 Up, 7 Down, 8 R, 9 L
// Change which GBA buttons the game sees this frame (modKeys() stays unfiltered).
void modFilterKeys(uint32_t (*fn)(uint32_t keys));
int modKeyDown(int scancode);  // PC keyboard, SDL scancode (e.g. SDL_SCANCODE_F1 = 58)
int modKeyPressed(int scancode); // true only on the frame the key went down

// --- pausing ---------------------------------------------------------------------
// While paused the game does not run, but on-frame callbacks still do, with the
// last game frame restored underneath each time. This is how a mod shows its own
// menu. The game never sees buttons pressed while paused; modKeys() still does.
void modSetPaused(int paused);
int modPaused(void);

// --- save states ---------------------------------------------------------------
// Snapshot of the whole machine, kept in a file next to the battery save.
// Call these from an on-frame callback. Both return 1 on success.
// True for one frame, shortly after the game finishes writing its own battery
// save (the in-game save point).
int modGameSaved(void);
int modSaveState(int slot);
int modLoadState(int slot);
// The same snapshots in memory: buf holds modStateSize() bytes. 1 on success.
size_t modStateSize(void);
int modStateSave(void* buf);
int modStateLoad(const void* buf);

// --- holding the game back (lockstep netplay) ---------------------------------
// fn is called just before each game frame with the buttons the game is about
// to see (it may change them). Returning 0 holds the game this frame: the game
// does not run, the picture stays, and on-frame callbacks still run (so the mod
// can draw "waiting" and read its socket). The held frames are not counted by
// modGameFrame(), which counts only frames the game actually ran.
void modSetFrameGate(int (*fn)(uint32_t* keys));
unsigned modGameFrame(void);

// --- drawing on top of the game ------------------------------------------------
// The frame is 240x160; colours are 0xBBGGRR.
uint32_t* modScreen(void);
void modRect(int x, int y, int w, int h, uint32_t color);
void modText(int x, int y, uint32_t color, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void modTextScale(int scale);  // 1 = 4x6 pixel cells (default), 2 = 8x12
void modTextPlain(int on);     // 1 = no black box behind the text, just a dark edge (for drawing on a backdrop)
uint32_t modRandom(uint32_t n); // 0 .. n-1

#endif
