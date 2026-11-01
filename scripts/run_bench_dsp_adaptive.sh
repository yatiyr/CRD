#!/bin/bash
# v11-t: build + run the adaptive-filter (NLMS/LMS) throughput bench (Cerid) at each CPU level, then liquid-dsp eqlms
# (runtime/examples/bench_dsp_adaptive_liquid.c, CRD_BUILD_HESAP_VS_LIQUID) once at the host's ISA. MATLAB runs on
# Windows (tests/numerics/hesap-dsp/bench_adaptive_matlab.m). Single-threaded, m=32 taps, N=1M.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_LIQUID=ON)
bash scripts/run-lane-bench.sh bench_dsp_adaptive_vs_refs "${options[@]}"
echo ""
echo "--- liquid-dsp eqlms ---"
CRD_BENCH_LEVELS=detected bash scripts/run-lane-bench.sh bench_dsp_adaptive_liquid "${options[@]}"
