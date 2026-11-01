#!/usr/bin/env python3
"""Install the pinned CUDA Toolkit (tools/cuda-linux-x64 in cmake/pins.json) for the CUDA peer programs of the boards.

The toolkit is the set of NVIDIA's redistributable component archives the pin lists (the compiler, its crt headers
and libNVVM, the runtime, CCCL, NVRTC, nvJitLink, cuFFT and cuBLAS), each verified by its SHA-256 and unpacked below
its own root into one prefix by pins.install_tree, which refuses a path two archives place differently. `nvcc
--version` must report the pinned compiler (the pin's cuda_nvcc component). The prefix is installed under
`~/.cerid/tools/cuda/<version>/linux-x64` by default, beside a stamp (`cerid-cuda.json`), so a matching install is
reused; it is the CUDAToolkit_ROOT of a CRD_BUILD_GPU_VS_CUDA tree (scripts/run_bench_gpu_cuda.sh). Prints
CRD_CUDA_ROOT=<prefix>. Linux x64 only: a Windows host builds with its installed toolkit (CUDA_PATH).
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

PIN_NAME = 'cuda-linux-x64'
STAMP = 'cerid-cuda.json'
NVCC = 'bin/nvcc'
COMPILER = 'cuda_nvcc'


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'cuda' / pin['version'] / 'linux-x64'


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'cuda' / pin['version'] / 'dl'


def compiler_version(pin):
    """The version of the pin's compiler component, the one `nvcc --version` reports."""
    versions = [spec['version'] for spec in pin['files'].values() if spec.get('component') == COMPILER]
    if len(versions) != 1:
        raise pins.PinError(f'tools/{PIN_NAME} must list exactly one {COMPILER} archive')
    return versions[0]


def installed_version(nvcc, pin):
    """The toolkit version when `nvcc --version` reports the pin's compiler (its `V<version>` line), else what it
    reports, or None."""
    if not Path(nvcc).is_file():
        return None
    result = subprocess.run([str(nvcc), '--version'], capture_output=True, text=True)
    match = re.search(r'\bV(\d+(?:\.\d+)+)\s*$', result.stdout, re.M) if result.returncode == 0 else None
    if match is None:
        return None
    return pin['version'] if match.group(1) == compiler_version(pin) else match.group(1)


def install(destination=None, download_dir=None, pin=None, check=True):
    """Return the installed toolkit prefix under `destination`, unpacking the archives unless a matching install is
    present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    compiler_version(pin)
    nvcc = pins.install_tree(pin, destination or default_destination(pin), download_dir or default_download_dir(pin),
                             NVCC, STAMP, label='CUDA Toolkit',
                             version_of=(lambda path: installed_version(path, pin)) if check else None)
    root = nvcc.parent.parent
    print(f'CRD_CUDA_ROOT={root}')
    return root


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the toolkit '
                        '(default: ~/.cerid/tools/cuda/<version>/linux-x64)')
    parser.add_argument('--download-dir', type=Path, help='where the verified archives are kept (archives already '
                        'there are verified instead of downloaded)')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('the registry pins the CUDA Toolkit for Linux x64 only')
    install(args.destination, args.download_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
