#!/usr/bin/env python3
"""Install the pinned Tamarin prover and the Maude it runs on (tools/tamarin-linux-x64 in cmake/pins.json), the
protocol model checker of NET.2's model clause.

Two release archives without a top directory, each verified by its SHA-256 and unpacked into one prefix through
pins.install_tree: Tamarin's Linux x64 binary (`tamarin-prover`) and Maude's Linux x86_64 build (`maude` and its
prelude files). `tamarin-prover --version`, pointed at that Maude, must report the pinned Tamarin and accept the pinned
Maude. The prefix goes under `~/.cerid/tools/tamarin/<version>` by default, beside a stamp (`cerid-tamarin.json`) that
lets a matching install be reused. Prints CRD_TAMARIN=<tamarin-prover>, which a CRD_BUILD_PROTOCOL_MODEL configure
reads (`--github-env` also appends it to that file); the crd-crypto-protocol-model CTest proves the models in
tests/foundation/crypto/model with it (scripts/check-protocol-model.py). Linux x64 only: the registry pins no other
build. Contract: docs/design/pinned-inputs.md; the model: docs/design/network-foundation.md#protocol-model.
"""
from pathlib import Path
import argparse
import platform
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN_NAME = 'tamarin-linux-x64'
STAMP = 'cerid-tamarin.json'
TAMARIN = 'tamarin-prover'
MAUDE = 'maude'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'tamarin' / pin['version']


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'tamarin' / 'dl'


def reported_versions(text):
    """(Tamarin's version, the Maude version it checked) from `tamarin-prover --version`, each None when absent."""
    tamarin = re.search(r'^Tamarin version (\S+)$', text, re.M)
    maude = re.search(r'checking version: (\S+?)\. OK\.', text)
    return (tamarin.group(1) if tamarin else None), (maude.group(1) if maude else None)


def version_of(pin):
    """A version_of for pins.install_tree: the pinned Tamarin version when the binary reports it and accepts the
    pinned Maude beside it, else what was found."""
    def check(tool):
        result = subprocess.run([str(tool), f'--with-maude={Path(tool).parent / MAUDE}', '--version'],
                                capture_output=True, text=True, env={'PATH': '/usr/bin:/bin'})
        tamarin, maude = reported_versions(result.stdout + result.stderr)
        if result.returncode != 0 or tamarin is None:
            return None
        return tamarin if maude == pin['maude_version'] else f'{tamarin} with Maude {maude}'
    return check


def install(destination=None, download_dir=None, pin=None, check=True):
    """Return the installed tamarin-prover under `destination`, unpacking the archives unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    tool = pins.install_tree(pin, destination or default_destination(pin), download_dir or default_download_dir(pin),
                             TAMARIN, STAMP, label='Tamarin', version_of=version_of(pin) if check else None)
    print(f'CRD_TAMARIN={tool}')
    return tool


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the prefix '
                        '(default: ~/.cerid/tools/tamarin/<version>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archives are kept (archives already '
                        'there are verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_TAMARIN=<tamarin-prover> to this file (GitHub '
                        'Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins Tamarin for Linux x64 only')
    tool = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_TAMARIN={tool}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
