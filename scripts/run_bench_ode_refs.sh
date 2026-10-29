#!/bin/bash
# The hesap-ode board against SUNDIALS CVODE (dense LU and KLU, the host's libsundials-dev and libsuitesparse-dev) and
# Boost.Odeint's controlled dopri5 (the pinned Boost release as crd-bench-boost-peer at the host's ISA), one thread,
# at each CPU level (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS, CRD_BENCH_CPUS). Every row reports its achieved
# error against a tight-tolerance reference (see the bench file header). CRD_INPUT_ARCHIVES names a directory holding
# the pinned archive (cmake/pins.json boost) for an offline configure.
#   bash scripts/run_bench_ode_refs.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_HESAP_VS_SUNDIALS=ON -DCRD_BUILD_HESAP_VS_BOOST=ON)
if [[ -n ${CRD_INPUT_ARCHIVES:-} ]]; then
    options+=("-DCRD_INPUT_ARCHIVES=$CRD_INPUT_ARCHIVES")
fi
bash scripts/run-lane-bench.sh bench_ode_vs_refs "${options[@]}"
