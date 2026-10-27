#!/bin/bash
# The 1D NUFFT board: Cerid's NufftPlan against FINUFFT (built at ~/finufft, CRD_FINUFFT_DIR), type 1 and 2, one thread
# (FINUFFT opts.nthreads=1, OMP_NUM_THREADS=1), at each CPU level (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS,
# CRD_BENCH_CPUS). --profile prints the spread, FFT and deconvolution phase split instead, from a tree of its own
# built with CRD_HESAP_FFT_PROFILE.
#   bash scripts/run_bench_nufft.sh [--profile]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ ${1:-} == --profile ]]; then
    export CRD_LANE_ROOT=${CRD_LANE_ROOT:-$HOME/cerid-mirror-build-fft-profile}
    profile=(bench_nufft_vs_finufft -DCRD_BUILD_HESAP_VS_FINUFFT=ON -DCRD_HESAP_FFT_PROFILE=ON)
    CRD_BENCH_LEVELS= bash scripts/run-lane-bench.sh "${profile[@]}"
    # The phase lines go to stderr; each phase once per size.
    bash scripts/run-lane-bench.sh "${profile[@]}" 2>&1 >/dev/null | grep nufft-prof | awk '!seen[$3$4]++'
else
    bash scripts/run-lane-bench.sh bench_nufft_vs_finufft -DCRD_BUILD_HESAP_VS_FINUFFT=ON
fi
