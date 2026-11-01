#!/bin/bash
# v11c: build + run the comms throughput bench (Cerid f64) at each CPU level, then the liquid-dsp peer (f32,
# runtime/examples/bench_comms_liquid.c, CRD_BUILD_HESAP_VS_LIQUID) once at the host's ISA. Single-threaded.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_LIQUID=ON)
bash scripts/run-lane-bench.sh bench_comms_vs_refs "${options[@]}"
echo ""
echo "--- liquid-dsp peer (f32) ---"
CRD_BENCH_LEVELS=detected bash scripts/run-lane-bench.sh bench_comms_liquid "${options[@]}"
