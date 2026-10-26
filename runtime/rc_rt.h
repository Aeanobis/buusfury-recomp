// Runtime glue for statically recompiled blocks.
//
// A block is a run of guest instructions compiled into one C function. Guest
// CPU state lives in mGBA's ARMCore, and every instruction leaves that state
// exactly as the interpreter would (pc, prefetch, cycle count), so a block can
// stop after any instruction and hand over to the interpreter or to hardware
// event processing.
#ifndef RC_RT_H
#define RC_RT_H

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-inlines.h>

struct RcEntry {
	uint32_t addr;
	uint16_t thumb;
	uint16_t size; // guest code bytes covered by the block
	void (*fn)(struct ARMCore*);
};

void rcRegisterAll(void (*reg)(const struct RcEntry*, unsigned));

#ifndef RC_NO_ISA
// Instruction bodies, taken from mGBA's interpreter and made always-inline so
// that a constant opcode folds the decode away (see Makefile: gen/isa-*-inl.h).
#include "isa-thumb-inl.h"
#include "isa-arm-inl.h"

#define RC_EVENT_CHECK \
	if (UNLIKELY(cpu->cycles >= cpu->nextEvent)) { \
		return; \
	}

#define RC_T_PRE(A, P0, P1) \
	cpu->gprs[ARM_PC] = (A) + 4; \
	cpu->prefetch[0] = (P0); \
	cpu->prefetch[1] = (P1);

// Thumb instruction that cannot change pc
#define TI(A, H, OP, P0, P1) \
	RC_T_PRE(A, P0, P1) \
	H(cpu, OP); \
	RC_EVENT_CHECK

// Thumb conditional branch: leave the block if taken
#define TB(A, H, OP, P0, P1) \
	RC_T_PRE(A, P0, P1) \
	H(cpu, OP); \
	if (cpu->gprs[ARM_PC] != (A) + 4) { \
		return; \
	} \
	RC_EVENT_CHECK

// Thumb instruction that always ends the block
#define TX(A, H, OP, P0, P1) \
	RC_T_PRE(A, P0, P1) \
	H(cpu, OP); \
	return;

#define RC_A_PRE(A, P0, P1) \
	cpu->gprs[ARM_PC] = (A) + 8; \
	cpu->prefetch[0] = (P0); \
	cpu->prefetch[1] = (P1);

#define RC_A_EXEC(H, OP, COND) \
	if ((COND) == 0xE || ((COND) != 0xF && ARMTestCondition(cpu, (COND)))) { \
		H(cpu, OP); \
	} else { \
		cpu->cycles += ARM_PREFETCH_CYCLES; \
	}

#define AI(A, H, OP, COND, P0, P1) \
	RC_A_PRE(A, P0, P1) \
	RC_A_EXEC(H, OP, COND) \
	if (cpu->gprs[ARM_PC] != (A) + 8) { \
		return; \
	} \
	RC_EVENT_CHECK

#define AX(A, H, OP, COND, P0, P1) \
	RC_A_PRE(A, P0, P1) \
	RC_A_EXEC(H, OP, COND) \
	return;

#endif
#endif
