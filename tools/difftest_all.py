"""Run the difference tester over all test scenarios and merge the reports.

    python tools/difftest_all.py [--exe port/build/render720.exe] [--roms Game] [--quick]
                                 [--out report.txt] [--only NAME,NAME]
    python tools/difftest_all.py --record-coverage


Every scenario runs `render720 --difftest` (see port/src/logic) in parallel; the merged
report adds up calls / tested calls / mismatches per rewritten routine, averages the
original's cycles per call (input for tools/update_cycles.py) and keeps the first mismatch
of each routine with the scenario it came from.  Exit status 1 if anything mismatched.

Scenarios: the attract mode without input, random play with three seeds, each of the four parks (entered by setting the park
number 0x027A and the game phase 0x0F26 = 1), the ticket gate event (0x032A = 1), a series of
other terrain events, power-on with the test switch held (self test) and a game ending with a
high score (initials entry).

--record-coverage runs the scenarios on the reference model (ref720 --coverage) and adds
what they execute to re/coverage/main_cov.txt, main_edges.txt, sound_cov.txt and
sound_edges.txt: the translators discover code from these files, so code that only these
scenarios reach (parts of the parks, the self test) gets translated too.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

EVENTS = ['1100:032A:0005', '1150:032A:000D', '1200:032A:000E', '1250:032A:0010', '1300:032A:0013',
          '1350:032A:0014', '1400:032A:0011', '1450:032A:0001', '1520:0F26:0001']


def scenarios(selftest_script):
    s = [('attract', 3000, []),                     # power-on, no input: the attract mode and its demo
         ('random3', 12000, ['--random', '3']),
         ('random7', 12000, ['--random', '7']),
         ('random11', 12000, ['--random', '11'])]
    for p in range(4):
        s.append(('park%d' % p, 6000, ['--random', '5', '--poke', '1200:027A:010%d' % p, '--poke', '1200:0F26:0001']))
    s.append(('gate', 6000, ['--random', '3', '--poke', '1000:032A:0001', '--poke', '1100:0F26:0001']))
    ev = ['--random', '3']
    for e in EVENTS:
        ev += ['--poke', e]
    s.append(('events', 6000, ev))
    # the trick moves 0x40-0x57 (A5:54CE and its helpers), which the game only starts on ramp
    # jumps, forced one after the other; 0x0EA0 makes a landing set the direction from the
    # velocity (A7:4A66)
    tr = ['--random', '3']
    for i in range(24):
        tr += ['--poke', '%d:01EB:%04X' % (1100 + 20 * i, 0x40 + i)]
    for f in (1700, 1800, 1900):
        tr += ['--poke', '%d:0EA0:0001' % f]
    s.append(('tricks', 3000, tr))
    s.append(('selftest', 6000, ['--random', '9', '--script', selftest_script]))
    # the self test's controller screens (F:FB5E, F:FA00, F:FADE), which random input never
    # reaches (it does not move the centre sensor): the kick button past the ROM check and the
    # sound screen, then the controller turned one step every 10 frames (standing still in
    # between) through the centre and all sectors, about four turns
    rotary = os.path.join(os.path.dirname(selftest_script), 'rotary.txt')
    lines = ['0 test 1']
    for f in (1400, 1600):
        lines += ['%d b1 1' % f, '%d b1 0' % (f + 4)]
    lines += ['%d spin 1' % (1700 + 10 * i) for i in range(600)]
    with open(rotary, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    s.append(('rotary', 7800, ['--script', rotary]))
    s.append(('hiscore', 6000, ['--random', '3', '--poke', '2500:023E:9999', '--poke', '2500:0240:0009']))
    return s


LINE = re.compile(r'\s+(\S+:[0-9A-F]{4}) (\S+)\s+calls\s+(\d+)(.*)')


def parse(text):
    out = {}
    last = None
    for line in text.splitlines():
        m = LINE.match(line)
        if m:
            key, name, calls, rest = m.group(1), m.group(2), int(m.group(3)), m.group(4)
            r = {'name': name, 'calls': calls, 'tested': 0, 'bad': 0, 'cycles': 0.0, 'native': 0.0, 'charged': None,
                 'waits': 0, 'unwound': 0, 'first': None}
            t = re.search(r'tested\s+(\d+)\s+mismatches (\d+)\s+original (\d+) cycles/call \(charged (-?\d+)\)', rest)
            if t:
                r['tested'], r['bad'] = int(t.group(1)), int(t.group(2))
                r['cycles'], r['charged'] = float(t.group(3)), int(t.group(4))
            n = re.search(r'\(charged -?\d+\) native (\d+)', rest)
            if n:
                r['native'] = float(n.group(1))
            w = re.search(r'waits \(not compared\) (\d+)', rest)
            if w:
                r['waits'] = int(w.group(1))
            u = re.search(r'left by an interrupt (\d+)', rest)
            if u:
                r['unwound'] = int(u.group(1))
            out[key] = r
            last = key
        elif last and line.strip().startswith('first mismatch:'):
            out[last]['first'] = line.strip()[len('first mismatch:'):].strip()
    return out


def merge_lines(path, new_files):
    """Add the lines of new_files that path does not contain yet (kept sorted after the old ones)."""
    old = open(path, encoding='utf-8').read().splitlines()
    have = set(old)
    add = set()
    for f in new_files:
        if os.path.exists(f):
            add.update(l for l in open(f, encoding='utf-8').read().splitlines() if l.strip() and l not in have)
    if add:
        nl = chr(10)
        open(path, 'w', encoding='utf-8', newline=nl).write(nl.join(old + sorted(add)) + nl)
    return len(add)


def record_coverage(args, todo, tmp):
    ref = os.path.join(os.path.dirname(args.exe), 'ref720.exe')

    def run(sc):
        name, frames, extra = sc
        base = os.path.join(tmp, name)
        cmd = [ref, '--roms', args.roms, '--frames', str(frames), '--out', tmp,
               '--coverage', base + '_cov.txt', '--sound-coverage', base + '_scov.txt'] + extra
        subprocess.run(cmd, capture_output=True)
        return base

    with ThreadPoolExecutor(max_workers=min(args.jobs, len(todo))) as ex:
        bases = list(ex.map(run, todo))
    cov = os.path.join(ROOT, 're', 'coverage')
    for target, suffix in (('main_cov.txt', '_cov.txt'), ('main_edges.txt', '_cov.txt.edges'),
                           ('sound_cov.txt', '_scov.txt'), ('sound_edges.txt', '_scov.txt.edges')):
        n = merge_lines(os.path.join(cov, target), [b + suffix for b in bases])
        print('%-16s +%d' % (target, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(ROOT, 'port', 'build', 'render720.exe'))
    ap.add_argument('--roms', default=os.path.join(ROOT, 'Game'))
    ap.add_argument('--quick', action='store_true', help='a third of the frames')
    ap.add_argument('--only', default='', help='comma-separated scenario names')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--out', default='')
    ap.add_argument('--record-coverage', action='store_true')
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix='difftest_')
    selftest = os.path.join(tmp, 'selftest.txt')
    open(selftest, 'w').write('0 test 1\n')
    todo = scenarios(selftest)
    if args.only:
        names = set(args.only.split(','))
        todo = [s for s in todo if s[0] in names]

    if args.record_coverage:
        record_coverage(args, todo, tmp)
        return

    def run(sc):
        name, frames, extra = sc
        if args.quick:
            frames //= 3
        cmd = [args.exe, '--roms', args.roms, '--frames', str(frames), '--difftest'] + extra
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        return name, p.returncode, p.stdout + p.stderr

    merged, header, problems = {}, '', []
    with ThreadPoolExecutor(max_workers=min(args.jobs, len(todo))) as ex:
        for name, rc, text in ex.map(run, todo):
            if not header:
                header = next((l for l in text.splitlines() if l.startswith('rewritten routines')), '')
            if rc != 0 or 'done:' not in text:
                problems.append('%s: render720 exited with %d' % (name, rc))
            for l in text.splitlines():
                if l.startswith('logic:'):
                    problems.append('%s: %s' % (name, l))
            for key, r in parse(text).items():
                m = merged.setdefault(key, {'name': r['name'], 'calls': 0, 'tested': 0, 'bad': 0, 'cyc': 0.0, 'ncyc': 0.0,
                                            'charged': r['charged'], 'waits': 0, 'unwound': 0, 'first': None,
                                            'where': set()})
                m['calls'] += r['calls']
                m['tested'] += r['tested']
                m['bad'] += r['bad']
                m['cyc'] += r['cycles'] * r['tested']
                m['ncyc'] += r['native'] * r['tested']
                m['waits'] += r['waits']
                m['unwound'] += r['unwound']
                if r['charged'] is not None:
                    m['charged'] = r['charged']
                if r['tested']:
                    m['where'].add(name)
                if r['first'] and not m['first']:
                    m['first'] = '[%s] %s' % (name, r['first'])

    lines = [header, 'scenarios: ' + ', '.join(s[0] for s in todo) + (' (quick)' if args.quick else '')]
    untested = []
    for key in sorted(merged, key=lambda k: (k.split(':')[0], k)):
        m = merged[key]
        s = '  %s %-22s calls %8d' % (key, m['name'], m['calls'])
        if m['tested']:
            s += '  tested %8d  mismatches %d  original %d cycles/call (charged %s) native %d' % (
                m['tested'], m['bad'], round(m['cyc'] / m['tested']), m['charged'], round(m['ncyc'] / m['tested']))
        elif not m['waits']:
            untested.append('%s %s' % (key, m['name']))
        if m['waits']:
            s += '  waits (not compared) %d' % m['waits']
        if m['unwound']:
            s += '  left by an interrupt %d' % m['unwound']
        lines.append(s)
        if m['first']:
            lines.append('      first mismatch: ' + m['first'])
    for line in open(os.path.join(ROOT, 'port', 'src', 'logic', 'routines.txt'), encoding='utf-8'):
        f = line.split()
        if f and not f[0].startswith('#') and f[0] not in merged:
            untested.append('%s %s (never called)' % (f[0], f[1] if len(f) > 1 else ''))
    bad = sum(m['bad'] for m in merged.values())
    lines.append('')
    lines.append('total mismatches: %d' % bad)
    if untested:
        lines.append('never tested (%d): %s' % (len(untested), ', '.join(untested)))
    lines += problems
    report = '\n'.join(lines) + '\n'
    if args.out:
        open(args.out, 'w', encoding='utf-8').write(report)
    sys.stdout.write(report)
    sys.exit(1 if bad or problems else 0)


if __name__ == '__main__':
    main()
