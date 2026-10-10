"""Set the cycle cost charged by each rewritten routine so that it takes as long as the original.

    render720 --frames 20000 --random 3 --difftest > report.txt
    python tools/update_cycles.py report.txt

The difference tester reports "original N cycles/call (charged C) native M" per routine: N and M
include the routines called on the way (which charge their own costs), C is the routine's own
charge.  The new charge is C + N - M, the original's cost minus what the callees already
charge; for a routine that calls nothing that is N.  The routine tables in
port/src/logic/routines_*.cpp end each entry with the charged cost, which is updated here.
Keeping the charge close to the original keeps the game's timing close to the arcade's.
Polling routines (wait_routine / wait_c_routine) charge their cycles themselves and are skipped.
"""
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def main():
    measured = {}
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        m = re.match(r'\s+\S+:[0-9A-F]{4} (\S+)\s+calls\s+\d+\s+tested\s+(\d+)\s+mismatches \d+\s+original (\d+) cycles/call'
                     r' \(charged (-?\d+)\)(?: native (\d+))?', line)
        if m and int(m.group(2)) > 0:
            orig, charged = int(m.group(3)), int(m.group(4))
            measured[m.group(1)] = max(0, charged + orig - int(m.group(5))) if m.group(5) else orig
    changed = 0
    for path in glob.glob(os.path.join(ROOT, 'port', 'src', 'logic', 'routines_*.cpp')):
        s = open(path, encoding='utf-8').read()
        for name, cyc in measured.items():
            s, n = re.subn(r'((?<![A-Za-z_])(?:c|asm|irq)_routine\("[A-Z0-9]+", 0x[0-9A-Fa-f]+, "%s",[^)]*?, )-?\d+\)' % re.escape(name), r'\g<1>%d)' % cyc, s)
            changed += n
        open(path, 'w', encoding='utf-8', newline='\n').write(s)
    print('updated %d routine costs from %d measurements' % (changed, len(measured)))


if __name__ == '__main__':
    main()
