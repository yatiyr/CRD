#!/bin/bash
# The dispatch-cost board (SIMD-DISPATCH, ADR-0134 §8): a crd kernel table's call against Google Highway's
# HWY_DYNAMIC_DISPATCH (bench_dispatch_cost; Highway as the pinned crd-bench-highway-peer library at the baseline, its
# targets chosen at run time) on one thread at each CPU level (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS,
# CRD_BENCH_CPUS). An arm whose output leaves the portable level's bits exits 1. CRD_INPUT_ARCHIVES names a directory
# holding the pinned archive (cmake/pins.json highway) for an offline configure.
#   bash scripts/run_bench_dispatch.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_DISPATCH_VS_HIGHWAY=ON)
if [[ -n ${CRD_INPUT_ARCHIVES:-} ]]; then
    options+=("-DCRD_INPUT_ARCHIVES=$CRD_INPUT_ARCHIVES")
fi
bash scripts/run-lane-bench.sh bench_dispatch_cost "${options[@]}"
