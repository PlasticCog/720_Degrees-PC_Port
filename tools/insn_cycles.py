"""Cycle cost of each instruction of a routine in the generated translation.

    python tools/insn_cycles.py SEG:ADDR [END]

Prints "ADDR INSN cycles" for the labels from ADDR up to END (default: the next routine).
Needed for polling loops (logic::tick), which charge the original's cost per pass.
"""
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def main():
    seg, addr = sys.argv[1].split(':')
    start = int(addr, 16)
    end = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    for path in sorted(glob.glob(os.path.join(ROOT, 'port', 'src', 'game', 't11_%s_*.cpp' % seg))):
        lines = open(path, encoding='utf-8').read().splitlines()
        on = False
        for i, l in enumerate(lines):
            m = re.match(r'L_([0-9A-F]{4}): // (.*)', l)
            if m:
                a = int(m.group(1), 16)
                if a == start:
                    on = True
                elif on and ((end is not None and a >= end) or (end is None and a < start)):
                    return
                if on:
                    c = re.search(r'T\.icount -= (\d+);', lines[i + 1])
                    print('%04X  %-32s %s' % (a, m.group(2).strip(), c.group(1) if c else '?'))
            elif on and end is None and l.startswith('// ---- '):
                return
        if on:
            return


if __name__ == '__main__':
    main()
