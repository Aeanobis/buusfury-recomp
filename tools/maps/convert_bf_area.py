#!/usr/bin/env python3
"""Carry areas (maps) of Buu's Fury into Legacy of Goku II.

Both games run the same map engine: 56-byte area records, 80-byte map headers,
layer records of compressed 2 KB screen chunks, two 8 KB collision bitmaps, a
tile list and a table of 16 KB tile banks, all compressed by the same routine.
So a map is carried over verbatim, block by block, with its pointers relocated
into free space in the enlarged ROM and the layer and exit class words
rewritten to LoG2's. What is NOT carried: the object/trigger/NPC/enemy lists,
scripts (the area comes out empty but for its save points, which become
LoG2's). Exits are carried, with their target zone shifted, so the areas join
up; so are tile animations and decorations.

Colours: each game keeps one global 256-colour background palette, and they
differ, so the mod (bf_areas.c) swaps the palette while such an area is shown
(the tiles stay as they are: no decoding needed).

    convert_bf_area.py banks  OUT.inc                       the shared tile banks (once)
    convert_bf_area.py area   ZONE AREA ROM_OFFSET OUT.inc  one area, as LoG2 zone 20+ZONE, no exits
    convert_bf_area.py all    ROM_OFFSET OUT.inc            every area that can be, with exits

Run with python3 -I from the project directory."""
import bisect, struct, sys

BF_ROM = 'build/buusfury/rom.gba'
BF_AREAS, BF_AREA_COUNT = 0x8EA18, 419
BF_BANK_TABLE, BF_BANKS = 0x179764, 187
BF_LAYER, BF_LAYER_EMPTY, BF_LAYER_OTHER = 0x08008087, 0x08007A41, 0x0800897B
LG2_LAYER, LG2_LAYER_EMPTY = 0x080056F9, 0x08005F49
# Exit records carry two class words each. Matched by what the exits of each
# class look like in each game (edge strips vs doorways, and the "kind" bytes).
EXIT_CLASSES = { 0x08007675: 0x0801EEE3, 0x080072CD: 0x0801EAAB, 0x0800776D: 0x0801F263 }
BF_ZONES = range(1, 23)
# Save points: a trigger of this class (one per area) in each game; LoG2's
# record is {class, class2, u16 x1, y1, x2, y2}
BF_SAVE_CLASS = 0x08007499
LG2_SAVE_CLASSES = (0x0801ED13, 0x0801F1ED)
BANKS_ROM = 0xC00000            # where the shared banks go: the table, then the banks
ZONE_SHIFT = 20                 # Buu's Fury zone z becomes LoG2 zone 20+z
NO_NAME_TEXT = 221              # the area-name text number that shows no banner
MUSIC = 41                      # an LoG2 music number: Kami's Lookout's (7, Dr Gero's lab, was unnerving)

bf = open(BF_ROM, 'rb').read()

def r32(o): return struct.unpack_from('<I', bf, o)[0]
def r16(o): return struct.unpack_from('<H', bf, o)[0]
def isptr(v): return 0x08000000 <= v < 0x08000000 + len(bf)
def rom(v): return v - 0x08000000

def record(zone, area):
    for i in range(BF_AREA_COUNT):
        o = BF_AREAS + i * 56
        if bf[o] == zone and bf[o + 1] == area:
            return o
    raise SystemExit('no Buu\'s Fury area %d:%d' % (zone, area))

def header(rec, variant=0):
    return rom(r32(rom(r32(rec + 44)) + 4 * variant))

def layer_pointers(layer):
    cls = r32(layer)
    if cls == BF_LAYER_EMPTY:
        return []
    w, h = bf[layer + 20], bf[layer + 21]
    return [r32(layer + 24 + 4 * i) for i in range(w * h)]

# ---- the compressed length of a block: up to where the next one starts ----
def all_block_starts():
    starts = set()
    for i in range(BF_AREA_COUNT):
        rec = BF_AREAS + i * 56
        for v in range(bf[rec + 2]):
            h = header(rec, v)
            for L in range(4):
                p = r32(h + 20 + 4 * L)
                if isptr(p):
                    starts.add(rom(p))
                    for c in layer_pointers(rom(p)):
                        if c:
                            starts.add(rom(c))
            for off in (44, 52, 56, 72):
                p = r32(h + off)
                if isptr(p):
                    starts.add(rom(p))
            for k in range(r32(h + 12)):  # tile animations: their records and frames
                a = rom(r32(rom(r32(h + 16)) + 4 * k))
                starts.add(a)
                for f in range(bf[a]):
                    starts.add(rom(r32(a + 4 + 4 * f)))
            if r32(h + 36):
                starts.add(rom(r32(h + 40)))
            starts.add(h)
    for i in range(BF_BANKS):
        starts.add(rom(r32(BF_BANK_TABLE + 4 * i)))
    return sorted(a for a in starts if a % 4 == 0)  # a block is word-aligned; anything else was not a pointer

STARTS = all_block_starts()
BANK_STARTS = sorted(rom(r32(BF_BANK_TABLE + 4 * i)) for i in range(BF_BANKS))

def block_length(o):
    """A compressed block's length is not stored, so: up to where the next
    block starts (for a bank, the next bank: they lie one after another, and a
    few extra bytes of the next one do no harm)."""
    size = r32(o + 4)
    if r32(o) == 0:
        return 8 + size
    starts = BANK_STARTS if o in BANK_STARTS else STARTS
    i = bisect.bisect_right(starts, o)
    end = starts[i] if i < len(starts) else len(bf)
    if starts is BANK_STARTS and i >= len(starts):  # the last bank: up to whatever comes next
        j = bisect.bisect_right(STARTS, o)
        end = STARTS[j] if j < len(STARTS) else len(bf)
    return min(end - o, 8 + size + size // 8 + 256)  # this scheme never swells data by more

# ---- a blob laid out at a fixed ROM address ----
class Blob:
    def __init__(self, base):
        self.base, self.data = base, bytearray()
    def alloc(self, n):
        while len(self.data) % 4:
            self.data.append(0)
        o = len(self.data)
        self.data += bytes(n)
        return o
    def put(self, o, b):
        self.data[o:o + len(b)] = b
    def addr(self, o):
        return 0x08000000 + self.base + o
    def copy_block(self, o):
        b = bf[o:o + block_length(o)]
        at = self.alloc(len(b))
        self.put(at, b)
        return self.addr(at)

def write_inc(path, name, blob, extra=''):
    with open(path, 'w') as f:
        f.write('// Generated by tools/maps/convert_bf_area.py. Do not edit.\n')
        f.write('const uint32_t %sRomOffset = 0x%X;\nconst uint32_t %sSize = %d;\n' % (name, blob.base, name, len(blob.data)))
        f.write(extra)
        f.write('static const uint8_t %sData[] = {\n' % name)
        for i in range(0, len(blob.data), 32):
            f.write(','.join(str(b) for b in blob.data[i:i + 32]) + ',\n')
        f.write('};\n')

# ---- the shared banks ----
def banks(out):
    blob = Blob(BANKS_ROM)
    table = blob.alloc(4 * BF_BANKS)
    for i in range(BF_BANKS):
        p = blob.copy_block(rom(r32(BF_BANK_TABLE + 4 * i)))
        blob.put(table + 4 * i, struct.pack('<I', p))
    write_inc(out, 'bfBanks', blob, 'const uint32_t bfBankTable = 0x%08X;\n' % blob.addr(table))
    print('banks: %d KB' % (len(blob.data) // 1024))

# ---- one area, into a blob: returns (record address, arrival x, y, width, height) ----
def area_into(blob, zone, area_no, with_exits=True):
    rec = record(zone, area_no)
    if bf[rec + 2] != 1:
        print('note: %d:%d has %d variants, taking the first' % (zone, area_no, bf[rec + 2]))
    h = header(rec)
    if r32(h + 60) != 0:
        raise SystemExit('map mode %d (affine) is not supported' % r32(h + 60))
    new_rec = blob.alloc(56)
    map_table = blob.alloc(4)
    new_h = blob.alloc(80)
    layers = []
    blank = None  # one all-transparent chunk for every empty layer
    for L in range(4):
        p = rom(r32(h + 20 + 4 * L))
        cls = r32(p)
        if cls == BF_LAYER_EMPTY:
            # LoG2's empty-layer class leaves the background's screen block
            # as it was (stale VRAM, drawn over the real layer when the
            # priorities tie), so an empty layer becomes a real one showing a
            # single chunk of tile 0, which is blank: what LoG2's own maps do
            if blank is None:
                blank = blob.alloc(8 + 2048)
                blob.put(blank, struct.pack('<II', 0, 2048))
            at = blob.alloc(28)
            blob.put(at, struct.pack('<IIIII', LG2_LAYER, 0x400, 0x400, 0, 0) + bytes([1, 1, 0, 0]) + struct.pack('<I', blob.addr(blank)))
        elif cls == BF_LAYER:
            chunks = layer_pointers(p)
            at = blob.alloc(24 + 4 * len(chunks))
            blob.put(at, bf[p:p + 24])
            blob.put(at, struct.pack('<I', LG2_LAYER))
            for i, c in enumerate(chunks):  # a chunk pointer can be 0: nothing there (the engine skips it)
                blob.put(at + 24 + 4 * i, struct.pack('<I', blob.copy_block(rom(c)) if c else 0))
        else:
            raise SystemExit('layer %d is of a kind (%08X) not supported' % (L, cls))
        layers.append(blob.addr(at))
    # tile animations: {u8 frames, u8 tiles, u16 VRAM slot, frame pointers};
    # a frame is {u32 duration, bitstream} with no size, decoded by the same
    # routine: copied verbatim, up to the next thing in the ROM
    n_anims = r32(h + 12)
    anims = blob.alloc(4 * n_anims) if n_anims else 0
    for k in range(n_anims):
        a = rom(r32(rom(r32(h + 16)) + 4 * k))
        frames, tiles = bf[a], bf[a + 1]
        at = blob.alloc(4 + 4 * frames)
        blob.put(at, bf[a:a + 4])
        for f in range(frames):
            fo = rom(r32(a + 4 + 4 * f))
            i = bisect.bisect_right(STARTS, fo)
            end = STARTS[i] if i < len(STARTS) else len(bf)
            length = min(end - fo, 4 + tiles * 64 + tiles * 8 + 64)
            fat = blob.alloc(length)
            blob.put(fat, bf[fo:fo + length])
            blob.put(at + 4 + 4 * f, struct.pack('<I', blob.addr(fat)))
        blob.put(anims + 4 * k, struct.pack('<I', blob.addr(at)))
    # decorations: an array of 24-byte records and one graphics block
    n_decos = r32(h + 36)
    decos = decos_gfx = 0
    if n_decos:
        d = rom(r32(h + 40))
        decos = blob.alloc(24 * n_decos)
        blob.put(decos, bf[d:d + 24 * n_decos])
        decos_gfx = blob.copy_block(rom(r32(h + 44)))
    bitmap_a = blob.copy_block(rom(r32(h + 52)))
    bitmap_b = blob.copy_block(rom(r32(h + 56)))
    tiles = blob.copy_block(rom(r32(h + 72)))
    # exits: the same records, the classes LoG2's, the target zone shifted
    n_exits = r32(h + 64) if with_exits else 0
    exits = blob.alloc(4 * n_exits) if n_exits else 0
    for k in range(n_exits):
        e = rom(r32(rom(r32(h + 68)) + 4 * k))
        rec_e = bytearray(bf[e:e + 24])
        for off in (0, 4):
            cls = struct.unpack_from('<I', rec_e, off)[0]
            if cls not in EXIT_CLASSES:
                raise SystemExit('exit class %08X not known' % cls)
            rec_e[off:off + 4] = struct.pack('<I', EXIT_CLASSES[cls])
        if rec_e[17] in BF_ZONES:
            rec_e[17] += ZONE_SHIFT
        at = blob.alloc(24)
        blob.put(at, rec_e)
        blob.put(exits + 4 * k, struct.pack('<I', blob.addr(at)))
    # save points: Buu's Fury's become LoG2's, at the same spots
    saves = []
    for k in range(bf[rec + 3]):
        t = rom(r32(rom(r32(rec + 16)) + 8 * k + 4))
        if r32(t) == BF_SAVE_CLASS:
            # Buu's Fury's pads are a little smaller than LoG2's and the game
            # tests a point above the feet: 8 px more at the bottom so that
            # standing anywhere on the disc counts
            x1, y1, x2, y2 = struct.unpack_from('<4H', bf, t + 8)
            saves.append(struct.pack('<4H', x1, y1, x2, y2 + 8))
    triggers = blob.alloc(8 * len(saves)) if saves else 0
    for k, rect in enumerate(saves):
        at = blob.alloc(16)
        blob.put(at, struct.pack('<II', *LG2_SAVE_CLASSES) + rect)
        blob.put(triggers + 8 * k, struct.pack('<II', 0, blob.addr(at)))
    hdr = bytearray(bf[h:h + 80])
    for off in (12, 16, 36, 40, 44, 64, 68):
        hdr[off:off + 4] = bytes(4)
    for L in range(4):
        hdr[20 + 4 * L:24 + 4 * L] = struct.pack('<I', layers[L])
    hdr[52:56] = struct.pack('<I', bitmap_a)
    hdr[56:60] = struct.pack('<I', bitmap_b)
    hdr[72:76] = struct.pack('<I', tiles)
    hdr[76:80] = struct.pack('<I', 0x08000000 + BANKS_ROM)  # the shared table sits first in the banks blob
    if n_exits:
        hdr[64:68] = struct.pack('<I', n_exits)
        hdr[68:72] = struct.pack('<I', blob.addr(exits))
    if n_anims:
        hdr[12:16] = struct.pack('<I', n_anims)
        hdr[16:20] = struct.pack('<I', blob.addr(anims))
    if n_decos:
        hdr[36:40] = struct.pack('<I', n_decos)
        hdr[40:44] = struct.pack('<I', blob.addr(decos))
        hdr[44:48] = struct.pack('<I', decos_gfx)
    blob.put(new_h, hdr)
    blob.put(map_table, struct.pack('<I', blob.addr(new_h)))
    r = bytearray(56)
    r[0], r[1], r[2], r[8] = ZONE_SHIFT + zone, area_no, 1, 1  # +8 reaches the world object's +12: 1 = fighting allowed
    r[12:16] = struct.pack('<I', NO_NAME_TEXT)
    r[36:40] = struct.pack('<I', MUSIC)
    r[44:48] = struct.pack('<I', blob.addr(map_table))
    if saves:
        r[3] = len(saves)
        r[16:20] = struct.pack('<I', blob.addr(triggers))
    blob.put(new_rec, r)
    # somewhere to arrive: a walkable tile (bit set in bitmap A) nearest the middle
    w, hgt = r16(h + 8) + 240, r16(h + 10) + 160
    ax, ay = w // 2, hgt // 2
    try:
        sys.path.insert(0, 'tools/maps')
        from bf_unpack import unpack
        A = unpack(bf, rom(r32(h + 52)), 32000, 1280)
        best = None
        for ty in range(hgt // 8):
            for tx in range(w // 8):
                if (A[ty * 32 + (tx >> 3)] >> (tx & 7)) & 1:
                    d = (tx * 8 + 4 - ax) ** 2 + (ty * 8 + 4 - ay) ** 2
                    if best is None or d < best[0]:
                        best = (d, tx * 8 + 4, ty * 8 + 8)
        if best:
            ax, ay = best[1], best[2]
    except Exception as e:
        print('note: %d:%d arrival point from the map middle (%s)' % (zone, area_no, e))
    return blob.addr(new_rec), ax, ay, w, hgt

def area(zone, area_no, base, out):
    blob = Blob(base)
    rec_addr, ax, ay, w, hgt = area_into(blob, zone, area_no, with_exits=False)
    name = 'bfArea_%d_%d' % (zone, area_no)
    write_inc(out, name, blob, 'const uint32_t %sRecord = 0x%08X;\nconst int %sZone = %d, %sArea = %d, %sX = %d, %sY = %d;\n' % (
        name, rec_addr, name, ZONE_SHIFT + zone, name, area_no, name, ax, name, ay))
    print('%d:%d -> LoG2 %d:%d: %d x %d px, %d KB, arrive at (%d, %d)' % (zone, area_no, ZONE_SHIFT + zone, area_no, w, hgt, len(blob.data) // 1024, ax, ay))

# ---- every area, one blob, with an index for the mod ----
def all_areas(base, out):
    blob = Blob(base)
    index, skipped, biggest = [], [], {}
    for i in range(BF_AREA_COUNT):
        rec = BF_AREAS + i * 56
        z, a = bf[rec], bf[rec + 1]
        try:
            rec_addr, ax, ay, w, hgt = area_into(blob, z, a)
        except SystemExit as e:
            skipped.append('%d:%d (%s)' % (z, a, e))
            continue
        index.append((ZONE_SHIFT + z, a, rec_addr, ax, ay))
        if z not in biggest or w * hgt > biggest[z][0]:
            biggest[z] = (w * hgt, a, ax, ay, w, hgt)
    lines = ['static const struct { uint8_t zone, area; uint32_t record; int16_t x, y; } bfAreaIndex[] = {']
    for z, a, r, x, y in index:
        lines.append('\t{ %d, %d, 0x%08X, %d, %d },' % (z, a, r, x, y))
    lines.append('};')
    lines.append('// The biggest map of each zone: somewhere to start')
    lines.append('static const struct { const char* name; uint8_t zone, area; int16_t x, y; } bfPlaces[] = {')
    for z in sorted(biggest):
        _, a, x, y, w, hgt = biggest[z]
        lines.append('\t{ "BF ZONE %d (%dX%d)", %d, %d, %d, %d },' % (z, w, hgt, ZONE_SHIFT + z, a, x, y))
    lines.append('};')
    write_inc(out, 'bfAreas', blob, '\n'.join(lines) + '\n')
    print('%d areas, %d KB; skipped %d: %s' % (len(index), len(blob.data) // 1024, len(skipped), ', '.join(skipped)))

if __name__ == '__main__':
    if sys.argv[1] == 'banks':
        banks(sys.argv[2])
    elif sys.argv[1] == 'all':
        all_areas(int(sys.argv[2], 0), sys.argv[3])
    else:
        area(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4], 0), sys.argv[5])
