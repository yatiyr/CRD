#!/usr/bin/env python3
"""Install the pinned swtpm and tss2 (tools/swtpm-linux-x64 in cmake/pins.json), the TPM 2.0 the key store's Linux
protector and device are tested against.

Ubuntu 24.04's builds, eight packages from Launchpad's permanent librarian: swtpm and libtpms (the TCG reference
TPM 2.0 as a simulator) and the tss2 libraries crd-crypto loads at run time (libtss2-esys, -sys, -mu, -tctildr and the
swtpm TCTI, with the pcap TCTI that records the bus), each verified by its SHA-256 and unpacked with `dpkg-deb -x` into one prefix without root, so the same
install runs on a developer's machine, WSL and the hosted lanes; swtpm must report the pinned upstream version. The
prefix goes under `~/.cerid/tools/swtpm/<version>` by default, beside a stamp (`cerid-swtpm.json`) that lets a matching
install be reused. Prints CRD_SWTPM=<swtpm>, which a CRD_BUILD_TPM_VS_SWTPM configure reads (`--github-env` also
appends it to that file); the crd-crypto-tpm-swtpm CTest runs the simulator and the tests with the prefix's libraries
(tests/foundation/crypto/tpm_simulator_session.py). Linux x64 only: the registry pins no other build.
Contract: docs/design/pinned-inputs.md; the protector and device: docs/design/network-foundation.md#key-store (K13).
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

PIN_NAME = 'swtpm-linux-x64'
STAMP = 'cerid-swtpm.json'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'swtpm' / pin['version']


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'swtpm' / 'dl'


def library_path(swtpm):
    """The LD_LIBRARY_PATH of the prefix: its multiarch libraries (tss2, libtpms) and swtpm's private library."""
    libraries = ubuntu_packages.library_dir(Path(swtpm).parents[2])
    return f'{libraries}:{libraries / "swtpm"}'


def reported_version(swtpm):
    """The upstream version `swtpm --version` reports, or None."""
    result = subprocess.run([str(swtpm), '--version'], capture_output=True, text=True,
                            env={'LD_LIBRARY_PATH': library_path(swtpm), 'PATH': '/usr/bin:/bin'})
    match = re.match(r'TPM emulator version (\S+),', result.stdout) if result.returncode == 0 else None
    return match.group(1) if match else None


def install(destination=None, download_dir=None, pin=None, extract=ubuntu_packages.unpack,
            version_of=reported_version):
    """Return the installed swtpm under `destination`, unpacking the packages unless a matching install is present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    swtpm = ubuntu_packages.install(pin, destination or default_destination(pin),
                                    download_dir or default_download_dir(pin), STAMP, 'swtpm', extract, version_of)
    print(f'CRD_SWTPM={swtpm}')
    return swtpm


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the prefix '
                        '(default: ~/.cerid/tools/swtpm/<version>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified packages are kept (packages already '
                        'there are verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_SWTPM=<swtpm> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins swtpm for Linux x64 only')
    swtpm = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_SWTPM={swtpm}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
