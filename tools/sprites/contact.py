#!/usr/bin/env python3
"""Contact sheet: one thumbnail per sprite definition (first frame, facing down).
contact.py lg2|bf ROM EXTRACT_DIR OUT.png [first] [count]
Thumbnails run left to right, 16 per row, in definition-number order; the
numbers are printed on stdout row by row."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import bf_defs
import lg2_defs
from sheet import load_palette, tiles_to_raster, write_png

CELL, COLS, SCALE = 48, 16, 2


def pieces_of(game, fr):
    return fr if game == 'bf' else [fr]


def draw_frame(img, W, ox, oy, game, fr, exdir, pal):
    """Draws a frame with its origin (the character's feet) at (ox, oy)."""
    for (x, y, w, h, a0, a1, gfx) in pieces_of(game, fr):
        data = open(os.path.join(exdir, '%08X.bin' % gfx), 'rb').read()
        ras = tiles_to_raster(data, w, h)
        hflip = a1 & 0x1000
        for py in range(h):
            for px in range(w):
                idx = ras[py][w - 1 - px if hflip else px]
                if not idx:
                    continue
                sx, sy = ox + x + px, oy + y + py
                if 0 <= sx < W // SCALE and sy >= 0:
                    col = bytes(pal[idx])
                    for k in range(SCALE):
                        o = ((sy * SCALE + k) * W + sx * SCALE) * 3
                        if o + 3 * SCALE <= len(img):
                            img[o:o + 3 * SCALE] = col * SCALE


def main():
    game, rom, exdir, out = sys.argv[1:5]
    first = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0
    count = int(sys.argv[6], 0) if len(sys.argv) > 6 else 10000
    d = open(rom, 'rb').read()
    defs = (bf_defs if game == 'bf' else lg2_defs).parse(d)
    pal = load_palette(os.path.join(exdir, 'objpalette.bin'))
    numbers = [n for n in sorted(defs) if n >= first][:count]
    rows = (len(numbers) + COLS - 1) // COLS
    W, H = COLS * CELL * SCALE, rows * CELL * SCALE
    img = bytearray(b'\x30\x30\x40' * (W * H))
    for i, n in enumerate(numbers):
        fr = next((fl[0] for per_dir in defs[n]['anims'] for fl in per_dir[:1] if fl), None)
        if fr:
            draw_frame(img, W, (i % COLS) * CELL + CELL // 2 - 8, (i // COLS) * CELL + CELL - 10, game, fr, exdir, pal)
    write_png(out, img, W, H)
    for r in range(rows):
        print(' '.join('%3d' % n for n in numbers[r * COLS:(r + 1) * COLS]))


if __name__ == '__main__':
    main()
