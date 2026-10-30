#!/usr/bin/env bash
# Build one runtime/examples program in the linux-gcc-release lane tree of the WSL source mirror and run it once per CPU
# level (ADR-0134: CRD_CPU_LEVEL selects every kernel family's level), pinned with taskset and with the peer libraries
# held to one thread. The run_bench_*.sh boards and prof_fft_kernel.sh call it; CMake links the program, so no script
# keeps a link line of its own.
#   bash scripts/run-lane-bench.sh <target> [-D<option>=<value> ...] [-- <program arguments>]
# The options are configured into the tree when its cache lacks them. Environment: CRD_BENCH_LEVELS (default: every
# level the pinned CPUs run, `cpu_level_record --levels`; detected runs without an override, and an empty list only
# builds), CRD_BENCH_CPUS (the taskset list, default 4), CRD_LANE_ROOT (a tree root of its own for an option that
# changes the measured code, such as CRD_HESAP_FFT_PROFILE). Before each level's run, cpu_level_record prints the level
# line under the same CRD_CPU_LEVEL and CPU list (ADR-0134 §8) and stops the board when the level did not apply (above
# the host or the build's cap). A forced x64 level also hides the features above it from glibc's ifuncs (GLIBC_TUNABLES
# glibc.cpu.hwcaps), so libm and the string routines run the bodies a CPU of that level gets (fma without the FMA
# instruction on sse4.2 and sse2); portable keeps the host's glibc, as its targets (architectures without x64 levels)
# have the FPU's fma. The peers dispatch on their own CPUID and stay at the host's.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 1 || $1 == -* ]]; then
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
bash scripts/wsl-lane.sh build "$PRESET" "$target" cpu_level_record
cpus=${CRD_BENCH_CPUS:-4}
if [[ -v CRD_BENCH_LEVELS ]]; then
    level_list=$CRD_BENCH_LEVELS
else
    # The last line: the lane's mirror check prints its own first.
    level_list=$(bash scripts/wsl-lane.sh run "$PRESET" -- taskset -c "$cpus" "$tree/runtime/cpu_level_record" --levels |
        tail -n 1)
fi
read -r -a levels <<< "$level_list"
# The glibc.cpu.hwcaps names glibc's x86 ifuncs select on, above each forced level.
above_avx2=-AVX512F,-AVX512CD,-AVX512BW,-AVX512DQ,-AVX512VL
above_sse42=$above_avx2,-AVX,-AVX2,-FMA,-FMA4,-BMI1,-BMI2,-LZCNT,-MOVBE
above_sse2=$above_sse42,-SSSE3,-SSE4_1,-SSE4_2,-POPCNT
for level in "${levels[@]}"; do
    select=()
    hidden=
    case $level in
        avx2) hidden=$above_avx2 ;;
        sse4.2) hidden=$above_sse42 ;;
        sse2) hidden=$above_sse2 ;;
    esac
    if [[ $level != detected ]]; then
        select=("CRD_CPU_LEVEL=$level")
    fi
    if [[ -n $hidden ]]; then
        select+=("GLIBC_TUNABLES=glibc.cpu.hwcaps=$hidden")
    fi
    echo "--- $target, CPU level: $level${hidden:+ (glibc hwcaps $hidden)} ---"
    bash scripts/wsl-lane.sh run "$PRESET" -- env "${select[@]}" taskset -c "$cpus" \
        "$tree/runtime/cpu_level_record" --expect "$level"
    bash scripts/wsl-lane.sh run "$PRESET" -- env "${select[@]}" MKL_NUM_THREADS=1 OMP_NUM_THREADS=1 \
        taskset -c "$cpus" "$tree/runtime/$target" "$@"
done
