#!/usr/bin/env bash
# The fft kernel against the pre-row engine (docs/bench/README.md#fast-ab-boards): prof_fft_kernel (the same source on
# both engines: one f64 forward size in a tight loop) of the pre-row engine (scripts/build-prerow.sh, avx2 only) and of
# HEAD's linux-gcc-release lane tree at each named level, alternating processes on one pinned CPU through
# scripts/ab-board.py, one board per size, the pre-row engine the reference.
#   bash scripts/ab_fft_prerow.sh <out dir> <log2 n>:<iterations> [...]      e.g. 10:400000 20:60
# Environment: CRD_BENCH_LEVELS (HEAD's levels, default "detected avx2"), CRD_BENCH_CPUS (default 3).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 2 ]]; then
    sed -n '2,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
out=$1
shift
bash scripts/wsl-lane.sh build linux-gcc-release prof_fft_kernel > /dev/null
head_program=$(bash scripts/wsl-lane.sh tree linux-gcc-release)/runtime/prof_fft_kernel
prerow_program=$(bash scripts/build-prerow.sh prof_fft_kernel | tail -n 1)/runtime/prof_fft_kernel
row='^N=(?P<key>\d+) iters=\d+ +(?P<value>[\d.]+) ns/fft'
for size in "$@"; do
    lg=${size%%:*}
    iterations=${size#*:}
    arguments=(--cmd "prerow=$prerow_program $lg $iterations" --arm "prerow=prerow:$row")
    for level in ${CRD_BENCH_LEVELS:-detected avx2}; do
        select=
        if [[ $level != detected ]]; then
            select="env CRD_CPU_LEVEL=$level "
        fi
        arguments+=(--cmd "$level=$select$head_program $lg $iterations" --arm "head-$level=$level:$row")
    done
    mkdir -p "$out/n$lg"
    # The host's state before and after (docs/bench/README.md item 5); a contended host is named, not waited for.
    python3 scripts/build-bench.py host > "$out/n$lg/host-before.txt" 2>&1 ||
        echo "[ab] host contended ($?): $out/n$lg/host-before.txt"
    python3 scripts/ab-board.py "${arguments[@]}" --reference prerow --cpus "${CRD_BENCH_CPUS:-3}" \
        --log-dir "$out/n$lg" --json "$out/n$lg/board.json" | tee "$out/n$lg/board.md"
    python3 scripts/build-bench.py host > "$out/n$lg/host-after.txt" 2>&1 ||
        echo "[ab] host contended ($?): $out/n$lg/host-after.txt"
done
