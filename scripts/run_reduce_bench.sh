#!/bin/bash
# The reduction board (docs/bench/2026-07-02-v14c-reduce-vs-reproblas.md) on the linux-gcc-release lane tree of the
# WSL source mirror: the reduce and tensor-kernel gates, the Cerid bench at each CPU level the host runs
# (run-lane-bench.sh; CRD_CPU_LEVEL selects the Tier R kernel's level, ADR-0134), then the ReproBLAS peer
# (external/reproblas, external/PEER_ORACLES.md), all single-threaded on one pinned core.
#   bash scripts/run_reduce_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-c|^tensor kernels'
bash scripts/run-lane-bench.sh bench_reduce -DCRD_BUILD_RETAINED_BENCHES=ON
REPROBLAS="$PWD/external/reproblas"
if [[ -x $REPROBLAS/crd_reproblas_bench ]]; then
    echo "--- ReproBLAS (same machine) ---"
    bash scripts/wsl-lane.sh run linux-gcc-release -- taskset -c 4 "$REPROBLAS/crd_reproblas_bench"
fi
