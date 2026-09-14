#!/usr/bin/env python3
"""Install the pinned, signed Microsoft WARP software rasterizer for app-local use beside DX12 test executables.
Run with Python 3.12+. Pass the installed DLL to CMake as -DCRD_WARP_DLL=<destination>/d3d10warp.dll; every
test executable then stages a copy in its own directory, which the D3D12 runtime loads ahead of the OS build.
The OS WARP shipped with Windows 10.0.26100 over-reads DXR state-object inputs under AddressSanitizer
(docs/recipes/2026-09-13-dx12-pinned-warp.md); the 1.0.20 package does not. Only the pinned package hash is trusted.
The version, URL, package and member hashes come from cmake/pins.json (tools/warp).
"""
from pathlib import Path
import argparse
import hashlib
import shutil
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN = pins.entry('tools', 'warp')
VERSION = PIN['version']
PACKAGE = PIN['file']
URL = PIN['url']
USER_AGENT = pins.USER_AGENT
SHA256 = PIN['sha256']
MEMBER = PIN['member']
MEMBER_SHA256 = PIN['member_sha256']
OUTPUT = 'd3d10warp.dll'


def install(destination, archive):
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    # Downloads go through a .partial file and are verified before the final name exists; an archive handed in is
    # verified in place. Package identity comes from the pinned SHA-256, never from the response.
    archive = pins.fetch(PIN, destination, archive, label='WARP package', sha=SHA256)
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
