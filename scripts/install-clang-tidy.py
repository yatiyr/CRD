#!/usr/bin/env python3
"""Install the pinned clang-tidy 20 (the strict-analysis gate's tool) from cmake/pins.json.

On Linux (a developer host) the pin is tools/clang-tidy-linux, PyPI's clang-tidy wheel (a zip archive, members below
`clang_tidy/data/`); on Windows (the hosted clang-tidy lane) it is tools/llvm, LLVM's own Windows release archive (a
tar.xz, members below the pin's `root`), which replaced KyleMayes/install-llvm-action (no Node 24 release,
2026-10-26). The archive is fetched through pins.fetch(), so it is verified before the final name exists (an already
downloaded copy in --download-dir, by default `~/.cerid/downloads`, is verified instead of downloaded). Only the
pinned binary member and the pinned builtin-header directory are extracted, so the install keeps the layout
clang-tidy finds its resource directory by: `bin/clang-tidy` (`bin/clang-tidy.exe`) and `lib/clang/20/include/`. Every
member must be a regular file inside the destination; the binary's own SHA-256 and the header count must match the
pin, and the binary must report the pinned LLVM version. The tree is assembled beside the destination and renamed
into place only when it is complete. A stamp (`cerid-clang-tidy.json`) records the pin, so a matching install is
reused. The default destination, `~/.cerid/tools/clang-tidy`, is where the gate looks before PATH on Linux
(scripts/cerid_dev/environment.py); Windows developer hosts use LLVM 20.1.8 at C:/LLVM-20.1.8.
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
import tarfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN_NAME = 'llvm' if platform.system() == 'Windows' else 'clang-tidy-linux'
NAME = 'clang-tidy'
STAMP = 'cerid-clang-tidy.json'
DATA = 'clang_tidy/data/'
LLVM_VERSION = re.compile(r'LLVM version (\d+\.\d+\.\d+)')


def default_destination():
    return Path.home() / '.cerid' / 'tools' / NAME


def default_download_dir():
    return Path.home() / '.cerid' / 'downloads'


def binary(destination, pin=None):
    """The installed binary: `bin/clang-tidy`, or the pinned member's own name under `bin/` (`clang-tidy.exe`)."""
    name = PurePosixPath(pin['member']).name if pin is not None else 'clang-tidy'
    return Path(destination) / 'bin' / name


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


def reusable(destination, stamp, check, pin=None):
    path = Path(destination) / STAMP
    if not path.is_file():
        return False
    try:
        recorded = json.loads(path.read_text(encoding='utf-8'))
    except ValueError:
        return False
    tool = binary(destination, pin)
    if recorded != stamp or not tool.is_file():
        return False
    return not check or installed_version(tool) == stamp['version']


def data_path(member, root=DATA):
    """An archive member's path below `root` (the wheel's `clang_tidy/data/`, the LLVM archive's top directory);
    anything else, an absolute path or a `..` is refused."""
    if not member.startswith(root):
        raise pins.PinError(f'archive member outside {root}: {member}')
    relative = PurePosixPath(member[len(root):])
    if not relative.parts or relative.is_absolute() or '..' in relative.parts:
        raise pins.PinError(f'archive member escapes the install: {member}')
    return Path(*relative.parts)


def regular_file(info):
    """A zip member that is a regular file: not a directory, and its Unix mode names no other file type (a symbolic
    link above all); a member written without a file type is a plain file."""
    kind = stat.S_IFMT(info.external_attr >> 16)
    return not info.is_dir() and kind in (0, stat.S_IFREG)


def header_prefix(pin):
    """The pinned header directory, checked: it ends in a slash and lies below the pin's root."""
    prefix = pin['headers']
    if not prefix.endswith('/'):
        raise pins.PinError(f'the pinned header directory must end in a slash: {prefix}')
    data_path(prefix.rstrip('/'), pin.get('root', DATA))
    return prefix


def copy_member(source, target, root, name):
    """Write one archive member's bytes (the open `source`) to `target`, which must lie inside `root`."""
    if root.resolve() not in target.resolve().parents:
        raise pins.PinError(f'archive member escapes the install: {name}')
    target.parent.mkdir(parents=True, exist_ok=True)
    with source, target.open('wb') as output:
        shutil.copyfileobj(source, output)


def counted(archive, count, pin):
    if count != pin['header_count']:
        raise pins.PinError(f'{Path(archive).name}: {count} files under {pin["headers"]}, the registry pins '
                            f'{pin["header_count"]}')
    return count


def extract_headers(archive, pin, root):
    """Copy every regular file under the pin's `headers` directory of a zip archive to `root`; returns the number
    copied."""
    prefix = header_prefix(pin)
    count = 0
    with zipfile.ZipFile(archive) as package:
        for info in package.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            if not regular_file(info):
                raise pins.PinError(f'{Path(archive).name}: member is not a regular file: {info.filename}')
            copy_member(package.open(info), root / data_path(info.filename, pin.get('root', DATA)), root,
                        info.filename)
            count += 1
    return counted(archive, count, pin)


def extract_tar(archive, pin, root, tool):
    """One pass over a tar archive (the LLVM release's 5,000 members are decompressed once, in order): the pinned
    binary to `tool` and every regular file under the pin's `headers` directory below `root`; returns the number of
    headers. A member taken that is not a regular file (a link above all) is refused; directory entries are skipped."""
    prefix = header_prefix(pin)
    found = False
    count = 0
    with tarfile.open(archive, 'r|*') as package:
        for info in package:
            wanted = info.name == pin['member'] or (info.name.startswith(prefix) and not info.isdir())
            if not wanted:
                continue
            if not info.isfile():
                raise pins.PinError(f'{Path(archive).name}: member is not a regular file: {info.name}')
            if info.name == pin['member']:
                copy_member(package.extractfile(info), tool, root, info.name)
                found = True
            else:
                copy_member(package.extractfile(info), root / data_path(info.name, pin['root']), root, info.name)
                count += 1
    if not found:
        raise pins.PinError(f'{Path(archive).name}: no member {pin["member"]}')
    return count


def install(destination=None, archive=None, download_dir=None, pin=None, check=True):
    """Return the installed clang-tidy under `destination`, extracting it unless a matching install is present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    destination = Path(destination or default_destination()).resolve()
    stamp = expected_stamp(pin)
    if reusable(destination, stamp, check, pin):
        print(f'clang-tidy {pin["version"]} already installed at {binary(destination, pin)}; stamp and version match.')
        return binary(destination, pin)
    download_dir = Path(download_dir or default_download_dir()).resolve()
    verified = pins.fetch(pin, download_dir, archive, label='clang-tidy archive')
    staging = destination.with_name(destination.name + '.partial')
    if staging.exists():
        shutil.rmtree(staging)
    try:
        tool = staging / data_path(pin['member'], pin.get('root', DATA))
        if tool != binary(staging, pin) or tool.name not in ('clang-tidy', 'clang-tidy.exe'):
            raise pins.PinError(f'the pinned member {pin["member"]} does not install as bin/clang-tidy')
        tool.parent.mkdir(parents=True)
        zipped = zipfile.is_zipfile(verified)
        headers = None
        if zipped:
            pins.extract_member(verified, pin['member'], tool)
        else:
            headers = extract_tar(verified, pin, staging, tool)
        actual = pins.sha256(tool)
        if actual != pin['member_sha256']:
            raise pins.PinError(f'clang-tidy binary checksum mismatch: expected {pin["member_sha256"]}, got {actual}')
        tool.chmod(0o755)
        headers = extract_headers(verified, pin, staging) if zipped else counted(verified, headers, pin)
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
    print(f'Installed clang-tidy {pin["version"]} and {headers} builtin headers; archive and binary SHA-256 verified.')
    return binary(destination, pin)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, default=default_destination(),
                        help='install prefix (bin/clang-tidy and lib/clang/20/include land under it)')
    parser.add_argument('--download-dir', type=Path, default=default_download_dir(),
                        help='where the verified archive is kept (default: ~/.cerid/downloads)')
    parser.add_argument('--archive', type=Path, help='an already downloaded archive to verify and use')
    parser.add_argument('--github-env', type=Path,
                        help="append CRD_CI_TIDY=<installed binary> to this file (the workflow's $GITHUB_ENV)")
    args = parser.parse_args(argv)
    if platform.system() not in ('Linux', 'Windows'):
        parser.error("the pinned archives are a Linux wheel and LLVM's Windows x64 release")
    tool = install(args.destination, args.archive, args.download_dir)
    if args.github_env is not None:
        with args.github_env.open('a', encoding='utf-8') as env_file:
            env_file.write(f'CRD_CI_TIDY={tool}\n')
    print(f'CRD_CLANG_TIDY={tool}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
