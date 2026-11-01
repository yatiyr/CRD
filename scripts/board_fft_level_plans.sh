#!/usr/bin/env bash
# The per-level fft plan board (ADR-0134 §7): every forward the planner offers at each size of a type, timed at each
# CPU level in passes, written as the board directory `scripts/gen_fft_level_plans.py --import` reads:
# pass<k>-<type>-<label>.log (the program's output) and pass<k>-level-<label>.txt (the level line it ran under). Each
# pass runs every level once, so a level's passes spread over the board's span instead of running back to back. The
# programs run through scripts/run-lane-bench.sh (the linux-gcc-release lane tree, pinned with taskset, the peers one
# thread, a forced level's glibc features hidden): bench_fft_f32 (every arm beside MKL) for f32, bench_fft_vs_refs
# (its f64 arms beside FFTW, pocketfft and MKL) for f64.
#   bash scripts/board_fft_level_plans.sh <out dir> [--type f32|f64] [--passes N] [--sizes A-B]
# --sizes (f32 only, default 10-22) is bench_fft_f32's; --passes defaults to 5. Environment: CRD_BENCH_LEVELS (the
# level entries, as run-lane-bench.sh takes them; default detected), CRD_BENCH_CPUS (the taskset list, default 4).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
usage()
{
    sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}
if [[ $# -lt 1 || $1 == -* ]]; then
    usage
fi
out=$1
shift
kind=f32
passes=5
sizes=
while [[ $# -gt 0 ]]; do
    case $1 in
        --type) kind=${2:-} ;;
        --passes) passes=${2:-} ;;
        --sizes) sizes=${2:-} ;;
        *) usage ;;
    esac
    shift 2 || usage
done
case $kind in
    f32)
        program=bench_fft_f32
        arguments=(--sizes "${sizes:-10-22}")
        ;;
    f64)
        program=bench_fft_vs_refs
        arguments=()
        if [[ -n $sizes ]]; then
            echo "board_fft_level_plans.sh: bench_fft_vs_refs times its own sizes; --sizes is f32's" >&2
            exit 2
        fi
        ;;
    *) usage ;;
esac
if [[ ! $passes =~ ^[1-9][0-9]*$ ]]; then
    usage
fi
read -r -a levels <<< "${CRD_BENCH_LEVELS:-detected}"
if [[ ${#levels[@]} -eq 0 ]]; then
    usage
fi
mkdir -p "$out"
CRD_BENCH_LEVELS= bash scripts/run-lane-bench.sh "$program" # build only, so no pass's log carries the build
for ((pass = 1; pass <= passes; ++pass)); do
    for entry in "${levels[@]}"; do
        log=$out/pass$pass-$kind-$entry.log
        echo "--- pass $pass of $passes, $kind, level $entry ---"
        CRD_BENCH_LEVELS=$entry bash scripts/run-lane-bench.sh "$program" -- "${arguments[@]}" > "$log"
        # cpu_level_record's line: the selection the importer files the pass's arms under, beside the program's own
        # "fft kernel level:" line (the unit it ran).
        grep -m 1 '^cpu ' "$log" > "$out/pass$pass-level-$entry.txt"
        grep -E '^(fft kernel level|n=[0-9]+ (winner|canonical))' "$log" | tr '\n' ' '
        echo
    done
done
