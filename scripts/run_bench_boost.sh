#!/bin/bash
# The boards against Boost.Math: the gamma and error-function family (bench_special_vs_refs), Bessel and Airy
# (bench_bessel_vs_refs), the transcendental tail (bench_special_v12d) and the univariate distributions
# (bench_dist_boost), Boost.Math as the pinned crd-bench-boost-peer library at the host's ISA, one thread, at each CPU
# level (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS, CRD_BENCH_CPUS). Every row compares the two sides' values and a
# bench exits 1 on a mismatch. CRD_INPUT_ARCHIVES names a directory holding the pinned archive (cmake/pins.json
# boost) for an offline configure. scipy and MATLAB keep their own programs (runtime/examples/bench_special_refs.py,
# bench_bessel_refs.py, bench_special_matlab.m, tests/numerics/hesap-stats/bench_dist_matlab.m).
#   bash scripts/run_bench_boost.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
options=(-DCRD_BUILD_HESAP_VS_BOOST=ON)
if [[ -n ${CRD_INPUT_ARCHIVES:-} ]]; then
    options+=("-DCRD_INPUT_ARCHIVES=$CRD_INPUT_ARCHIVES")
fi
for bench in bench_special_vs_refs bench_bessel_vs_refs bench_special_v12d bench_dist_boost; do
    bash scripts/run-lane-bench.sh "$bench" "${options[@]}"
done
