#!/bin/bash
# v11-b: build + run the window-generation throughput shootout (Cerid vs scipy vs liquid-dsp vs MATLAB).
# Single-threaded, Cerid at each CPU level. N=2^20 (compute-bound — the fair compiled-vs-compiled regime). The scipy
# column runs here, the liquid-dsp peer (runtime/examples/bench_dsp_windows_vs_liquid.c, CRD_BUILD_HESAP_VS_LIQUID)
# once at the host's ISA, MATLAB's .m separately. Honest: window gen is one-time setup, not a streaming hot path (see
# the .cpp header).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_BUILD_HESAP_VS_LIQUID=ON)
bash scripts/run-lane-bench.sh bench_dsp_windows_vs_refs "${options[@]}"
echo ""
echo "--- scipy reference: python3 runtime/examples/bench_dsp_windows_refs.py ---"
python3 runtime/examples/bench_dsp_windows_refs.py
echo ""
echo "--- liquid-dsp windows and firdes_kaiser ---"
CRD_BENCH_LEVELS=detected bash scripts/run-lane-bench.sh bench_dsp_windows_vs_liquid "${options[@]}"
