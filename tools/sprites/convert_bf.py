#!/usr/bin/env python3
"""Converts a Buu's Fury character sprite into a Legacy of Goku II sprite definition.

convert_bf.py NAME BF_DEF LG2_TEMPLATE_DEF ROM_OFFSET "lg2anim:bfanim,..." OUT.c

The result is a block of bytes meant to sit at ROM_OFFSET in the Legacy of
Goku II ROM (its internal pointers are already set for that address), written
as a C array. It holds, in Legacy of Goku II's layout:
  - a sprite definition (header and collision boxes copied from the template
    definition, so the character collides like the one it stands in for)
  - a frame list for every animation and direction
  - the graphics, stored uncompressed and recoloured to Legacy of Goku II's
    fixed sprite palette (nearest colour)

Animation numbers mean different things in the two games, so the mapping says
which Buu's Fury animation to use for each Legacy of Goku II animation number
("9:13"); "9:13+1" skips that animation's first frame, "21:13-" plays it backwards,
"9:10[0.3.8.10]" takes just those frames of it, in that order.
Legacy of Goku II's code asks for specific frame numbers, so each animation is
given at least as many frames as the original characters have, by spreading
the Buu's Fury frames evenly.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import bf_defs
import lg2_defs

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
ANIMATIONS = 23
# Frames the game may ask for per animation: the most any playable character's
# sprite has (Gohan, Piccolo, Vegeta, Trunks, Goku)
PLAYER_DEFS = (53, 55, 56, 57, 58, 59, 103, 104, 137, 138, 139, 140, 128, 129, 130, 132, 133, 134, 60, 61)


def palette(path):
    raw = open(path, 'rb').read()
    out = []
    for i in range(256):
        c = struct.unpack_from('<H', raw, i * 2)[0]
        out.append((c & 31, (c >> 5) & 31, (c >> 10) & 31))
    return out


def colour_table(src, dst):
    """For each source palette entry, the closest destination entry (never 0, which is transparent)."""
    table = [0] * 256
    for i in range(1, 256):
        r, g, b = src[i]
        best = min(range(1, 256), key=lambda j: 3 * (dst[j][0] - r) ** 2 + 4 * (dst[j][1] - g) ** 2 + 2 * (dst[j][2] - b) ** 2)
        table[i] = best
    return bytes(table)


def main():
    name, bf_number, template, offset, mapping, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5], sys.argv[6]
    lrom = open(os.path.join(ROOT, 'build/lgoku2/rom.gba'), 'rb').read()
    brom = open(os.path.join(ROOT, 'build/buusfury/rom.gba'), 'rb').read()
    ldefs = lg2_defs.parse(lrom)
    bdef = bf_defs.parse(brom)[bf_number]
    table = colour_table(palette(os.path.join(ROOT, 'sprites/palettes/buusfury-obj.bin')),
                         palette(os.path.join(ROOT, 'sprites/palettes/lgoku2-obj.bin')))
    amap = {}
    for pair in mapping.split(','):
        a, b = pair.split(':')
        chosen = None
        if '[' in b:  # "10[0.3.8.10]": just these frames of it, in this order
            b, rest = b.split('[')
            chosen = [int(f) for f in rest.rstrip(']').split('.')]
        reverse = b.endswith('-')
        b = b.rstrip('-')
        skip = int(b.split('+')[1]) if '+' in b else 0
        amap[int(a)] = (int(b.split('+')[0]), skip, reverse, chosen)

    need = [1] * ANIMATIONS
    for n in PLAYER_DEFS:
        for a, per_dir in enumerate(ldefs[n]['anims'][:ANIMATIONS]):
            need[a] = max(need[a], max((len(fl) for fl in per_dir), default=0))

    base = 0x08000000 + offset
    tpl = ldefs[template]['addr'] - 0x08000000
    header = bytearray(lrom[tpl:tpl + 0x4C])
    header[2] = ANIMATIONS
    rows_at = 0x4C
    lists_at = rows_at + ANIMATIONS * 16
    rows = bytearray(ANIMATIONS * 16)
    lists = bytearray()
    gfx = bytearray()
    gfx_at = {}      # Buu's Fury graphics address -> offset within gfx
    list_at = {}     # (bf anim, dir, frames) -> offset within lists
    plan = []

    for a in range(ANIMATIONS):
        src, skip, reverse, chosen = amap.get(a, (0, 0, False, None))
        if src >= len(bdef['anims']) or not any(bdef['anims'][src]):
            src, skip, reverse, chosen = 0, 0, False, None
        for k in range(4):
            frames = bdef['anims'][src][k] or bdef['anims'][0][k]
            frames = frames[skip:] or frames
            if chosen and all(f < len(frames) for f in chosen):
                frames = [frames[f] for f in chosen]
            if reverse:
                frames = frames[::-1]
            count = max(need[a], len(frames)) if len(frames) >= need[a] else need[a]
            picks = tuple(min(i * len(frames) // count, len(frames) - 1) for i in range(count))
            key = (src, k, picks)
            if key not in list_at:
                list_at[key] = len(lists)
                for i in picks:
                    (x, y, w, h, a0, a1, g) = frames[i][0]
                    if g not in gfx_at:
                        data = open(os.path.join(ROOT, 'build/sprites/bf/%08X.bin' % g), 'rb').read()[:w * h]
                        gfx_at[g] = len(gfx)
                        gfx += struct.pack('<II', 0, w * h) + data.translate(table)
                        gfx += b'\0' * (-len(gfx) % 4)
                    lists += struct.pack('<bbBBHHI', x, y, w, h, a0, a1, gfx_at[g])  # pointer fixed up below
            struct.pack_into('<I', rows, (a * 4 + k) * 4, list_at[key])
        plan.append((a, src, need[a]))

    gfx_base = lists_at + len(lists)
    for o in range(8, len(lists), 12):
        struct.pack_into('<I', lists, o, base + gfx_base + struct.unpack_from('<I', lists, o)[0])
    for o in range(0, len(rows), 4):
        struct.pack_into('<I', rows, o, base + lists_at + struct.unpack_from('<I', rows, o)[0])
    blob = bytes(header) + bytes(rows) + bytes(lists) + bytes(gfx)

    with open(out, 'w') as f:
        f.write('// Generated by tools/sprites/convert_bf.py from Buu\'s Fury sprite definition %d. Do not edit.\n' % bf_number)
        f.write('// Legacy of Goku II animation <- Buu\'s Fury animation (frames provided):\n')
        for a, src, n in plan:
            f.write('//   %2d <- %2d (%d)\n' % (a, src, n))
        f.write('#include <stdint.h>\n\nconst uint32_t %sRomOffset = 0x%X;\nconst uint32_t %sSize = %d;\nconst uint8_t %sData[] = {' % (name, offset, name, len(blob), name))
        for i, b in enumerate(blob):
            f.write(('\n\t' if i % 20 == 0 else ' ') + '0x%02X,' % b)
        f.write('\n};\n')
    print('%s: %d bytes for ROM offset 0x%X-0x%X (%d graphics blocks)' % (name, len(blob), offset, offset + len(blob), len(gfx_at)))


if __name__ == '__main__':
    main()
