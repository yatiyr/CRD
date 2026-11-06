#!/usr/bin/env python3
"""Install the pinned NASM for Linux (tools/nasm-linux-x64 in cmake/pins.json), the assembler of ISA-L's x86-64
erasure code.

Ubuntu 24.04's build, one package from Launchpad's permanent librarian, verified by its SHA-256 and unpacked with
`dpkg-deb -x` into a prefix without root, so the same install runs on a developer's machine, WSL and the hosted lanes;
`nasm -v` must report the pinned upstream version. The prefix goes under `~/.cerid/tools/nasm/<version>` by default,
beside a stamp (`cerid-nasm.json`) that lets a matching install be reused. Prints CRD_NASM=<nasm>, which a Linux
CRD_BUILD_ERASURE_VS_ISAL configure reads (`--github-env` also appends it to that file): ISA-L's assembly peer of the
Reed-Solomon oracle and crd-hesap-erasure-board (HESAP-NET). Windows x64 builds take the pinned nasm.exe through
cmake/pins.json `nasm` instead. Linux x64 only: the registry pins no other build.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import platform
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402
import ubuntu_packages  # noqa: E402

PIN_NAME = 'nasm-linux-x64'
STAMP = 'cerid-nasm.json'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'nasm' / pin['version']


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'nasm' / 'dl'


def reported_version(nasm):
    """The version `nasm -v` reports, or None."""
    result = subprocess.run([str(nasm), '-v'], capture_output=True, text=True, env={'PATH': '/usr/bin:/bin'})
    match = re.match(r'NASM version (\S+)', result.stdout) if result.returncode == 0 else None
    return match.group(1) if match else None


def install(destination=None, download_dir=None, pin=None, extract=ubuntu_packages.unpack,
            version_of=reported_version):
    """Return the installed nasm under `destination`, unpacking the package unless a matching install is present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    nasm = ubuntu_packages.install(pin, destination or default_destination(pin),
                                   download_dir or default_download_dir(pin), STAMP, 'NASM', extract, version_of)
    print(f'CRD_NASM={nasm}')
    return nasm


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the prefix '
                        '(default: ~/.cerid/tools/nasm/<version>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified package is kept (a package already '
                        'there is verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_NASM=<nasm> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins NASM for Linux x64 only')
    nasm = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_NASM={nasm}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
