"""Package a built copy of the game for one platform.

    python tools/package.py [--out dist] [--version X.Y.Z] [--build-dir port/build-release]

Collects the programs the build copied to the repository's top folder (720degrees,
editor720 and, on Windows, SDL3.dll; --build-dir takes them from that build folder instead,
which other builds cannot overwrite), an empty Game folder with its README, the README and
the third-party licences into dist/720degrees-<version>-<platform>.zip.  ROM files are
never included, and the game and the editor contain no translated program code; build with
-DD720_ROM_FREE=ON (as the releases are) so they also leave out the data generated from the
ROMs (level_palettes.inc).
"""
import argparse
import datetime
import os
import platform
import stat
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def git_version():
    try:
        out = subprocess.run(['git', 'describe', '--tags', '--always', '--dirty'], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.strip()
        return out.lstrip('v') or 'dev'
    except (OSError, subprocess.CalledProcessError):
        return datetime.date.today().strftime('%Y%m%d')


def platform_name():
    system = {'Windows': 'windows', 'Darwin': 'macos', 'Linux': 'linux'}.get(platform.system(), platform.system().lower())
    machine = platform.machine().lower()
    arch = {'amd64': 'x64', 'x86_64': 'x64', 'arm64': 'arm64', 'aarch64': 'arm64'}.get(machine, machine)
    return '%s-%s' % (system, arch)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    ap.add_argument('--version', default=None)
    ap.add_argument('--build-dir', default=None, help='take the programs from this build folder')
    args = ap.parse_args()
    src = os.path.abspath(args.build_dir) if args.build_dir else ROOT

    exe = '.exe' if os.name == 'nt' else ''
    programs = ['720degrees' + exe, 'editor720' + exe]
    if os.name == 'nt':
        programs.append('SDL3.dll')
    missing = [p for p in programs if not os.path.exists(os.path.join(src, p))]
    if missing:
        sys.exit('not built yet: %s (build first: cmake --build port/build)' % ', '.join(missing))

    licences = [
        ('Dear ImGui (MIT)', 'port/third_party/imgui/LICENSE.txt'),
        ('ymfm (BSD-3-Clause)', 'port/third_party/ymfm/LICENSE'),
    ]
    sdl = [os.path.join(ROOT, 'port', 'third_party', d, 'LICENSE.txt')
           for d in sorted(os.listdir(os.path.join(ROOT, 'port', 'third_party'))) if d.startswith('SDL3-')]
    sdl += [os.path.join(ROOT, 'port', 'build', '_deps', 'sdl3-src', 'LICENSE.txt')]
    if args.build_dir:
        sdl += [os.path.join(src, '_deps', 'sdl3-src', 'LICENSE.txt')]
    sdl = [p for p in sdl if os.path.exists(p)]
    notices = ['Third-party software in this package\n',
               'SDL3 (zlib licence), Dear ImGui (MIT), ymfm (BSD-3-Clause).\n'
               'Sound and video behaviour follows MAME\'s atarisy2 driver and device cores\n'
               '(BSD-3-Clause, Aaron Giles and contributors).\n']
    for title, rel in licences:
        path = os.path.join(ROOT, rel)
        if os.path.exists(path):
            notices.append('\n==== %s ====\n\n%s' % (title, open(path, encoding='utf-8', errors='replace').read()))
    if sdl:
        notices.append('\n==== SDL3 (zlib) ====\n\n%s' % open(sdl[0], encoding='utf-8', errors='replace').read())

    version = args.version or git_version()
    name = '720degrees-%s-%s' % (version, platform_name())
    os.makedirs(args.out, exist_ok=True)
    zpath = os.path.join(args.out, name + '.zip')

    def add_file(z, src, arc, executable=False):
        info = zipfile.ZipInfo.from_file(src, arc)
        info.compress_type = zipfile.ZIP_DEFLATED
        mode = 0o755 if executable else 0o644
        info.external_attr = (stat.S_IFREG | mode) << 16
        with open(src, 'rb') as f:
            z.writestr(info, f.read())

    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in programs:
            add_file(z, os.path.join(src, p), name + '/' + p, executable=not p.endswith('.dll'))
        add_file(z, os.path.join(ROOT, 'README.md'), name + '/README.md')
        add_file(z, os.path.join(ROOT, 'Game', 'README.txt'), name + '/Game/README.txt')
        z.writestr(name + '/THIRD_PARTY_NOTICES.txt', ''.join(notices))
    print('wrote %s (%.1f MB)' % (zpath, os.path.getsize(zpath) / 1e6))


if __name__ == '__main__':
    main()
