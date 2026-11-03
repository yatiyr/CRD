#!/usr/bin/env python3
"""Install the pinned SoftHSM and OpenSC's PKCS #11 spy (tools/softhsm-linux-x64 in cmake/pins.json), the PKCS #11
token the key store's PKCS #11 device is tested against.

Ubuntu 24.04's builds, four packages from Launchpad's permanent librarian: SoftHSM 2.6.1 (softhsm2-util, its
configuration and the libsofthsm2.so module) and OpenSC's opensc-pkcs11 for pkcs11-spy.so, a module that records every
call it passes to another; each is verified by its SHA-256 and unpacked with `dpkg-deb -x` into one prefix without root,
so the same install runs on a developer's machine, WSL and the hosted lanes; softhsm2-util must report the pinned
upstream version. The prefix goes under `~/.cerid/tools/softhsm/<version>` by default, beside a stamp
(`cerid-softhsm.json`) that lets a matching install be reused. Prints CRD_SOFTHSM=<softhsm2-util>, which a
CRD_BUILD_PKCS11_VS_SOFTHSM configure reads (`--github-env` also appends it to that file); the crd-crypto-pkcs11-softhsm
CTest makes a token of its own and runs the tests against it (tests/foundation/crypto/pkcs11_token_session.py). Linux
x64 only: the registry pins no other build.
Contract: docs/design/pinned-inputs.md; the device: docs/design/network-foundation.md#key-store (K14).
"""
from pathlib import Path
import argparse
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402
import ubuntu_packages  # noqa: E402

PIN_NAME = 'softhsm-linux-x64'
STAMP = 'cerid-softhsm.json'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'softhsm' / pin['version']


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'softhsm' / 'dl'


def reported_version(util):
    """The version `softhsm2-util --version` reports, or None."""
    result = subprocess.run([str(util), '--version'], capture_output=True, text=True, env={'PATH': '/usr/bin:/bin'})
    return (result.stdout.strip() or None) if result.returncode == 0 else None


def install(destination=None, download_dir=None, pin=None, extract=ubuntu_packages.unpack,
            version_of=reported_version):
    """Return the installed softhsm2-util under `destination`, unpacking the packages unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    util = ubuntu_packages.install(pin, destination or default_destination(pin),
                                   download_dir or default_download_dir(pin), STAMP, 'SoftHSM', extract, version_of)
    print(f'CRD_SOFTHSM={util}')
    return util


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the prefix '
                        '(default: ~/.cerid/tools/softhsm/<version>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified packages are kept (packages already '
                        'there are verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_SOFTHSM=<softhsm2-util> to this file (GitHub '
                        'Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins SoftHSM for Linux x64 only')
    util = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_SOFTHSM={util}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
