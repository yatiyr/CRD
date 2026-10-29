#!/bin/bash
# The permute board (SIMD-DISPATCH; docs/bench/2026-07-02-v14d-permute-vs-hptt.md) on the linux-gcc-release lane tree of
# the WSL source mirror: the permute gates, then bench_permute's arms (detail::PermuteArm, one binary, each arm gated
# against a reference walk; a miss exits 1) at each CPU level the host runs (run-lane-bench.sh; CRD_CPU_LEVEL selects
# the tensor kernels' level, ADR-0134): the serial arms on one P-core thread, the multithreaded arms on 8 workers (one
# thread a P-core) and 16 (both threads of each), then the HPTT peer on the same CPUs (external/hptt, f32,
# external/PEER_ORACLES.md).
#   bash scripts/run_permute_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-permute-tests crd-hesap-tensor-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^(v14-d |permute board arms|tensor kernels: .* permute )'
bash scripts/run-lane-bench.sh bench_permute -DCRD_BUILD_RETAINED_BENCHES=ON -- --one-thread
CRD_BENCH_CPUS=0,2,4,6,8,10,12,14 bash scripts/run-lane-bench.sh bench_permute -DCRD_BUILD_RETAINED_BENCHES=ON -- \
    --threads 8
CRD_BENCH_CPUS=0-15 bash scripts/run-lane-bench.sh bench_permute -DCRD_BUILD_RETAINED_BENCHES=ON -- --threads 16
HPTT="$PWD/external/hptt"
if [[ -x $HPTT/crd_hptt_bench ]]; then
    echo "--- HPTT (same machine) ---"
    bash scripts/wsl-lane.sh run linux-gcc-release -- env -C "$HPTT" taskset -c 4 ./crd_hptt_bench 1
    bash scripts/wsl-lane.sh run linux-gcc-release -- env -C "$HPTT" taskset -c 0,2,4,6,8,10,12,14 ./crd_hptt_bench 8
    bash scripts/wsl-lane.sh run linux-gcc-release -- env -C "$HPTT" taskset -c 0-15 ./crd_hptt_bench 16
fi
