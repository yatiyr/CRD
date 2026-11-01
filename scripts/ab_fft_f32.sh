#!/usr/bin/env bash
# The fast fft f32 board (docs/bench/README.md#fast-ab-boards): the level's planned complex-f32 forward against MKL in
# one process per repetition (bench_fft_f32 --planned, both timed every rep), through scripts/ab-board.py on one
# pinned CPU. With CRD_AB_CANDIDATE (another build's bench_fft_f32) the candidate's planned forward is a third arm and
# HEAD's planned forward the reference, so one board answers both "did the change win" and "where does it stand
# against MKL"; the two builds must print the same bits at every size (ab-board.py --same), or the board stops.
# With CRD_AB_ARMS (algorithm names) the program times every forward the planner offers at each size (no --planned)
# and each named algorithm is an arm beside MKL, all from one process a repetition: one build's algorithms against
# each other and against MKL (their bits differ by design; each is gated against the FFTW f64 truth in the program).
# With both, each named algorithm of the candidate is an arm too (cand-<name>), from its own process a repetition
# interleaved with HEAD's: a change that alters an algorithm's bits (a split, a plan) judged against HEAD's edition
# of the same algorithm and against MKL; the reference is HEAD's first named arm unless CRD_AB_REFERENCE names one.
# Speed ratios above 1 mean the arm is faster than the reference.
#   bash scripts/ab_fft_f32.sh <out dir> [--sizes A-B] [ab-board.py options ...]
# Environment: CRD_CPU_LEVEL (the level, as the boards select it), CRD_BENCH_CPUS (the taskset CPU, default 3),
# CRD_AB_CANDIDATE (the candidate program), CRD_AB_HEAD (HEAD's program, such as a copy saved before the candidate
# was built into the lane tree; default the lane tree's), CRD_AB_ARMS (space-separated algorithm names, such as
# "in-place-3 standalone-hier"), CRD_AB_REFERENCE (the named arms' reference: mkl, the default, or one of them),
# CRD_AB_OFFSET (the transformed buffer's bytes past a 64-byte boundary, bench_fft_f32 --offset; 16 is a malloc'd
# buffer's alignment, which slows the 8K to 64K forwards of Cerid and MKL alike).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ $# -lt 1 || $1 == -* ]]; then
    sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
flags="--sizes $sizes"
if [[ -n ${CRD_AB_OFFSET:-} ]]; then # only when set: a build before the option refuses it
    flags+=" --offset $CRD_AB_OFFSET"
fi
program=${CRD_AB_HEAD:-$(bash scripts/wsl-lane.sh tree linux-gcc-release)/runtime/bench_fft_f32}
# A board row: n | arm | Cerid ms | MKL ms | ratio | errors | bits.
cerid='^(?P<key>\d+) +\| +\S+ +\| +(?P<value>[\d.]+) +\|'
mkl='^(?P<key>\d+) +\| +\S+ +\| +[\d.]+ +\| +(?P<value>[\d.]+) +\|'
bits='^(?P<key>\d+) +\|.*\| (?P<value>[0-9a-f]{16})$'
arguments=(--cmd "head=$program --planned $flags" --arm "cerid=head:$cerid" --arm "mkl=head:$mkl")
reference=mkl
if [[ -n ${CRD_AB_ARMS:-} ]]; then
    read -r -a names <<< "$CRD_AB_ARMS"
    # MKL is timed beside every arm of a size; the first named arm's row carries it.
    arguments=(--cmd "head=$program $flags"
               --arm "mkl=head:^(?P<key>\d+) +\| +${names[0]} +\| +[\d.]+ +\| +(?P<value>[\d.]+) +\|")
    for name in "${names[@]}"; do
        arguments+=(--arm "$name=head:^(?P<key>\d+) +\| +$name +\| +(?P<value>[\d.]+) +\|")
    done
    reference=${CRD_AB_REFERENCE:-mkl}
    if [[ -n ${CRD_AB_CANDIDATE:-} ]]; then
        arguments+=(--cmd "candidate=$CRD_AB_CANDIDATE $flags")
        for name in "${names[@]}"; do
            arguments+=(--arm "cand-$name=candidate:^(?P<key>\d+) +\| +$name +\| +(?P<value>[\d.]+) +\|")
        done
        reference=${CRD_AB_REFERENCE:-${names[0]}}
    fi
elif [[ -n ${CRD_AB_CANDIDATE:-} ]]; then
    arguments+=(--cmd "candidate=$CRD_AB_CANDIDATE --planned $flags" --arm "candidate=candidate:$cerid")
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
