#!/usr/bin/env python3
"""Install the pinned mold linker (the opt-in GCC and Clang linker of build performance) from cmake/pins.json.

The archive (tools/mold-linux) is fetched through pins.fetch(), so it is verified before the final name exists; only
the pinned `bin/mold` member is extracted, its own SHA-256 is verified, and the binary must report the pinned version.
Beside it the installer creates `ld.mold`, the name `-fuse-ld=mold` looks for in the `-B` directory the CMake module
passes, as a link to `mold`. Prints the path to pass as -DCRD_LINKER=<path>. Linux only: mold links ELF programs.
Contract: docs/design/build-performance.md#the-linker; registry contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import os
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN = pins.entry('tools', 'mold-linux')
VERSION = PIN['version']
ALIAS = 'ld.mold'


def install(destination, archive=None, download_dir=None, pin=None, check=True):
    """Return the verified mold path under `destination`, with its `ld.mold` link beside it."""
    pin = PIN if pin is None else pin
    output = pins.install_member(pin, destination, archive, download_dir, label='mold')
    alias = output.parent / ALIAS
    if alias.is_symlink() or alias.exists():
        alias.unlink()
    try:
        alias.symlink_to(output.name)
    except OSError:
        # A symbolic link needs a privilege on Windows without developer mode (the tooling suite runs there too); a
        # hard link names the same file.
        os.link(output, alias)
    if check:
        reported = subprocess.run([str(output), '--version'], capture_output=True, text=True, check=True).stdout.strip()
        if not reported.startswith(f'mold {pin["version"]} '):
            output.unlink()
            alias.unlink()
            raise pins.PinError(f'installed binary reports {reported!r}, registry pins mold {pin["version"]}')
    print(f'Installed mold {pin["version"]}; archive and binary SHA-256 verified; {ALIAS} links to it.')
    print(f'CRD_LINKER={output}')
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, required=True, help='directory receiving mold and ld.mold')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept (default: destination)')
    parser.add_argument('--archive', type=Path, help='an already downloaded official archive (checksum required)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux':
        parser.error('mold links ELF programs; install it on the Linux host (WSL: python3 scripts/install-mold.py)')
    install(args.destination, args.archive, args.download_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
