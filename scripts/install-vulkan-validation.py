#!/usr/bin/env python3
"""Install pinned Linux x86_64 validation tooling without replacing the system GPU driver/loader.

Run with Python 3.12+. Set VK_LAYER_PATH to <destination>/share/vulkan/explicit_layer.d
and prepend <destination>/lib to LD_LIBRARY_PATH before scoped GPU tests.
The version, URL, SHA-256 and member list come from cmake/pins.json (tools/vulkan-sdk-linux).
"""
from pathlib import Path
import argparse
import json
import shutil
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN = pins.entry('tools', 'vulkan-sdk-linux')
VERSION = PIN['version']
ARCHIVE = PIN['file']
URL = PIN['url']
USER_AGENT = pins.USER_AGENT
SHA256 = PIN['sha256']
MEMBERS = tuple(PIN['members'])


def install(destination, archive):
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    # Downloads go through a .partial file and are verified before the final name exists; an archive handed in is
    # verified in place. Identity comes from the pinned SHA-256, never from the HTTP response alone.
    archive = pins.fetch(PIN, destination, archive, label='SDK', sha=SHA256)
    wanted = {f'{VERSION}/x86_64/{name}': name for name in MEMBERS}
    found = set()
    with tarfile.open(archive, 'r|xz') as sdk:
        for member in sdk:
            if member.name not in wanted:
                continue
            if not member.isfile():
                raise ValueError(f'SDK member is not a regular file: {member.name}')
            relative = wanted[member.name]
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            # Only exact, pinned member names are copied; archive paths never select output locations.
            with sdk.extractfile(member) as source, output.open('wb') as target:
                shutil.copyfileobj(source, target)
            found.add(relative)
            if len(found) == len(MEMBERS):
                break
    if found != set(MEMBERS):
        raise ValueError(f'Missing validation SDK members: {set(MEMBERS) - found}')
    manifest = json.loads((destination / MEMBERS[1]).read_text(encoding='utf-8'))
    if manifest['layer']['api_version'] != PIN['api_version']:
        raise ValueError('Unexpected validation layer API version')
    print(f'Installed Khronos validation {VERSION}; archive SHA-256 verified.')
    print(f'VK_LAYER_PATH={destination / "share/vulkan/explicit_layer.d"}')
    print(f'LD_LIBRARY_PATH prefix={destination / "lib"}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--archive', type=Path, help='Use an already downloaded official archive (checksum required)')
    arguments = parser.parse_args()
    install(arguments.destination, arguments.archive)
