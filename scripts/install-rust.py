#!/usr/bin/env python3
"""Install the pinned Rust toolchain (tools/rust-linux-x64 in cmake/pins.json) for the repository's Rust test peers.

The toolchain is four of the Rust project's component archives for x86_64 Linux (rustc, rust-std, Cargo and Clippy),
each verified by its SHA-256 and unpacked by pins.install_tree: every archive's component directory (its `root`) goes
into one prefix, which is then the layout rustup installs (bin/rustc, bin/cargo, bin/cargo-clippy, lib/rustlib/
<target>); the installer's own script and metadata beside it are left out, each archive's licences go to
licenses/<archive>/ and the per-component `manifest.in` is omitted. `rustc --version` and `cargo --version` must both
report the pinned version. The prefix is installed under `~/.cerid/tools/rust/<version>/linux-x64` by default, beside a
stamp (`cerid-rust.json`), so a matching install is reused. Prints CRD_CARGO=<cargo>, which a CRD_BUILD_MLS_VS_OPENMLS
configure reads (`--github-env` also appends it to that file); its consumers lint with `cargo clippy` and build with
`cargo build --locked` against a committed Cargo.lock (every crate addressed by its SHA-256), with CARGO_HOME inside
the build tree. Linux x64 only: the registry pins no other build.
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

PIN_NAME = 'rust-linux-x64'
STAMP = 'cerid-rust.json'
RUSTC = 'bin/rustc'
CARGO = 'bin/cargo'
TOOLS = (CARGO, 'bin/cargo-clippy', 'bin/clippy-driver')


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'rust' / pin['version'] / 'linux-x64'


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'rust' / pin['version'] / 'dl'


def reported_version(tool, name):
    """The version `<tool> --version` reports when it names `name` (`rustc 1.99.1 (...)`), or None."""
    if not Path(tool).is_file():
        return None
    result = subprocess.run([str(tool), '--version'], capture_output=True, text=True)
    match = re.match(rf'{name} (\d+\.\d+\.\d+)\b', result.stdout) if result.returncode == 0 else None
    return match.group(1) if match else None


def installed_version(rustc):
    """The toolchain's version when rustc and the Cargo beside it report the same one, else what rustc reports."""
    version = reported_version(rustc, 'rustc')
    cargo = reported_version(Path(rustc).parent / 'cargo', 'cargo')
    return version if cargo == version else f'rustc {version}, cargo {cargo}'


def install(destination=None, download_dir=None, pin=None, check=True):
    """Return the installed `cargo` under `destination`, unpacking the archives unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    if pin['binary'] != RUSTC:
        raise pins.PinError(f'tools/{PIN_NAME} must name {RUSTC} as its binary')
    rustc = pins.install_tree(pin, destination or default_destination(pin), download_dir or default_download_dir(pin),
                              RUSTC, STAMP, label='Rust', version_of=installed_version if check else None)
    prefix = rustc.parent.parent
    missing = [tool for tool in TOOLS if not (prefix / tool).is_file()]
    if missing:
        raise pins.PinError(f'the Rust toolchain at {prefix} has no {", ".join(missing)}')
    cargo = prefix / CARGO
    print(f'CRD_CARGO={cargo}')
    return cargo


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the toolchain '
                        '(default: ~/.cerid/tools/rust/<version>/linux-x64)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archives are kept (archives already '
                        'there are verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_CARGO=<cargo> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins the Rust toolchain for Linux x64 only')
    cargo = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_CARGO={cargo}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
