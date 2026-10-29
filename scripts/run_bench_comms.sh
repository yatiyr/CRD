#!/bin/bash
# v11c: build + run the comms throughput bench (Cerid f64) then the liquid-dsp peer (f32). WSL, single-threaded.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_comms_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- liquid-dsp peer (f32) ---"
gcc -O3 -march=native -D_GNU_SOURCE runtime/examples/bench_comms_liquid.c -lliquid -lm -o /tmp/bench_comms_liquid 2>/tmp/liquid_comms_err.log \
  && /tmp/bench_comms_liquid || { echo "(liquid build failed — see /tmp/liquid_comms_err.log)"; head -20 /tmp/liquid_comms_err.log; }
