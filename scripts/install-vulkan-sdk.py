#!/usr/bin/env python3
"""Install the pinned Windows Vulkan SDK from a SHA-256-verified installer.

Reads cmake/pins.json (tools/vulkan-sdk-windows): the installer is downloaded to `--download-dir` through a
`.partial` file, verified against the pinned digest and run silently with a local repository that lists no package
as its only repository (the installer carries every package it installs, so the run reads no update metadata from
LunarG's remote repository); the installed layout is checked before the script reports success. `--archive` uses
an already downloaded installer (still verified) for offline setup.
Prints `VULKAN_SDK=<root>` on success. Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

INSTALL_ARGUMENTS = ('--accept-licenses', '--default-answer', '--confirm-command', 'install')
# `--set-temp-repository` makes this repository the only one the Qt Installer Framework's update fetch reads; the
# hosted experiment (run 41298875777, docs/sessions/2026-11-01-repo-3-installer-verdict.md) installed the same tree
# with it and no fetch from the remote repository.
REPOSITORY = 'vulkan-sdk-repository'
UPDATES_XML = ('<Updates>\n <ApplicationName>{AnyApplication}</ApplicationName>\n'
               ' <ApplicationVersion>1.0.0</ApplicationVersion>\n <Checksum>true</Checksum>\n</Updates>\n')
LAYOUT = ('Include/vulkan/vulkan.h', 'Lib/vulkan-1.lib', 'Bin/glslc.exe')


def local_repository(directory):
    """Write a repository whose Updates.xml lists no package to `directory`; returns its file URI."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'Updates.xml').write_text(UPDATES_XML, encoding='utf-8')
    return directory.resolve().as_uri()


def arguments(download_dir):
    """The installer's command-line arguments, with the local repository written under `download_dir`."""
    return [*INSTALL_ARGUMENTS, '--set-temp-repository', local_repository(Path(download_dir) / REPOSITORY)]


def installed(root):
    return all((root / relative).is_file() for relative in LAYOUT)


def install(download_dir, archive=None, root=None, run=subprocess.run):
    pin = pins.entry('tools', 'vulkan-sdk-windows')
    root = Path(root or pin['install_root'])
    if installed(root):
        print(f'Vulkan SDK {pin["version"]} already installed at {root}')
        return root
    installer = pins.fetch(pin, download_dir, archive, label='Vulkan SDK installer')
    completed = run([str(installer), *arguments(download_dir)], check=False)
    if completed.returncode != 0:
        raise pins.PinError(f'Vulkan SDK installer exited with {completed.returncode}')
    missing = [relative for relative in LAYOUT if not (root / relative).is_file()]
    if missing:
        raise pins.PinError(f'Vulkan SDK {pin["version"]} install at {root} lacks {missing}')
    print(f'Installed Vulkan SDK {pin["version"]} at {root}; installer SHA-256 verified.')
    return root


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--download-dir', type=Path,
                        default=Path(os.environ.get('RUNNER_TEMP') or os.environ.get('TEMP') or '.'),
                        help='directory receiving the verified installer and the local repository '
                             '(default: RUNNER_TEMP or TEMP)')
    parser.add_argument('--archive', type=Path, help='an already downloaded official installer (checksum required)')
    parser.add_argument('--root', type=Path, help='expected install root (default: the pinned install_root)')
    args = parser.parse_args(argv)
    root = install(args.download_dir, args.archive, args.root)
    print(f'VULKAN_SDK={root}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
