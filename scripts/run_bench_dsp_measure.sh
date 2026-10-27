#!/bin/bash
# v11-s: build + run the detection/measurement bench (Cerid) then the scipy column. MATLAB (thd/snr) runs on Windows.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_dsp_measure_vs_refs -DCRD_BUILD_RETAINED_BENCHES=ON
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_measure_refs.py
