#!/usr/bin/env python3
"""Converts a Buu's Fury boss sprite into a Legacy of Goku II enemy sprite definition.
convert_bf_boss.py NAME BF_DEF LG2_TEMPLATE_DEF ROM_OFFSET "lg2anim:bfanim,..." OUT.inc

Like convert_bf.py, but for the big bosses (Fat Buu, Super Buu, Kid Buu...):
their frames are made of several pieces, which are drawn together into one
piece per frame (Legacy of Goku II's frames have one), and only the thirteen
animations an enemy's AI uses are written (0 idle, 1 walk, 2 flinch, 3 down,
5 fire pose, 6-8 attacks, 12), each sampled down to the frame counts the
game's own fighters have. Unmapped animations show the idle pose.
"""
import os
import struct
import sys
sys.path.insert(0, os.path.dirname(__file__))
import bf_defs
from convert_bf import palette, colour_table
from sheet import tiles_to_raster

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
LG2_TABLE = 0x692E4C
ANIMATIONS = 13
NEED = [2, 4, 4, 4, 4, 3, 4, 4, 4, 4, 4, 4, 4]   # frames per animation: what the game's fighters have (Cell Jr, Frieza)
SHAPES = {(8, 8): (0, 0), (16, 16): (0, 1), (32, 32): (0, 2), (64, 64): (0, 3), (16, 8): (1, 0), (32, 8): (1, 1), (32, 16): (1, 2), (64, 32): (1, 3),
          (8, 16): (2, 0), (8, 32): (2, 1), (16, 32): (2, 2), (32, 64): (2, 3)}

def oam_size(w, h):
    """The smallest sprite shape holding w x h."""
    best = None
    for (sw, sh) in SHAPES:
        if sw >= w and sh >= h and (best is None or sw * sh < best[0] * best[1]):
            best = (sw, sh)
    return best

def composite(frame, exdir):
    """Pieces -> (x, y, w, h, raster rows) with the frame's origin kept."""
    x1 = min(p[0] for p in frame); y1 = min(p[1] for p in frame)
    x2 = max(p[0] + p[2] for p in frame); y2 = max(p[1] + p[3] for p in frame)
    w, h = oam_size(x2 - x1, y2 - y1)
    if w is None:
        raise SystemExit('a frame is %d x %d: too big for one piece' % (x2 - x1, y2 - y1))
    ras = [bytearray(w) for _ in range(h)]
    for (x, y, pw, ph, a0, a1, gfx) in frame:
        data = open(os.path.join(exdir, '%08X.bin' % gfx), 'rb').read()
        src = tiles_to_raster(data, pw, ph)
        hflip = a1 & 0x1000
        vflip = a1 & 0x2000
        for py in range(ph):
            for px in range(pw):
                v = src[ph - 1 - py if vflip else py][pw - 1 - px if hflip else px]
                if v:
                    ras[y - y1 + py][x - x1 + px] = v
    return x1, y1, w, h, ras

def raster_to_tiles(ras, w, h):
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for y in range(8):
                out += ras[ty + y][tx:tx + 8]
    return bytes(out)

def main():
    name, bf_number, template, offset, mapping, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5], sys.argv[6]
    lrom = open(os.path.join(ROOT, 'build/lgoku2/rom.gba'), 'rb').read()
    brom = open(os.path.join(ROOT, 'build/buusfury/rom.gba'), 'rb').read()
    exdir = os.path.join(ROOT, 'build/sprites/bf')
    bdef = bf_defs.parse(brom)[bf_number]
    table = colour_table(palette(os.path.join(ROOT, 'sprites/palettes/buusfury-obj.bin')),
                         palette(os.path.join(ROOT, 'sprites/palettes/lgoku2-obj.bin')))
    amap = {int(a): int(b) for a, b in (pair.split(':') for pair in mapping.split(','))}
    base = 0x08000000 + offset
    tpl = struct.unpack_from('<I', lrom, LG2_TABLE + 4 * template)[0] - 0x08000000
    header = bytearray(lrom[tpl:tpl + 0x4C])
    header[2] = ANIMATIONS
    rows_at = 0x4C
    lists_at = rows_at + ANIMATIONS * 16
    rows = bytearray(ANIMATIONS * 16)
    lists = bytearray()
    gfx = bytearray()
    gfx_at = {}
    list_at = {}
    plan = []
    for a in range(ANIMATIONS):
        src = amap.get(a, 0)
        if src >= len(bdef['anims']) or not any(bdef['anims'][src]):
            src = 0
        for k in range(4):
            frames = bdef['anims'][src][k] or bdef['anims'][0][k]
            count = NEED[a]
            picks = tuple(min(i * len(frames) // count, len(frames) - 1) for i in range(count))
            key = (src, k, picks)
            if key not in list_at:
                list_at[key] = len(lists)
                for i in picks:
                    frame = frames[i]
                    gkey = tuple((p[6], p[0], p[1], p[5] & 0x3000) for p in frame)
                    if gkey not in gfx_at:
                        x, y, w, h, ras = composite(frame, exdir)
                        shape, size = SHAPES[(w, h)]
                        data = raster_to_tiles(ras, w, h).translate(table)
                        gfx_at[gkey] = (len(gfx), x, y, w, h, shape, size)
                        gfx += struct.pack('<II', 0, w * h) + data
                        gfx += b'\0' * (-len(gfx) % 4)
                    g, x, y, w, h, shape, size = gfx_at[gkey]
                    a0 = (frame[0][4] & 0x3FFF) | (shape << 14)
                    a1 = (frame[0][5] & 0x0FFF) | (size << 14)
                    lists += struct.pack('<bbBBHHI', x, y, w, h, a0, a1, g)
            struct.pack_into('<I', rows, (a * 4 + k) * 4, list_at[key])
        plan.append((a, src, NEED[a]))
    gfx_base = lists_at + len(lists)
    for o in range(8, len(lists), 12):
        struct.pack_into('<I', lists, o, base + gfx_base + struct.unpack_from('<I', lists, o)[0])
    for o in range(0, len(rows), 4):
        struct.pack_into('<I', rows, o, base + lists_at + struct.unpack_from('<I', rows, o)[0])
    blob = bytes(header) + bytes(rows) + bytes(lists) + bytes(gfx)
    with open(out, 'w') as f:
        f.write('// Generated by tools/sprites/convert_bf_boss.py from Buu\'s Fury sprite definition %d. Do not edit.\n' % bf_number)
        f.write('// Legacy of Goku II animation <- Buu\'s Fury animation (frames):\n')
        for a, src, n in plan:
            f.write('//   %2d <- %2d (%d)\n' % (a, src, n))
        f.write('#include <stdint.h>\n\nconst uint32_t %sRomOffset = 0x%X;\nconst uint32_t %sSize = %d;\nconst uint8_t %sData[] = {' % (name, offset, name, len(blob), name))
        for i, b in enumerate(blob):
            f.write(('\n\t' if i % 20 == 0 else ' ') + '0x%02X,' % b)
        f.write('\n};\n')
    print('%s: %d bytes for ROM offset 0x%X-0x%X (%d frames)' % (name, len(blob), offset, offset + len(blob), len(gfx_at)))

if __name__ == '__main__':
    main()
