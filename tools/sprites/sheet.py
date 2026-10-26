#!/usr/bin/env python3
"""Draws a sprite sheet PNG from extracted frames.
sheet.py ROM EXTRACT_DIR DEF_NUMBER OUT.png [scale]   (Legacy of Goku II layout)
One row per animation and direction; frames left to right."""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
import lg2_defs


def tiles_to_raster(data, w, h):
    """Graphics are stored as 8x8 tiles, row by row; returns rows of palette indices."""
    rows = [bytearray(w) for _ in range(h)]
    t = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for y in range(8):
                rows[ty + y][tx:tx + 8] = data[t * 64 + y * 8:t * 64 + y * 8 + 8]
            t += 1
    return rows


def write_png(path, pixels, w, h):
    raw = b''.join(b'\0' + bytes(pixels[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def load_palette(path):
    raw = open(path, 'rb').read()
    pal = []
    for i in range(256):
        c = struct.unpack_from('<H', raw, i * 2)[0]
        pal.append(((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3))
    return pal


def main():
    """sheet.py lg2|bf ROM EXTRACT_DIR DEF_NUMBER OUT.png [scale]
    One row per animation number (empty rows kept, so row = animation number):
    every frame facing the first direction, then the first frame of each other direction."""
    import contact
    import bf_defs
    game, rom, exdir, number, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0), sys.argv[5]
    scale = int(sys.argv[6]) if len(sys.argv) > 6 else 2
    contact.SCALE = scale
    d = open(rom, 'rb').read()
    sd = (bf_defs if game == 'bf' else lg2_defs).parse(d)[number]
    pal = load_palette(os.path.join(exdir, 'objpalette.bin'))
    cell = 40
    rows = []
    for per_dir in sd['anims']:
        row = list(per_dir[0]) if per_dir and per_dir[0] else []
        row += [None]
        row += [fl[0] for fl in per_dir[1:] if fl]
        rows.append(row)
    cols = max(len(r) for r in rows)
    W, H = cols * cell * scale, len(rows) * cell * scale
    img = bytearray(b'\x30\x30\x40' * (W * H))
    for r, row in enumerate(rows):
        # a tick mark every fifth row to make counting easy
        if r % 5 == 0:
            for k in range(4 * scale):
                o = ((r * cell * scale + k) * W) * 3
                img[o:o + 3 * 6 * scale] = b'\xff\xff\x00' * (6 * scale)
        for c, fr in enumerate(row):
            if fr:
                contact.draw_frame(img, W, c * cell + 12, r * cell + cell - 4, game, fr, exdir, pal)
    write_png(out, img, W, H)
    print('%s definition %d: %d animations, %d directions' % (game, number, len(sd['anims']), sd['dirs']))


if __name__ == '__main__':
    main()
