#!/bin/bash
# v11-i: the filter-application (sosfilt, 12th-order cascade, 10M samples) throughput board. The Cerid bench
# (runtime/examples/bench_dsp_sosfilt_vs_refs.cpp) at each CPU level, then liquid-dsp's iirfilt on a 12th-order
# cascade (runtime/examples/bench_dsp_sosfilt_vs_liquid.c, CRD_BUILD_HESAP_VS_LIQUID) once at the host's ISA.
# Single-threaded.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_LIQUID=ON)
bash scripts/run-lane-bench.sh bench_dsp_sosfilt_vs_refs "${options[@]}"
echo ""
echo "--- liquid-dsp iirfilt (second-order sections) ---"
CRD_BENCH_LEVELS=detected bash scripts/run-lane-bench.sh bench_dsp_sosfilt_vs_liquid "${options[@]}"
