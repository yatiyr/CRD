#!/usr/bin/env python3
"""Build and install the pinned ClangBuildAnalyzer (compile-time profiles) from its commit-addressed source archive.

Reads cmake/pins.json (tools/clang-build-analyzer). The archive is fetched through pins.fetch(), so it is verified
before the final name exists, and extracted with tarfile's `data` filter (no absolute paths, no escaping links). The
tool is built with CMake and the host's C++ compiler into a user directory (no administrator rights:
`~/.cerid/tools/clang-build-analyzer` on Linux, `%USERPROFILE%\\.cerid\\tools\\clang-build-analyzer` on Windows from a
shell with the MSVC environment); the installed `--version` must report the pinned version. A stamp
(`cerid-clang-build-analyzer.json`) records the pin, so a cached install is reused only while it matches.
Contract: docs/design/build-performance.md#compile-time-profiles and docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

STAMP = 'cerid-clang-build-analyzer.json'
NAME = 'clang-build-analyzer'
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def binary(destination):
    return Path(destination) / 'bin' / ('ClangBuildAnalyzer.exe' if os.name == 'nt' else 'ClangBuildAnalyzer')


def installed_version(destination):
    """The version `ClangBuildAnalyzer --version` prints (colour codes removed), or None."""
    tool = binary(destination)
    if not tool.is_file():
        return None
    result = subprocess.run([str(tool), '--version'], capture_output=True, text=True)
    match = re.search(r'ClangBuildAnalyzer\s+(\S+)', ANSI.sub('', result.stdout + result.stderr))
    return match.group(1) if match else None


def expected_stamp(pin):
    return {'version': pin['version'], 'commit': pin['commit'], 'sha256': pin['sha256']}


def reusable(destination, stamp):
    path = Path(destination) / STAMP
    if not path.is_file():
        return False
    try:
        recorded = json.loads(path.read_text(encoding='utf-8'))
    except ValueError:
        return False
    return recorded == stamp and installed_version(destination) == stamp['version']


def extract(archive, root):
    """Extract the archive under `root` with the `data` filter; return its single top-level directory."""
    with tarfile.open(archive, 'r:gz') as tar:
        tops = {Path(member.name).parts[0] for member in tar.getmembers() if Path(member.name).parts}
        if len(tops) != 1:
            raise pins.PinError(f'{archive} must hold one top-level directory, found {sorted(tops)}')
        tar.extractall(root, filter='data')
    return Path(root) / tops.pop()


def run(command, log):
    print('+ ' + ' '.join(command), flush=True)
    log.write('+ ' + ' '.join(command) + '\n')
    log.flush()
    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)


def install(destination, archive=None, download_dir=None, work_dir=None, jobs=None, pin=None, runner=run):
    """Return the installed ClangBuildAnalyzer, building it unless a matching install is present."""
    pin = pins.entry('tools', NAME) if pin is None else pin
    destination = Path(destination).resolve()
    stamp = expected_stamp(pin)
    tool = binary(destination)
    if reusable(destination, stamp):
        print(f'ClangBuildAnalyzer {pin["version"]} already installed at {tool}; stamp and version match.')
        return tool
    work_dir = Path(work_dir or tempfile.mkdtemp(prefix='cerid-cba-')).resolve()
    download_dir = Path(download_dir).resolve() if download_dir else work_dir
    verified = pins.fetch(pin, download_dir, archive, label='ClangBuildAnalyzer source')
    if destination.exists():
        shutil.rmtree(destination)
    source_root = work_dir / 'src'
    if source_root.exists():
        shutil.rmtree(source_root)
    source = extract(verified, source_root)
    build = work_dir / 'build'
    configure = ['cmake', '-S', str(source), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
                 f'-DCMAKE_INSTALL_PREFIX={destination}', '-DCMAKE_INSTALL_BINDIR=bin']
    if shutil.which('ninja'):
        configure += ['-G', 'Ninja']
    started = time.monotonic()
    log_path = work_dir / 'clang-build-analyzer-build.log'
    with log_path.open('w', encoding='utf-8') as log:
        try:
            runner(configure, log)
            runner(['cmake', '--build', str(build), '--config', 'Release', '--parallel',
                    str(jobs or os.cpu_count() or 2)], log)
            runner(['cmake', '--install', str(build), '--config', 'Release'], log)
        except subprocess.CalledProcessError:
            sys.stderr.write(log_path.read_text(encoding='utf-8', errors='replace')[-6000:])
            raise
    reported = installed_version(destination)
    if reported != pin['version']:
        raise pins.PinError(f'{tool} reports {reported!r}, expected ClangBuildAnalyzer {pin["version"]}')
    (destination / STAMP).write_text(json.dumps(stamp, indent=2) + '\n', encoding='utf-8')
    print(f'Built ClangBuildAnalyzer {pin["version"]} ({pin["commit"][:12]}) in {time.monotonic() - started:.0f} s; '
          f'installed {tool}.')
    return tool


def default_destination():
    return Path.home() / '.cerid' / 'tools' / NAME


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, default=default_destination(),
                        help='install prefix (bin/ClangBuildAnalyzer lands under it)')
    parser.add_argument('--archive', type=Path, help='an already downloaded source archive to verify and use')
    parser.add_argument('--download-dir', type=Path, help='directory receiving the download (default: work dir)')
    parser.add_argument('--work-dir', type=Path, help='source and build directory (default: a temporary directory)')
    parser.add_argument('--jobs', type=int, help='parallel build jobs (default: every core)')
    args = parser.parse_args(argv)
    tool = install(args.destination, args.archive, args.download_dir, args.work_dir, args.jobs)
    print(f'CLANG_BUILD_ANALYZER={tool}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
