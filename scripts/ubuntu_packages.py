"""Install a pinned set of Ubuntu packages into one prefix, without root (the tools a Linux test runs against).

A pin of this kind (cmake/pins.json `tools`) names its packages under `files` (each a Launchpad librarian URL, its
size and SHA-256), the program the set provides (`binary`, relative to the prefix) and the Ubuntu `version` whose
upstream part that program must report. Each package is verified by its SHA-256 and unpacked with `dpkg-deb -x`, which
needs no root and runs no maintainer script, into a staging directory that replaces the prefix only once the program
reports the pinned version; a stamp beside it lets a matching install be reused. Used by
scripts/install-gnome-keyring.py and scripts/install-swtpm.py. Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import json
import shutil
import subprocess

import pins


def unpack(package, prefix):
    """Unpack one Debian package's data into `prefix` (dpkg-deb -x needs no root and runs no maintainer script)."""
    subprocess.run(['dpkg-deb', '-x', str(package), str(prefix)], check=True)


def library_dir(prefix):
    """The prefix's multiarch library directory."""
    return Path(prefix) / 'usr' / 'lib' / 'x86_64-linux-gnu'


def install(pin, destination, download_dir, stamp_name, label, extract, version_of):
    """Return the pin's program under `destination`, unpacking its packages unless a matching install is present.

    `version_of(program)` returns the upstream version the staged program reports (or None); it must equal the part
    of the pin's version before the first '-'.
    """
    destination = Path(destination)
    program = destination / pin['binary']
    upstream = pin['version'].split('-')[0]
    stamp = {'version': pin['version'], 'files': {name: spec['sha256'] for name, spec in pin['files'].items()}}
    stamp_path = destination / stamp_name
    if stamp_path.is_file() and json.loads(stamp_path.read_text(encoding='utf-8')) == stamp and program.is_file():
        return program
    packages = [pins.fetch(None, Path(download_dir), url=spec['url'], sha=spec['sha256'], file=name, label=name)
                for name, spec in pin['files'].items()]
    staging = destination.with_name(destination.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for package in packages:
        extract(package, staging)
    staged = staging / pin['binary']
    if not staged.is_file():
        shutil.rmtree(staging)
        raise pins.PinError(f'the packages of {label} hold no {pin["binary"]}')
    reported = version_of(staged)
    if reported != upstream:
        shutil.rmtree(staging)
        raise pins.PinError(f'the installed program reports {reported!r}, the registry pins {label} {upstream}')
    shutil.rmtree(destination, ignore_errors=True)
    staging.replace(destination)
    stamp_path.write_text(json.dumps(stamp, indent=1) + '\n', encoding='utf-8')
    print(f'Installed {label} {pin["version"]}; {len(packages)} packages verified by SHA-256.')
    return program
