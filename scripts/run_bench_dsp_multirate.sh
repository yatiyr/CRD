#!/bin/bash
# v11-k: build + run the resample_poly throughput bench (Cerid) then scipy + liquid-dsp columns. MATLAB runs on
# Windows (tests/numerics/hesap-dsp/bench_multirate_matlab.m). WSL, single-threaded, each CPU level, N=1M, up/down = 3/2 + 2/3.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dsp_multirate_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_multirate_refs.py
echo ""
echo "--- liquid-dsp rresamp ---"
if [ -f runtime/examples/bench_dsp_multirate_liquid.c ]; then
  gcc -O3 -march=native runtime/examples/bench_dsp_multirate_liquid.c -lliquid -lm -o /tmp/bench_liquid_mr 2>/tmp/liquid_mr_err.log \
    && /tmp/bench_liquid_mr || { echo "(liquid build failed — see /tmp/liquid_mr_err.log)"; head -5 /tmp/liquid_mr_err.log; }
fi
