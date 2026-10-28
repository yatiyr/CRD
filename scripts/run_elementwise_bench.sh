#!/bin/bash
# The elementwise board (docs/bench/2026-07-02-v14b-elementwise-broadcast.md) on the linux-gcc-release lane tree of
# the WSL source mirror: the elementwise and tensor-kernel gates, the Cerid bench at each CPU level the host runs
# (CRD_CPU_LEVEL selects the tensor kernels' level, ADR-0134), then the Python peers, single-threaded on one core.
#   bash scripts/run_elementwise_bench.sh [level ...]     levels default to the detected one and portable
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LEVELS=("${@:-}")
if [[ ${#LEVELS[@]} -eq 1 && -z ${LEVELS[0]} ]]; then
    LEVELS=(detected portable)
fi
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-tests crd-hesap-tensor crd-math crd-jobs \
    crd-containers crd-memory crd-vm crd-log crd-core
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-b|^tensor kernels'
bash scripts/wsl-lane.sh run linux-gcc-release -- bash -c '
set -euo pipefail
inc=()
for d in numerics/hesap-tensor numerics/hesap-stats foundation/core foundation/math foundation/containers \
         foundation/memory foundation/log foundation/units foundation/jobs; do
    inc+=(-I "$S/engine/$d/include")
done
# General code at the tree'"'"'s SIMD target, like the engine; the binary ops run the dispatched tensor kernels.
target=$(grep -o -m1 -- "-DCRD_SIMD_TARGET=[0-9]" "$B/compile_commands.json")
g++ -O3 -march=x86-64 -std=c++20 -DNDEBUG "$target" "${inc[@]}" -I "$B/engine/core/include" \
    "$S/scripts/bench_elementwise.cpp" \
    "$B/engine/hesap-tensor/libcrd-hesap-tensor.a" "$B/engine/jobs/libcrd-jobs.a" \
    "$B/engine/containers/libcrd-containers.a" "$B/engine/memory/libcrd-memory.a" "$B/engine/vm/libcrd-vm.a" \
    "$B/engine/log/libcrd-log.a" "$B/engine/math/libcrd-math.a" "$B/engine/core/libcrd-core.a" \
    -o bench_elementwise_bin
for level in '"${LEVELS[*]}"'; do
    echo "--- Cerid, CPU level: $level ---"
    if [[ $level == detected ]]; then
        taskset -c 4 ./bench_elementwise_bin
    else
        CRD_CPU_LEVEL=$level taskset -c 4 ./bench_elementwise_bin
    fi
done
echo "--- python peers (single thread) ---"
OMP_NUM_THREADS=1 taskset -c 4 python3 "$S/scripts/bench_elementwise_peers.py"
'
