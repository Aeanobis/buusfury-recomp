#!/usr/bin/env python3
"""Reads Buu's Fury's sprite definitions straight from the ROM.

parse(rom) -> {def number: {'dirs', 'anims': [[frames per direction]]}} in the
same shape as lg2_defs.parse, so the two games' sprites can be compared and
converted. A frame is a list of pieces (x, y, w, h, oam0, oam1, graphics address).

Layout (worked out from the ROM, names are ours):
  table at 0x6B6BB0: 308 pointers to definitions
  definition: u8 animations, u8 0, u8 0, u8 directions; u32 0; 10 08 0a 04; u32 0;
              4 collision boxes of s32 x, y, w, h; u32; then one pointer per animation
  animation:  u32 frames; then frames x directions pointers to frame records
  frame:      u32 pieces; s8 x, y, u8 w, h (bounding box);
              pieces x { s8 x, y, u8 w, h, u16 oam0, oam1, pointer to graphics }
  graphics:   u32 type, u32 unpacked size, data (same compression as Legacy of Goku II)
"""
import struct
import sys

TABLE = 0x6B6BB0
COUNT = 308
SIZES = {8, 16, 32, 64}


def r32(d, o):
    return struct.unpack_from('<I', d, o)[0] if 0 <= o <= len(d) - 4 else 0


def in_rom(d, a):
    return 0x08000000 <= a < 0x08000000 + len(d)


def frame(d, addr):
    o = addr - 0x08000000
    n = r32(d, o)
    if not 1 <= n <= 8:
        return None
    pieces = []
    for i in range(n):
        x, y, w, h, a0, a1, gfx = struct.unpack_from('<bbBBHHI', d, o + 8 + 12 * i)
        if w not in SIZES or h not in SIZES or not in_rom(d, gfx):
            return None
        kind, size = struct.unpack_from('<II', d, gfx - 0x08000000)
        if kind > 2 or size != w * h:
            return None
        pieces.append((x, y, w, h, a0, a1, gfx))
    return pieces


def parse(d):
    defs = {}
    for n in range(COUNT):
        p = r32(d, TABLE + 4 * n)
        if not in_rom(d, p):
            continue
        o = p - 0x08000000
        # +8..+9 is the sprite's cell size: 10 08 for people, 20 10 / 40 10 for
        # the big bosses (Fat Buu, Super Buu, Broly...), whose header says 16
        # directions but which hold 8 like everyone else
        if d[o + 9] not in (0x08, 0x10, 0x40):
            continue
        count, dirs = d[o], d[o + 3]
        # the big bosses say 16 here: their records hold four facings (down,
        # up, left, right) with four times the frames the count says
        big = dirs == 16
        if big:
            dirs = 4
        if dirs not in (1, 2, 4, 8) or count == 0 or count > 40:
            continue
        anims = []
        for a in range(count):
            ap = r32(d, o + 0x54 + 4 * a)
            per_dir = [[] for _ in range(dirs)]
            if in_rom(d, ap):
                ao = ap - 0x08000000
                nframes = r32(d, ao) * (4 if big else 1)
                if 1 <= nframes <= 256:
                    for f in range(nframes):
                        for k in range(dirs):
                            fp = r32(d, ao + 4 + 4 * (f * dirs + k))
                            fr = frame(d, fp) if in_rom(d, fp) else None
                            if fr:
                                per_dir[k].append(fr)
            anims.append(per_dir)
        defs[n] = {'dirs': dirs, 'anims': anims, 'addr': p}
    return defs


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    defs = parse(d)
    blocks = {}
    for sd in defs.values():
        for per_dir in sd['anims']:
            for fl in per_dir:
                for fr in fl:
                    for (x, y, w, h, a0, a1, gfx) in fr:
                        blocks[gfx] = w * h
    print('%d sprite definitions, %d distinct graphics blocks' % (len(defs), len(blocks)), file=sys.stderr)
    for gfx in sorted(blocks):
        print('%08X %d' % (gfx, blocks[gfx]))
