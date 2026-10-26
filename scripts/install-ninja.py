#!/usr/bin/env python3
"""Install the pinned Ninja (the build tool of every Ninja preset on the hosted lanes) from cmake/pins.json.

The archive (tools/ninja-windows or tools/ninja-linux by host) is fetched through pins.fetch(), so it is verified
before the final name exists; only the pinned member is extracted, its own SHA-256 is verified, and the binary must
report the pinned version. Prints the directory to put on PATH; with --github-path it is also appended to that file
(the workflow's $GITHUB_PATH). It replaced seanmiddleditch/gha-setup-ninja, which has no Node 24 release (2026-10-26).
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

TOOL = 'ninja-windows' if platform.system() == 'Windows' else 'ninja-linux'


def install(destination, archive=None, download_dir=None, pin=None, check=True):
    """Return the verified ninja path under `destination`."""
    pin = pins.entry('tools', TOOL) if pin is None else pin
    output = pins.install_member(pin, destination, archive, download_dir, label='ninja')
    if check:
        reported = subprocess.run([str(output), '--version'], capture_output=True, text=True, check=True).stdout.strip()
        if reported != pin['version']:
            output.unlink()
            raise pins.PinError(f'installed binary reports {reported!r}, registry pins ninja {pin["version"]}')
    print(f'Installed ninja {pin["version"]}; archive and binary SHA-256 verified.')
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, required=True, help='directory receiving the binary')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept (default: destination)')
    parser.add_argument('--archive', type=Path, help='an already downloaded official archive (checksum required)')
    parser.add_argument('--github-path', type=Path, help="append the binary's directory to this file ($GITHUB_PATH)")
    args = parser.parse_args(argv)
    output = install(args.destination, args.archive, args.download_dir)
    if args.github_path is not None:
        with args.github_path.open('a', encoding='utf-8') as path_file:
            path_file.write(f'{output.parent}\n')
    print(f'CRD_NINJA_DIR={output.parent}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
