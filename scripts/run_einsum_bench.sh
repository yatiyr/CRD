#!/bin/bash
# The einsum execution board (docs/bench/2026-07-03-v14f-einsum-exec-vs-numpy-torch.md) on the linux-gcc-release lane
# tree of the WSL source mirror: the einsum gates, the Cerid bench at each CPU level the host runs (CRD_CPU_LEVEL
# selects the level of the tensor and dense kernels, ADR-0134), then the Python peers, single-threaded on one core.
#   bash scripts/run_einsum_bench.sh [level ...]     levels default to the detected one and portable
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LEVELS=("${@:-}")
if [[ ${#LEVELS[@]} -eq 1 && -z ${LEVELS[0]} ]]; then
    LEVELS=(detected portable)
fi
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-einsum-tests crd-hesap-tensor crd-hesap-dense \
    crd-hesap crd-math crd-jobs crd-containers crd-memory crd-vm crd-log crd-core
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-f'
bash scripts/wsl-lane.sh run linux-gcc-release -- bash -c '
set -euo pipefail
inc=()
for d in numerics/hesap-tensor numerics/hesap-dense numerics/hesap numerics/hesap-stats foundation/core \
         foundation/math foundation/containers foundation/memory foundation/log foundation/units foundation/jobs; do
    inc+=(-I "$S/engine/$d/include")
done
# General code at the tree'"'"'s SIMD target, like the engine; the products run the dispatched kernels.
target=$(grep -o -m1 -- "-DCRD_SIMD_TARGET=[0-9]" "$B/compile_commands.json")
g++ -O3 -march=native -std=c++20 -DNDEBUG "$target" "${inc[@]}" -I "$B/engine/core/include" \
    "$S/scripts/bench_einsum.cpp" \
    "$B/engine/hesap-dense/libcrd-hesap-dense.a" "$B/engine/hesap/libcrd-hesap.a" \
    "$B/engine/hesap-tensor/libcrd-hesap-tensor.a" "$B/engine/jobs/libcrd-jobs.a" \
    "$B/engine/math/libcrd-math.a" "$B/engine/containers/libcrd-containers.a" "$B/engine/memory/libcrd-memory.a" \
    "$B/engine/vm/libcrd-vm.a" "$B/engine/log/libcrd-log.a" "$B/engine/core/libcrd-core.a" -o bench_einsum_bin
for level in '"${LEVELS[*]}"'; do
    echo "--- Cerid, CPU level: $level (1T pinned) ---"
    if [[ $level == detected ]]; then
        taskset -c 4 ./bench_einsum_bin
    else
        CRD_CPU_LEVEL=$level taskset -c 4 ./bench_einsum_bin
    fi
done
echo "--- peers (1T pinned) ---"
OMP_NUM_THREADS=1 taskset -c 4 python3 "$S/scripts/bench_einsum_peers.py"
'
