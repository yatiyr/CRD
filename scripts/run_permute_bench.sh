#!/bin/bash
# The permute board (SIMD-DISPATCH; docs/bench/2026-07-02-v14d-permute-vs-hptt.md) on the linux-gcc-release lane tree of
# the WSL source mirror: the permute gates, then bench_permute's arms (detail::PermuteArm, one binary, each arm gated
# against a reference walk; a miss exits 1) at each CPU level the host runs (run-lane-bench.sh; CRD_CPU_LEVEL selects
# the tensor kernels' level, ADR-0134): the serial arms on one P-core thread, the multithreaded arms on 8 workers (one
# thread a P-core) and 16 (both threads of each), with HPTT's plans of every element type on as many threads, gated
# like the arms (crd-bench-hptt-peer from the pinned archive, cmake/pins.json hptt). CRD_INPUT_ARCHIVES names a
# directory holding that archive for an offline configure.
#   bash scripts/run_permute_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-permute-tests crd-hesap-tensor-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^(v14-d |permute board arms|tensor kernels: .* permute )'
options=(-DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_HPTT=ON)
if [[ -n ${CRD_INPUT_ARCHIVES:-} ]]; then
    options+=("-DCRD_INPUT_ARCHIVES=$CRD_INPUT_ARCHIVES")
fi
bash scripts/run-lane-bench.sh bench_permute "${options[@]}" -- --one-thread
CRD_BENCH_CPUS=0,2,4,6,8,10,12,14 bash scripts/run-lane-bench.sh bench_permute "${options[@]}" -- --threads 8
CRD_BENCH_CPUS=0-15 bash scripts/run-lane-bench.sh bench_permute "${options[@]}" -- --threads 16
