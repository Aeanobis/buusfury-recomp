// The game is not in the executable: the player's own ROM is read at start.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Finds the ROM (explicitPath, $RC_ROM, <data dir>/<save id>/<save id>.gba,
// <program dir>/<game id>.gba, ./<game id>.gba), checks it is the dump this
// build was made from (ROM_SHA1), applies the build's patches and pads it as
// the build did for the recompiler. On failure it says why (stderr, and a
// message box unless quiet) and returns NULL. The buffer lives for the whole run.
uint8_t* rcLoadRom(const char* explicitPath, int quiet, size_t* sizeOut);
