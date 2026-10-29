#!/bin/bash
# v9-z: the IMEX-vs-ARKODE work-precision board (see the bench file header),
# built against the host's SUNDIALS in the linux-gcc-release lane tree of the WSL source mirror and run at each CPU
# level the host runs (run-lane-bench.sh; CRD_CPU_LEVEL selects the dense and sparse kernels' level, ADR-0134).
# SUNDIALS is the second column of each row, measured in the same process.
#   bash scripts/run_bench_imex.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/run-lane-bench.sh bench_ode_imex_vs_arkode -DCRD_BUILD_HESAP_VS_SUNDIALS=ON
