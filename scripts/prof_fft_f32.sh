#!/usr/bin/env bash
# Profile the complex-f32 forward of each named side at one size on the linux-gcc-release lane tree's bench_fft_f32
# (built with -DCRD_BUILD_HESAP_VS_FFT_REFS=ON), each side's own process (bench_fft_f32 --loop): perf stat's top-down
# levels 1 and 2 with cycles, instructions and branch misses, then perf record's call graph and the hottest symbols.
# The copy side is the loop's own overhead (the input copy every transform starts from), whose counts a side's
# include. Run on the benchmark machine (docs/bench/README.md#fast-ab-boards); one pinned CPU, the selected level by
# CRD_CPU_LEVEL as the boards select it.
#   bash scripts/prof_fft_f32.sh <log2 n> <iterations> <side> [side ...]     (sides: planned, an algorithm, mkl, copy)
# Environment: CRD_BENCH_CPUS (the taskset CPU, default 3), CRD_PROF_OUT (the output directory, default
# ~/boards/prof-fft-f32), CRD_PROF_RECORD (0: perf stat only).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 3 ]]; then
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
lg=$1
iterations=$2
shift 2
tree=$(bash scripts/wsl-lane.sh tree linux-gcc-release)
program=$tree/runtime/bench_fft_f32
cpu=${CRD_BENCH_CPUS:-3}
out=${CRD_PROF_OUT:-$HOME/boards/prof-fft-f32}
mkdir -p "$out"
for side in "$@"; do
    stem=$out/n$lg-${CRD_CPU_LEVEL:-detected}-$side
    loop=(taskset -c "$cpu" "$program" --loop "$side" --sizes "$lg-$lg" --iterations "$iterations")
    echo "=== $side (2^$lg, $iterations iterations, CPU $cpu, level ${CRD_CPU_LEVEL:-detected}) ==="
    perf stat -o "$stem.topdown1.txt" -M TopdownL1 -e cycles,instructions,branch-misses "${loop[@]}"
    perf stat -o "$stem.topdown2.txt" -M TopdownL2 "${loop[@]}" > /dev/null
    grep -E 'cycles|instructions|branch-misses|tma_|seconds time' "$stem.topdown1.txt" "$stem.topdown2.txt" |
        sed "s|$out/||"
    if [[ ${CRD_PROF_RECORD:-1} != 0 ]]; then
        perf record -q -g -o "$stem.perf.data" "${loop[@]}" > /dev/null
        perf report -i "$stem.perf.data" --no-children --percent-limit 2 --stdio 2>/dev/null |
            grep -E '^ +[0-9.]+%' | head -n 12 | tee "$stem.symbols.txt"
    fi
done
