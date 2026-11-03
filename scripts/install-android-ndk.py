#!/usr/bin/env python3
"""Install the pinned Android NDK (tools/android-ndk-linux-x64 in cmake/pins.json) for the Android test lanes.

The NDK is Google's release zip for Linux x64 from the SDK repository (dl.google.com/android/repository), verified by
its SHA-256 (the registry also records the SHA-1 the repository index lists) and unpacked by pins.install_tree, whose
zip reader keeps every file's mode and the NDK's symbolic links; the archive's top directory is the prefix. The NDK's
source.properties must report the pinned Pkg.Revision. The prefix goes under `~/.cerid/tools/android-ndk/<version>`
by default, beside a stamp (`cerid-android-ndk.json`), so a matching install is reused. Prints CRD_ANDROID_NDK=<root>,
which the Android presets read for their toolchain file (`--github-env` also appends it to that file). Only the
test lanes use it (NET.2's Android Keystore protector): never a shipped build. Linux x64 only: the
registry pins no other host build.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import platform
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN_NAME = 'android-ndk-linux-x64'
STAMP = 'cerid-android-ndk.json'
TOOLCHAIN = 'build/cmake/android.toolchain.cmake'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'android-ndk' / pin['version']


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'android-ndk' / 'dl'


def reported_version(toolchain):
    """The Pkg.Revision of the NDK whose CMake toolchain file is `toolchain`, or None."""
    try:
        text = (Path(toolchain).parents[2] / 'source.properties').read_text(encoding='utf-8')
    except OSError:
        return None
    for line in text.splitlines():
        key, _, value = line.partition('=')
        if key.strip() == 'Pkg.Revision':
            return value.strip()
    return None


def install(destination=None, download_dir=None, pin=None):
    """Return the installed NDK root under `destination`, unpacking the archive unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    toolchain = pins.install_tree(pin, destination or default_destination(pin),
                                  download_dir or default_download_dir(pin), TOOLCHAIN, STAMP, label='Android NDK',
                                  version_of=reported_version)
    root = toolchain.parents[2]
    print(f'CRD_ANDROID_NDK={root}')
    return root


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the NDK '
                        '(default: ~/.cerid/tools/android-ndk/<version>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archive is kept (an archive already '
                        'there is verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_ANDROID_NDK=<root> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins the Android NDK for Linux x64 only')
    root = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_ANDROID_NDK={root}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
