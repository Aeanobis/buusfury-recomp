#!/usr/bin/env python3
"""Python port of the games' IWRAM decompressor (LG2 ROM 0x7D6F2C = IWRAM 0x03000000), transcribed
from the ARM disassembly. unpack(rom, offset_of_block) -> bytes. Block = {u32 type, u32 size, data}."""
import struct, sys

class Bits:
    def __init__(self, d, o):
        self.d, self.o, self.buf, self.left = d, o, 0, 0
    def word(self):
        w = struct.unpack_from('<I', self.d, self.o)[0]
        self.o += 4
        return w
    def bit(self):
        self.left -= 1
        if self.left < 0:
            self.buf = self.word()
            self.left = 31
        b = (self.buf >> 31) & 1
        self.buf = (self.buf << 1) & 0xFFFFFFFF
        return b
    def bits(self, n):
        if self.left == 0:
            self.buf = self.word()
            self.left = 32
        take = min(n, self.left)
        v = self.buf >> (32 - take) if take else 0
        self.buf = (self.buf << take) & 0xFFFFFFFF
        self.left -= take
        n -= take
        if n == 0:
            return v
        self.buf = self.word()
        self.left = 32 - n
        hi = self.buf >> self.left
        v = hi + (v << n)
        self.buf = (self.buf << n) & 0xFFFFFFFF
        return v
    def gamma(self):
        g = 1
        while True:
            g = (g << 1) + self.bit()
            if not self.bit():
                return g

def unpack(d, o, K2=None, K1=None):
    kind, size = struct.unpack_from('<II', d, o)
    if kind == 0:
        return bytes(d[o + 8:o + 8 + size])
    s = Bits(d, o + 8)
    out = bytearray()
    lit_bits, delta, low_bits, last = kind, 0, 8, 1
    while len(out) < size:
        if s.bit():
            out.append((s.bits(lit_bits) + delta) & 0xFF)
            continue
        if s.bit():
            g = s.gamma()
            if g == 2:
                n = s.gamma()
            else:
                last = s.bits(low_bits) + ((g - 3) << low_bits)
                n = s.gamma()
                if last >= 0x10000: n += 3
                elif last >= K2: n += 2
                elif last >= K1: n += 1
                elif last <= 127: n += 4
            for _ in range(n):
                out.append(out[-last])
            continue
        if not s.bit():
            v = s.bits(7)
            if v:
                last = v
                n = s.bits(2) + 2
                for _ in range(n):
                    out.append(out[-last])
            else:
                v = s.bits(2)
                if v == 0:
                    break
                low_bits = s.bits(v + 3)
            continue
        v = s.bits(4) - 1
        if v == 0:
            out.append(0)
        elif v > 0:
            out.append(out[-v])
        else:
            if s.bit():
                while True:
                    for _ in range(256):
                        out.append(s.bits(8))
                    if not s.bit():
                        break
            else:
                delta = 0
                lit_bits = 7 + s.bit()
                if lit_bits != 8:
                    delta = s.bits(8)
    return bytes(out[:size])

if __name__ == '__main__':
    path, off = sys.argv[1], int(sys.argv[2], 0)
    d = open(path, 'rb').read()
    out = unpack(d, off, int(sys.argv[3], 0), int(sys.argv[4], 0))
    sys.stdout.buffer.write(out)
