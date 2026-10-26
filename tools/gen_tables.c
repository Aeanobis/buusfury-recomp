// Prints the handler name for every Thumb (opcode>>6) and ARM decode-table slot.
#include <stdio.h>
#include <mgba/internal/arm/emitter-thumb.h>
#include <mgba/internal/arm/emitter-arm.h>
#define S2(...) #__VA_ARGS__
#define S(...) S2(__VA_ARGS__)
int main(void) {
	puts("T:" S(DECLARE_THUMB_EMITTER_BLOCK(_ThumbInstruction)));
	puts("A:" S(DECLARE_ARM_EMITTER_BLOCK(_ARMInstruction)));
	return 0;
}
