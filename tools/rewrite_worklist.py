"""Worklist for rewriting the main program's routines (see port/src/logic).

    python tools/rewrite_worklist.py [--all] [--limit N]

Lists the routines of the translated program that do not have a hand-written replacement
yet, with their size and the routines they call.  Routines whose callees are all rewritten
(or that call nothing) come first: they can be replaced and tested on their own.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import t11_to_cpp  # noqa: E402

CSV_HELPERS = {('F', 0xF138), ('F', 0xF144), ('F', 0xF152), ('F', 0xF162)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--all', action='store_true', help='include rewritten routines')
    ap.add_argument('--limit', type=int, default=60)
    args = ap.parse_args()

    db, funcs, fnmap, owner, by_seg = t11_to_cpp.build()
    done = {(s, a) for (s, a, _) in t11_to_cpp.OVERRIDES}
    by_entry = {(f.seg, f.entry): f for f in funcs}
    routines = []
    for (seg, a), kind in sorted(db.entries.items()):
        if kind == 'csvbody' or (seg, a) not in by_entry or (seg, a) in CSV_HELPERS:
            continue
        parts = [by_entry[(seg, a)]]
        if db.entries.get((seg, a + 4)) == 'csvbody' and (seg, a + 4) in by_entry:
            parts.append(by_entry[(seg, a + 4)])
        size = sum(len(p.addrs) for p in parts)
        callees = set()
        for p in parts:
            for x in p.addrs:
                ins = db.insns[(p.seg, x)]
                if ins.flow == 'call' and ins.target is not None:
                    t = ins.target
                    tseg = 'F' if t >= 0x8000 else None
                    if tseg is None:
                        # banked target: resolved banks recorded by codedb
                        for s in db.banked_calls.get((p.seg, x), []):
                            callees.add(s)
                        continue
                    if ('F', t) not in CSV_HELPERS:
                        callees.add(('F', t))
                elif ins.flow == 'icall':
                    callees.add(('?', 0))
        covered = any((p.seg, x) in db.covered for p in parts for x in p.addrs) if hasattr(db, 'covered') else True
        pending = sorted(c for c in callees if c not in done)
        routines.append(((seg, a), kind, size, pending, (seg, a) in done, covered))

    total = sum(r[2] for r in routines)
    rewritten = sum(r[2] for r in routines if r[4])
    print('routines: %d, rewritten: %d (%d of %d instructions in routines, %.1f%%)' % (
        len(routines), sum(1 for r in routines if r[4]), rewritten, total, 100.0 * rewritten / max(1, total)))
    todo = [r for r in routines if args.all or not r[4]]
    todo.sort(key=lambda r: (len(r[3]) > 0, len(r[3]), r[2]))
    for (seg, a), kind, size, pending, is_done, covered in todo[:args.limit]:
        calls = ' '.join('%s:%04X' % c for c in pending[:6]) + (' ...' if len(pending) > 6 else '')
        print('%s %s:%04X %-8s %4d insns  %s%s' % ('x' if is_done else ' ', seg, a, kind, size,
              'calls ' + calls if pending else 'leaf', '' if covered else '  (never executed in tests)'))


if __name__ == '__main__':
    main()
