#!/bin/bash
# The Lanczos gamma batch board (docs/bench/2026-06-27-v12-statistics-special.md) on the linux-gcc-release lane tree of
# the WSL source mirror: the special-kernel gates, the one-thread lgamma and tgamma rows at each CPU level the host
# runs (run-lane-bench.sh; CRD_CPU_LEVEL selects the hesap-special level, ADR-0134), then scipy on one thread, all on
# one pinned core. The bench exits 1 when a batch is not the portable level's bits. MATLAB's single-thread column runs
# on Windows (CRD_MATLAB_1T=1 matlab -batch "run('runtime/examples/bench_special_matlab.m')"); the Boost column comes
# from bench_special_vs_refs (scripts/run_bench_boost.sh).
#   bash scripts/run_bench_special_batch.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-special-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^special kernels|^special batch'
bash scripts/run-lane-bench.sh bench_special_batch -DCRD_BUILD_RETAINED_BENCHES=ON -- 1
echo "--- scipy (same machine, single thread) ---"
bash scripts/wsl-lane.sh run linux-gcc-release -- env OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 taskset -c 4 \
    python3 "$PWD/runtime/examples/bench_special_refs.py"
