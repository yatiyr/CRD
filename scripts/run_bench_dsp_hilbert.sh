#!/bin/bash
# v11-l: build + run the Hilbert throughput bench (Cerid) then the scipy column. MATLAB column is run separately
# on Windows (tests/numerics/hesap-dsp/bench_hilbert_matlab.m). WSL, single-threaded, each CPU level, N=2^16 + 2^20.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dsp_hilbert_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_hilbert_refs.py
