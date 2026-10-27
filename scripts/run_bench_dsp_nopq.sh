#!/bin/bash
# v11-n/o/p/q: build + run the compute-bound benchmarks (Cerid czt + arburg) then the scipy czt column. MATLAB runs
# on Windows (tests/numerics/hesap-dsp/bench_nopq_matlab.m). WSL, single-threaded, each CPU level.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dsp_nopq_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_nopq_refs.py
