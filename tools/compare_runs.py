"""Compare whole runs with and without the rewritten routines, frame by frame.

    python tools/compare_runs.py [--only selftest,park0] [--frames N]

The difference tester (tools/difftest_all.py) compares every call of a rewritten routine,
except calls that wait for an interrupt (the self test screens, button waits).  This tool
covers those: each scenario of difftest_all runs twice, with the rewrites and with the
translated original only (--no-rewrite), and the RAM dumps are compared per frame (main RAM
outside the stack page, sprite RAM, scroll).  Rewritten routines run without interrupts in
between, so some drift is expected in play with random input; the self test should match
closely and the frame counters should agree.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import difftest_all  # noqa: E402

REC = 0x1000 + 0x800 + 4


def compare(a, b):
    n = min(len(a), len(b)) // REC
    first_ram = first_vis = None
    vis = 0
    for f in range(n):
        ra, rb = a[f * REC:(f + 1) * REC], b[f * REC:(f + 1) * REC]
        if ra[0x1000:] != rb[0x1000:]:
            vis += 1
            if first_vis is None:
                first_vis = f
        if first_ram is None and (ra[:0x0100] != rb[:0x0100] or ra[0x0180:0x1000] != rb[0x0180:0x1000]):
            first_ram = f
    last = (n - 1) * REC
    frames = (a[last + 0x01C4] | a[last + 0x01C5] << 8, b[last + 0x01C4] | b[last + 0x01C5] << 8)
    return n, first_ram, first_vis, vis, frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(HERE, '..', 'port', 'build', 'render720.exe'))
    ap.add_argument('--roms', default=os.path.join(HERE, '..', 'Game'))
    ap.add_argument('--only', default='')
    ap.add_argument('--frames', type=int, default=0, help='override the scenario lengths')
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='compare_')
    selftest = os.path.join(tmp, 'selftest.txt')
    open(selftest, 'w').write('0 test 1\n')
    todo = difftest_all.scenarios(selftest)
    if args.only:
        todo = [s for s in todo if s[0] in set(args.only.split(','))]

    def run(job):
        (name, frames, extra), rewrite = job
        out = os.path.join(tmp, '%s_%d.bin' % (name, rewrite))
        cmd = [args.exe, '--roms', args.roms, '--frames', str(args.frames or frames), '--ram-dump', out] + extra
        if not rewrite:
            cmd.append('--no-rewrite')
        subprocess.run(cmd, capture_output=True)
        return out

    jobs = [(s, r) for s in todo for r in (1, 0)]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        outs = list(ex.map(run, jobs))
    for i, sc in enumerate(todo):
        a = open(outs[2 * i + 1], 'rb').read()
        b = open(outs[2 * i], 'rb').read()
        n, fr, fv, vis, frames = compare(a, b)
        print('%-9s %5d frames  first RAM difference %-5s  first visible %-5s  frames looking different %5d  '
              'frame counter %04X/%04X%s' % (sc[0], n, fr, fv, vis, frames[0], frames[1],
                                             '' if frames[0] == frames[1] else '  (differs)'))


if __name__ == '__main__':
    main()
