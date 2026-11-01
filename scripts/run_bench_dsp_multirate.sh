#!/bin/bash
# v11-k: build + run the resample_poly throughput bench (Cerid) at each CPU level, then the scipy column and liquid-dsp
# rresamp (runtime/examples/bench_dsp_multirate_liquid.c, CRD_BUILD_HESAP_VS_LIQUID) once at the host's ISA. MATLAB
# runs on Windows (tests/numerics/hesap-dsp/bench_multirate_matlab.m). Single-threaded, N=1M, up/down = 3/2 + 2/3.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_LIQUID=ON)
bash scripts/run-lane-bench.sh bench_dsp_multirate_vs_refs "${options[@]}"
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_multirate_refs.py
echo ""
echo "--- liquid-dsp rresamp ---"
CRD_BENCH_LEVELS=detected bash scripts/run-lane-bench.sh bench_dsp_multirate_liquid "${options[@]}"
