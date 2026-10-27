#!/bin/bash
# Profile the Cerid FFT at one size in a tight loop (runtime/examples/prof_fft_kernel.cpp): the timing with the
# per-phase cycle counters at each CPU level, from a tree of its own built with CRD_HESAP_FFT_PROFILE (the counters
# change the measured code), then perf stat (IPC, FP, ports, cache) on the linux-gcc-release lane tree's counter-free
# build. Use an L1-resident size (log2 N = 10) for the kernel alone.
#   bash scripts/prof_fft_kernel.sh [log2N] [iterations]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LG=${1:-10}
IT=${2:-2000000}
echo "=== timing and phase counters ==="
CRD_LANE_ROOT=${CRD_FFT_PROFILE_ROOT:-$HOME/cerid-mirror-build-fft-profile} \
    bash scripts/run-lane-bench.sh prof_fft_kernel -DCRD_BUILD_RETAINED_BENCHES=ON -DCRD_HESAP_FFT_PROFILE=ON \
    -- "$LG" "$IT"
echo "=== perf stat (IPC + FP + ports + cache) ==="
CRD_BENCH_LEVELS= bash scripts/run-lane-bench.sh prof_fft_kernel -DCRD_BUILD_RETAINED_BENCHES=ON
bash scripts/wsl-lane.sh run linux-gcc-release -- bash -c '
set -o pipefail
perf stat -d -d \
    -e cycles,instructions,fp_arith_inst_retired.256b_packed_double \
    -e uops_dispatched.port_0,uops_dispatched.port_1,uops_dispatched.port_5,uops_dispatched.port_6 \
    -e L1-dcache-loads,L1-dcache-load-misses \
    taskset -c 4 "$B/runtime/prof_fft_kernel" "$1" "$2" 2>&1 | tail -30 ||
    perf stat taskset -c 4 "$B/runtime/prof_fft_kernel" "$1" "$2" 2>&1 | tail -20
' prof "$LG" "$IT"
