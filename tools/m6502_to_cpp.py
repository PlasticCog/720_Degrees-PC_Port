"""Static translator: 720 Degrees sound program (6502 machine code) -> C++.

Code discovery: runtime coverage (re/work/*/scov.txt[.edges]) + vectors + static traversal
+ the two RTS-dispatch tables.  Each instruction is translated with the exact semantics of
the reference interpreter (port/src/ref/m6502.cpp): same flags, bus accesses and cycles.
Output: port/src/game/s65_code.cpp, s65_tables.cpp, s65_decls.h
"""
import os, sys, glob, collections
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
OUTDIR = os.path.join(ROOT, 'port', 'src', 'game')
sys.path.insert(0, HERE)
import romset
ROMDIR = romset.ROMDIR

MEM = bytearray(0x10000)
for i, n in enumerate(['136047-2134.2a', '136047-1135.2b', '136047-2136.2cd']):
    MEM[0x4000 + i * 0x4000:0x8000 + i * 0x4000] = open(os.path.join(ROMDIR, n), 'rb').read()


def rd(a):
    return MEM[a & 0xffff]


def rd16(a):
    return rd(a) | (rd(a + 1) << 8)


BASE_CYCLES = [
    7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
]

# opcode -> (mnemonic, mode)   modes: imp acc imm zp zpx zpy abs absx absy indx indy ind rel
OPS = {}
def _d(mn, mode, *codes):
    for c in codes:
        OPS[c] = (mn, mode)
_d('LDA', 'imm', 0xa9); _d('LDA', 'zp', 0xa5); _d('LDA', 'zpx', 0xb5); _d('LDA', 'abs', 0xad)
_d('LDA', 'absx', 0xbd); _d('LDA', 'absy', 0xb9); _d('LDA', 'indx', 0xa1); _d('LDA', 'indy', 0xb1)
_d('LDX', 'imm', 0xa2); _d('LDX', 'zp', 0xa6); _d('LDX', 'zpy', 0xb6); _d('LDX', 'abs', 0xae); _d('LDX', 'absy', 0xbe)
_d('LDY', 'imm', 0xa0); _d('LDY', 'zp', 0xa4); _d('LDY', 'zpx', 0xb4); _d('LDY', 'abs', 0xac); _d('LDY', 'absx', 0xbc)
_d('STA', 'zp', 0x85); _d('STA', 'zpx', 0x95); _d('STA', 'abs', 0x8d); _d('STA', 'absx', 0x9d)
_d('STA', 'absy', 0x99); _d('STA', 'indx', 0x81); _d('STA', 'indy', 0x91)
_d('STX', 'zp', 0x86); _d('STX', 'zpy', 0x96); _d('STX', 'abs', 0x8e)
_d('STY', 'zp', 0x84); _d('STY', 'zpx', 0x94); _d('STY', 'abs', 0x8c)
_d('TAX', 'imp', 0xaa); _d('TAY', 'imp', 0xa8); _d('TXA', 'imp', 0x8a); _d('TYA', 'imp', 0x98)
_d('TSX', 'imp', 0xba); _d('TXS', 'imp', 0x9a)
_d('PHA', 'imp', 0x48); _d('PLA', 'imp', 0x68); _d('PHP', 'imp', 0x08); _d('PLP', 'imp', 0x28)
for mn, base in (('AND', 0x20), ('ORA', 0x00), ('EOR', 0x40), ('ADC', 0x60), ('SBC', 0xe0), ('CMP', 0xc0)):
    _d(mn, 'imm', base + 0x09); _d(mn, 'zp', base + 0x05); _d(mn, 'zpx', base + 0x15); _d(mn, 'abs', base + 0x0d)
    _d(mn, 'absx', base + 0x1d); _d(mn, 'absy', base + 0x19); _d(mn, 'indx', base + 0x01); _d(mn, 'indy', base + 0x11)
_d('BIT', 'zp', 0x24); _d('BIT', 'abs', 0x2c)
_d('CPX', 'imm', 0xe0); _d('CPX', 'zp', 0xe4); _d('CPX', 'abs', 0xec)
_d('CPY', 'imm', 0xc0); _d('CPY', 'zp', 0xc4); _d('CPY', 'abs', 0xcc)
_d('INC', 'zp', 0xe6); _d('INC', 'zpx', 0xf6); _d('INC', 'abs', 0xee); _d('INC', 'absx', 0xfe)
_d('DEC', 'zp', 0xc6); _d('DEC', 'zpx', 0xd6); _d('DEC', 'abs', 0xce); _d('DEC', 'absx', 0xde)
_d('INX', 'imp', 0xe8); _d('INY', 'imp', 0xc8); _d('DEX', 'imp', 0xca); _d('DEY', 'imp', 0x88)
for mn, base in (('ASL', 0x00), ('ROL', 0x20), ('LSR', 0x40), ('ROR', 0x60)):
    _d(mn, 'acc', base + 0x0a); _d(mn, 'zp', base + 0x06); _d(mn, 'zpx', base + 0x16)
    _d(mn, 'abs', base + 0x0e); _d(mn, 'absx', base + 0x1e)
_d('JMP', 'abs', 0x4c); _d('JMP', 'ind', 0x6c); _d('JSR', 'abs', 0x20); _d('RTS', 'imp', 0x60)
_d('RTI', 'imp', 0x40); _d('BRK', 'imp', 0x00)
for mn, c in (('BPL', 0x10), ('BMI', 0x30), ('BVC', 0x50), ('BVS', 0x70), ('BCC', 0x90), ('BCS', 0xb0), ('BNE', 0xd0), ('BEQ', 0xf0)):
    _d(mn, 'rel', c)
for mn, c in (('CLC', 0x18), ('SEC', 0x38), ('CLI', 0x58), ('SEI', 0x78), ('CLV', 0xb8), ('CLD', 0xd8), ('SED', 0xf8), ('NOP', 0xea)):
    _d(mn, 'imp', c)
SIZE = {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'abs': 3, 'absx': 3, 'absy': 3,
        'indx': 2, 'indy': 2, 'ind': 3, 'rel': 2}

VECTORS = {rd16(0xfffc): 'reset', rd16(0xfffa): 'nmi', rd16(0xfffe): 'irq'}
RTS_TABLES = [(0x434B, 0x4638, 15), (0x4378, 0x5ECA, 24)]


class Ins:
    def __init__(self, a):
        self.a = a
        self.op = rd(a)
        self.mn, self.mode = OPS.get(self.op, ('NOP?', 'imp'))
        self.size = SIZE[self.mode]
        self.b1 = rd(a + 1)
        self.w = rd16(a + 1)
        self.next = (a + self.size) & 0xffff
        self.target = None
        if self.mode == 'rel':
            off = self.b1 - 256 if self.b1 & 0x80 else self.b1
            self.target = (self.next + off) & 0xffff
        elif self.mn in ('JMP', 'JSR') and self.mode == 'abs':
            self.target = self.w

    def text(self):
        m = self.mode
        o = {'imp': '', 'acc': 'A', 'imm': '#$%02X' % self.b1, 'zp': '$%02X' % self.b1, 'zpx': '$%02X,X' % self.b1,
             'zpy': '$%02X,Y' % self.b1, 'abs': '$%04X' % self.w, 'absx': '$%04X,X' % self.w,
             'absy': '$%04X,Y' % self.w, 'indx': '($%02X,X)' % self.b1, 'indy': '($%02X),Y' % self.b1,
             'ind': '($%04X)' % self.w, 'rel': '$%04X' % (self.target or 0)}[m]
        return (self.mn + ' ' + o).strip()


def discover():
    seeds = set(VECTORS)
    files = [os.path.join(ROOT, 're', 'coverage', 'sound_cov.txt')] + glob.glob(os.path.join(ROOT, 're', 'work', '*', 'scov.txt'))
    for f in files:
        if not os.path.exists(f):
            continue
        for line in open(f):
            seeds.add(int(line, 16))
        ef = f.replace('sound_cov', 'sound_edges') if f.endswith('sound_cov.txt') else f + '.edges'
        if os.path.exists(ef):
            for line in open(ef):
                a, b = line.split()
                seeds.add(int(b, 16))
    entries = set(VECTORS)
    for site, tab, n in RTS_TABLES:
        for i in range(n):
            t = (rd16(tab + 2 * i) + 1) & 0xffff
            seeds.add(t)
            entries.add(t)
    insns = {}
    work = collections.deque(sorted(s for s in seeds if s >= 0x4000))
    while work:
        a = work.popleft()
        while a >= 0x4000 and a not in insns:
            ins = Ins(a)
            insns[a] = ins
            if ins.mn in ('RTS', 'RTI', 'BRK') or ins.mn == 'NOP?' or (ins.mn == 'JMP' and ins.mode == 'ind'):
                break
            if ins.mn == 'JMP':
                work.append(ins.target)
                break
            if ins.mn == 'JSR':
                entries.add(ins.target)
                work.append(ins.target)
            elif ins.mode == 'rel':
                work.append(ins.target)
            a = ins.next
    return insns, entries


def h(v):
    return '0x%04X' % (v & 0xffff)


class Fn:
    def __init__(self, entry):
        self.entry = entry
        self.addrs = []
        self.labels = set()
        self.name = 'snd_%04X' % entry


def mem_read(addr_expr, const=None):
    if const is not None:
        a = const
        if a >= 0x4000:
            return 'SROM[%s]' % h(a)
        if (a & 0xdfff) < 0x1000:
            return 'SR[%s]' % h(a & 0xfff)
        return 'sio_rd(%s)' % h(a)
    return 'srd(%s)' % addr_expr


def mem_write(val, addr_expr, const=None):
    if const is not None:
        a = const
        if a >= 0x4000:
            return '/* write to ROM %s ignored */' % h(a)
        if (a & 0xdfff) < 0x1000:
            return 'SR[%s] = (uint8_t)(%s);' % (h(a & 0xfff), val)
        return 'sio_wr(%s, %s);' % (h(a), val)
    return 'swr(%s, %s);' % (addr_expr, val)


def gen_insn(ins, fn, fnmap):
    out = []
    e = out.append
    e('S.cyc += %d;' % BASE_CYCLES[ins.op])
    mode, mn = ins.mode, ins.mn
    const = None
    ad = None
    # effective address
    if mode == 'zp':
        const = ins.b1
    elif mode == 'zpx':
        e('ad = (uint8_t)(%s + S.x);' % h(ins.b1)); ad = 'ad'
    elif mode == 'zpy':
        e('ad = (uint8_t)(%s + S.y);' % h(ins.b1)); ad = 'ad'
    elif mode == 'abs' and mn not in ('JMP', 'JSR'):
        const = ins.w
    elif mode in ('absx', 'absy'):
        reg = 'S.x' if mode == 'absx' else 'S.y'
        pen = mn not in ('STA', 'INC', 'DEC', 'ASL', 'LSR', 'ROL', 'ROR')
        e('ad = (uint16_t)(%s + %s);' % (h(ins.w), reg))
        if pen:
            e('if ((%s ^ ad) & 0xff00) S.cyc++;' % h(ins.w))
        ad = 'ad'
    elif mode == 'indx':
        e('{ uint8_t z = (uint8_t)(%s + S.x); ad = (uint16_t)(SR[z] | (SR[(uint8_t)(z + 1)] << 8)); }' % h(ins.b1)); ad = 'ad'
    elif mode == 'indy':
        z = ins.b1
        pen = mn not in ('STA',)
        e('{ uint16_t b = (uint16_t)(SR[%s] | (SR[%s] << 8)); ad = (uint16_t)(b + S.y);%s }' %
          (h(z), h((z + 1) & 0xff), ' if ((b ^ ad) & 0xff00) S.cyc++;' if pen else ''))
        ad = 'ad'

    def R():
        if mode == 'imm':
            return h(ins.b1)
        return mem_read(ad, const)

    def W(val):
        return mem_write(val, ad, const)

    nz = 'setnz(%s);'
    if mn == 'LDA': e('S.a = %s; setnz(S.a);' % R())
    elif mn == 'LDX': e('S.x = %s; setnz(S.x);' % R())
    elif mn == 'LDY': e('S.y = %s; setnz(S.y);' % R())
    elif mn == 'STA': e(W('S.a'))
    elif mn == 'STX': e(W('S.x'))
    elif mn == 'STY': e(W('S.y'))
    elif mn == 'TAX': e('S.x = S.a; setnz(S.x);')
    elif mn == 'TAY': e('S.y = S.a; setnz(S.y);')
    elif mn == 'TXA': e('S.a = S.x; setnz(S.a);')
    elif mn == 'TYA': e('S.a = S.y; setnz(S.a);')
    elif mn == 'TSX': e('S.x = S.s; setnz(S.x);')
    elif mn == 'TXS': e('S.s = S.x;')
    elif mn == 'PHA': e('spush(S.a);')
    elif mn == 'PLA': e('S.a = spull(); setnz(S.a);')
    elif mn == 'PHP': e('spush(S.p | FB | FU);')
    elif mn == 'PLP': e('S.p = (uint8_t)((spull() | FU) & ~FB);')
    elif mn == 'AND': e('S.a &= %s; setnz(S.a);' % R())
    elif mn == 'ORA': e('S.a |= %s; setnz(S.a);' % R())
    elif mn == 'EOR': e('S.a ^= %s; setnz(S.a);' % R())
    elif mn == 'ADC': e('op_adc(%s);' % R())
    elif mn == 'SBC': e('op_sbc(%s);' % R())
    elif mn == 'CMP': e('op_cmp(S.a, %s);' % R())
    elif mn == 'CPX': e('op_cmp(S.x, %s);' % R())
    elif mn == 'CPY': e('op_cmp(S.y, %s);' % R())
    elif mn == 'BIT': e('op_bit(%s);' % R())
    elif mn in ('INC', 'DEC'):
        e('v = (uint8_t)(%s %s 1); %s setnz(v);' % (R(), '+' if mn == 'INC' else '-', W('v')))
    elif mn == 'INX': e('S.x++; setnz(S.x);')
    elif mn == 'INY': e('S.y++; setnz(S.y);')
    elif mn == 'DEX': e('S.x--; setnz(S.x);')
    elif mn == 'DEY': e('S.y--; setnz(S.y);')
    elif mn in ('ASL', 'LSR', 'ROL', 'ROR'):
        f = 'op_' + mn.lower()
        if mode == 'acc':
            e('S.a = %s(S.a);' % f)
        else:
            e('v = %s(%s); %s' % (f, R(), W('v')))
    elif mn in ('CLC', 'SEC', 'CLI', 'SEI', 'CLV', 'CLD', 'SED'):
        e({'CLC': 'S.p &= ~FC;', 'SEC': 'S.p |= FC;', 'CLI': 'S.p &= ~FI;', 'SEI': 'S.p |= FI;',
           'CLV': 'S.p &= ~FV;', 'CLD': 'S.p &= ~FD;', 'SED': 'S.p |= FD;'}[mn])
    elif mn in ('NOP', 'NOP?'):
        pass
    elif mode == 'rel':
        cond = {'BPL': '!(S.p & FN)', 'BMI': '(S.p & FN)', 'BVC': '!(S.p & FV)', 'BVS': '(S.p & FV)',
                'BCC': '!(S.p & FC)', 'BCS': '(S.p & FC)', 'BNE': '!(S.p & FZ)', 'BEQ': '(S.p & FZ)'}[mn]
        t = ins.target
        extra = 1 + (1 if ((t ^ ins.next) & 0xff00) else 0)
        if t in fn.labels:
            e('if (%s) { S.cyc += %d; if (!sync6(%s)) goto mismatch; goto L_%04X; }' % (cond, extra, h(t), t))
        else:
            e('if (%s) { S.cyc += %d; if (!sync6(%s)) goto mismatch; S.pc = %s; sdispatch(); goto mismatch; }' % (cond, extra, h(t), h(t)))
    elif mn == 'JMP':
        if mode == 'abs':
            t = ins.target
            if t in fn.labels:
                e('if (!sync6(%s)) goto mismatch; goto L_%04X;' % (h(t), t))
            else:
                e('if (!sync6(%s)) goto mismatch; S.pc = %s; %s goto mismatch;' % (h(t), h(t), call_expr(t, fnmap)))
            return out, False
        e('S.pc = (uint16_t)(srd(%s) | (srd(%s) << 8));' % (h(ins.w), h((ins.w & 0xff00) | ((ins.w + 1) & 0xff))))
        e('sync6_dyn(); goto dispatch_local;')
        return out, False
    elif mn == 'JSR':
        t = ins.target
        ret = (ins.a + 2) & 0xffff
        e('spush(%s); spush(%s);' % (h(ret >> 8), h(ret & 0xff)))
        e('if (!sync6(%s)) goto mismatch; S.pc = %s; sshadow_push(%s); %s sshadow_pop();' % (h(t), h(t), h(ins.next), call_expr(t, fnmap)))
        e('if (S.pc != %s) goto mismatch;' % h(ins.next))
        return out, True
    elif mn == 'RTS':
        e('S.pc = spull(); S.pc |= spull() << 8; S.pc++;')
        e('sync6_dyn(); return;')
        return out, False
    elif mn == 'RTI':
        e('S.p = (uint8_t)((spull() | FU) & ~FB); S.pc = spull(); S.pc |= spull() << 8;')
        e('sync6_dyn(); return;')
        return out, False
    elif mn == 'BRK':
        e('S.cyc -= 7; sbrk(%s); if (S.pc != %s) goto mismatch;' % (h((ins.a + 2) & 0xffff), h((ins.a + 2) & 0xffff)))
        return out, True
    else:
        raise Exception('unhandled ' + mn)
    e('if (!sync6(%s)) goto mismatch;' % h(ins.next))
    return out, True


def call_expr(t, fnmap):
    if t in fnmap:
        return '%s();' % fnmap[t]
    return 'sdispatch();'


HEADER = '''// Generated by tools/m6502_to_cpp.py from the 720 Degrees sound program ROMs. Do not edit.
#include "s65rt.h"
using namespace s65rt;
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
'''


def main():
    insns, entries = discover()
    addrs = sorted(insns)
    entries = set(e for e in entries if e in insns)
    funcs = []
    owner = {}
    cur = None
    prev_end = None
    for a in addrs:
        if cur is None or a in entries or (prev_end is not None and a != prev_end and a not in cur.labels and False):
            cur = Fn(a)
            funcs.append(cur)
        cur.addrs.append(a)
        cur.labels.add(a)
        owner[a] = cur
        prev_end = insns[a].next
    fnmap = {f.entry: f.name for f in funcs}
    lines = [HEADER, '#include "s65_decls.h"', '']
    for f in funcs:
        lines.append('// ---- %04X%s' % (f.entry, ('  ' + VECTORS[f.entry]) if f.entry in VECTORS else ''))
        lines.append('void %s()' % f.name)
        lines.append('{')
        lines.append('\tuint16_t ad = 0; uint8_t v = 0;')
        lines.append('dispatch_local:')
        lines.append('\tswitch (S.pc) {')
        for a in f.addrs:
            lines.append('\tcase 0x%04X: goto L_%04X;' % (a, a))
        lines.append('\tdefault: return;')
        lines.append('\t}')
        lines.append('mismatch:')
        lines.append('\tif (sin_shadow(S.pc)) return;')
        lines.append('\tgoto dispatch_local;')
        for i, a in enumerate(f.addrs):
            ins = insns[a]
            lines.append('L_%04X: // %s' % (a, ins.text()))
            body, falls = gen_insn(ins, f, fnmap)
            lines.extend('\t' + l for l in body)
            nexta = f.addrs[i + 1] if i + 1 < len(f.addrs) else None
            if falls and nexta != ins.next:
                lines.append('\tS.pc = %s; %s goto mismatch;' % (h(ins.next), call_expr(ins.next, fnmap) if ins.next in insns else 'sdispatch();'))
        lines.append('}')
        lines.append('')
    os.makedirs(OUTDIR, exist_ok=True)
    open(os.path.join(OUTDIR, 's65_code.cpp'), 'w').write('\n'.join(lines))
    with open(os.path.join(OUTDIR, 's65_decls.h'), 'w') as fh:
        fh.write('// Generated by tools/m6502_to_cpp.py\n#pragma once\n')
        for f in funcs:
            fh.write('void %s();\n' % f.name)
    with open(os.path.join(OUTDIR, 's65_tables.cpp'), 'w') as fh:
        fh.write(HEADER + '#include "s65_decls.h"\n\n')
        fh.write('const s65fn s65_table[0xC000] = {\n')
        row = []
        for a in range(0x4000, 0x10000):
            row.append(owner[a].name if a in owner else 'nullptr')
            if len(row) == 8:
                fh.write('\t' + ', '.join(row) + ',\n')
                row = []
        fh.write('};\n')
    print('sound program: %d instructions, %d functions' % (len(insns), len(funcs)))


if __name__ == '__main__':
    main()
