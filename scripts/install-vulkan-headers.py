#!/usr/bin/env python3
"""Assemble the pinned Linux Vulkan header tree (Khronos headers plus SPIRV-Reflect sources) by SHA-256.

Reads cmake/pins.json (tools/vulkan-headers, tools/spirv-reflect, tools/spirv-headers). The Vulkan-Headers
archive is downloaded and verified, and only its `include/` subtree is extracted; each SPIRV-Reflect file is
downloaded and verified individually. Nothing is written under the destination until its digest matched.
`--archive-dir` supplies already downloaded files (still verified) for offline setup. Prints
`VULKAN_SDK=<destination>` on success. Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import shutil
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

REFLECT_DIR = 'Source/SPIRV-Reflect'
FILE_LAYOUT = {
    ('spirv-reflect', 'spirv_reflect.h'): f'{REFLECT_DIR}/spirv_reflect.h',
    ('spirv-reflect', 'spirv_reflect.c'): f'{REFLECT_DIR}/spirv_reflect.c',
    ('spirv-headers', 'include/spirv/unified1/spirv.h'): f'{REFLECT_DIR}/include/spirv/unified1/spirv.h',
}


def extract_include(archive, destination):
    """Copy <top>/include/** of the archive into <destination>/include; every member path is validated."""
    written = 0
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar:
            parts = Path(member.name).parts
            if len(parts) < 3 or parts[1] != 'include' or '..' in parts or Path(member.name).is_absolute():
                continue
            if not member.isfile():
                continue
            relative = Path(*parts[1:])
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source, output.open('wb') as target:
                shutil.copyfileobj(source, target)
            written += 1
    if not written:
        raise pins.PinError(f'{archive} holds no include/ subtree')
    return written


def install(destination, archive_dir=None, download_dir=None):
    destination = Path(destination).resolve()
    download_dir = Path(download_dir or tempfile.mkdtemp(prefix='cerid-vulkan-headers-'))
    headers = pins.entry('tools', 'vulkan-headers')
    local = Path(archive_dir) / headers['file'] if archive_dir else None
    archive = pins.fetch(headers, download_dir, local if local and local.exists() else None, label='Vulkan-Headers')
    count = extract_include(archive, destination)
    for (tool, name), relative in FILE_LAYOUT.items():
        pin = pins.entry('tools', tool)['files'][name]
        file_name = Path(name).name
        local = Path(archive_dir) / file_name if archive_dir else None
        verified = pins.fetch(None, download_dir, local if local and local.exists() else None, label=name,
                              url=pin['url'], sha=pin['sha256'], file=file_name)
        output = destination / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(verified, output)
    print(f'Installed Vulkan headers {headers["version"]} ({count} header files) and SPIRV-Reflect into {destination}; every digest verified.')
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, required=True, help='directory that becomes VULKAN_SDK')
    parser.add_argument('--archive-dir', type=Path, help='directory of already downloaded files (checksums required)')
    parser.add_argument('--download-dir', type=Path, help='directory receiving downloads (default: a temporary directory)')
    args = parser.parse_args(argv)
    destination = install(args.destination, args.archive_dir, args.download_dir)
    print(f'VULKAN_SDK={destination}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
