#!/bin/bash
# v11w-b: build + run the wavedec throughput bench (Cerid) then the pywt column. MATLAB runs on Windows
# (tests/numerics/hesap-wavelet/bench_wavelet_matlab.m, 1-thread). WSL, single-threaded, each CPU level, N=1M.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_wavelet_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- pywt reference (C core, 1 thread) ---"
OMP_NUM_THREADS=1 python3 runtime/examples/bench_wavelet_refs.py
