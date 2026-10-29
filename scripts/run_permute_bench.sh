#!/bin/bash
# The permute board (docs/bench/2026-07-02-v14d-permute-vs-hptt.md) on the linux-gcc-release lane tree of the WSL
# source mirror: the permute gates, the Cerid bench at each CPU level the host runs (run-lane-bench.sh; CRD_CPU_LEVEL
# selects the tensor kernels' level, ADR-0134) on 1, 8 and 16 workers, then the HPTT peer (external/hptt,
# external/PEER_ORACLES.md).
#   bash scripts/run_permute_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-permute-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R 'permute'
bash scripts/run-lane-bench.sh bench_permute -DCRD_BUILD_RETAINED_BENCHES=ON
CRD_BENCH_CPUS=0,2,4,6,8,10,12,14 bash scripts/run-lane-bench.sh bench_permute -DCRD_BUILD_RETAINED_BENCHES=ON -- mt
CRD_BENCH_CPUS=0-15 bash scripts/run-lane-bench.sh bench_permute -DCRD_BUILD_RETAINED_BENCHES=ON -- mt16
HPTT="$PWD/external/hptt"
if [[ -x $HPTT/crd_hptt_bench ]]; then
    echo "--- HPTT (same machine) ---"
    bash scripts/wsl-lane.sh run linux-gcc-release -- env -C "$HPTT" taskset -c 4 ./crd_hptt_bench 1
    bash scripts/wsl-lane.sh run linux-gcc-release -- env -C "$HPTT" taskset -c 0,2,4,6,8,10,12,14 ./crd_hptt_bench 8
fi
