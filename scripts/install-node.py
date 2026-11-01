#!/usr/bin/env python3
"""Install the pinned Node.js runtime (tools/node-linux-x64 in cmake/pins.json) for the repository's npm tools.

Node runs from its whole distribution (`bin/node`, the bundled npm under lib/node_modules and the `bin/npm` link), so
every member below the pin's `root` is extracted by pins.install_tree: the archive's SHA-256 is verified before use,
nothing outside the root or escaping the tree is extracted, `bin/node`'s own digest (`member_sha256`) is verified and
`node --version` must report the pinned version. The tree is installed under
`~/.cerid/tools/node/<version>/linux-x64` by default, beside a stamp (`cerid-node.json`), so a matching install is
reused. Its consumers run `npm ci` against a committed lockfile (every package addressed by its SHA-512 integrity) with
`--ignore-scripts`: the CI dashboard publisher (ci/dashboard) and the MCP Inspector of the MCP conformance evidence
(tests/tools/ceridc/mcp/inspector). Prints CRD_NODE=<node>. Linux x64 only: the registry pins no other build.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN_NAME = 'node-linux-x64'
STAMP = 'cerid-node.json'
NODE = 'bin/node'
NPM_CLI = 'lib/node_modules/npm/bin/npm-cli.js'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'node' / pin['version'] / 'linux-x64'


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'node' / pin['version'] / 'dl'


def installed_version(node):
    """The version `node --version` reports (without its `v`), or None."""
    if not Path(node).is_file():
        return None
    result = subprocess.run([str(node), '--version'], capture_output=True, text=True)
    reported = result.stdout.strip()
    return reported[1:] if result.returncode == 0 and reported.startswith('v') else None


def install(destination=None, archive=None, download_dir=None, pin=None, check=True):
    """Return the installed `node` under `destination`, extracting the distribution unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    node = pins.install_tree(pin, destination or default_destination(pin), download_dir or default_download_dir(pin),
                             NODE, STAMP, archive, label='Node.js', version_of=installed_version if check else None)
    print(f'CRD_NODE={node}')
    return node


def npm_command(node):
    """The command that runs the npm bundled with `node`, independent of PATH and of the `bin/npm` link."""
    return [str(node), str(Path(node).parent.parent / NPM_CLI)]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the distribution '
                        '(default: ~/.cerid/tools/node/<version>/linux-x64)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept')
    parser.add_argument('--archive', type=Path, help='an already downloaded official archive (checksum required)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins Node.js for Linux x64 only')
    install(args.destination, args.archive, args.download_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
