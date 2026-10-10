"""720 Degrees level (playfield map) decoder.

Format (reverse engineered from the map streamer in bank A5 (fn_A5_4004/42BC/438C/41CE)
and the block decoder fn_F_E82E):

  * A level map is a grid of 16-bit cell words.  Cell (cx, cy) of level L is read from
    M = base + cy*stride + cx*2, where M is a banked pointer: bank register value
    ((M >> 3) & 0x1C00) + MAP_BANK[L], address 0x4000 + (M & 0x1FFF).
  * A cell word P is a banked pointer to a block: bank ((P >> 3) & 0x1C00) + BLOCK_BANK[L]
    (levels 1-4 use 0xA000 instead when P & 1), address 0x4000 + (P & 0x1FFF).
  * A block is 16x16 tiles stored as two 16x8 halves:
        byte 0: attribute of the top half (bit 7: block has an object list)
        byte 1: attribute of the bottom half
        byte 2: offset of the bottom half's data (relative to byte 3)
        byte 3..: run-length coded tiles of the top half, then the bottom half
  * Run-length coding, 128 tiles per half, row major:
        control byte c: count = c & 0x3F, mode = (c >> 6) & 3, followed by a low byte
        mode 0: each tile supplies a new low byte (high byte kept)
        mode 1: repeat the previous tile
        mode 2: each tile is a full little-endian word (the first word is c's 2 bytes)
        mode 3: tile code increments by one per tile
    The first tile of a run uses the low byte that follows the control byte (mode 2 adds
    a high byte).  Tile codes whose low 11 bits are < 16 are ground placeholders: they are
    replaced through a per-level table (pointer table at 0xFF54) and get a pseudo-random
    variation (+0..7) from the table at 0xFEE2.
  * Tile word bits: 0-9 tile index, 10 tile-bank select, 11-13 palette, 14-15 priority.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import romset

MEM = romset.main_region()


def w16(a):
    return MEM[a] | (MEM[a + 1] << 8)


MAP_BANK = [w16(0xFEB2 + 2 * i) for i in range(8)]
BLOCK_BANK = [w16(0xFEA2 + 2 * i) for i in range(8)]
SUBST_PTR = [w16(0xFF54 + 2 * i) for i in range(8)]
RANDOM_VAR = [((MEM[0xFEE2 + i] ^ 0x80) - 0x80) for i in range(32)]


def b3(a):
    return 0x10000 + 3 * 0x2000 + (a - 0x6000)


def banked(bankreg, ptr):
    """ROM offset of banked pointer ptr (low 13 bits) with bank register value bankreg (window A)."""
    entry = romset.bank_entry(bankreg & 0xffff)
    return romset.bank_base(entry) + (ptr & 0x1fff)


def map_params(level, cls=0):
    """(base pointer, stride bytes) for a level (and difficulty class for levels 1-4)."""
    if level in (1, 2, 3, 4):
        sub = w16(b3(0x7F8C) + 2 * level)
        return w16(b3(sub) + 2 * cls), MEM[b3(sub) + 12 + cls]
    return w16(b3(0x7F8C) + 2 * level), MEM[b3(0x7F9C) + level]


def cell_rom_offset(level, m):
    return banked(((m >> 3) & 0x1C00) + MAP_BANK[level], m)


def read_cell(level, m):
    o = cell_rom_offset(level, m)
    return MEM[o] | (MEM[o + 1] << 8)


def block_offset(level, p, alt=False):
    base = BLOCK_BANK[level]
    if alt:
        base = 0x6000
    elif level in (1, 2, 3, 4) and (p & 1):
        base = 0xA000
    return banked(((p >> 3) & 0x1C00) + base, p)


def subst_table(level):
    t = SUBST_PTR[level]
    return [w16(t + 2 * i) for i in range(16)]


def decode_half(level, p, half, alt=False, rng=None, raw=False):
    """Decode one 16x8 half of block p. Returns (attr, tiles[128], placeholders[128])."""
    o = block_offset(level, p, alt)
    attr = MEM[o] if half == 0 else MEM[o + 1]
    pos = o + 3 + (MEM[o + 2] if half else 0)
    sub = subst_table(level)
    tiles, places = [], []
    count = 0
    mode = 0
    cur = 0
    last = 0
    first = True
    for i in range(128):
        if count == 0:
            c = MEM[pos]; pos += 1
            count = c & 0x3f
            mode = (c >> 6) & 3
            cur = (cur & 0xff00) | MEM[pos]; pos += 1
            if mode == 2:
                cur = (cur & 0x00ff) | (MEM[pos] << 8); pos += 1
            fresh = True
        else:
            fresh = False
            if mode == 0:
                cur = (cur & 0xff00) | MEM[pos]; pos += 1
            elif mode == 2:
                cur = MEM[pos] | (MEM[pos + 1] << 8); pos += 2
            elif mode == 3:
                cur = (cur + 1) & 0xffff
        if mode == 1 and not fresh:
            out = last
            place = places[-1] if places else False
        else:
            code = cur & 0x7ff
            out = cur
            place = False
            if code <= 15 and sub[code] != 0 and not raw:
                base = sub[code] | (cur & 0xf800)
                if rng is not None:
                    out = (base + rng()) & 0xffff
                else:
                    out = base
                place = True
        tiles.append(out)
        places.append(place)
        last = out
        count = (count - 1) & 0xffff
    return attr, tiles, places, pos


def level_grid(level, cls=0, rows=None, cols=None):
    base, stride = map_params(level, cls)
    cols = cols or stride // 2
    rows = rows or 32
    grid = []
    for cy in range(rows):
        row = []
        for cx in range(cols):
            m = (base + cy * stride + cx * 2) & 0xffff
            row.append(read_cell(level, m))
        grid.append(row)
    return grid


if __name__ == '__main__':
    for lv in range(8):
        print(lv, ['%04X' % v for v in map_params(lv)], 'mapbank %04X blockbank %04X subst %04X' % (MAP_BANK[lv], BLOCK_BANK[lv], SUBST_PTR[lv]))
