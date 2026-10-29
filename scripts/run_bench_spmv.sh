#!/bin/bash
# The SELL spmv board: Cerid's SELL-C-sigma spmv of f64, f32 and both complex types against Eigen (its peer library at
# the host's ISA) at sizes from the host's caches and two DRAM sizes, at each CPU level (scripts/run-lane-bench.sh:
# CRD_BENCH_LEVELS). First the level's arms on one pinned CPU (CRD_BENCH_CPUS, default 4: the production spmv, the
# U32 and Delta16 forms, the sweeps, every gather form and pair width), then each job count's cut arms on every CPU
# with Eigen on as many threads. A Cerid arm off the CSR spmv's bits, Eigen off its bound, or a pick refused where the
# level gathers exits 1. CRD_INPUT_ARCHIVES names a directory holding the pinned eigen archive (cmake/pins.json) for an
# offline configure.
#   bash scripts/run_bench_spmv.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_HESAP_VS_REFERENCE=ON)
if [[ -n ${CRD_INPUT_ARCHIVES:-} ]]; then
    options+=("-DCRD_INPUT_ARCHIVES=$CRD_INPUT_ARCHIVES")
fi
bash scripts/run-lane-bench.sh bench_hesap_spmv_vs_reference "${options[@]}" -- --one-thread
CRD_BENCH_CPUS="0-$(($(getconf _NPROCESSORS_ONLN) - 1))" \
    bash scripts/run-lane-bench.sh bench_hesap_spmv_vs_reference "${options[@]}" -- --threads
