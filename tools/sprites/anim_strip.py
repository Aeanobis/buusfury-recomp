#!/usr/bin/env python3
"""Draws chosen animations of a Buu's Fury sprite, one row each (facing down), frames left to right.
anim_strip.py BF_DEF OUT.png ANIM [ANIM ...]     e.g. anim_strip.py 270 /tmp/x.png 9 10 14 21
A row like 21/3 draws that animation facing direction 3 (0 down, 1 up, 2 left, 3 right).
Used to work out which Buu's Fury animation should stand in for which Legacy of Goku II one."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import bf_defs
import contact
from sheet import load_palette, write_png

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
CELL, SCALE = 48, 3


def main():
    number, out = int(sys.argv[1]), sys.argv[2]
    anims = [(int(a.split('/')[0]), int(a.split('/')[1]) if '/' in a else 0) for a in sys.argv[3:]]
    exdir = os.path.join(ROOT, 'build/sprites/bf')
    d = bf_defs.parse(open(os.path.join(ROOT, 'build/buusfury/rom.gba'), 'rb').read())[number]
    pal = load_palette(os.path.join(exdir, 'objpalette.bin'))
    most = max(len(d['anims'][a][k]) for a, k in anims if a < len(d['anims']))
    w, h = most * CELL * SCALE, len(anims) * CELL * SCALE
    old = contact.SCALE
    contact.SCALE = SCALE
    img = bytearray(b'\x30\x30\x40' * (w * h))
    for row, (a, k) in enumerate(anims):
        frames = d['anims'][a][k] if a < len(d['anims']) else []
        for i, fr in enumerate(frames):
            contact.draw_frame(img, w, i * CELL + CELL // 2 - 8, row * CELL + CELL - 8, 'bf', fr, exdir, pal)
    contact.SCALE = old
    write_png(out, img, w, h)


if __name__ == '__main__':
    main()
