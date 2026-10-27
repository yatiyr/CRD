#!/bin/bash
# v11-t: build + run the adaptive-filter (NLMS/LMS) throughput bench (Cerid) then liquid-dsp eqlms. MATLAB runs on
# Windows (tests/numerics/hesap-dsp/bench_adaptive_matlab.m). WSL, single-threaded, each CPU level, m=32 taps, N=1M.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dsp_adaptive_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- liquid-dsp eqlms ---"
gcc -O3 -mavx2 -mfma runtime/examples/bench_dsp_adaptive_liquid.c -lliquid -lm -o /tmp/bench_liquid_ad 2>/tmp/liquid_ad_err.log \
  && /tmp/bench_liquid_ad || { echo "(liquid build failed — see /tmp/liquid_ad_err.log)"; head -5 /tmp/liquid_ad_err.log; }
