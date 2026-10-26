#!/usr/bin/env python3
"""Reads Legacy of Goku II's sprite definitions straight from the ROM.

parse(rom) -> {def number: {'dirs', 'anims': [[frames per direction]]}}
A frame is (x, y, w, h, oam0, oam1, graphics address).
"""
import struct
import sys

TABLE = 0x692E4C
COUNT = 157
SIZES = {8, 16, 32, 64}


def r32(d, o):
    return struct.unpack_from('<I', d, o)[0] if 0 <= o <= len(d) - 4 else 0


def in_rom(d, a):
    return 0x08000000 <= a < 0x08000000 + len(d)


def frames(d, addr, stop):
    out = []
    o = addr - 0x08000000
    while o + 12 <= len(d) and (stop is None or o + 0x08000000 < stop) and len(out) < 64:
        x, y, w, h, a0, a1, gfx = struct.unpack_from('<bbBBHHI', d, o)
        if w not in SIZES or h not in SIZES or not in_rom(d, gfx):
            break
        kind, size = struct.unpack_from('<II', d, gfx - 0x08000000)
        if kind > 2 or size != w * h:
            break
        out.append((x, y, w, h, a0, a1, gfx))
        o += 12
    return out


def parse(d):
    defs = {}
    for n in range(COUNT):
        p = r32(d, TABLE + 4 * n)
        if not in_rom(d, p):
            continue
        o = p - 0x08000000
        if d[o] != 0 or d[o + 3] != 0x10 or r32(d, o + 4) != 8:
            continue   # some other kind of object
        dirs, count = d[o + 1], d[o + 2]
        rows = o + 12 + 16 * dirs
        lists = sorted({r32(d, rows + 4 * i) for i in range(count * dirs)} - {0})
        anims = []
        for a in range(count):
            per_dir = []
            for k in range(dirs):
                ptr = r32(d, rows + 4 * (a * dirs + k))
                if not in_rom(d, ptr):
                    per_dir.append([])
                    continue
                later = [x for x in lists if x > ptr]
                per_dir.append(frames(d, ptr, later[0] if later else None))
            anims.append(per_dir)
        defs[n] = {'dirs': dirs, 'anims': anims, 'addr': p}
    return defs


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    defs = parse(d)
    blocks = {}
    for n, sd in defs.items():
        for per_dir in sd['anims']:
            for fl in per_dir:
                for (x, y, w, h, a0, a1, gfx) in fl:
                    blocks[gfx] = w * h
    print('%d sprite definitions, %d distinct frames' % (len(defs), len(blocks)), file=sys.stderr)
    for gfx in sorted(blocks):
        print('%08X %d' % (gfx, blocks[gfx]))
