#!/usr/bin/env python3
"""Install the pinned gnome-keyring (tools/gnome-keyring-linux-x64 in cmake/pins.json), the Secret Service the key
store's Linux protector is tested against.

The daemon is Ubuntu's build, three packages from Launchpad's permanent librarian (gnome-keyring and the two GCR
libraries it needs beyond a stock Ubuntu 24.04), each verified by its SHA-256 and unpacked with `dpkg-deb -x` into
one prefix without root, so the same install runs on a developer's machine, WSL and the hosted lanes. Its daemon
must report the pinned upstream version. The prefix goes under `~/.cerid/tools/gnome-keyring/<version>` by default,
beside a stamp (`cerid-gnome-keyring.json`) that lets a matching install be reused. Prints CRD_GNOME_KEYRING=<daemon>,
which a CRD_BUILD_SECRET_SERVICE_VS_GNOME_KEYRING configure reads (`--github-env` also appends it to that file); the
crd-crypto-secret-service CTests run the daemon in a session bus of their own
(tests/foundation/crypto/secret_service_session.py). Linux x64 only: the registry pins no other build.
Contract: docs/design/pinned-inputs.md; the protector: docs/design/network-foundation.md#key-store (decision K10).
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

PIN_NAME = 'gnome-keyring-linux-x64'
STAMP = 'cerid-gnome-keyring.json'
unpack = ubuntu_packages.unpack


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'gnome-keyring' / pin['version']


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'gnome-keyring' / 'dl'


def reported_version(daemon):
    """The upstream version `gnome-keyring-daemon --version` reports, or None."""
    result = subprocess.run([str(daemon), '--version'], capture_output=True, text=True,
                            env={'LD_LIBRARY_PATH': str(library_dir(daemon)), 'PATH': '/usr/bin:/bin'})
    match = re.match(r'gnome-keyring-daemon: (\S+)', result.stdout) if result.returncode == 0 else None
    return match.group(1) if match else None


def library_dir(daemon):
    """The prefix's multiarch library directory, which the daemon's GCR libraries are loaded from."""
    return ubuntu_packages.library_dir(Path(daemon).parents[2])


def install(destination=None, download_dir=None, pin=None, extract=unpack, version_of=reported_version):
    """Return the installed daemon under `destination`, unpacking the packages unless a matching install is present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    daemon = ubuntu_packages.install(pin, destination or default_destination(pin),
                                     download_dir or default_download_dir(pin), STAMP, 'gnome-keyring', extract,
                                     version_of)
    print(f'CRD_GNOME_KEYRING={daemon}')
    return daemon


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the prefix '
                        '(default: ~/.cerid/tools/gnome-keyring/<version>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified packages are kept (packages already '
                        'there are verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path,
                        help='append CRD_GNOME_KEYRING=<daemon> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins gnome-keyring for Linux x64 only')
    daemon = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_GNOME_KEYRING={daemon}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
