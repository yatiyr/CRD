#!/bin/bash
# The DCT-II board: Cerid's DctPlan against pocketfft (the numpy/scipy.fft backend, its peer library at the host's
# ISA) and FFTW (REDFT10), f64, one thread, at each CPU level (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS,
# CRD_BENCH_CPUS). The peers are installed on the WSL host (CRD_BUILD_HESAP_VS_FFT_REFS in runtime/CMakeLists.txt).
#   bash scripts/run_bench_dct.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dct_vs_refs -DCRD_BUILD_HESAP_VS_FFT_REFS=ON
