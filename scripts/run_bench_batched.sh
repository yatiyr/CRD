#!/bin/bash
# The batched linear-algebra board (docs/bench/2026-07-05-v14h-batched-la.md): bench_batched's Cerid rows (batched
# GEMM, Cholesky, LU, LU factor+solve and small SVD over 10k and 100k matrices of order 4 to 16) and, with
# CRD_BUILD_HESAP_VS_MKL, MKL's cblas_dgemm_batch_strided and LAPACKE loops beside them, at each CPU level
# (scripts/run-lane-bench.sh holds MKL to one thread). The torch, numpy and MATLAB rows come from
# scripts/v14h_peers.py.
#   bash scripts/run_bench_batched.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_batched -DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_MKL=ON
