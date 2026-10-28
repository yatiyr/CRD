#!/usr/bin/env python3
"""Install the pinned Intel SDE (the emulator the avx512 kernel arms run under) from cmake/pins.json.

The pin is tools/sde-windows on Windows and tools/sde-linux elsewhere: Intel's release archive, fetched through
pins.fetch(), so it is verified before the final name exists (an already downloaded copy in --download-dir, by default
`~/.cerid/tools/sde/<version>/dl-<os>`, is verified instead of downloaded). SDE runs from its whole kit (Pin, its
runtime and the CPU models under misc/cpuid), so every member below the pin's `root` is extracted, through tarfile's
`data` filter: a member outside the root, an absolute or escaping path or link, or a device file is refused, and the
Linux kit's relative links inside the tree are kept (Pin's loader is one). On Windows such a link becomes a hard link
to its target inside the tree: a Windows symlink needs a privilege and cannot resolve a POSIX separator, and links
are extracted after every file so a target always exists first. The tree is assembled beside the
destination (by default `~/.cerid/tools/sde/<version>/<os>-kit`) and renamed into place only when it is complete,
and the pinned `binary` must report the pinned version. A stamp (`cerid-sde.json`) records the pin, so a matching
install is reused and a directory without one is never replaced. Prints CRD_SDE=<binary>, which
`--github-env` also appends to that file: CMake reads CRD_SDE from the environment at configure and registers the
emulated arms (crd_add_kernel_level_tests, cmake/CrdSimd.cmake). Ubuntu's Yama ptrace scope must be 0 for Pin to
attach (Intel's README); the hosted Linux lane sets it.
Contract: docs/design/pinned-inputs.md; ADR-0134 §8.
"""
from pathlib import Path, PurePosixPath
import argparse
import copy
import json
import platform
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

WINDOWS = platform.system() == 'Windows'
PIN_NAME = 'sde-windows' if WINDOWS else 'sde-linux'
HOST = 'win' if WINDOWS else 'lin'
STAMP = 'cerid-sde.json'
SDE_VERSION = re.compile(r'Version:\s+(\d+\.\d+\.\d+)\b')


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'sde' / pin['version'] / (HOST + '-kit')


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'sde' / pin['version'] / ('dl-' + HOST)


def binary(destination, pin):
    return Path(destination) / PurePosixPath(pin['binary'])


def installed_version(tool):
    """The version `sde -version` reports, or None."""
    if not Path(tool).is_file():
        return None
    result = subprocess.run([str(tool), '-version'], capture_output=True, text=True)
    match = SDE_VERSION.search(result.stdout + result.stderr)
    return match.group(1) if result.returncode == 0 and match else None


def expected_stamp(pin):
    return {'version': pin['version'], 'sha256': pin['sha256']}


def reusable(destination, pin, check):
    path = Path(destination) / STAMP
    try:
        recorded = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False
    tool = binary(destination, pin)
    if recorded != expected_stamp(pin) or not tool.is_file():
        return False
    return not check or installed_version(tool) == pin['version']


def kit_members(package, root):
    """The archive's members below `root` (which must end in a slash), with the root stripped; any other member is
    refused, so the archive cannot place a file beside the kit."""
    if not root.endswith('/'):
        raise pins.PinError(f'the pinned SDE root must end in a slash: {root}')
    members = []
    for info in package.getmembers():
        name = info.name.rstrip('/')
        if name == root.rstrip('/'):
            continue
        if not name.startswith(root):
            raise pins.PinError(f'archive member outside {root}: {info.name}')
        info.name = name[len(root):]
        members.append(info)
    return sorted(members, key=lambda info: info.issym() or info.islnk())


def kit_filter(member, path):
    """tarfile's `data` filter; on Windows a symlink it accepted becomes a hard link to the same member."""
    member = tarfile.data_filter(member, path)
    if WINDOWS and member.issym():
        target = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), member.linkname))
        member = copy.copy(member)
        member.type, member.linkname = tarfile.LNKTYPE, target
    return member


def install(destination=None, archive=None, download_dir=None, pin=None, check=True):
    """Return the installed SDE binary under `destination`, extracting the kit unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    destination = Path(destination or default_destination(pin)).resolve()
    if reusable(destination, pin, check):
        print(f'Intel SDE {pin["version"]} already installed at {binary(destination, pin)}; stamp and version match.')
    else:
        if destination.exists() and not (destination / STAMP).is_file():
            raise pins.PinError(f'{destination} exists and holds no kit this script installed; choose another '
                                '--destination')
        download_dir = Path(download_dir or default_download_dir(pin)).resolve()
        verified = pins.fetch(pin, download_dir, archive, label='Intel SDE archive')
        staging = destination.with_name(destination.name + '.partial')
        if staging.exists():
            shutil.rmtree(staging)
        staging.mkdir(parents=True)
        try:
            with tarfile.open(verified, 'r:*', errorlevel=2) as package:
                package.extractall(staging, members=kit_members(package, pin['root']), filter=kit_filter)
            tool = binary(staging, pin)
            if not tool.is_file():
                raise pins.PinError(f'{verified.name}: the kit has no {pin["binary"]}')
            if check:
                reported = installed_version(tool)
                if reported != pin['version']:
                    raise pins.PinError(f'installed SDE reports {reported!r}, the registry pins {pin["version"]}')
            (staging / STAMP).write_text(json.dumps(expected_stamp(pin), indent=2) + '\n', encoding='utf-8')
            if destination.exists():
                shutil.rmtree(destination)
            staging.rename(destination)
        except BaseException:
            shutil.rmtree(staging, ignore_errors=True)
            raise
        print(f'Installed Intel SDE {pin["version"]}; archive SHA-256 verified, {pin["binary"]} reports the version.')
    tool = binary(destination, pin)
    print(f'CRD_SDE={tool}')
    return tool


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the kit (default: ~/.cerid/tools/sde)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept')
    parser.add_argument('--archive', type=Path, help='an already downloaded official archive (checksum required)')
    parser.add_argument('--github-env', type=Path, help='append CRD_SDE=<binary> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    tool = install(args.destination, args.archive, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_SDE={tool}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
