#!/bin/bash
# v11-b: build + run the window-generation throughput shootout (Cerid vs scipy vs MATLAB). WSL, single-threaded,
# each CPU level. N=2^20 (compute-bound — the fair compiled-vs-compiled regime). Run the .py + MATLAB .m separately fo
# the scipy/MATLAB columns. Honest: window gen is one-time setup, not a streaming hot path (see the .cpp header).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dsp_windows_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- scipy reference: python3 runtime/examples/bench_dsp_windows_refs.py ---"
python3 runtime/examples/bench_dsp_windows_refs.py
