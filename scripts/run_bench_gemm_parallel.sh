#!/bin/bash
# gemm_parallel's crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE) on the linux-gcc-release lane tree of
# the WSL source mirror: the lever gates, then bench_hesap_gemm_parallel (detail::GemmParallelPick's arms against
# serial gemm in one binary, each arm gated bit for bit; a miss exits 1) at each CPU level the host runs
# (run-lane-bench.sh), one process per thread count: 4 and 8 workers (one thread a P-core), 16 (both threads of each)
# and 32 (every logical CPU, E-cores included).
#   bash scripts/run_bench_gemm_parallel.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-dense-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^gemm_parallel'
CRD_BENCH_CPUS=0,2,4,6 bash scripts/run-lane-bench.sh bench_hesap_gemm_parallel -- --threads 4
CRD_BENCH_CPUS=0,2,4,6,8,10,12,14 bash scripts/run-lane-bench.sh bench_hesap_gemm_parallel -- --threads 8
CRD_BENCH_CPUS=0-15 bash scripts/run-lane-bench.sh bench_hesap_gemm_parallel -- --threads 16
CRD_BENCH_CPUS=0-31 bash scripts/run-lane-bench.sh bench_hesap_gemm_parallel -- --threads 32
