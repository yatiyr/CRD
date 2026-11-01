#!/usr/bin/env bash
# The GPU compute and FFT boards' CUDA programs (bench/CMakeLists.txt, CRD_BUILD_GPU_VS_CUDA): the cuFFT, CUB and
# cuBLASLt peers, the CKIR kernels' CUDA duels and the Nsight Compute targets, built in a linux-gcc-release lane tree of
# their own against the pinned CUDA Toolkit (scripts/install-cuda.py) and run once each on the first CUDA device (a GPU
# board: the CPU level does not change their kernels). The tree is its own (CRD_LANE_ROOT, default
# ~/cerid-cuda-build): a toolkit in its cache also builds the engine's CUDA modules, which the other boards' trees leave
# out. A Windows NVIDIA host runs this under WSL2, whose driver exposes the GPU to Linux programs.
#   bash scripts/run_bench_gpu_cuda.sh [--build-only] [program ...]
# --build-only stops after the build (a host without an NVIDIA GPU); named programs run alone, in the given order.
# A program exits nonzero on a failed CUDA call or a wrong result, and the first failing program stops the board.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
build_only=0
if [[ ${1:-} == --build-only ]]; then
    build_only=1
    shift
fi
export CRD_LANE_ROOT=${CRD_LANE_ROOT:-$HOME/cerid-cuda-build}
PRESET=linux-gcc-release
cuda_root=$(python3 scripts/install-cuda.py | sed -n 's/^CRD_CUDA_ROOT=//p')
if [[ -z $cuda_root ]]; then
    echo "scripts/install-cuda.py printed no CRD_CUDA_ROOT" >&2
    exit 1
fi
tree=$(bash scripts/wsl-lane.sh tree "$PRESET")
# A tree whose last configure failed keeps its cache but has no build file, so that is configured again too.
if [[ ! -f $tree/build.ninja ]] || ! grep -Eqx "CRD_BUILD_GPU_VS_CUDA:BOOL=ON" "$tree/CMakeCache.txt" ||
    ! grep -Fqx "CUDAToolkit_ROOT:PATH=$cuda_root" "$tree/CMakeCache.txt"; then
    bash scripts/wsl-lane.sh configure "$PRESET" -DCRD_BUILD_GPU_VS_CUDA=ON "-DCUDAToolkit_ROOT:PATH=$cuda_root"
fi
bash scripts/wsl-lane.sh build "$PRESET" crd-gpu-vs-cuda
if [[ $build_only == 1 ]]; then
    exit 0
fi
mapfile -t built < "$tree/bench/cuda-programs.txt"
programs=()
for name in "$@"; do
    found=
    for program in "${built[@]}"; do
        if [[ $(basename "$program") == "$name" ]]; then
            found=$program
        fi
    done
    if [[ -z $found ]]; then
        echo "no CUDA program named $name (bench/CMakeLists.txt lists the programs)" >&2
        exit 2
    fi
    programs+=("$found")
done
if [[ $# -eq 0 ]]; then
    programs=("${built[@]}")
fi
for program in "${programs[@]}"; do
    echo "== $(basename "$program")"
    bash scripts/wsl-lane.sh run "$PRESET" -- "$program"
done
