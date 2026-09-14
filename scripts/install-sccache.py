#!/usr/bin/env python3
"""Install the pinned sccache binary (the compiler launcher measured by REPO.DEV.7) from cmake/pins.json.

The archive (tools/sccache-windows or tools/sccache-linux by host) is fetched through pins.fetch(), so it is
verified before the final name exists; only the pinned member is extracted, its own SHA-256 is verified, and the
binary must report the pinned version. Prints the path to pass as -DCRD_COMPILER_LAUNCHER=<path>.
Contract: docs/design/build-performance.md; registry contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import hashlib
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

TOOL = 'sccache-windows' if platform.system() == 'Windows' else 'sccache-linux'
PIN = pins.entry('tools', TOOL)
VERSION = PIN['version']


def extract_member(archive, member, output):
    """Copy exactly `member` out of a zip or tar.gz archive to `output`; archive paths never choose the location."""
    archive = Path(archive)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as package:
            info = package.getinfo(member)
            if info.is_dir():
                raise pins.PinError(f'{archive.name}: member is not a regular file: {member}')
            with package.open(info) as source, output.open('wb') as target:
                shutil.copyfileobj(source, target)
        return
    with tarfile.open(archive, 'r:*') as package:
        info = package.getmember(member)
        if not info.isfile():
            raise pins.PinError(f'{archive.name}: member is not a regular file: {member}')
        source = package.extractfile(info)
        with source, output.open('wb') as target:
            shutil.copyfileobj(source, target)


def install(destination, archive=None, download_dir=None, pin=None, check=True):
    """Return the verified sccache path under `destination`."""
    pin = PIN if pin is None else pin
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    download_dir = Path(download_dir).resolve() if download_dir else destination
    archive = pins.fetch(pin, download_dir, archive, label='sccache archive')
    output = destination / Path(pin['member']).name
    extract_member(archive, pin['member'], output)
    with output.open('rb') as installed:
        actual = hashlib.file_digest(installed, 'sha256').hexdigest()
    if actual != pin['member_sha256']:
        output.unlink()
        raise pins.PinError(f'sccache binary checksum mismatch: expected {pin["member_sha256"]}, got {actual}')
    output.chmod(0o755)
    if check:
        reported = subprocess.run([str(output), '--version'], capture_output=True, text=True, check=True).stdout.strip()
        if reported != f'sccache {pin["version"]}':
            output.unlink()
            raise pins.PinError(f'installed binary reports {reported!r}, registry pins sccache {pin["version"]}')
    print(f'Installed sccache {pin["version"]}; archive and binary SHA-256 verified.')
    print(f'CRD_COMPILER_LAUNCHER={output}')
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, required=True, help='directory receiving the binary')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept (default: destination)')
    parser.add_argument('--archive', type=Path, help='an already downloaded official archive (checksum required)')
    args = parser.parse_args(argv)
    install(args.destination, args.archive, args.download_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
