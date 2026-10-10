"""Code database for the 720 Degrees main (T-11) program.

Combines dynamic coverage/edge logs from the reference model (re/work/*/cov.txt[.edges])
with static recursive-descent traversal, per memory segment:

    'F'        fixed program ROM          0x8000-0xFFFF
    'A<n>'     ROM bank n in window A     0x4000-0x5FFF
    'B<n>'     ROM bank n in window B     0x6000-0x7FFF

Static helpers resolve jump tables (ADD tbl(Rn),PC / JMP @tbl(Rn)) and the bank used by
calls into the banked windows (by tracking writes to the bank registers 0x1400/0x1402).

Outputs re/codedb.json (consumed by the C++ translator).
"""
import sys, os, json, glob, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import romset, t11dis

ROOT = os.path.join(HERE, '..')
MEM = romset.main_region()
CSV = (0xF138, 0xF144, 0xF152, 0xF162)       # compiler register-save helpers
VECTORS = {0x84D4: 'irq_vblank_a', 0x899C: 'irq_vblank_b', 0xB66E: 'irq_32v_a', 0xB8FC: 'irq_32v_b',
           0xB9D4: 'irq_32v_c', 0x891E: 'irq_sound_default', 0x8938: 'irq_unused_a', 0x8924: 'irq_unused_b',
           0x8000: 'reset'}


def seg_of(pc, bank):
    if pc >= 0x8000:
        return 'F'
    if 0x4000 <= pc < 0x6000:
        return 'A%d' % bank
    if 0x6000 <= pc < 0x8000:
        return 'B%d' % bank
    return 'RAM'


def seg_bank(seg):
    return int(seg[1:]) if seg[0] in 'AB' else None


def rom_offset(seg, addr):
    if seg == 'F':
        return addr
    base = 0x4000 if seg[0] == 'A' else 0x6000
    return 0x10000 + seg_bank(seg) * 0x2000 + (addr - base)


def reader(seg):
    def rw(a):
        a &= 0xffff
        if a >= 0x8000:
            o = a
        elif seg != 'F' and ((seg[0] == 'A' and 0x4000 <= a < 0x6000) or (seg[0] == 'B' and 0x6000 <= a < 0x8000)):
            o = rom_offset(seg, a)
        else:
            return 0
        return MEM[o] | (MEM[o + 1] << 8)
    return rw


def window_of(addr):
    if 0x4000 <= addr < 0x6000:
        return 'A'
    if 0x6000 <= addr < 0x8000:
        return 'B'
    return None


def target_seg(from_seg, target):
    """Segment of a static control-flow target, or None if bank is unknown."""
    if target >= 0x8000:
        return 'F'
    w = window_of(target)
    if w and from_seg != 'F' and from_seg[0] == w:
        return from_seg
    return None


def in_seg(seg, a):
    if seg == 'F':
        return a >= 0x8000
    if seg[0] == 'A':
        return 0x4000 <= a < 0x6000
    return 0x6000 <= a < 0x8000


class CodeDB:
    def __init__(self):
        self.insns = {}            # (seg, addr) -> Insn
        self.covered = set()
        self.dyn_edges = collections.defaultdict(set)   # (seg,addr) -> {(seg,addr)}
        self.entries = {}          # (seg, addr) -> reason
        self.unresolved = []
        self.static_tables = {}
        self.static_banked = {}
        self.seen = set()

    def load_dynamic(self):
        files = [os.path.join(ROOT, 're', 'coverage', 'main_cov.txt')]
        files += glob.glob(os.path.join(ROOT, 're', 'work', '*', 'cov.txt'))
        for f in files:
            if not os.path.exists(f):
                continue
            for line in open(f):
                pc, b = line.split()
                pc, b = int(pc, 16), int(b)
                self.covered.add((seg_of(pc, b), pc))
            ef = f + '.edges' if not f.endswith('main_cov.txt') else f.replace('main_cov', 'main_edges')
            if os.path.exists(ef):
                for line in open(ef):
                    a, ab, t, tb = line.split()
                    a, ab, t, tb = int(a, 16), int(ab), int(t, 16), int(tb)
                    self.dyn_edges[(seg_of(a, ab), a)].add((seg_of(t, tb), t))

    def decode(self, seg, a):
        k = (seg, a)
        if k not in self.insns:
            self.insns[k] = t11dis.decode(reader(seg), a)
        return self.insns[k]

    def initial_seeds(self):
        seeds = list(sorted(self.covered))
        for k in list(self.dyn_edges.keys()):
            seeds.extend(self.dyn_edges[k])
        seeds.extend(('F', pc) for pc in VECTORS)
        return seeds

    def traverse(self, seeds):
        work = collections.deque(seeds)
        seen = self.seen
        while work:
            seg, a = work.popleft()
            while True:
                if (seg, a) in seen or seg == 'RAM' or not in_seg(seg, a) or (a & 1):
                    break
                seen.add((seg, a))
                ins = self.decode(seg, a)
                nxt = (a + ins.size) & 0xffff
                f = ins.flow
                tgt = None
                if ins.target is not None:
                    ts = target_seg(seg, ins.target)
                    if ts:
                        tgt = (ts, ins.target)
                if f in ('br', 'jmp'):
                    if tgt:
                        work.append(tgt)
                    break
                if f in ('cbr', 'sob'):
                    if tgt:
                        work.append(tgt)
                    a = nxt
                    continue
                if f == 'call':
                    if tgt:
                        work.append(tgt)
                        self.entries.setdefault(tgt, 'call')
                        if tgt[0] == 'F' and tgt[1] in CSV and ins.ops[0] == 'R1':
                            self.entries.setdefault((seg, nxt), 'csvbody')
                    a = nxt
                    continue
                if f in ('icall', 'trap'):
                    a = nxt
                    continue
                if f in ('ijmp', 'ret', 'rti', 'halt', 'illegal'):
                    break
                a = nxt

    # ---- static resolution helpers
    def predecessors(self, seg, a, n=40):
        """Linear predecessors of instruction (seg,a) among decoded instructions."""
        if getattr(self, '_ends_n', -1) != len(self.insns):
            ends = {}
            for (s2, a2), i in self.insns.items():
                ends[(s2, (a2 + i.size) & 0xffff)] = (s2, a2)
            self._ends, self._ends_n = ends, len(self.insns)
        out = []
        k = (seg, a)
        for _ in range(n):
            k = self._ends.get(k)
            if k is None:
                break
            ins = self.insns[k]
            if ins.flow in ('br', 'jmp', 'ijmp', 'ret', 'rti'):
                break
            out.append(ins)
        return out

    def static_bank(self, seg, a, window):
        reg = 0x1400 if window == 'A' else 0x1402
        preds = self.predecessors(seg, a)
        for idx, ins in enumerate(preds):
            if ins.mn in ('MOV', 'CLR') and ins.ops:
                d = ins.ops[-1]
                if not isinstance(d, str) and d.kind == 'abs' and d.ext == reg:
                    if ins.mn == 'CLR':
                        return romset.bank_entry(0)
                    sop = ins.ops[0]
                    if sop.kind == 'imm':
                        return romset.bank_entry(sop.ext)
                    if sop.kind == 'reg':
                        for p2 in preds[idx + 1:]:
                            if p2.mn == 'MOV' and not isinstance(p2.ops[1], str) and p2.ops[1].kind == 'reg' \
                                    and p2.ops[1].reg == sop.reg and p2.ops[0].kind == 'imm':
                                return romset.bank_entry(p2.ops[0].ext)
                    return None
        return None

    def jump_table(self, seg, a, ins):
        """ADD tbl(Rn),PC: relative offset table; JMP @tbl(Rn): absolute pointer table."""
        src = ins.ops[0]
        if isinstance(src, str) or src.mode not in (6, 7) or src.reg == 7:
            return []
        base = src.ext
        rn = src.reg
        if base < 0x4000:
            return []
        bound = None
        for p in self.predecessors(seg, a, 12):
            if p.mn == 'CMP' and not isinstance(p.ops[0], str):
                o0, o1 = p.ops
                if o0.kind == 'reg' and o0.reg == rn and o1.kind == 'imm':
                    bound = o1.ext
                    break
                if o1.kind == 'reg' and o1.reg == rn and o0.kind == 'imm':
                    bound = o0.ext
                    break
        rwb = reader(seg) if base < 0x8000 else reader('F')
        out = []
        n = (bound // 2 + 1) if bound is not None and bound < 0x200 else 64
        for i in range(n):
            e = rwb(base + 2 * i)
            t = ((a + ins.size + e) & 0xffff) if ins.mn == 'ADD' else e
            ts = target_seg(seg, t)
            if ts is None or (t & 1):
                if bound is None:
                    break
                continue
            if bound is None:
                if abs(t - a) > 0x1000:
                    break
                if t11dis.decode(reader(ts), t).flow == 'illegal':
                    break
            out.append((ts, t))
        return out

    def resolve_static(self):
        new = []
        for (seg, a), ins in list(self.insns.items()):
            if (seg, a) not in self.seen:
                continue
            if ins.flow == 'ijmp' and ins.mn in ('ADD', 'JMP') and (seg, a) not in self.static_tables:
                tg = self.jump_table(seg, a, ins)
                self.static_tables[(seg, a)] = tg
                new.extend(tg)
            if ins.flow == 'call' and ins.target is not None and target_seg(seg, ins.target) is None \
                    and (seg, a) not in self.static_banked:
                w = window_of(ins.target)
                b = self.static_bank(seg, a, w) if w else None
                self.static_banked[(seg, a)] = b
                if b is not None:
                    t = ('%s%d' % (w, b), ins.target)
                    self.entries.setdefault(t, 'call')
                    new.append(t)
        return new

    def classify(self):
        """Add dynamic call/jump targets as entries; record indirect sites."""
        self.indirect = {}
        self.banked_calls = {}
        for (seg, a), ins in self.insns.items():
            if (seg, a) not in self.seen:
                continue
            if ins.flow in ('icall', 'ijmp', 'call', 'jmp', 'trap'):
                tg = set(t for t in self.dyn_edges.get((seg, a), set()) if not (t[1] in VECTORS and t[1] != ins.target))
                if ins.flow in ('icall', 'ijmp', 'trap'):
                    tg |= set(self.static_tables.get((seg, a), []))
                    self.indirect[(seg, a)] = sorted(tg)
                    if not tg and (seg, a) in self.covered and not (ins.mn == 'JSR' and ins.ops[1].text() == '@(SP)+'):
                        self.unresolved.append((seg, a, ins.text()))
                if ins.flow in ('call', 'icall'):
                    for t in tg:
                        self.entries.setdefault(t, 'call')
                if ins.flow == 'call' and target_seg(seg, ins.target) is None:
                    sb = self.static_banked.get((seg, a))
                    if sb is not None:
                        tg.add(('%s%d' % (window_of(ins.target), sb), ins.target))
                    self.banked_calls[(seg, a)] = sorted(tg)
        for pc, name in VECTORS.items():
            self.entries[('F', pc)] = name

    def save(self):
        seen = self.seen
        out = {
            'insns': sorted('%s:%04X:%d' % (s, a, i.size) for (s, a), i in self.insns.items() if (s, a) in seen),
            'entries': sorted('%s:%04X:%s' % (s, a, r) for (s, a), r in self.entries.items()),
            'indirect': {('%s:%04X' % k): ['%s:%04X' % t for t in v] for k, v in self.indirect.items()},
            'banked_calls': {('%s:%04X' % k): ['%s:%04X' % t for t in v] for k, v in self.banked_calls.items()},
            'covered': sorted('%s:%04X' % k for k in self.covered),
        }
        json.dump(out, open(os.path.join(ROOT, 're', 'codedb.json'), 'w'), indent=0)


def build():
    db = CodeDB()
    db.load_dynamic()
    db.traverse(db.initial_seeds())
    while True:
        before = len(db.seen)
        db.traverse(db.resolve_static())
        if len(db.seen) == before:
            break
    db.classify()
    return db


def main():
    db = build()
    db.save()
    segs = collections.Counter(s for s, a in db.seen)
    cov = collections.Counter(s for s, a in db.covered)
    print('instructions per segment (static+dynamic / covered):')
    for s in sorted(segs):
        print('  %-4s %6d %6d' % (s, segs[s], cov[s]))
    print('entries:', len(db.entries))
    print('indirect sites:', len(db.indirect))
    for k, v in sorted(db.indirect.items()):
        print('  %s:%04X  %-28s -> %s' % (k[0], k[1], db.insns[k].text(), ' '.join('%s:%04X' % t for t in v[:10]) + (' ...(%d)' % len(v) if len(v) > 10 else '')))
    unk = [(k, v) for k, v in sorted(db.banked_calls.items()) if not v]
    print('banked calls: %d, with unknown bank: %d' % (len(db.banked_calls), len(unk)))
    for k, v in unk:
        print('  %s:%04X  %s' % (k[0], k[1], db.insns[k].text()))
    print('unresolved covered indirect sites:', db.unresolved)


if __name__ == '__main__':
    main()
