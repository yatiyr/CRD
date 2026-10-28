#!/bin/bash
# The permute board (docs/bench/2026-07-02-v14d-permute-vs-hptt.md) on the linux-gcc-release lane tree of the WSL
# source mirror: the permute gates, the Cerid bench at each CPU level the host runs (CRD_CPU_LEVEL selects the tensor
# kernels' level, ADR-0134) on 1, 8 and 16 workers, then the HPTT peer (external/hptt, external/PEER_ORACLES.md).
#   bash scripts/run_permute_bench.sh [level ...]     levels default to the detected one and portable
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LEVELS=("${@:-}")
if [[ ${#LEVELS[@]} -eq 1 && -z ${LEVELS[0]} ]]; then
    LEVELS=(detected portable)
fi
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-permute-tests crd-hesap-tensor crd-math crd-jobs \
    crd-containers crd-memory crd-vm crd-log crd-core cpu_level_record
bash scripts/wsl-lane.sh test linux-gcc-release -R 'permute'
HPTT="$PWD/external/hptt"
bash scripts/wsl-lane.sh run linux-gcc-release -- bash -c '
set -euo pipefail
source "$S/scripts/bench-level.sh"
inc=()
for d in numerics/hesap-tensor numerics/hesap-stats foundation/core foundation/math foundation/containers \
         foundation/memory foundation/log foundation/units foundation/jobs; do
    inc+=(-I "$S/engine/$d/include")
done
# General code at the tree'"'"'s SIMD target, like the engine; the f32 regions run the dispatched tensor kernels.
target=$(grep -o -m1 -- "-DCRD_SIMD_TARGET=[0-9]" "$B/compile_commands.json")
g++ -O3 -march=x86-64 -std=c++20 -DNDEBUG "$target" "${inc[@]}" -I "$B/engine/core/include" \
    "$S/scripts/bench_permute.cpp" \
    "$B/engine/hesap-tensor/libcrd-hesap-tensor.a" "$B/engine/jobs/libcrd-jobs.a" \
    "$B/engine/containers/libcrd-containers.a" "$B/engine/memory/libcrd-memory.a" "$B/engine/vm/libcrd-vm.a" \
    "$B/engine/log/libcrd-log.a" "$B/engine/math/libcrd-math.a" "$B/engine/core/libcrd-core.a" -o bench_permute_bin
for level in '"${LEVELS[*]}"'; do
    echo "--- Cerid, CPU level: $level ---"
    at_level "$level" 4 ./bench_permute_bin
    at_level "$level" 0,2,4,6,8,10,12,14 ./bench_permute_bin mt
    at_level "$level" 0-15 ./bench_permute_bin mt16
done
if [[ -x '"$HPTT"'/crd_hptt_bench ]]; then
    echo "--- HPTT (same machine) ---"
    cd '"$HPTT"'
    taskset -c 4 ./crd_hptt_bench 1
    taskset -c 0,2,4,6,8,10,12,14 ./crd_hptt_bench 8
fi
'
