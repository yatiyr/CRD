#!/usr/bin/env bash
# The fast fft f32 board (docs/bench/README.md#fast-ab-boards): the level's planned complex-f32 forward against MKL in
# one process per repetition (bench_fft_f32 --planned, both timed every rep), through scripts/ab-board.py on one
# pinned CPU. With CRD_AB_CANDIDATE (another build's bench_fft_f32) the candidate's planned forward is a third arm and
# HEAD's planned forward the reference, so one board answers both "did the change win" and "where does it stand
# against MKL"; the two builds must print the same bits at every size (ab-board.py --same), or the board stops.
# Speed ratios above 1 mean the arm is faster than the reference.
#   bash scripts/ab_fft_f32.sh <out dir> [--sizes A-B] [ab-board.py options ...]
# Environment: CRD_CPU_LEVEL (the level, as the boards select it), CRD_BENCH_CPUS (the taskset CPU, default 3),
# CRD_AB_CANDIDATE (the candidate program), CRD_AB_HEAD (HEAD's program, such as a copy saved before the candidate
# was built into the lane tree; default the lane tree's).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 1 || $1 == -* ]]; then
    sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi
out=$1
shift
sizes=10-22
if [[ ${1:-} == --sizes ]]; then
    sizes=$2
    shift 2
fi
mkdir -p "$out"
program=${CRD_AB_HEAD:-$(bash scripts/wsl-lane.sh tree linux-gcc-release)/runtime/bench_fft_f32}
# A board row: n | arm | Cerid ms | MKL ms | ratio | errors | bits.
cerid='^(?P<key>\d+) +\| +\S+ +\| +(?P<value>[\d.]+) +\|'
mkl='^(?P<key>\d+) +\| +\S+ +\| +[\d.]+ +\| +(?P<value>[\d.]+) +\|'
bits='^(?P<key>\d+) +\|.*\| (?P<value>[0-9a-f]{16})$'
arguments=(--cmd "head=$program --planned --sizes $sizes" --arm "cerid=head:$cerid" --arm "mkl=head:$mkl")
reference=mkl
if [[ -n ${CRD_AB_CANDIDATE:-} ]]; then
    arguments+=(--cmd "candidate=$CRD_AB_CANDIDATE --planned --sizes $sizes" --arm "candidate=candidate:$cerid")
    arguments+=(--same "head,candidate:$bits")
    reference=cerid
fi
# The host's state before and after (docs/bench/README.md item 5); a contended host is named, not waited for.
python3 scripts/build-bench.py host > "$out/host-before.txt" 2>&1 ||
    echo "[ab] host contended ($?): $out/host-before.txt"
python3 scripts/ab-board.py "${arguments[@]}" --reference "$reference" --cpus "${CRD_BENCH_CPUS:-3}" \
    --log-dir "$out" --json "$out/board.json" "$@" | tee "$out/board.md"
python3 scripts/build-bench.py host > "$out/host-after.txt" 2>&1 ||
    echo "[ab] host contended ($?): $out/host-after.txt"
