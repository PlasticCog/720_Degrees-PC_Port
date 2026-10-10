"""ROM loading helpers for Atari 720 Degrees (rev 4, MAME set '720').

Builds the CPU-visible memory images exactly as the MAME driver does
(Source_Emulator/mame/src/mame/atari/atarisy2.cpp, ROM_START(720)).
"""
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
ROMDIR = os.environ.get('D720_ROM_DIR') or next(
    (d for d in (os.path.join(_HERE, '..', 'Game'), os.path.join(_HERE, '..', 'rom'))
     if os.path.exists(os.path.join(d, '136047-3126.7lm'))), os.path.join(_HERE, '..', 'Game'))


def _rd(name):
    with open(os.path.join(ROMDIR, name), 'rb') as f:
        return f.read()


def _load16(region, name, offset):
    """ROM_LOAD16_BYTE: bytes go to every other address starting at offset."""
    data = _rd(name)
    region[offset:offset + 2 * len(data):2] = data


def main_region():
    """0x90000 byte 'maincpu' region. 0x8000-0xffff = fixed program ROM,
    0x10000.. = 8K banks selectable into 0x4000 and 0x6000."""
    r = bytearray(0x90000)
    _load16(r, '136047-3126.7lm', 0x008000)
    _load16(r, '136047-3127.7mn', 0x008001)
    _load16(r, '136047-3128.6fh', 0x010000)
    _load16(r, '136047-4131.6mn', 0x010001)
    _load16(r, '136047-1129.6hj', 0x030000)
    _load16(r, '136047-1132.6p', 0x030001)
    _load16(r, '136047-1130.6k', 0x050000)
    _load16(r, '136047-1133.6r', 0x050001)
    return r


def sound_region():
    r = bytearray(0x10000)
    r[0x4000:0x8000] = _rd('136047-2134.2a')
    r[0x8000:0xc000] = _rd('136047-1135.2b')
    r[0xc000:0x10000] = _rd('136047-2136.2cd')
    return r


def tiles_region():
    r = bytearray(0x40000)
    for name, off in [('136047-1121.6a', 0x00000), ('136047-1122.6b', 0x08000),
                      ('136047-1123.7a', 0x10000), ('136047-1124.7b', 0x18000),
                      ('136047-1117.6c', 0x20000), ('136047-1118.6d', 0x28000),
                      ('136047-1119.7d', 0x30000), ('136047-1120.7c', 0x38000)]:
        r[off:off + 0x8000] = _rd(name)
    return r


def sprites_region():
    """ROMREGION_INVERT; each 64K ROM is split: first half at hi offset, second half (ROM_CONTINUE) at lo."""
    r = bytearray(0x100000)
    table = [('136047-1109.6t', 0x20000, 0x00000), ('136047-1110.6sr', 0x28000, 0x08000),
             ('136047-1111.6p', 0x30000, 0x10000), ('136047-1112.6n', 0x38000, 0x18000),
             ('136047-1113.6m', 0x60000, 0x40000), ('136047-1114.6l', 0x68000, 0x48000),
             ('136047-1115.6kj', 0x70000, 0x50000), ('136047-1116.6jh', 0x78000, 0x58000),
             ('136047-1101.5t', 0xa0000, 0x80000), ('136047-1102.5sr', 0xa8000, 0x88000),
             ('136047-1103.5p', 0xb0000, 0x90000), ('136047-1104.5n', 0xb8000, 0x98000),
             ('136047-1105.5m', 0xe0000, 0xc0000), ('136047-1106.5l', 0xe8000, 0xc8000),
             ('136047-1107.5kj', 0xf0000, 0xd0000), ('136047-1108.5jh', 0xf8000, 0xd8000)]
    for name, a, b in table:
        d = _rd(name)
        r[a:a + 0x8000] = d[:0x8000]
        r[b:b + 0x8000] = d[0x8000:]
    return bytes(x ^ 0xff for x in r)


def chars_region():
    return _rd('136047-1125.4t')


def eeprom():
    return _rd('720-eeprom.bin')


def bank_entry(data16):
    """Bank register value (written at 0x1400/0x1402) -> bank entry index.
    Mirrors atarisy2_state::bankselect_w."""
    n = ((data16 >> 10) & 0o77) ^ 3
    b = lambda v, i: (v >> i) & 1
    # bitswap<6>(n, 5,4,1,0,3,2)
    return (b(n, 5) << 5) | (b(n, 4) << 4) | (b(n, 1) << 3) | (b(n, 0) << 2) | (b(n, 3) << 1) | b(n, 2)


def bank_base(entry):
    return 0x10000 + entry * 0x2000
