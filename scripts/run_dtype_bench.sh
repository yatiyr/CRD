#!/bin/bash
# The dtype conversion board (docs/bench/2026-07-02-v14a-dtype-converts.md) on the linux-gcc-release lane tree of the
# WSL source mirror: the hesap-tensor gate, the Cerid probe at each CPU level the host runs (CRD_CPU_LEVEL selects the
# conversion and tensor kernels' level, ADR-0134), then the single-threaded peers.
#   bash scripts/run_dtype_bench.sh [level ...]     levels default to the detected one and portable
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LEVELS=("${@:-}")
if [[ ${#LEVELS[@]} -eq 1 && -z ${LEVELS[0]} ]]; then
    LEVELS=(detected portable)
fi
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-tests crd-math crd-memory crd-vm crd-log crd-core
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-a'
bash scripts/wsl-lane.sh run linux-gcc-release -- bash -c '
set -euo pipefail
inc=()
for d in numerics/hesap-tensor numerics/hesap-stats foundation/core foundation/math foundation/containers \
         foundation/memory foundation/log foundation/units; do
    inc+=(-I "$S/engine/$d/include")
done
# The probe is release code like the engine; the SR draws come from libcrd-hesap-tensor.a (the Philox block loop of
# the dispatched tensor kernels) and the conversions from libcrd-math.a (the dispatched conversion kernels).
g++ -O3 -march=x86-64 -std=c++20 -DNDEBUG "${inc[@]}" -I "$B/engine/core/include" "$S/scripts/bench_dtypes.cpp" \
    "$B/engine/hesap-tensor/libcrd-hesap-tensor.a" "$B/engine/memory/libcrd-memory.a" "$B/engine/vm/libcrd-vm.a" \
    "$B/engine/log/libcrd-log.a" "$B/engine/math/libcrd-math.a" "$B/engine/core/libcrd-core.a" -o bench_dtypes_bin
for level in '"${LEVELS[*]}"'; do
    echo "--- Cerid, CPU level: $level ---"
    if [[ $level == detected ]]; then
        taskset -c 4 ./bench_dtypes_bin
    else
        CRD_CPU_LEVEL=$level taskset -c 4 ./bench_dtypes_bin
    fi
done
echo "--- python peers (same machine, single thread) ---"
OMP_NUM_THREADS=1 taskset -c 4 python3 "$S/scripts/bench_dtypes_peers.py"
'
