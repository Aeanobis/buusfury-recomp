// Saving without the wait. The game keeps its save on an 8 KB EEPROM, written
// 8 bytes at a time: after each write (DMA3 to 0x0D000000, the routine at
// 0x0804092C) it polls the chip's ready bit while counting scanlines, up to
// 0x88 of them, before the next. The emulated chip is ready the moment the
// DMA ends, and the 1024 writes of a save took 136 frames (2.3 s) of polling
// all the same. The poll loop's head is hooked: the routine's own counters on
// the stack are set as if the wait had passed and the chip had answered, so
// it leaves by its own exit with its own success.
#include "bf.h"

#define EEPROM_WRITE_POLL 0x080409E8 // the head of the ready-bit loop in the write routine; sp + 0xA4: polls answered, sp + 0xAC: scanlines elapsed

static int onPoll(uint32_t* r) {
	modWrite16(r[13] + 0xA4, 1);
	modWrite32(r[13] + 0xAC, 0x89);
	return MOD_CONTINUE;
}

MOD_INIT(fastsave) {
	modHook(EEPROM_WRITE_POLL, onPoll);
}
