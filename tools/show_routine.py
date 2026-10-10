"""Print a routine of the main program for rewriting: its disassembly (from re/main_t11.lst,
including the body reached through the C register-save helper) and Ghidra's decompiled C
(re/decompiled), if available.

    python tools/show_routine.py F:8E78 A5:4442 ...
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
LST = os.path.join(ROOT, 're', 'main_t11.lst')
DEC = os.path.join(ROOT, 're', 'decompiled')


def listing_blocks():
    """seg -> list of (addr, text) in listing order"""
    out = {}
    seg = None
    for line in open(LST, encoding='utf-8', errors='replace'):
        m = re.match(r'^(sub|loc)_([A-Z]\d*)_([0-9A-F]{4}):', line)
        if m:
            seg = m.group(2)
        m = re.match(r'^\s+([0-9A-F]{4})\s', line)
        if m and seg:
            out.setdefault(seg, []).append((int(m.group(1), 16), line.rstrip()))
        elif seg and (line.startswith('loc_') or line.startswith('sub_')):
            out.setdefault(seg, []).append((None, line.rstrip()))
    return out


def show(seg, addr, blocks, dec_cache, with_c=True):
    print('=' * 100)
    print('%s:%04X' % (seg, addr))
    rows = blocks.get(seg, [])
    # print from the routine start until a blank-separated routine boundary after an RTS/JMP
    started = False
    count = 0
    for a, text in rows:
        if a == addr:
            started = True
        if not started:
            continue
        if a is None:
            if text.startswith('sub_') and count > 0 and not text.startswith('sub_%s_%04X' % (seg, addr + 4)):
                break
            print(text)
            continue
        print(text)
        count += 1
        if count > 400:
            print('...')
            break
    # decompiled C
    path = os.path.join(DEC, 'main_%s_decompiled.c' % seg)
    if with_c and os.path.exists(path):
        src = dec_cache.setdefault(path, open(path, encoding='utf-8', errors='replace').read())
        for a2 in (addr + 4, addr):
            m = re.search(r'\n// %x\n(.*?)(?=\n// [0-9a-f]+\n|\Z)' % a2, src, re.S)
            if m:
                print('--- decompiled %s:%04X' % (seg, a2))
                print(m.group(1).strip()[:6000])
                break


def main():
    blocks = listing_blocks()
    cache = {}
    with_c = '--asm' not in sys.argv
    for arg in sys.argv[1:]:
        if arg.startswith('--'):
            continue
        seg, a = arg.upper().split(':')
        show(seg, int(a, 16), blocks, cache, with_c)


if __name__ == '__main__':
    main()
