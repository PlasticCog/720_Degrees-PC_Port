"""Extract the 720 Degrees ROM files from MAME zip files dropped into the ROM folder.

Any *.zip in the folder is searched for the ROM files (names starting with '136047-' and
'720-eeprom.bin'); files that are not in the folder yet are extracted next to the zip.
Used by romset.py and by CMake before the program ROMs are translated:

    python tools/romzip.py <folder>
"""
import os
import sys
import zipfile


def is_rom_name(name):
    base = os.path.basename(name)
    return base.startswith('136047-') or base == '720-eeprom.bin'


def extract(folder):
    """Returns the number of files extracted."""
    count = 0
    if not os.path.isdir(folder):
        return 0
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith('.zip'):
            continue
        try:
            with zipfile.ZipFile(os.path.join(folder, f)) as z:
                for info in z.infolist():
                    base = os.path.basename(info.filename)
                    if info.is_dir() or not is_rom_name(base):
                        continue
                    target = os.path.join(folder, base)
                    if os.path.exists(target):
                        continue
                    with z.open(info) as src, open(target, 'wb') as dst:
                        dst.write(src.read())
                    count += 1
        except (zipfile.BadZipFile, OSError) as e:
            print('romzip: cannot read %s: %s' % (f, e), file=sys.stderr)
    return count


def has_roms(folder):
    if os.path.exists(os.path.join(folder, '136047-3126.7lm')):
        return True
    if not os.path.isdir(folder):
        return False
    for f in os.listdir(folder):
        if f.lower().endswith('.zip'):
            try:
                with zipfile.ZipFile(os.path.join(folder, f)) as z:
                    if any(os.path.basename(n) == '136047-3126.7lm' for n in z.namelist()):
                        return True
            except (zipfile.BadZipFile, OSError):
                pass
    return False


if __name__ == '__main__':
    n = extract(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'Game'))
    print('extracted %d ROM file(s)' % n)
