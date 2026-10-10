"""Static translator: 720 Degrees main program (DEC T-11 machine code) -> C++.

Every instruction found by codedb.py is translated into C++ statements with exactly the
semantics of the original CPU (flags, bus access order, MAME cycle counts).  Instructions
are grouped into functions (one per call target / interrupt entry); branches become goto,
calls become C++ calls, and every instruction ends with a cheap checkpoint (`sync`) where
interrupts and time-slice ends are handled.

Output: port/src/game/t11_<segment>.cpp and t11_tables.cpp

Rewritten routines: port/src/logic/routines.txt lists the routines that have a hand-written
C++ replacement (port/src/logic).  For each, the translated function is renamed fn_..._orig
and a small wrapper is generated that hands calls to the replacement (logic::enter); the
original stays available for the difference tester and for falling back.
"""
import sys, os, re, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import codedb, t11dis

ROOT = os.path.join(HERE, '..')
OUTDIR = os.path.join(ROOT, 'port', 'src', 'game')

# ------------------------------------------------------------------ cycle table (from MAME, see t11_cycles.txt)
HANDLERS, CYCLES = [], {}
for _line in open(os.path.join(HERE, 't11_cycles.txt')):
    if _line.startswith('#') or not _line.strip():
        continue
    _n, _c = _line.split()
    HANDLERS.append(_n)
    CYCLES.setdefault(_n, int(_c))
assert len(HANDLERS) == 8192


def handler(op):
    return HANDLERS[op >> 3]


def kind_of(op):
    return re.sub(r'_(rg|rgd|in|ind|de|ded|ix|ixd)(_(rg|rgd|in|ind|de|ded|ix|ixd))?$', '', handler(op))


SYMBOLS = {}
_symf = os.path.join(ROOT, 're', 'symbols.txt')
if os.path.exists(_symf):
    for line in open(_symf):
        line = line.split('#')[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            SYMBOLS[parts[0].upper()] = parts[1]


def fn_name(seg, addr):
    base = 'fn_%s_%04X' % (seg, addr)
    sym = SYMBOLS.get('%s:%04X' % (seg, addr))
    return base + ('_' + sym if sym else '')


def h(v):
    return '0x%04X' % (v & 0xffff)


# routines with a hand-written replacement (port/src/logic/routines.txt)
OVERRIDES = []          # [(seg, addr, name)]
_ovf = os.path.join(ROOT, 'port', 'src', 'logic', 'routines.txt')
if os.path.exists(_ovf):
    for line in open(_ovf):
        line = line.split('#')[0].strip()
        if not line:
            continue
        parts = line.split()
        seg, addr = parts[0].upper().split(':')
        OVERRIDES.append((seg, int(addr, 16), parts[1] if len(parts) > 1 else ''))


# immediates that mods may change at runtime (see port/src/native/t11rt.h)
# immediates turned into runtime variables: level mods (block banks) and the debug menu (physics)
PATCH_IMM = {('F', 0xE8B6): ('g_patch_town_alt_base', 0x6000), ('F', 0xE8B0): ('g_patch_park_odd_base', 0xA000),
             ('F', 0xA068): ('g_patch_gravity', 0x000C),          # MOV #12,@#0x02EE   gravity while airborne
             ('A5', 0x4F2C): ('g_patch_kick_speed', 0x0180)}      # MOV #0x180,@#0x01B4 speed of a kick from standstill


class Unsupported(Exception):
    pass


class InsnGen:
    """Generates C++ for one instruction."""

    def __init__(self, seg, ins, fn, fnmap):
        self.seg, self.ins, self.fn, self.fnmap = seg, ins, fn, fnmap
        self.A = ins.addr
        self.N = (ins.addr + ins.size) & 0xffff
        self.op = ins.op
        self.wi = 1
        self.pc = (self.A + 2) & 0xffff
        self.out = []
        self.dst_ea = None
        self.falls = True

    # ---- helpers
    def emit(self, s):
        self.out.append(s)

    def ext(self):
        w = self.ins.words[self.wi]
        addr = self.pc
        self.wi += 1
        self.pc = (self.pc + 2) & 0xffff
        if 0x8000 <= addr < 0x8040:
            self.emit('slap(%s);' % h(addr))
        return w

    def reg(self, r):
        return h(self.pc) if r == 7 else 'T.r[%d]' % r

    def rd(self, ea, byte):
        """read expression for ea = ('c', const) or ('v', expr)"""
        k, v = ea
        if k == 'c':
            if byte:
                if v < 0x1000:
                    return 'RAM[%s]' % h(v)
                if v >= 0x8040:
                    return 'ROM[%s]' % h(v)
                return 'rb(%s)' % h(v)
            v &= 0xfffe
            if v < 0x1000:
                return 'ram16(%s)' % h(v)
            if v >= 0x8040:
                return 'rom16(%s)' % h(v)
            return 'rw(%s)' % h(v)
        return ('rb(%s)' if byte else 'rw(%s)') % v

    def wr(self, ea, byte, val):
        k, v = ea
        if k == 'c':
            if byte:
                if v < 0x1000:
                    return 'RAM[%s] = (uint8_t)(%s);' % (h(v), val)
                return 'wb(%s, %s);' % (h(v), val)
            v &= 0xfffe
            if v < 0x1000:
                return 'ram16w(%s, %s);' % (h(v), val)
            return 'ww(%s, %s);' % (h(v), val)
        return ('wb(%s, %s);' if byte else 'ww(%s, %s);') % (v, val)

    def dummy_rd(self, ea, byte):
        k, v = ea
        if k == 'c' and (v < 0x1000 or v >= 0x8040):
            return  # no side effects
        self.emit('(void)%s;' % self.rd(ea, byte))

    def ea(self, m, r, byte, opfetch=True):
        """effective address of a memory operand; returns ('c', const) or ('v', 'ea')"""
        step = 1 if (byte and r < 6) else 2
        if r == 7:
            if m == 1:
                return ('c', self.pc)
            if m == 2:
                a = self.pc
                self.ext()
                return ('c', a)
            if m == 3:
                return ('c', self.ext())
            if m == 6:
                x = self.ext()
                return ('c', (x + self.pc) & 0xffff)
            if m == 7:
                x = self.ext()
                self.emit('ea = %s;' % self.rd(('c', (x + self.pc) & 0xffff), False))
                return ('v', 'ea')
            raise Unsupported('PC mode %d' % m)
        R = 'T.r[%d]' % r
        if m == 1:
            self.emit('ea = %s;' % R)
        elif m == 2:
            self.emit('ea = %s; %s += %d;' % (R, R, step))
        elif m == 3:
            self.emit('ea = %s; %s += 2; ea = rw(ea);' % (R, R))
        elif m == 4:
            self.emit('%s -= %d; ea = %s;' % (R, step, R))
        elif m == 5:
            self.emit('%s -= 2; ea = rw(%s);' % (R, R))
        elif m == 6:
            x = self.ext()
            self.emit('ea = (%s + %s) & 0xffff;' % (h(x), R))
        elif m == 7:
            x = self.ext()
            self.emit('ea = rw((%s + %s) & 0xffff);' % (h(x), R))
        return ('v', 'ea')

    # source operand -> C++ variable s
    def src(self, byte):
        m, r = (self.op >> 9) & 7, (self.op >> 6) & 7
        if m == 0:
            v = self.reg(r)
            self.emit('s = %s;' % (('(%s) & 0xff' % v) if byte else v))
            return
        if r == 7 and m == 2:
            imm = self.ext()
            pp = PATCH_IMM.get((self.seg, self.A))
            if pp:
                assert imm == pp[1], 'patch point mismatch at %s:%04X' % (self.seg, self.A)
                self.emit('s = %s;   // patch point (default %s)' % (pp[0], h(imm)))
            else:
                self.emit('s = %s;' % h(imm))     # immediate: full word even for byte ops
            return
        ea = self.ea(m, r, byte)
        self.emit('s = %s;' % self.rd(ea, byte))

    # destination read (RMW) -> d, remembers ea
    def dst_get(self, byte):
        m, r = (self.op >> 3) & 7, self.op & 7
        if m == 0:
            v = self.reg(r)
            self.emit('d = %s;' % (('(%s) & 0xff' % v) if byte else v))
            self.dst_ea = None
            return
        ea = self.ea(m, r, byte)
        self.dst_ea = ea
        self.emit('d = %s;' % self.rd(ea, byte))

    def dst_put(self, byte, val):
        m, r = (self.op >> 3) & 7, self.op & 7
        if m == 0:
            if r == 7:
                self.emit('T.r[7] = (uint16_t)(%s);' % (('(%s) & 0xff' % val) if byte else val))
                self.jump_dynamic = True
                return
            if byte:
                self.emit('T.r[%d] = (uint16_t)((T.r[%d] & 0xff00) | ((%s) & 0xff));' % (r, r, val))
            else:
                self.emit('T.r[%d] = (uint16_t)(%s);' % (r, val))
            return
        self.emit(self.wr(self.dst_ea, byte, val))

    def dst_put_t11(self, byte, val, read_first=True):
        """MOV/MOVB/CLR/CLRB/SXT destination: T-11 read-before-write for memory"""
        m, r = (self.op >> 3) & 7, self.op & 7
        if m == 0:
            if r == 7:
                self.emit('T.r[7] = (uint16_t)(%s);' % val)
                self.jump_dynamic = True
                return
            self.emit('T.r[%d] = (uint16_t)(%s);' % (r, val))
            return
        ea = self.ea(m, r, byte)
        if read_first:
            self.dummy_rd(ea, byte)
        self.emit(self.wr(ea, byte, val))

    # ---- control helpers
    def goto(self, target, cond=None):
        local = target in self.fn.labels
        body = []
        if local:
            body.append('if (!sync(%s)) goto mismatch;' % h(target))
            body.append('goto L_%04X;' % target)
        else:
            body.append('if (!sync(%s)) goto mismatch;' % h(target))
            body.append('T.r[7] = %s; %s goto mismatch;' % (h(target), self.call_expr(target)))
        if cond:
            self.emit('if (%s) { %s }' % (cond, ' '.join(body)))
        else:
            self.out.extend(body)

    def call_expr(self, target):
        """C++ statement calling the code at target (T.r[7] already == target)"""
        if target >= 0x8000 and ('F', target) in self.fnmap:
            return '%s();' % self.fnmap[('F', target)]
        if self.seg != 'F' and codedb.target_seg(self.seg, target) == self.seg and (self.seg, target) in self.fnmap:
            return '%s();' % self.fnmap[(self.seg, target)]
        return 'dispatch();'

    def fallthrough(self):
        self.emit('if (!sync(%s)) goto mismatch;' % h(self.N))

    # ---- main
    def gen(self):
        op = self.op
        k = kind_of(op)
        hname = handler(op)
        self.jump_dynamic = False
        dm, dr = (op >> 3) & 7, op & 7
        sr = (op >> 6) & 7
        if self.A < 0x8040 and self.A >= 0x8000:
            self.emit('slap(%s);' % h(self.A))

        def cyc(n):
            self.emit('T.icount -= %d;' % n)

        if k in ('op_0001', 'mark', 'illegal', 'mul', 'div', 'ash', 'ashc'):
            cyc(48)
            self.emit('trap_to(010, %s); if (T.r[7] != %s) goto mismatch;' % (h(self.N), h(self.N)))
            return self.out
        if k == 'illegal4':
            cyc(48)
            self.emit('trap_to(004, %s); if (T.r[7] != %s) goto mismatch;' % (h(self.N), h(self.N)))
            return self.out
        if k == 'op_0000':
            sub = op & 0x3f
            if sub == 0:
                cyc(48)
                self.emit('halt_insn(%s); goto mismatch;' % h(self.N))
                self.falls = False
            elif sub == 1:
                self.emit('T.icount = 0; T.wait_state = true;')
                self.fallthrough()
            elif sub in (2, 6):
                cyc(24 if sub == 2 else 33)
                self.emit('T.r[7] = pop(); T.psw = (uint8_t)pop(); T.check_irqs = true;')
                self.emit('sync_dyn(); return;')
                self.falls = False
            elif sub in (3, 4):
                cyc(48)
                self.emit('trap_to(%s, %s); if (T.r[7] != %s) goto mismatch;' % ('014' if sub == 3 else '020', h(self.N), h(self.N)))
            elif sub == 5:
                cyc(110)
                self.fallthrough()
            elif sub == 7:
                self.emit('T.r[0] = (uint16_t)((T.r[0] & 0xff00) | 4);')
                self.fallthrough()
            else:
                cyc(48)
                self.emit('trap_to(010, %s); if (T.r[7] != %s) goto mismatch;' % (h(self.N), h(self.N)))
            return self.out

        cyc(CYCLES[hname])
        B = k.endswith('b') and k not in ('sob',)
        if k in ('mov',):
            self.src(False)
            self.emit('f_nzv(nzw(s));')
            self.dst_put_t11(False, 's')
        elif k == 'movb':
            self.src(True)
            self.emit('f_nzv(nzb(s));')
            if dm == 0:
                if dr == 7:
                    self.emit('T.r[7] = (uint16_t)(int16_t)(int8_t)s;')
                    self.jump_dynamic = True
                else:
                    self.emit('T.r[%d] = (uint16_t)(int16_t)(int8_t)s;' % dr)
            else:
                self.dst_put_t11(True, 's')
        elif k in ('cmp', 'cmpb'):
            self.src(B); self.dst_get(B)
            w = 'b' if B else 'w'
            self.emit('res = s - d; f_nzvc(nz%s(res) | v%s(s, d, res) | c%s(res));' % (w, w, w))
        elif k in ('bit', 'bitb'):
            self.src(B); self.dst_get(B)
            self.emit('res = d & s; f_nzv(nz%s(res));' % ('b' if B else 'w'))
        elif k in ('bic', 'bicb', 'bis', 'bisb'):
            self.src(B); self.dst_get(B)
            opx = '& ~' if k.startswith('bic') else '|'
            self.emit('res = d %s s; f_nzv(nz%s(res));' % (opx, 'b' if B else 'w'))
            self.dst_put(B, 'res')
        elif k in ('add', 'sub'):
            self.src(False); self.dst_get(False)
            self.emit('res = d %s s; f_nzvc(nzw(res) | vw(s, d, res) | cw(res));' % ('+' if k == 'add' else '-'))
            self.dst_put(False, 'res')
        elif k == 'xor':
            self.emit('s = %s;' % self.reg(sr))
            self.dst_get(False)
            self.emit('res = d ^ s; f_nzv(nzw(res));')
            self.dst_put(False, 'res')
        elif k in ('clr', 'clrb'):
            if dm == 0:
                if k == 'clr':
                    self.emit('T.r[%d] = 0;' % dr)
                else:
                    self.emit('T.r[%d] &= 0xff00;' % dr)
                if dr == 7:
                    self.jump_dynamic = True
            else:
                self.dst_put_t11(k == 'clrb', '0')
            self.emit('f_nzvc(ZF);')
        elif k in ('com', 'comb'):
            self.dst_get(B)
            self.emit('res = ~d; f_nzvc(nz%s(res) | CF);' % ('b' if B else 'w'))
            self.dst_put(B, 'res')
        elif k in ('inc', 'incb', 'dec', 'decb'):
            self.dst_get(B)
            inc = k.startswith('inc')
            lim = ('0x7f' if inc else '0x80') if B else ('0x7fff' if inc else '0x8000')
            self.emit('res = d %s 1; f_nzv(nz%s(res) | (d == %s ? VF : 0));' % ('+' if inc else '-', 'b' if B else 'w', lim))
            self.dst_put(B, 'res')
        elif k in ('neg', 'negb'):
            self.dst_get(B)
            lim = '0x80' if B else '0x8000'
            self.emit('res = -d; f_nzvc(nz%s(res) | (d == %s ? VF : 0) | (res ? CF : 0));' % ('b' if B else 'w', lim))
            self.dst_put(B, 'res')
        elif k in ('adc', 'adcb', 'sbc', 'sbcb'):
            self.emit('s = C();')
            self.dst_get(B)
            w = 'b' if B else 'w'
            self.emit('res = d %s s; f_nzvc(nz%s(res) | v%s(s, d, res) | c%s(res));' % ('+' if k.startswith('adc') else '-', w, w, w))
            self.dst_put(B, 'res')
        elif k in ('tst', 'tstb'):
            self.dst_get(B)
            self.emit('f_nzvc(nz%s(d));' % ('b' if B else 'w'))
        elif k in ('ror', 'rorb'):
            self.dst_get(B)
            self.emit('res = (d >> 1) | (C() << %d); f_nzvc(nz%s(res) | (d & 1)); f_shift();' % (7 if B else 15, 'b' if B else 'w'))
            self.dst_put(B, 'res')
        elif k in ('rol', 'rolb'):
            self.dst_get(B)
            self.emit('res = (d << 1) | C(); f_nzvc(nz%s(res) | ((d >> %d) & 1)); f_shift();' % ('b' if B else 'w', 7 if B else 15))
            self.dst_put(B, 'res')
        elif k in ('asr', 'asrb'):
            self.dst_get(B)
            self.emit('res = (d >> 1) | (d & %s); f_nzvc(nz%s(res) | (d & 1)); f_shift();' % ('0x80' if B else '0x8000', 'b' if B else 'w'))
            self.dst_put(B, 'res')
        elif k in ('asl', 'aslb'):
            self.dst_get(B)
            self.emit('res = d << 1; f_nzvc(nz%s(res) | ((d >> %d) & 1)); f_shift();' % ('b' if B else 'w', 7 if B else 15))
            self.dst_put(B, 'res')
        elif k == 'swab':
            self.dst_get(False)
            self.emit('res = ((d >> 8) & 0xff) + (d << 8); f_nzvc(nzb(res));')
            self.dst_put(False, 'res')
        elif k == 'sxt':
            self.emit('T.psw &= ~(ZF | VF); if (fN()) res = -1; else { res = 0; T.psw |= ZF; }')
            self.dst_put_t11(False, 'res')
        elif k == 'mtps':
            self.dst_get(True)
            self.emit('T.psw = (uint8_t)((T.psw & ~0xef) | (d & 0xef)); T.check_irqs = true;')
        elif k == 'mfps':
            self.emit('res = T.psw; f_nzv(nzb(res));')
            if dm == 0:
                self.emit('T.r[%d] = (uint16_t)(int16_t)(int8_t)res;' % dr)
            else:
                self.dst_put_t11(True, 'res', read_first=False)
        elif k == 'jmp':
            ea = self.ea(dm, dr, False)
            self.falls = False
            if ea[0] == 'c':
                self.goto(ea[1])
                return self.out
            self.emit('T.r[7] = (uint16_t)ea;')
            self.emit('sync_dyn(); goto dispatch_local;')
            return self.out
        elif k == 'jsr':
            ea = self.ea(dm, dr, False)
            link = h(self.N) if sr == 7 else 'T.r[%d]' % sr
            if ea[0] == 'v':
                self.emit('{ uint16_t t = (uint16_t)ea; push(%s);%s T.r[7] = t; }' % (link, '' if sr == 7 else ' T.r[%d] = %s;' % (sr, h(self.N))))
                self.emit('sync_dyn(); shadow_push(%s); dispatch(); shadow_pop();' % h(self.N))
            else:
                t = ea[1]
                self.emit('push(%s);%s T.r[7] = %s;' % (link, '' if sr == 7 else ' T.r[%d] = %s;' % (sr, h(self.N)), h(t)))
                self.emit('if (!sync(%s)) goto mismatch; shadow_push(%s); %s shadow_pop();' % (h(t), h(self.N), self.call_expr(t)))
            self.emit('if (T.r[7] != %s) goto mismatch;' % h(self.N))
            return self.out
        elif k == 'rts':
            if dr == 7:
                self.emit('T.r[7] = pop();')
            else:
                self.emit('T.r[7] = T.r[%d]; T.r[%d] = pop();' % (dr, dr))
            self.emit('sync_dyn(); return;')
            self.falls = False
            return self.out
        elif k in ('ccc', 'scc'):
            if op & 15:
                self.emit('T.psw %s;' % (('&= (uint8_t)~%d' % (op & 15)) if k == 'ccc' else ('|= %d' % (op & 15))))
        elif k in ('br', 'bne', 'beq', 'bge', 'blt', 'bgt', 'ble', 'bpl', 'bmi', 'bhi', 'blos', 'bvc', 'bvs', 'bcc', 'bcs'):
            cond = {'br': None, 'bne': '!fZ()', 'beq': 'fZ()', 'bge': 'fN() == fV()', 'blt': 'fN() != fV()',
                    'bgt': '!fZ() && fN() == fV()', 'ble': 'fZ() || fN() != fV()', 'bpl': '!fN()', 'bmi': 'fN()',
                    'bhi': '!fC() && !fZ()', 'blos': 'fC() || fZ()', 'bvc': '!fV()', 'bvs': 'fV()',
                    'bcc': '!fC()', 'bcs': 'fC()'}[k]
            self.goto(self.ins.target, cond)
            if cond is None:
                self.falls = False
                return self.out
        elif k == 'sob':
            self.emit('T.r[%d] -= 1;' % sr)
            self.goto(self.ins.target, 'T.r[%d] != 0' % sr)
        elif k in ('emt', 'trap'):
            self.emit('trap_to(%s, %s); if (T.r[7] != %s) goto mismatch;' % ('030' if k == 'emt' else '034', h(self.N), h(self.N)))
            return self.out
        else:
            raise Unsupported(k)

        if self.jump_dynamic:
            self.emit('sync_dyn(); goto dispatch_local;')
            self.falls = False
            return self.out
        self.fallthrough()
        return self.out


class Func:
    def __init__(self, seg, entry):
        self.seg, self.entry = seg, entry
        self.addrs = []
        self.labels = set()
        self.name = fn_name(seg, entry)


def build():
    db = codedb.build()
    by_seg = collections.defaultdict(list)
    for (s, a) in db.seen:
        by_seg[s].append(a)
    entries_by_seg = collections.defaultdict(set)
    for (s, a) in db.entries:
        if (s, a) in db.seen:
            entries_by_seg[s].add(a)
    funcs = []
    fnmap = {}          # (seg, entry) -> name  (only true entries)
    owner = {}          # (seg, addr) -> Func
    for s, addrs in by_seg.items():
        addrs.sort()
        ents = entries_by_seg[s]
        cur = None
        for a in addrs:
            if cur is None or a in ents:
                cur = Func(s, a)
                funcs.append(cur)
            cur.addrs.append(a)
            cur.labels.add(a)
            owner[(s, a)] = cur
    for f in funcs:
        fnmap[(f.seg, f.entry)] = f.name
    return db, funcs, fnmap, owner, by_seg


HEADER = '''// Generated by tools/t11_to_cpp.py from the 720 Degrees program ROMs. Do not edit by hand:
// fix the translator or re/symbols.txt and regenerate.
#include "t11rt.h"
using namespace t11rt;
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
'''


def gen_function(db, f, fnmap, override_index=None):
    out = []
    out.append('// ---- %s:%04X%s' % (f.seg, f.entry, ('  ' + db.entries.get((f.seg, f.entry), '')) if (f.seg, f.entry) in db.entries else ''))
    if override_index is not None:
        # hand-written replacement in port/src/logic; the translation stays as _orig
        out.append('void %s_orig();' % f.name)
        out.append('void %s()' % f.name)
        out.append('{')
        out.append('\tif (T.r[7] == 0x%04X && logic::enter(%d)) return;' % (f.entry, override_index))
        out.append('\t%s_orig();' % f.name)
        out.append('}')
        out.append('void %s_orig()' % f.name)
    else:
        out.append('void %s()' % f.name)
    out.append('{')
    out.append('\tint s = 0, d = 0, res = 0; uint32_t ea = 0;')
    out.append('dispatch_local:')
    out.append('\tswitch (T.r[7]) {')
    for a in f.addrs:
        out.append('\tcase 0x%04X: goto L_%04X;' % (a, a))
    out.append('\tdefault: return;')
    out.append('\t}')
    out.append('mismatch:')
    out.append('\tif (in_shadow(T.r[7])) return;')
    out.append('\tgoto dispatch_local;')
    unsupported = 0
    for i, a in enumerate(f.addrs):
        ins = db.insns[(f.seg, a)]
        out.append('L_%04X: // %s' % (a, ins.text()))
        g = InsnGen(f.seg, ins, f, fnmap)
        try:
            lines = g.gen()
        except Unsupported as e:
            unsupported += 1
            g.falls = False
            lines = ['T.r[7] = 0x%04X; fatal_unknown_pc(0x%04X); // unsupported: %s' % (a, a, e)]
        out.extend('	' + l for l in lines)
        nxt = (a + ins.size) & 0xffff
        nexta = f.addrs[i + 1] if i + 1 < len(f.addrs) else None
        if g.falls and nexta != nxt:
            # sequential flow continues in code owned by another function
            out.append('	T.r[7] = %s; %s goto mismatch;' % (h(nxt), g.call_expr(nxt)))
    out.append('}')
    out.append('')
    return out, unsupported


def main():
    db, funcs, fnmap, owner, by_seg = build()
    os.makedirs(OUTDIR, exist_ok=True)
    for f in os.listdir(OUTDIR):
        if f.startswith('t11_') and f.endswith('.cpp'):
            os.remove(os.path.join(OUTDIR, f))
    # group functions into files (~1500 instructions per file)
    files = collections.defaultdict(list)
    for f in funcs:
        files[f.seg].append(f)
    total_unsupported = 0
    decls = []
    funcs_by_entry = {(f.seg, f.entry): f for f in funcs}
    override_index = {}
    override_rows = []
    for (seg, addr, oname) in OVERRIDES:
        f = funcs_by_entry.get((seg, addr))
        if f is None:
            print('warning: rewritten routine %s:%04X (%s) is not a routine entry; ignored' % (seg, addr, oname))
            continue
        # size of the routine: its entry plus a body reached through the C register-save helper
        size = len(f.addrs)
        body = funcs_by_entry.get((seg, addr + 4))
        if body is not None and db.entries.get((seg, addr + 4)) == 'csvbody':
            size += len(body.addrs)
        override_index[(seg, addr)] = len(override_rows)
        override_rows.append((seg, addr, f.name, size))
    file_list = []
    for seg, fl in sorted(files.items()):
        fl.sort(key=lambda x: x.entry)
        chunk, n, part = [], 0, 0
        chunks = []
        for f in fl:
            chunk.append(f)
            n += len(f.addrs)
            if n > 1500:
                chunks.append(chunk)
                chunk, n = [], 0
        if chunk:
            chunks.append(chunk)
        for ci, ch in enumerate(chunks):
            name = 't11_%s_%d.cpp' % (seg, ci)
            lines = [HEADER, '#include "t11_decls.h"', '#include "logic.h"', '']
            for f in ch:
                body, u = gen_function(db, f, fnmap, override_index.get((f.seg, f.entry)))
                total_unsupported += u
                lines.extend(body)
            open(os.path.join(OUTDIR, name), 'w').write('\n'.join(lines))
            file_list.append(name)
    # declarations
    with open(os.path.join(OUTDIR, 't11_decls.h'), 'w') as fh:
        fh.write('// Generated by tools/t11_to_cpp.py\n#pragma once\n')
        for f in funcs:
            fh.write('void %s();\n' % f.name)
    # dispatch tables
    with open(os.path.join(OUTDIR, 't11_tables.cpp'), 'w') as fh:
        fh.write(HEADER + '#include "t11_decls.h"\n\n')
        segs_present = sorted(by_seg.keys())
        for s in segs_present:
            base = 0x8000 if s == 'F' else (0x4000 if s[0] == 'A' else 0x6000)
            size = 0x8000 if s == 'F' else 0x2000
            arr = ['nullptr'] * (size // 2)
            for a in by_seg[s]:
                arr[(a - base) // 2] = owner[(s, a)].name
            fh.write('static const t11fn tab_%s[%d] = {\n' % (s, size // 2))
            for i in range(0, len(arr), 4):
                fh.write('\t' + ', '.join(arr[i:i + 4]) + ',\n')
            fh.write('};\n')
            fh.write('static const T11CodeSeg seg_%s = { 0x%04X, 0x%04X, tab_%s };\n\n' % (s, base, size, s))
        fh.write('const T11CodeSeg t11_seg_fixed = { 0x8000, 0x8000, tab_F };\n')
        fh.write('const T11CodeSeg *t11_seg_bank[2][64] = {\n')
        for w in 'AB':
            row = []
            for b in range(64):
                s = '%s%d' % (w, b)
                row.append('&seg_%s' % s if s in by_seg else 'nullptr')
            fh.write('\t{ ' + ', '.join(row) + ' },\n')
        fh.write('};\n')
    # rewritten routines: table used by port/src/logic to find originals, plus size statistics
    with open(os.path.join(OUTDIR, 't11_overrides.cpp'), 'w') as fh:
        fh.write(HEADER + '#include "t11_decls.h"\n#include "logic.h"\n\n')
        for (seg, addr, name, size) in override_rows:
            fh.write('void %s_orig();\n' % name)
        fh.write('\nextern const logic::Original t11_originals[] = {\n')
        for (seg, addr, name, size) in override_rows:
            fh.write('\t{ "%s", 0x%04X, %s_orig, %d },\n' % (seg, addr, name, size))
        fh.write('\t{ nullptr, 0, nullptr, 0 }\n};\n')
        fh.write('extern const int t11_original_count = %d;\n' % len(override_rows))
        fh.write('extern const int t11_total_instructions = %d;\n' % sum(len(f.addrs) for f in funcs))
    file_list.append('t11_overrides.cpp')
    with open(os.path.join(OUTDIR, 'files.cmake'), 'w') as fh:
        fh.write('set(T11_GENERATED\n')
        for n in file_list + ['t11_tables.cpp']:
            fh.write('  src/game/%s\n' % n)
        fh.write(')\n')
    print('functions: %d, instructions: %d, files: %d, unsupported: %d, rewritten routines: %d' %
          (len(funcs), sum(len(f.addrs) for f in funcs), len(file_list), total_unsupported, len(override_rows)))


if __name__ == '__main__':
    main()
