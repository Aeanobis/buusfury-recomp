#!/usr/bin/env python3
"""Static recompiler for a GBA ROM (ARMv4T: ARM + Thumb) -> C.

Pipeline
  1. Discover code: recursive descent from the reset vector, plus function
     pointers found in literal pools / data, Thumb jump tables, and any
     addresses recorded by a previous run of the game (profile feedback).
  2. Split the discovered code into blocks at every known entry point.
  3. Emit one C function per block. Each guest instruction becomes a call to
     the matching (always-inlined) instruction body with a constant opcode, so
     the C compiler folds the decode away and produces straight-line native
     code specialised for that instruction.

Runtime correctness never depends on discovery being complete: an address with
no block simply falls back to the interpreter and is logged for the next build.
"""
import argparse
import os
import re
import struct
import sys

ROM_BASE = 0x08000000
IWRAM_BASE = 0x03000000
MAX_BLOCK = 96          # guest instructions per block
BLOCKS_PER_FILE = 1200

T_COND = {'BEQ', 'BNE', 'BCS', 'BCC', 'BMI', 'BPL', 'BVS', 'BVC',
          'BHI', 'BLS', 'BGE', 'BLT', 'BGT', 'BLE'}


class Mem:
    """Guest address space as seen by the recompiler: the ROM plus any other
    code images (HLE BIOS, code the game copies into RAM)."""

    def __init__(self, rom_path):
        self.data = open(rom_path, 'rb').read()     # the ROM itself
        self.size = len(self.data)
        self.end = ROM_BASE + self.size
        self.segs = [(ROM_BASE, self.data)]

    def add(self, base, data):
        self.segs.append((base, data))

    def seg(self, a, n):
        for base, data in self.segs:
            if base <= a and a + n <= base + len(data):
                return base, data
        return None

    def has(self, a, n=2):
        return self.seg(a, n) is not None

    def in_rom(self, a, n=2):
        return ROM_BASE <= a and a + n <= self.end

    def r16(self, a):
        s = self.seg(a, 2)
        return struct.unpack_from('<H', s[1], a - s[0])[0] if s else 0

    def r32(self, a):
        s = self.seg(a, 4)
        return struct.unpack_from('<I', s[1], a - s[0])[0] if s else 0


def load_hle_bios(path):
    """mGBA's replacement BIOS, as a byte array in C source."""
    src = open(path).read()
    body = src[src.index('{', src.index('hleBios')) + 1:src.rindex('}')]
    return bytes(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', body))


def load_tables(path):
    t = a = None
    for line in open(path):
        k, v = line.split(':', 1)
        names = [x.strip() for x in v.split(',') if x.strip()]
        if k == 'T':
            t = names
        else:
            a = names
    assert len(t) == 0x400 and len(a) == 0x1000
    return t, a


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Recompiler:
    def __init__(self, rom, tt, at):
        self.rom = rom
        self.tt = tt
        self.at = at
        self.t_leaders = set()
        self.a_leaders = set()
        self.t_walked = set()
        self.a_walked = set()
        self.work = []
        self.confine = None     # (lo, hi): ignore new entries outside this range
        self.stats = {'jumptables': 0, 'ptr_scan': 0, 'literal': 0, 'profile': 0}

    # ---- entry bookkeeping -------------------------------------------------
    def add_thumb(self, a):
        a &= ~1
        if self.confine and not (self.confine[0] <= a < self.confine[1]):
            return
        if self.rom.has(a) and a not in self.t_leaders:
            self.t_leaders.add(a)
            self.work.append((a, True))

    def add_arm(self, a):
        a &= ~3
        if self.confine and not (self.confine[0] <= a < self.confine[1]):
            return
        if self.rom.has(a, 4) and a not in self.a_leaders:
            self.a_leaders.add(a)
            self.work.append((a, False))

    def is_rom_ptr(self, v):
        return self.rom.in_rom(v & ~1)

    # ---- Thumb -------------------------------------------------------------
    def tname(self, op):
        return self.tt[op >> 6][len('_ThumbInstruction'):]

    def tkind(self, op):
        """'term' = always leaves the block, 'cond' = may branch, None = falls through."""
        n = self.tname(op)
        if n in T_COND:
            return 'cond'
        if n in ('B', 'BL2', 'BX', 'POPR', 'SWI', 'ILL', 'BKPT'):
            return 'term'
        if n[:4] in ('ADD4', 'MOV3') and n[4] == '1' and (op & 7) == 7:
            return 'term'
        if n == 'LDMIA' and not (op & 0xFF):
            return 'term'
        return None

    def walk_thumb(self, start):
        rom = self.rom
        a = start
        lits = {}       # reg -> (value, age) from pc-relative literal loads
        prev = None     # (addr, op) of previous instruction
        while rom.has(a):
            if a in self.t_walked and a != start:
                break
            self.t_walked.add(a)
            op = rom.r16(a)
            n = self.tname(op)
            kind = self.tkind(op)
            for r in list(lits):
                v, age = lits[r]
                if age >= 6:
                    del lits[r]
                else:
                    lits[r] = (v, age + 1)

            if n == 'LDR3':
                v = rom.r32(((a + 4) & ~3) + ((op & 0xFF) << 2))
                lits[(op >> 8) & 7] = (v, 0)
                if self.is_rom_ptr(v) and (v & 1):
                    self.stats['literal'] += 1
                    self.add_thumb(v)
            elif n in T_COND:
                self.add_thumb(a + 4 + (sext(op & 0xFF, 8) << 1))
            elif n == 'B':
                self.add_thumb(a + 4 + (sext(op & 0x7FF, 11) << 1))
            elif n == 'BL2':
                if prev and self.tname(prev[1]) == 'BL1' and prev[0] == a - 2:
                    lr = prev[0] + 4 + (sext(prev[1] & 0x7FF, 11) << 12)
                    self.add_thumb((lr + ((op & 0x7FF) << 1)) & 0xFFFFFFFF)
                self.add_thumb(a + 2)
            elif n == 'SWI':
                self.add_thumb(a + 2)
            elif n == 'BX':
                rm = (op >> 3) & 0xF
                if rm == 15:
                    self.add_arm((a + 4) & ~3)
                elif rm in lits:
                    v = lits[rm][0]
                    if self.is_rom_ptr(v):
                        if v & 1:
                            self.add_thumb(v)
                        else:
                            self.add_arm(v)
            elif kind == 'term' and n[:4] in ('ADD4', 'MOV3'):
                self.thumb_jump_table(a, lits)

            if kind == 'term':
                break
            prev = (a, op)
            a += 2

    def thumb_jump_table(self, a, lits):
        """agbcc switch: ldr rX,=table ; add ; ldr ; mov pc,rY  with the table of
        absolute code addresses placed right after the dispatch code."""
        rom = self.rom
        cands = [v for v, _ in lits.values()
                 if rom.has(v, 4) and not (v & 3) and a < v <= a + 0x40]
        for table in cands:
            lo = None
            p = table
            n = 0
            while rom.has(p, 4) and n < 1024:
                v = rom.r32(p)
                if (v & 1) or not rom.has(v) or abs(v - a) > 0x8000 or v < table:
                    break
                if lo is not None and p >= lo:
                    break
                lo = v if lo is None else min(lo, v)
                self.add_thumb(v)
                p += 4
                n += 1
            if n:
                self.stats['jumptables'] += 1

    # ---- ARM ---------------------------------------------------------------
    def aname(self, op):
        return self.at[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)][len('_ARMInstruction'):]

    def aterm(self, op):
        """True if the instruction unconditionally leaves the block."""
        if (op >> 28) != 0xE:
            return False
        n = self.aname(op)
        if n in ('B', 'BX', 'SWI', 'ILL', 'BKPT', 'STUB'):
            return True
        cls = (op >> 25) & 7
        if cls in (0, 1):   # data processing with rd = pc (not the compare group)
            opc = (op >> 21) & 0xF
            is_dp = cls == 1 or (op & 0x90) != 0x90
            if is_dp and ((op >> 12) & 0xF) == 15 and not (8 <= opc <= 11):
                return True
        if cls in (2, 3) and (op & (1 << 20)) and ((op >> 12) & 0xF) == 15:
            return True     # ldr pc, ...
        if cls == 4 and (op & (1 << 20)) and (op & 0x8000):
            return True     # ldm ..., {.. pc}
        return False

    def walk_arm(self, start):
        rom = self.rom
        a = start
        lits = {}
        prev_op = 0
        while rom.has(a, 4):
            if a in self.a_walked and a != start:
                break
            self.a_walked.add(a)
            op = rom.r32(a)
            n = self.aname(op)
            cond = op >> 28
            for r in list(lits):
                v, age = lits[r]
                if age >= 4:
                    del lits[r]
                else:
                    lits[r] = (v, age + 1)

            if (op & 0x0F7F0000) == 0x051F0000:     # ldr rd, [pc, #+-imm]
                off = op & 0xFFF
                v = rom.r32(a + 8 + (off if op & (1 << 23) else -off))
                rd = (op >> 12) & 0xF
                lits[rd] = (v, 0)
                if self.is_rom_ptr(v):
                    if v & 1:
                        self.stats['literal'] += 1
                        self.add_thumb(v)
                    elif rd == 15:
                        self.add_arm(v)
            elif ((op >> 25) & 7) == 5:              # b / bl
                self.add_arm((a + 8 + (sext(op & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF)
                if op & (1 << 24):
                    self.add_arm(a + 4)
            elif (op & 0x0FFFFFF0) == 0x012FFF10:    # bx rm
                rm = op & 0xF
                if rm in lits and self.is_rom_ptr(lits[rm][0]):
                    v = lits[rm][0]
                    if v & 1:
                        self.add_thumb(v)
                    else:
                        self.add_arm(v)
                if prev_op == 0xE1A0E00F:            # mov lr, pc ; bx rm
                    self.add_arm(a + 4)
            elif n == 'SWI':
                self.add_arm(a + 4)

            if self.aterm(op):
                break
            prev_op = op
            a += 4

    # ---- discovery driver --------------------------------------------------
    def scan_pointers(self):
        """Function pointers stored in data: an odd ROM address whose target
        starts with a Thumb `push {..., lr}`."""
        rom = self.rom
        data = rom.data
        for off in range(0, rom.size - 3, 4):
            if data[off + 3] != 0x08:
                continue
            v = struct.unpack_from('<I', data, off)[0]
            if (v & 1) and rom.in_rom(v - 1) and (rom.r16(v - 1) & 0xFF00) == 0xB500:
                if (v - 1) not in self.t_leaders:
                    self.stats['ptr_scan'] += 1
                self.add_thumb(v)

    def load_profile(self, path):
        if not path or not os.path.exists(path):
            return
        for line in open(path):
            p = line.split()
            if len(p) < 2:
                continue
            a = int(p[1], 16)
            if not self.rom.has(a):
                continue
            if not self.rom.in_rom(a):
                self.stats['profile_nonrom'] = self.stats.get('profile_nonrom', 0) + 1
            before = len(self.t_leaders) + len(self.a_leaders)
            if p[0] == 'T':
                self.add_thumb(a)
            else:
                self.add_arm(a)
            self.stats['profile'] += len(self.t_leaders) + len(self.a_leaders) - before

    def drain(self):
        while self.work:
            a, thumb = self.work.pop()
            if thumb:
                self.walk_thumb(a)
            else:
                self.walk_arm(a)

    def sweep(self, lo, hi, thumb):
        """Linear sweep: compile everything in [lo, hi) that recursive descent
        did not reach. Instruction boundaries are unambiguous (fixed width), so
        this gives full coverage of a code region; functions only ever called
        through pointers held in RAM are found this way. Literal pools get
        decoded as code too, which is harmless because they never execute."""
        step = 2 if thumb else 4
        walked = self.t_walked if thumb else self.a_walked
        self.confine = (lo, hi)
        n = 0
        for a in range(lo, hi, step):
            if a not in walked:
                n += 1
                if thumb:
                    self.add_thumb(a)
                else:
                    self.add_arm(a)
                self.drain()
        self.confine = None
        self.stats['sweep'] = self.stats.get('sweep', 0) + n

    def rom_thumb_clusters(self):
        """Bodies of Thumb code in ROM: runs of discovered addresses with no
        large hole, ignoring small islands (usually data mistaken for code)."""
        addrs = sorted(a for a in self.t_walked if self.rom.in_rom(a))
        clusters = []
        for a in addrs:
            if clusters and a - clusters[-1][1] <= 0x2000:
                clusters[-1][1] = a
                clusters[-1][2] += 1
            else:
                clusters.append([a, a, 1])
        return [(lo, hi + 2) for lo, hi, n in clusters if n >= 512]

    def discover(self, profile, entries=None):
        self.add_arm(ROM_BASE)
        if self.rom.has(0, 4):      # HLE BIOS: SWI and IRQ vectors
            self.add_arm(0x08)
            self.add_arm(0x18)
        self.drain()
        self.load_profile(profile)
        self.load_profile(entries)
        self.drain()
        self.scan_pointers()
        self.drain()
        self.code_ranges = self.rom_thumb_clusters()
        for lo, hi in self.code_ranges:
            self.sweep(lo, hi, True)
        for base, data in self.rom.segs[1:]:    # BIOS and RAM images are ARM code
            end = len(data.rstrip(b'\0'))
            self.sweep(base, base + ((end + 3) & ~3), False)

    # ---- code generation ---------------------------------------------------
    @staticmethod
    def emit_resume(start, n, width, out):
        """A block can be left after any instruction (when a hardware event is
        due) and is later re-entered at that same instruction."""
        if n < 2:
            return
        out.append('\tswitch (cpu->gprs[ARM_PC] - %d) {' % width)
        for i in range(1, n):
            out.append('\tcase 0x%08XU: goto L%d;' % (start + i * width, i))
        out.append('\t}')

    def emit_thumb(self, start, out):
        rom = self.rom
        out.append('static void T_%08X(struct ARMCore* cpu) {' % start)
        a = start
        n = 0
        body = []
        while True:
            op = rom.r16(a)
            kind = self.tkind(op)
            macro = {'term': 'TX', 'cond': 'TB', None: 'TI'}[kind]
            body.append('%s\t%s(0x%08X, %s, 0x%04X, 0x%04X, 0x%04X)' % (
                'L%d:' % n if n else '', macro, a, self.tt[op >> 6], op, rom.r16(a + 2), rom.r16(a + 4)))
            n += 1
            if kind == 'term':
                break
            a += 2
            if a in self.t_leaders or not rom.has(a):
                break
            if n >= MAX_BLOCK:
                self.add_thumb(a)
                break
        self.emit_resume(start, n, 2, out)
        out.extend(body)
        out.append('}')
        return n * 2

    def emit_arm(self, start, out):
        rom = self.rom
        out.append('static void A_%08X(struct ARMCore* cpu) {' % start)
        a = start
        n = 0
        body = []
        while True:
            op = rom.r32(a)
            term = self.aterm(op)
            name = self.at[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)]
            body.append('%s\t%s(0x%08X, %s, 0x%08XU, %d, 0x%08XU, 0x%08XU)' % (
                'L%d:' % n if n else '', 'AX' if term else 'AI', a, name, op, op >> 28, rom.r32(a + 4), rom.r32(a + 8)))
            n += 1
            if term:
                break
            a += 4
            if a in self.a_leaders or not rom.has(a, 4):
                break
            if n >= MAX_BLOCK:
                self.add_arm(a)
                break
        self.emit_resume(start, n, 4, out)
        out.extend(body)
        out.append('}')
        return n * 4

    def generate(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        for f in os.listdir(outdir):
            if f.startswith('rc_blocks_') or f == 'rc_index.c':
                os.remove(os.path.join(outdir, f))
        done = set()
        blocks = []     # (addr, thumb, lines, bytes)
        insns = 0
        while True:
            todo = sorted([(a, True) for a in self.t_leaders if (a, True) not in done] +
                          [(a, False) for a in self.a_leaders if (a, False) not in done])
            if not todo:
                break
            for a, thumb in todo:
                done.add((a, thumb))
                lines = []
                size = self.emit_thumb(a, lines) if thumb else self.emit_arm(a, lines)
                insns += size // (2 if thumb else 4)
                blocks.append((a, thumb, lines, size))
            self.work.clear()
        blocks.sort(key=lambda b: (b[0], b[1]))

        nfiles = 0
        for i in range(0, len(blocks), BLOCKS_PER_FILE):
            chunk = blocks[i:i + BLOCKS_PER_FILE]
            with open(os.path.join(outdir, 'rc_blocks_%03d.c' % nfiles), 'w') as f:
                f.write('// Generated by tools/recomp.py. Do not edit.\n#include "rc_rt.h"\n\n')
                for _, _, lines, _ in chunk:
                    f.write('\n'.join(lines) + '\n\n')
                f.write('const struct RcEntry rc_tab_%03d[] = {\n' % nfiles)
                for a, thumb, _, size in chunk:
                    f.write('\t{ 0x%08XU, %d, %d, %s_%08X },\n' % (a, thumb, size, 'T' if thumb else 'A', a))
                f.write('};\nconst unsigned rc_tab_%03d_n = %d;\n' % (nfiles, len(chunk)))
            nfiles += 1
        with open(os.path.join(outdir, 'rc_index.c'), 'w') as f:
            f.write('// Generated by tools/recomp.py. Do not edit.\n#include "rc_rt.h"\n\n')
            for i in range(nfiles):
                f.write('extern const struct RcEntry rc_tab_%03d[];\nextern const unsigned rc_tab_%03d_n;\n' % (i, i))
            f.write('\nvoid rcRegisterAll(void (*reg)(const struct RcEntry*, unsigned)) {\n')
            for i in range(nfiles):
                f.write('\treg(rc_tab_%03d, rc_tab_%03d_n);\n' % (i, i))
            f.write('}\n\n')
            # Image of the RAM-resident code, used to check it is still what was compiled
            base, data = self.ram_image
            f.write('const uint32_t rcRamBase = 0x%08XU;\nconst uint32_t rcRamSize = %d;\n' % (base, len(data)))
            f.write('const uint8_t rcRamImage[] = {')
            for i, b in enumerate(data or b'\0'):
                f.write(('\n\t' if i % 16 == 0 else ' ') + '0x%02X,' % b)
            f.write('\n};\n')
        return len(blocks), insns, nfiles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--tables', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--profile', default=None, help='addresses the interpreter had to run in an earlier build')
    ap.add_argument('--entries', default=None, help='hand-maintained extra block entry points (same format as --profile)')
    ap.add_argument('--bios', default=None, help="mGBA's hle-bios.c")
    ap.add_argument('--iwram', default=None, help='dump of internal work RAM holding code the game copied there')
    args = ap.parse_args()

    rom = Mem(args.rom)
    tt, at = load_tables(args.tables)
    rc = Recompiler(rom, tt, at)
    if args.bios:
        rom.add(0, load_hle_bios(args.bios))
    rc.ram_image = (IWRAM_BASE, b'')
    if args.iwram and os.path.exists(args.iwram) and args.profile and os.path.exists(args.profile):
        # Only the part of RAM that was actually seen executing is treated as code
        pcs = [int(l.split()[1], 16) for l in open(args.profile) if len(l.split()) > 1]
        pcs = [a for a in pcs if IWRAM_BASE <= a < IWRAM_BASE + 0x8000]
        if pcs:
            dump = open(args.iwram, 'rb').read()
            lo = (min(pcs) - 0x40) & ~0xF
            hi = min(IWRAM_BASE + 0x8000, (max(pcs) + 0x400) & ~0xF)
            lo = max(lo, IWRAM_BASE)
            image = dump[lo - IWRAM_BASE:hi - IWRAM_BASE]
            rom.add(lo, image)
            rc.ram_image = (lo, image)
            at_rom = rom.data.find(image[0x40:0x140])
            print('recomp: RAM code image %08X-%08X (%s)' % (lo, hi,
                  'copied from ROM offset 0x%X' % (at_rom - 0x40) if at_rom >= 0 else 'not found verbatim in ROM'))
    rc.discover(args.profile, args.entries)
    nblocks, insns, nfiles = rc.generate(args.out)
    nt = sum(1 for a in rc.t_leaders)
    print('recomp: %d blocks (%d thumb, %d arm), %d guest instructions, %d C files' % (
        nblocks, nt, len(rc.a_leaders), insns, nfiles))
    print('recomp: unique code bytes: %d thumb + %d arm = %.1f%% of ROM' % (
        len(rc.t_walked) * 2, len(rc.a_walked) * 4,
        100.0 * (len(rc.t_walked) * 2 + len(rc.a_walked) * 4) / rom.size))
    print('recomp: Thumb code regions swept: %s' % ', '.join('%08X-%08X' % c for c in rc.code_ranges))
    print('recomp: sources: %s' % ', '.join('%s=%d' % kv for kv in sorted(rc.stats.items())))


if __name__ == '__main__':
    sys.exit(main())
