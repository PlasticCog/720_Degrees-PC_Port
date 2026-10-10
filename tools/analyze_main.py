"""Code discovery for the 720 Degrees main (T-11) program.

Recursive-descent traversal of the fixed program ROM (0x8000-0xFFFF) starting from
the reset address, following branches, jumps and calls.  Reports indirect control
flow so it can be resolved by hand (jump tables etc.), and writes a JSON database
consumed by the listing generator and the C++ translator.
"""
import sys, os, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romset, t11dis

MEM = romset.main_region()


def rw(a):
    a &= 0xffff
    return MEM[a] | (MEM[a + 1] << 8)


def in_fixed(a):
    return 0x8000 <= a <= 0xfffe


def discover(entries, extra_code=()):
    insns = {}
    funcs = set()
    work = collections.deque()
    for e in entries:
        work.append(e)
        funcs.add(e)
    for e in extra_code:
        work.append(e)
    indirect = []
    out_of_rom = collections.Counter()
    while work:
        a = work.popleft()
        while True:
            if a in insns:
                break
            if not in_fixed(a) or (a & 1):
                out_of_rom[a] += 1
                break
            ins = t11dis.decode(rw, a)
            insns[a] = ins
            nxt = (a + ins.size) & 0xffff
            f = ins.flow
            if f in ('br', 'jmp'):
                work.append(ins.target)
                break
            if f in ('cbr', 'sob'):
                work.append(ins.target)
                a = nxt
                continue
            if f == 'call':
                funcs.add(ins.target)
                work.append(ins.target)
                a = nxt
                continue
            if f == 'icall':
                indirect.append((a, 'icall', ins.text()))
                a = nxt
                continue
            if f == 'ijmp':
                indirect.append((a, 'ijmp', ins.text()))
                break
            if f in ('ret', 'rti', 'halt', 'illegal'):
                break
            if f == 'trap':
                indirect.append((a, 'trap', ins.text()))
                a = nxt
                continue
            if f == 'wait':
                a = nxt
                continue
            a = nxt
    return insns, funcs, indirect, out_of_rom


def main():
    insns, funcs, indirect, oor = discover([0x8000])
    print('instructions:', len(insns))
    print('functions   :', len(funcs))
    covered = sum(i.size for i in insns.values())
    print('bytes covered: %d of %d' % (covered, 0x8000))
    print('indirect sites:', len(indirect))
    for a, k, t in indirect:
        print('  %04X %-5s %s' % (a, k, t))
    print('targets outside fixed ROM:')
    for a, n in sorted(oor.items()):
        print('  %04X x%d' % (a, n))


if __name__ == '__main__':
    main()
