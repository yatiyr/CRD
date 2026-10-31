#!/usr/bin/env bash
# The fft kernel against the pre-row engine (docs/bench/README.md#fast-ab-boards): prof_fft_kernel (the same source on
# both engines: one size of the f64 forward in a tight loop) of the pre-row engine (scripts/build-prerow.sh, avx2
# only) and of HEAD's linux-gcc-release lane tree at each named level, alternating processes on one pinned CPU through
# scripts/ab-board.py, one board per size, the pre-row engine the reference. With CRD_AB_REFERENCE (another build's
# prof_fft_kernel, such as HEAD's saved before a candidate was built into the lane tree) that build is the reference
# instead, at the same level as the lane tree's in one board per size and level, and the two must print the same
# bits (ab-board.py --same). A size with :inverse times the inverse and with :f32 the complex-f32 transform (the
# reference build must take those arguments).
#   bash scripts/ab_fft_prerow.sh <out dir> <log2 n>:<iterations>[:inverse][:f32] [...]      e.g. 10:400000 20:60
# Environment: CRD_BENCH_LEVELS (HEAD's levels, default "detected avx2"), CRD_BENCH_CPUS (default 3),
# CRD_AB_REFERENCE (the reference program).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 2 ]]; then
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
out=$1
shift
# One size N:K[:inverse][:f32] into lg, iterations, flags (the program's further arguments) and stem; every size is
# checked before anything is built.
parse_size() {
    local parts flag
    IFS=: read -r -a parts <<< "$1"
    lg=${parts[0]}
    iterations=${parts[1]:-}
    flags=
    stem=n$lg
    if [[ ! $lg =~ ^[0-9]+$ || ! $iterations =~ ^[0-9]+$ ]]; then
        echo "ab_fft_prerow: $1 is not <log2 n>:<iterations>[:inverse][:f32]" >&2
        exit 2
    fi
    for flag in "${parts[@]:2}"; do
        if [[ $flag != inverse && $flag != f32 ]]; then
            echo "ab_fft_prerow: $1: $flag is neither inverse nor f32" >&2
            exit 2
        fi
        if [[ -z ${CRD_AB_REFERENCE:-} ]]; then
            echo "ab_fft_prerow: $1: the pre-row engine's prof_fft_kernel times the f64 forward only" >&2
            exit 2
        fi
        flags+=" $flag"
        stem+=-$flag
    done
}
for size in "$@"; do
    parse_size "$size"
done
bash scripts/wsl-lane.sh build linux-gcc-release prof_fft_kernel > /dev/null
head_program=$(bash scripts/wsl-lane.sh tree linux-gcc-release)/runtime/prof_fft_kernel
row='^N=(?P<key>\d+) iters=\d+ +(?P<value>[\d.]+) ns/fft'
bits='^N=(?P<key>\d+) .* bits=(?P<value>[0-9a-f]{16})$'
# One board: its arguments, then the directory it writes.
board() {
    local dir=$1
    shift
    mkdir -p "$dir"
    # The host's state before and after (docs/bench/README.md item 5); a contended host is named, not waited for.
    python3 scripts/build-bench.py host > "$dir/host-before.txt" 2>&1 ||
        echo "[ab] host contended ($?): $dir/host-before.txt"
    python3 scripts/ab-board.py "$@" --cpus "${CRD_BENCH_CPUS:-3}" --log-dir "$dir" --json "$dir/board.json" |
        tee "$dir/board.md"
    python3 scripts/build-bench.py host > "$dir/host-after.txt" 2>&1 ||
        echo "[ab] host contended ($?): $dir/host-after.txt"
}
if [[ -z ${CRD_AB_REFERENCE:-} ]]; then
    prerow_program=$(bash scripts/build-prerow.sh prof_fft_kernel | tail -n 1)/runtime/prof_fft_kernel
fi
for size in "$@"; do
    parse_size "$size"
    if [[ -n ${CRD_AB_REFERENCE:-} ]]; then
        for level in ${CRD_BENCH_LEVELS:-detected avx2}; do
            select=
            if [[ $level != detected ]]; then
                select="env CRD_CPU_LEVEL=$level "
            fi
            board "$out/$stem-$level" --cmd "reference=$select$CRD_AB_REFERENCE $lg $iterations$flags" \
                --cmd "head=$select$head_program $lg $iterations$flags" --arm "reference=reference:$row" \
                --arm "head-$level=head:$row" --same "reference,head:$bits" --reference reference
        done
        continue
    fi
    arguments=(--cmd "prerow=$prerow_program $lg $iterations$flags" --arm "prerow=prerow:$row")
    for level in ${CRD_BENCH_LEVELS:-detected avx2}; do
        select=
        if [[ $level != detected ]]; then
            select="env CRD_CPU_LEVEL=$level "
        fi
        arguments+=(--cmd "$level=$select$head_program $lg $iterations$flags" --arm "head-$level=$level:$row")
    done
    board "$out/$stem" "${arguments[@]}" --reference prerow
done
