#!/usr/bin/env python3
"""Install pinned Linux x86_64 validation tooling without replacing the system GPU driver/loader.

Run with Python 3.12+. Set VK_LAYER_PATH to <destination>/share/vulkan/explicit_layer.d
and prepend <destination>/lib to LD_LIBRARY_PATH before scoped GPU tests.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import tarfile
import urllib.request

VERSION = '1.4.341.1'
ARCHIVE = f'vulkansdk-linux-x86_64-{VERSION}.tar.xz'
URL = f'https://sdk.lunarg.com/sdk/download/{VERSION}/linux/{ARCHIVE}'
USER_AGENT = 'Cerid-SDK-Installer/1.0 (+https://github.com/yatiyr/CRD)'
SHA256 = '3bf0f762afb6c79bc6a9d9fb5998745ccff928800a29619b501ed9de7fd9789b'
MEMBERS = ('lib/libVkLayer_khronos_validation.so',
           'share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json')


def install(destination, archive):
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    if archive is None:
        archive = destination / ARCHIVE
        if not archive.exists():
            partial = archive.with_suffix('.partial')
            # The public SDK endpoint rejects Python's default user agent. Identify the actual client;
            # archive identity still comes from the pinned SHA-256, never the HTTP response alone.
            request = urllib.request.Request(URL, headers={'User-Agent': USER_AGENT})
            with urllib.request.urlopen(request, timeout=60) as response, partial.open('wb') as output:
                shutil.copyfileobj(response, output)
            partial.replace(archive)
    with archive.open('rb') as source:
        actual = hashlib.file_digest(source, 'sha256').hexdigest()
    if actual != SHA256:
        raise ValueError(f'SDK checksum mismatch: expected {SHA256}, got {actual}')
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
    if manifest['layer']['api_version'] != '1.4.341':
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
