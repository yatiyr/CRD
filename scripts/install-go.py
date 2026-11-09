#!/usr/bin/env python3
"""Install the pinned Go toolchain (tools/go-linux-x64 in cmake/pins.json) for the repository's Go test peers.

Go runs from its whole distribution (`bin/go`, its standard library and `pkg/tool`), so every member below the pin's
`root` is extracted by pins.install_tree: the archive's SHA-256 is verified before use, nothing outside the root or
escaping the tree is extracted, `bin/go`'s own digest (`member_sha256`) is verified and `go version` must report the
pinned version. The tree is installed under `~/.cerid/tools/go/<version>/linux-x64` by default, beside a stamp
(`cerid-go.json`), so a matching install is reused. Prints CRD_GO=<go>, which a CRD_BUILD_WEBTRANSPORT_VS_GO configure
reads (`--github-env` also appends it to that file); its consumers build against a committed go.sum (every module
addressed by its h1 hash) with GOTOOLCHAIN=local, the module cache inside the build tree. Linux x64 only: the registry
pins no other build.
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

PIN_NAME = 'go-linux-x64'
STAMP = 'cerid-go.json'
GO = 'bin/go'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'go' / pin['version'] / 'linux-x64'


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'go' / pin['version'] / 'dl'


def installed_version(go):
    """The version `go version` reports (`go version go1.27.3 linux/amd64`), or None."""
    if not Path(go).is_file():
        return None
    result = subprocess.run([str(go), 'version'], capture_output=True, text=True)
    match = re.match(r'go version go(\d+\.\d+(?:\.\d+)?)\s', result.stdout) if result.returncode == 0 else None
    return match.group(1) if match else None


def install(destination=None, archive=None, download_dir=None, pin=None, check=True):
    """Return the installed `go` under `destination`, extracting the distribution unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    go = pins.install_tree(pin, destination or default_destination(pin), download_dir or default_download_dir(pin),
                           GO, STAMP, archive, label='Go', version_of=installed_version if check else None)
    print(f'CRD_GO={go}')
    return go


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the distribution '
                        '(default: ~/.cerid/tools/go/<version>/linux-x64)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept')
    parser.add_argument('--archive', type=Path, help='an already downloaded official archive (checksum required)')
    parser.add_argument('--github-env', type=Path, help='append CRD_GO=<go> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins the Go toolchain for Linux x64 only')
    go = install(args.destination, args.archive, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_GO={go}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
