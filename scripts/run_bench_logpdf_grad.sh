#!/bin/bash
# The log-density gradient board (docs/bench/2026-06-27-v12-statistics-special.md, the per-leapfrog rows) on the
# linux-gcc-release lane tree of the WSL source mirror: the stats-kernel gates, the Cerid bench at each CPU level the
# host runs (run-lane-bench.sh; CRD_CPU_LEVEL selects the Student-t sum's hesap-stats level, ADR-0134), then the
# single-threaded JAX and PyTorch peers (runtime/examples/bench_logpdf_grad.py), all on one pinned core. The bench exits
# 1 when the level's Student-t sum is not the portable level's bits.
#   bash scripts/run_bench_logpdf_grad.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-stats-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^stats kernels'
bash scripts/run-lane-bench.sh bench_logpdf_grad -DCRD_BUILD_RETAINED_BENCHES=ON
echo "--- JAX and PyTorch peers (same machine, single thread) ---"
bash scripts/wsl-lane.sh run linux-gcc-release -- env OMP_NUM_THREADS=1 taskset -c 4 \
    python3 "$PWD/runtime/examples/bench_logpdf_grad.py"
