#!/usr/bin/env bash
# Build one runtime/examples program in the linux-gcc-release lane tree of the WSL source mirror and run it once per CPU
# level (ADR-0134: CRD_CPU_LEVEL selects every kernel family's level), pinned with taskset and with the peer libraries
# held to one thread. The run_bench_*.sh boards and prof_fft_kernel.sh call it; CMake links the program, so no script
# keeps a link line of its own.
#   bash scripts/run-lane-bench.sh <target> [-D<option>=<value> ...] [-- <program arguments>]
# The options are configured into the tree when its cache lacks them. Environment: CRD_BENCH_LEVELS (default
# "detected sse2 portable"; detected runs without an override, a level above the host runs at the detected one, and an
# empty list only builds), CRD_BENCH_CPUS (the taskset list, default 4), CRD_LANE_ROOT (a tree root of its own for an
# option that changes the measured code, such as CRD_HESAP_FFT_PROFILE).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 1 || $1 == -* ]]; then
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
target=$1
shift
options=()
while [[ $# -gt 0 && $1 == -D* ]]; do
    options+=("$1")
    shift
done
if [[ ${1:-} == -- ]]; then
    shift
fi
PRESET=linux-gcc-release
tree=$(bash scripts/wsl-lane.sh tree "$PRESET")
# wsl-lane.sh build configures a missing tree without options, so configure here when any option is not in the cache.
configure=0
if [[ ! -f $tree/CMakeCache.txt ]]; then
    configure=1
fi
for option in "${options[@]}"; do
    name=${option#-D}
    name=${name%%=*}
    if ! grep -Eqx -- "${name}:[A-Z]+=${option#*=}" "$tree/CMakeCache.txt" 2>/dev/null; then
        configure=1
    fi
done
if [[ $configure == 1 ]]; then
    bash scripts/wsl-lane.sh configure "$PRESET" "${options[@]}"
fi
bash scripts/wsl-lane.sh build "$PRESET" "$target"
read -r -a levels <<< "${CRD_BENCH_LEVELS-detected sse2 portable}"
for level in "${levels[@]}"; do
    select=()
    if [[ $level != detected ]]; then
        select=("CRD_CPU_LEVEL=$level")
    fi
    echo "--- $target, CPU level: $level ---"
    bash scripts/wsl-lane.sh run "$PRESET" -- env "${select[@]}" MKL_NUM_THREADS=1 OMP_NUM_THREADS=1 \
        taskset -c "${CRD_BENCH_CPUS:-4}" "$tree/runtime/$target" "$@"
done
