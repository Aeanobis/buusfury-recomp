#!/usr/bin/env python3
"""Converts a Legacy of Goku II sprite definition into Buu's Fury's layout, for
use in that game (the reverse of convert_bf.py).

convert_lg2.py NAME LG2_DEF BF_TEMPLATE_DEF ROM_OFFSET "bfanim:lg2anim,..." OUT.inc

The template (a Buu's Fury sprite of the same kind of character) gives the
header: collision boxes and how many animations there are. The mapping says
which Legacy of Goku II animation plays for each Buu's Fury animation number;
one not listed, or missing in the source, plays the standing one. Buu's Fury
animations carry their frame count, so each gets as many frames as the source
animation has. Graphics are stored raw (type 0), their colours mapped to
Buu's Fury's sprite palette. Needs both games built and their sprites
extracted (see extract.c): build/sprites/lg2."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bf_defs  # noqa: E402
import lg2_defs  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
HEADER = 0x54  # the definition's fixed part, before the animation pointers


def palette(path):
    raw = open(path, 'rb').read()
    return [((c & 31), ((c >> 5) & 31), ((c >> 10) & 31)) for c in (struct.unpack_from('<H', raw, i * 2)[0] for i in range(256))]


def colour_table(src, dst):
    table = [0] * 256
    for i in range(1, 256):
        r, g, b = src[i]
        table[i] = min(range(1, 256), key=lambda j: 3 * (dst[j][0] - r) ** 2 + 4 * (dst[j][1] - g) ** 2 + 2 * (dst[j][2] - b) ** 2)
    return bytes(table)


def main():
    name, lg2_number, template, offset, mapping, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5], sys.argv[6]
    lrom = open(os.path.join(ROOT, 'build/lgoku2/rom.gba'), 'rb').read()
    brom = open(os.path.join(ROOT, 'build/buusfury/rom.gba'), 'rb').read()
    ldef = lg2_defs.parse(lrom)[lg2_number]
    bdefs = bf_defs.parse(brom)
    tdef = bdefs[template + 1]  # bf_defs numbers from the table's first word; the game's numbers start one later
    table = colour_table(palette(os.path.join(ROOT, 'sprites/palettes/lgoku2-obj.bin')),
                         palette(os.path.join(ROOT, 'sprites/palettes/buusfury-obj.bin')))
    amap = {int(b): int(a) for b, a in (pair.split(':') for pair in mapping.split(','))}
    theader = brom[tdef['addr'] - 0x08000000:tdef['addr'] - 0x08000000 + HEADER]
    count, dirs = theader[0], theader[3]
    assert dirs == 4, 'the template has %d directions' % dirs
    base = 0x08000000 + offset
    header = bytearray(theader)
    anim_ptrs = bytearray(4 * count)
    body = bytearray()      # animation records and frame records
    gfx = bytearray()       # graphics blocks
    gfx_at = {}
    frame_at = {}
    plan = []

    def add_frame(entry):
        nonlocal gfx
        if entry in frame_at:
            return frame_at[entry]
        x, y, w, h, a0, a1, g = entry
        if g not in gfx_at:
            data = open(os.path.join(ROOT, 'build/sprites/lg2/%08X.bin' % g), 'rb').read()[:w * h]
            gfx_at[g] = len(gfx)
            gfx += struct.pack('<II', 0, w * h) + data.translate(table)
            gfx += b'\0' * (-len(gfx) % 4)
        at = len(body)
        body.extend(struct.pack('<IbbBB', 1, x, y, w, h))
        body.extend(struct.pack('<bbBBHHI', x, y, w, h, a0, a1, 0xFFFFFFFF))  # graphics pointer patched below
        frame_at[entry] = (at, gfx_at[g])
        return frame_at[entry]

    for b in range(count):
        a = amap.get(b, 0)
        if a >= len(ldef['anims']) or not any(ldef['anims'][a]):
            a = 0
        per_dir = ldef['anims'][a]
        n = max(len(per_dir[k]) for k in range(4))
        if n == 0:
            per_dir = ldef['anims'][0]
            n = max(len(per_dir[k]) for k in range(4))
        plan.append((b, a, n))
        rec_at = len(body)
        body.extend(struct.pack('<I', n))
        body.extend(b'\0' * (4 * n * 4))
        slots = []
        for f in range(n):
            for k in range(4):
                frames = per_dir[k] or per_dir[0]
                slots.append(add_frame(frames[min(f, len(frames) - 1)]))
        for i, (fat, gat) in enumerate(slots):
            struct.pack_into('<I', body, rec_at + 4 + 4 * i, 0)  # placeholder; patched once sizes are known
        anim_ptrs[4 * b:4 * b + 4] = struct.pack('<I', 0)
        # remember where to patch
        plan[-1] = (b, a, n, rec_at, slots)

    # lay out: header, animation pointers, body, graphics
    body_base = base + HEADER + len(anim_ptrs)
    gfx_base = body_base + len(body)
    gfx_base += -gfx_base % 4
    for (b, a, n, rec_at, slots) in plan:
        struct.pack_into('<I', anim_ptrs, 4 * b, body_base + rec_at)
        for i, (fat, gat) in enumerate(slots):
            struct.pack_into('<I', body, rec_at + 4 + 4 * i, body_base + fat)
    for entry, (fat, gat) in frame_at.items():
        struct.pack_into('<I', body, fat + 8 + 8, gfx_base + gat)
    blob = bytes(header) + bytes(anim_ptrs) + bytes(body)
    blob += b'\0' * (gfx_base - base - len(blob)) + bytes(gfx)
    with open(out, 'w') as f:
        f.write('// Generated by tools/sprites/convert_lg2.py from Legacy of Goku II sprite definition %d. Do not edit.\n' % lg2_number)
        f.write("// Buu's Fury animation <- Legacy of Goku II animation (frames):\n")
        for (b, a, n, rec_at, slots) in plan:
            f.write('//   %2d <- %2d (%d)\n' % (b, a, n))
        f.write('#include <stdint.h>\n\nconst uint32_t %sRomOffset = 0x%X;\nconst uint32_t %sSize = %d;\nconst uint8_t %sData[] = {' % (name, offset, name, len(blob), name))
        for i, c in enumerate(blob):
            f.write(('\n\t' if i % 20 == 0 else ' ') + '0x%02X,' % c)
        f.write('\n};\n')
    print('%s: %d bytes for ROM offset 0x%X-0x%X (%d frames, %d graphics blocks)' % (name, len(blob), offset, offset + len(blob), len(frame_at), len(gfx_at)))


if __name__ == '__main__':
    main()
