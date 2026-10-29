#!/bin/bash
# The symmetric eigensolvers' crd-jobs board (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE) on the linux-gcc-release
# lane tree of the WSL source mirror: the lever gates, then bench_hesap_eig_parallel (the multisection's chunk counts
# and dqds, and MrrrPick's granularity, against the one-job eigenvalues and eigenvectors in one binary, gated bit for
# bit or by the declared bound; a miss exits 1) at each CPU level the host runs (run-lane-bench.sh), one process per
# thread count: 4 and 8 workers (one thread a P-core), 16 (both threads of each) and 32 (every logical CPU, E-cores
# included).
#   bash scripts/run_bench_eig_parallel.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-dense-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^(tridiag_multisection_arm|eigvals_sym_arm|mrrr_vectors_arm)'
CRD_BENCH_CPUS=0,2,4,6 bash scripts/run-lane-bench.sh bench_hesap_eig_parallel -- --threads 4
CRD_BENCH_CPUS=0,2,4,6,8,10,12,14 bash scripts/run-lane-bench.sh bench_hesap_eig_parallel -- --threads 8
CRD_BENCH_CPUS=0-15 bash scripts/run-lane-bench.sh bench_hesap_eig_parallel -- --threads 16
CRD_BENCH_CPUS=0-31 bash scripts/run-lane-bench.sh bench_hesap_eig_parallel -- --threads 32
