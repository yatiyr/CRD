#!/usr/bin/env bash
# Profile the complex-f32 forward of each named side at one size on the linux-gcc-release lane tree's bench_fft_f32
# (built with -DCRD_BUILD_HESAP_VS_FFT_REFS=ON), each side's own process (bench_fft_f32 --loop): perf stat's top-down
# levels 1 and 2 with cycles, ref-cycles, task-clock (the side's clock: cycles over task-clock, and cycles over
# ref-cycles against the nominal rate), instructions and branch misses, then perf record's call graph and the hottest
# symbols. The copy side is the loop's own overhead (the input copy every transform starts from), whose counts a
# side's include. Run on the benchmark machine (docs/bench/README.md#fast-ab-boards); one pinned CPU, the selected
# level by CRD_CPU_LEVEL as the boards select it.
#   bash scripts/prof_fft_f32.sh <log2 n> <iterations> <side> [side ...]     (sides: planned, an algorithm, mkl, copy)
# Environment: CRD_BENCH_CPUS (the taskset CPU, default 3), CRD_PROF_OUT (the output directory, default
# ~/boards/prof-fft-f32), CRD_PROF_RECORD (0: perf stat only), CRD_PROF_EVENTS (a comma list of further perf events,
# such as l1d.replacement,l1d_pend_miss.fb_full,ocr.streaming_wr.any_response, counted in a pass of their own),
# CRD_PROF_PROGRAM (another build's bench_fft_f32, such as a saved HEAD copy, in place of the lane tree's),
# CRD_PROF_ROUNDS (K: after the passes, K rounds that run every side's loop in turn with --distribution under perf
# stat's clock events, so the sides alternate and share the host's state; each line is one side's stream: its
# per-transform minimum, median and mean beside its clock), CRD_PROF_PASSES (0: the distribution rounds only),
# CRD_PROF_OFFSET (the transformed buffer's bytes past a 64-byte boundary, bench_fft_f32 --offset; the files then
# carry -off<bytes>, so two offsets' runs keep their own).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 3 ]]; then
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
lg=$1
iterations=$2
shift 2
tree=$(bash scripts/wsl-lane.sh tree linux-gcc-release)
program=${CRD_PROF_PROGRAM:-$tree/runtime/bench_fft_f32}
cpu=${CRD_BENCH_CPUS:-3}
out=${CRD_PROF_OUT:-$HOME/boards/prof-fft-f32}
clock=task-clock,cycles,ref-cycles
mkdir -p "$out"
offset=() # only when set: a build before the option refuses it
if [[ -n ${CRD_PROF_OFFSET:-} ]]; then
    offset=(--offset "$CRD_PROF_OFFSET")
fi
passes=()
if [[ ${CRD_PROF_PASSES:-1} != 0 ]]; then
    passes=("$@")
fi
for side in "${passes[@]}"; do
    stem=$out/n$lg-${CRD_CPU_LEVEL:-detected}${CRD_PROF_OFFSET:+-off$CRD_PROF_OFFSET}-$side
    loop=(taskset -c "$cpu" "$program" --loop "$side" --sizes "$lg-$lg" --iterations "$iterations"
          "${offset[@]}")
    echo "=== $side (2^$lg, $iterations iterations, CPU $cpu, level ${CRD_CPU_LEVEL:-detected}) ==="
    perf stat -o "$stem.topdown1.txt" -M TopdownL1 -e "$clock,instructions,branch-misses" "${loop[@]}"
    perf stat -o "$stem.topdown2.txt" -M TopdownL2 "${loop[@]}" > /dev/null
    grep -E 'task-clock|cycles|instructions|branch-misses|tma_|seconds time' "$stem.topdown1.txt" \
        "$stem.topdown2.txt" | sed "s|$out/||"
    if [[ -n ${CRD_PROF_EVENTS:-} ]]; then
        perf stat -o "$stem.events.txt" -e "cycles,instructions,$CRD_PROF_EVENTS" "${loop[@]}" > /dev/null
        grep -E '^ +[0-9,]+ ' "$stem.events.txt" | sed "s|$out/||"
    fi
    if [[ ${CRD_PROF_RECORD:-1} != 0 ]]; then
        perf record -q -g -o "$stem.perf.data" "${loop[@]}" > /dev/null
        perf report -i "$stem.perf.data" --no-children --percent-limit 2 --stdio 2>/dev/null |
            grep -E '^ +[0-9.]+%' | head -n 12 | tee "$stem.symbols.txt"
    fi
done
# The distribution rounds: one line a side a round, its stream's per-transform times (microseconds, the copy
# untimed) and its clock (GHz: cycles over task-clock; the cycles to ref-cycles ratio against the nominal rate).
for ((round = 1; round <= ${CRD_PROF_ROUNDS:-0}; ++round)); do
    for side in "$@"; do
        stem=$out/n$lg-${CRD_CPU_LEVEL:-detected}${CRD_PROF_OFFSET:+-off$CRD_PROF_OFFSET}-$side-round$round
        perf stat -x, -o "$stem.clock.txt" -e "$clock" taskset -c "$cpu" "$program" --loop "$side" \
            --sizes "$lg-$lg" --iterations "$iterations" "${offset[@]}" --distribution > "$stem.txt"
        each=$(grep '^each transform' "$stem.txt" | sed 's/^each transform (without its copy, us): //')
        python3 - "$stem.clock.txt" "$round" "$side" "$each" << 'PY'
import sys
counts = {}
for line in open(sys.argv[1], encoding='utf-8'):
    fields = line.strip().split(',')
    if len(fields) > 2 and fields[2] in ('task-clock', 'cycles', 'ref-cycles') and fields[0][:1].isdigit():
        counts[fields[2]] = float(fields[0])
ghz = counts['cycles'] / (counts['task-clock'] * 1e6)
print(f'round {sys.argv[2]} {sys.argv[3]}: {sys.argv[4]} | {ghz:.2f} GHz, cycles/ref-cycles '
      f'{counts["cycles"] / counts["ref-cycles"]:.3f}')
PY
    done
done
