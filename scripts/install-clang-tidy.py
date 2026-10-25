#!/usr/bin/env python3
"""Install the pinned clang-tidy 20 (the strict-analysis gate's tool on a Linux developer host) from cmake/pins.json.

Reads cmake/pins.json (tools/clang-tidy-linux): PyPI's clang-tidy wheel, a zip archive. The wheel is fetched through
pins.fetch(), so it is verified before the final name exists (an already downloaded copy in --download-dir, by default
`~/.cerid/downloads`, is verified instead of downloaded). Only the pinned binary member and the pinned builtin-header
directory are extracted, below the wheel's `clang_tidy/data/`, so the install keeps the layout clang-tidy finds its
resource directory by: `bin/clang-tidy` and `lib/clang/20/include/`. Every member must be a regular file inside the
destination; the binary's own SHA-256 and the header count must match the pin, and the binary must report the pinned
LLVM version. The tree is assembled beside the destination and renamed into place only when it is complete. A stamp
(`cerid-clang-tidy.json`) records the pin, so a matching install is reused. The default destination,
`~/.cerid/tools/clang-tidy`, is where the gate looks before PATH (scripts/cerid_dev/environment.py). Linux only.
Contract: docs/BUILDING.md (strict analysis) and docs/design/pinned-inputs.md.
"""
from pathlib import Path, PurePosixPath
import argparse
import json
import platform
import re
import shutil
import stat
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN_NAME = 'clang-tidy-linux'
NAME = 'clang-tidy'
STAMP = 'cerid-clang-tidy.json'
DATA = 'clang_tidy/data/'
LLVM_VERSION = re.compile(r'LLVM version (\d+\.\d+\.\d+)')


def default_destination():
    return Path.home() / '.cerid' / 'tools' / NAME


def default_download_dir():
    return Path.home() / '.cerid' / 'downloads'


def binary(destination):
    return Path(destination) / 'bin' / 'clang-tidy'


def installed_version(tool):
    """The LLVM version `clang-tidy --version` reports, or None."""
    if not Path(tool).is_file():
        return None
    result = subprocess.run([str(tool), '--version'], capture_output=True, text=True)
    match = LLVM_VERSION.search(result.stdout)
    return match.group(1) if result.returncode == 0 and match else None


def expected_stamp(pin):
    return {'version': pin['version'], 'sha256': pin['sha256'], 'member_sha256': pin['member_sha256'],
            'header_count': pin['header_count']}


def reusable(destination, stamp, check):
    path = Path(destination) / STAMP
    if not path.is_file():
        return False
    try:
        recorded = json.loads(path.read_text(encoding='utf-8'))
    except ValueError:
        return False
    if recorded != stamp or not binary(destination).is_file():
        return False
    return not check or installed_version(binary(destination)) == stamp['version']


def data_path(member):
    """A wheel member's path below `clang_tidy/data/`; anything else, an absolute path or a `..` is refused."""
    if not member.startswith(DATA):
        raise pins.PinError(f'wheel member outside {DATA}: {member}')
    relative = PurePosixPath(member[len(DATA):])
    if not relative.parts or relative.is_absolute() or '..' in relative.parts:
        raise pins.PinError(f'wheel member escapes the install: {member}')
    return Path(*relative.parts)


def regular_file(info):
    """A zip member that is a regular file: not a directory, and its Unix mode names no other file type (a symbolic
    link above all); a member written without a file type is a plain file."""
    kind = stat.S_IFMT(info.external_attr >> 16)
    return not info.is_dir() and kind in (0, stat.S_IFREG)


def extract_headers(archive, pin, root):
    """Copy every regular file under the pin's `headers` directory to `root`; returns the number copied."""
    prefix = pin['headers']
    if not prefix.endswith('/'):
        raise pins.PinError(f'the pinned header directory must end in a slash: {prefix}')
    data_path(prefix.rstrip('/'))
    count = 0
    with zipfile.ZipFile(archive) as package:
        for info in package.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            if not regular_file(info):
                raise pins.PinError(f'{Path(archive).name}: member is not a regular file: {info.filename}')
            target = root / data_path(info.filename)
            if root.resolve() not in target.resolve().parents:
                raise pins.PinError(f'wheel member escapes the install: {info.filename}')
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(info) as source, target.open('wb') as output:
                shutil.copyfileobj(source, output)
            count += 1
    if count != pin['header_count']:
        raise pins.PinError(f'{Path(archive).name}: {count} files under {prefix}, the registry pins {pin["header_count"]}')
    return count


def install(destination=None, archive=None, download_dir=None, pin=None, check=True):
    """Return the installed clang-tidy under `destination`, extracting it unless a matching install is present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    destination = Path(destination or default_destination()).resolve()
    stamp = expected_stamp(pin)
    if reusable(destination, stamp, check):
        print(f'clang-tidy {pin["version"]} already installed at {binary(destination)}; stamp and version match.')
        return binary(destination)
    download_dir = Path(download_dir or default_download_dir()).resolve()
    verified = pins.fetch(pin, download_dir, archive, label='clang-tidy wheel')
    staging = destination.with_name(destination.name + '.partial')
    if staging.exists():
        shutil.rmtree(staging)
    try:
        tool = staging / data_path(pin['member'])
        if tool != binary(staging):
            raise pins.PinError(f'the pinned member {pin["member"]} does not install as bin/clang-tidy')
        tool.parent.mkdir(parents=True)
        pins.extract_member(verified, pin['member'], tool)
        actual = pins.sha256(tool)
        if actual != pin['member_sha256']:
            raise pins.PinError(f'clang-tidy binary checksum mismatch: expected {pin["member_sha256"]}, got {actual}')
        tool.chmod(0o755)
        headers = extract_headers(verified, pin, staging)
        if check:
            reported = installed_version(tool)
            if reported != pin['version']:
                raise pins.PinError(f'installed binary reports LLVM {reported!r}, the registry pins {pin["version"]}')
        (staging / STAMP).write_text(json.dumps(stamp, indent=2) + '\n', encoding='utf-8')
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    if destination.exists():
        shutil.rmtree(destination)
    staging.rename(destination)
    print(f'Installed clang-tidy {pin["version"]} and {headers} builtin headers; wheel and binary SHA-256 verified.')
    return binary(destination)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, default=default_destination(),
                        help='install prefix (bin/clang-tidy and lib/clang/20/include land under it)')
    parser.add_argument('--download-dir', type=Path, default=default_download_dir(),
                        help='where the verified wheel is kept (default: ~/.cerid/downloads)')
    parser.add_argument('--archive', type=Path, help='an already downloaded wheel to verify and use')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux':
        parser.error('the pinned wheel is a Linux build; Windows uses LLVM 20.1.8 at C:/LLVM-20.1.8')
    tool = install(args.destination, args.archive, args.download_dir)
    print(f'CRD_CLANG_TIDY={tool}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
