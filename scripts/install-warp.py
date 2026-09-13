#!/usr/bin/env python3
"""Install the pinned, signed Microsoft WARP software rasterizer for app-local use beside DX12 test executables.
Run with Python 3.12+. Pass the installed DLL to CMake as -DCRD_WARP_DLL=<destination>/d3d10warp.dll; every
test executable then stages a copy in its own directory, which the D3D12 runtime loads ahead of the OS build.
The OS WARP shipped with Windows 10.0.26100 over-reads DXR state-object inputs under AddressSanitizer
(docs/recipes/2026-09-13-dx12-pinned-warp.md); the 1.0.20 package does not. Only the pinned package hash is trusted.
"""
from pathlib import Path
import argparse
import hashlib
import shutil
import urllib.request
import zipfile

VERSION = '1.0.20'
PACKAGE = f'microsoft.direct3d.warp.{VERSION}.nupkg'
URL = f'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/{VERSION}/{PACKAGE}'
USER_AGENT = 'Cerid-SDK-Installer/1.0 (+https://github.com/yatiyr/CRD)'
SHA256 = 'e5fe5de661ce98b58ef9cfb736e73c0a7a2623d3bbf5f14839b2d55566d87e40'
MEMBER = 'build/native/bin/x64/d3d10warp.dll'
MEMBER_SHA256 = '2a08692cba4c130593329255627fb915d666c90cda53d284594bc8438fd4f49d'
OUTPUT = 'd3d10warp.dll'


def install(destination, archive):
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    if archive is None:
        archive = destination / PACKAGE
        if not archive.exists():
            partial = archive.with_suffix('.partial')
            # Identify the actual client; package identity still comes from the pinned SHA-256, never the response.
            request = urllib.request.Request(URL, headers={'User-Agent': USER_AGENT})
            with urllib.request.urlopen(request, timeout=60) as response, partial.open('wb') as output:
                shutil.copyfileobj(response, output)
            partial.replace(archive)
    with archive.open('rb') as source:
        actual = hashlib.file_digest(source, 'sha256').hexdigest()
    if actual != SHA256:
        raise ValueError(f'WARP package checksum mismatch: expected {SHA256}, got {actual}')
    output = destination / OUTPUT
    with zipfile.ZipFile(archive) as package:
        info = package.getinfo(MEMBER)
        if info.is_dir():
            raise ValueError(f'WARP package member is not a regular file: {MEMBER}')
        # Only the exact, pinned member is copied; archive paths never select output locations.
        with package.open(info) as source, output.open('wb') as target:
            shutil.copyfileobj(source, target)
    with output.open('rb') as installed:
        member_actual = hashlib.file_digest(installed, 'sha256').hexdigest()
    if member_actual != MEMBER_SHA256:
        output.unlink()
        raise ValueError(f'WARP DLL checksum mismatch: expected {MEMBER_SHA256}, got {member_actual}')
    print(f'Installed Microsoft WARP {VERSION}; package and DLL SHA-256 verified.')
    print(f'CRD_WARP_DLL={output}')
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--archive', type=Path, help='Use an already downloaded official package (checksum required)')
    arguments = parser.parse_args()
    install(arguments.destination, arguments.archive)
