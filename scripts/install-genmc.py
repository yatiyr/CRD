#!/usr/bin/env python3
"""Build and install the pinned GenMC model checker (DIAG.4b) from its commit-addressed source archive.

Reads cmake/pins.json (tools/genmc). The archive is fetched through pins.fetch(), so it is verified before the final
name exists, and extracted with tarfile's `data` filter (no absolute paths, no escaping links). GenMC is built
against the LLVM of the pinned major (`llvm_major`, default `/usr/lib/llvm-<major>`: Ubuntu's llvm-<major>-dev and
clang-<major>); a different LLVM major is refused before configuring. The installed `genmc --version` must report the
pinned version. A stamp (`cerid-genmc.json`) records the pin digest and the exact LLVM version, so a cached install is
reused only when both still match. Linux only. Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

STAMP = 'cerid-genmc.json'


def llvm_version(llvm_dir):
    config = Path(llvm_dir) / 'bin' / 'llvm-config'
    if not config.is_file():
        raise pins.PinError(f'{config} is missing; install llvm-<major>-dev and clang-<major>')
    return subprocess.run([str(config), '--version'], check=True, capture_output=True, text=True).stdout.strip()


def check_llvm(pin, version):
    if version.split('.', 1)[0] != pin['llvm_major']:
        raise pins.PinError(f'GenMC {pin["version"]} is pinned against LLVM {pin["llvm_major"]}, found LLVM {version}')


def expected_stamp(pin, version):
    return {'version': pin['version'], 'commit': pin['commit'], 'sha256': pin['sha256'], 'llvm': version}


def installed_version(destination):
    genmc = Path(destination) / 'bin' / 'genmc'
    if not genmc.is_file():
        return None
    result = subprocess.run([str(genmc), '--version'], capture_output=True, text=True)
    return (result.stdout + result.stderr).strip()


def reusable(destination, stamp):
    """True when `destination` holds an install made from this pin and this exact LLVM."""
    path = Path(destination) / STAMP
    if not path.is_file():
        return False
    try:
        recorded = json.loads(path.read_text(encoding='utf-8'))
    except ValueError:
        return False
    reported = installed_version(destination)
    return recorded == stamp and reported is not None and stamp['version'] in reported


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


def install(destination, llvm_dir=None, archive=None, download_dir=None, work_dir=None, jobs=None, cxx=None,
            pin=None):
    """Return the path of the installed genmc binary, building it unless a matching install is present."""
    pin = pins.entry('tools', 'genmc') if pin is None else pin
    destination = Path(destination).resolve()
    llvm_dir = Path(llvm_dir or f'/usr/lib/llvm-{pin["llvm_major"]}')
    version = llvm_version(llvm_dir)
    check_llvm(pin, version)
    stamp = expected_stamp(pin, version)
    genmc = destination / 'bin' / 'genmc'
    if reusable(destination, stamp):
        print(f'GenMC {pin["version"]} (LLVM {version}) already installed at {genmc}; stamp and version match.')
        return genmc
    work_dir = Path(work_dir or tempfile.mkdtemp(prefix='cerid-genmc-')).resolve()
    download_dir = Path(download_dir).resolve() if download_dir else work_dir
    verified = pins.fetch(pin, download_dir, archive, label='GenMC source')
    if destination.exists():
        shutil.rmtree(destination)
    source_root = work_dir / 'src'
    if source_root.exists():
        shutil.rmtree(source_root)
    source = extract(verified, source_root)
    build = work_dir / 'build'
    cxx = cxx or os.environ.get('CXX') or shutil.which('g++-14') or 'g++'
    configure = ['cmake', '-S', str(source), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
                 f'-DCMAKE_PREFIX_PATH={llvm_dir}', f'-DCMAKE_INSTALL_PREFIX={destination}',
                 f'-DCMAKE_CXX_COMPILER={cxx}']
    if shutil.which('ninja'):
        configure += ['-G', 'Ninja']
    started = time.monotonic()
    log_path = work_dir / 'genmc-build.log'
    with log_path.open('w', encoding='utf-8') as log:
        try:
            run(configure, log)
            run(['cmake', '--build', str(build), '--parallel', str(jobs or os.cpu_count() or 2)], log)
            run(['cmake', '--install', str(build)], log)
        except subprocess.CalledProcessError:
            sys.stderr.write(log_path.read_text(encoding='utf-8', errors='replace')[-6000:])
            raise
    reported = installed_version(destination)
    if reported is None or pin['version'] not in reported:
        raise pins.PinError(f'{genmc} reports {reported!r}, expected GenMC {pin["version"]}')
    (destination / STAMP).write_text(json.dumps(stamp, indent=2) + '\n', encoding='utf-8')
    print(f'Built GenMC {pin["version"]} ({pin["commit"][:12]}) against LLVM {version} with {cxx} in '
          f'{time.monotonic() - started:.0f} s; installed {genmc}.')
    return genmc


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, required=True, help='install prefix (bin/genmc lands under it)')
    parser.add_argument('--llvm-dir', type=Path, help='LLVM prefix holding bin/llvm-config and bin/clang')
    parser.add_argument('--archive', type=Path, help='an already downloaded source archive to verify and use')
    parser.add_argument('--download-dir', type=Path, help='directory receiving the download (default: work dir)')
    parser.add_argument('--work-dir', type=Path, help='source and build directory (default: a temporary directory)')
    parser.add_argument('--jobs', type=int, help='parallel build jobs (default: every core)')
    parser.add_argument('--cxx', help='C++20/23 compiler (default: $CXX, then g++-14, then g++)')
    args = parser.parse_args(argv)
    genmc = install(args.destination, args.llvm_dir, args.archive, args.download_dir, args.work_dir, args.jobs,
                    args.cxx)
    print(f'GENMC={genmc}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
