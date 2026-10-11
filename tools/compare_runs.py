"""Compare whole runs with and without the rewritten routines, frame by frame.

    python tools/compare_runs.py [--only selftest,park0] [--frames N] [--images K]
    python tools/compare_runs.py --exe port/build/render720native.exe --original-exe port/build/render720.exe

The difference tester (tools/difftest_all.py) compares every call of a rewritten routine,
except calls that wait for an interrupt (the self test screens, button waits).  This tool
compares whole runs: each scenario of difftest_all runs twice, with the rewrites and with the
translated original only (--no-rewrite), and compares

  - RAM per frame: main RAM outside the stack page 0x0100-0x017F (the first frame that
    differs), the stack page itself (registers and flags that interrupts push), and sprite
    RAM and scroll (frames looking different);
  - the rendered screen every K frames (--images, default 10; 0: off): playfield, sprites,
    text and palette, which the RAM dump does not hold;
  - the sound commands (frame and value).  Their cycle stamps are not compared: the
    rewrites charge an instruction's cycles after its access and the translation before, so
    a stamp taken during the write differs by that instruction's cycles while the time slices
    come out the same.

With --original-exe the two runs come from different programs: the rewrite from --exe (such
as render720native, built without the translated main program) and the original from
--original-exe --no-rewrite.

The rewrites follow the original instruction by instruction, so all of it should match.
Exit status 1 if anything differs.
"""
import argparse
import filecmp
import os
import shutil
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
    first_ram = first_vis = first_stack = None
    vis = 0
    for f in range(n):
        ra, rb = a[f * REC:(f + 1) * REC], b[f * REC:(f + 1) * REC]
        if ra[0x1000:] != rb[0x1000:]:
            vis += 1
            if first_vis is None:
                first_vis = f
        if first_ram is None and (ra[:0x0100] != rb[:0x0100] or ra[0x0180:0x1000] != rb[0x0180:0x1000]):
            first_ram = f
        if first_stack is None and ra[0x0100:0x0180] != rb[0x0100:0x0180]:
            first_stack = f
    last = (n - 1) * REC
    frames = (a[last + 0x01C4] | a[last + 0x01C5] << 8, b[last + 0x01C4] | b[last + 0x01C5] << 8)
    return n, first_ram, first_vis, vis, frames, first_stack


def frame_number(name):
    digits = ''.join(c for c in name if c.isdigit())
    return int(digits) if digits else 0


def compare_images(a, b):
    """(images compared, images differing, first differing frame)"""
    names = sorted(set(os.listdir(a)) & set(os.listdir(b)), key=frame_number)
    bad = [n for n in names if not filecmp.cmp(os.path.join(a, n), os.path.join(b, n), shallow=False)]
    return len(names), len(bad), frame_number(bad[0]) if bad else None


def compare_sound(a, b):
    """(commands, first differing line or None): frame and command only"""
    la = [l.split()[:2] for l in open(a).read().splitlines()]
    lb = [l.split()[:2] for l in open(b).read().splitlines()]
    if la == lb:
        return len(la), None
    first = next((k for k in range(min(len(la), len(lb))) if la[k] != lb[k]), min(len(la), len(lb)))
    return len(la), first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(HERE, '..', 'port', 'build', 'render720.exe'))
    ap.add_argument('--original-exe', default='',
                    help='run the original with this program (render720 --no-rewrite) and the rewrite with --exe, '
                         'e.g. --exe port/build/render720native.exe for the build without the translation')
    ap.add_argument('--roms', default=os.path.join(HERE, '..', 'Game'))
    ap.add_argument('--only', default='')
    ap.add_argument('--frames', type=int, default=0, help='override the scenario lengths')
    ap.add_argument('--images', type=int, default=10, help='compare the screen every K frames (0: off)')
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='compare_')
    selftest = os.path.join(tmp, 'selftest.txt')
    open(selftest, 'w').write('0 test 1\n')
    todo = difftest_all.scenarios(selftest)
    if args.only:
        todo = [s for s in todo if s[0] in set(args.only.split(','))]

    def run(job):
        (name, frames, extra), rewrite = job
        base = os.path.join(tmp, '%s_%d' % (name, rewrite))
        os.makedirs(base)
        exe = args.exe if rewrite else (args.original_exe or args.exe)
        cmd = [exe, '--roms', args.roms, '--frames', str(args.frames or frames),
               '--ram-dump', base + '.bin', '--sound-log', base + '.snd'] + extra
        if args.images:
            cmd += ['--out', base, '--png-every', str(args.images)]
        if not rewrite:
            cmd.append('--no-rewrite')
        subprocess.run(cmd, capture_output=True)
        return base

    jobs = [(s, r) for s in todo for r in (1, 0)]
    same = True
    try:
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            outs = list(ex.map(run, jobs))
        for i, sc in enumerate(todo):
            a, b = outs[2 * i], outs[2 * i + 1]
            n, fr, fv, vis, frames, fs_ = compare(open(a + '.bin', 'rb').read(), open(b + '.bin', 'rb').read())
            sounds, fs = compare_sound(a + '.snd', b + '.snd')
            line = '%-9s %5d frames  first RAM difference %-5s  first visible %-5s  frames looking different %5d  ' \
                   'frame counter %04X/%04X%s  sound commands %5d %s' % (
                       sc[0], n, fr, fv, vis, frames[0], frames[1], '' if frames[0] == frames[1] else ' (differs)',
                       sounds, 'same' if fs is None else 'differ from #%d' % fs)
            line += '  stack page %s' % ('same' if fs_ is None else 'differs from frame %d' % fs_)
            same &= fr is None and fv is None and frames[0] == frames[1] and fs is None and fs_ is None
            if args.images:
                count, bad, first = compare_images(a, b)
                line += '  screens %4d, %d differ%s' % (count, bad, ' (first frame %d)' % first if bad else '')
                same &= bad == 0
            print(line, flush=True)
            for p in (a, b):
                shutil.rmtree(p, ignore_errors=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)        # the RAM dumps and screens take a few GB
    sys.exit(0 if same else 1)


if __name__ == '__main__':
    main()
