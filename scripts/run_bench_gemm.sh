#!/bin/bash
# The one-thread GEMM board: Cerid's f32 and f64 GEMM against Eigen (its peer library at the host's ISA) and OpenBLAS
# on one thread at 64 to 1024, then the board's arms in one binary (the level's tile down gemm's Auto, Resident and
# Packed paths; detail::gemm_arm), at each CPU level
# (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS, CRD_BENCH_CPUS). A peer result off Cerid's or an arm's bits off gemm's
# exits 1. CRD_INPUT_ARCHIVES names a directory holding the pinned archives (cmake/pins.json eigen, OpenBLAS) for an
# offline configure.
#   bash scripts/run_bench_gemm.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_HESAP_VS_REFERENCE=ON)
if [[ -n ${CRD_INPUT_ARCHIVES:-} ]]; then
    options+=("-DCRD_INPUT_ARCHIVES=$CRD_INPUT_ARCHIVES")
fi
bash scripts/run-lane-bench.sh bench_hesap_gemm_vs_reference "${options[@]}" -- --one-thread
