# DIAG.11b (1a), (3), (5): the disabled mode judged on instructions per frame, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [jobs full-mode attribution](2026-10-24-diag-11b-jobs-full-attribution.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40006451399 (head
`6a964937`) failed only win-asan's gate, which `a832934d` repairs. 40050313292, 40047173030, 40041122184, 40039075298,
40029933433, 40029693899, 40022015924, 40020783027, 40018724265, 40014687187 and 40014471031 were cancelled with no
job (checked per run). 40032224637 (its win-asan job the first on `a832934d`) and 40050623248 were running; not waited
on.

## Method

The two-binary timing pair moved -11.0% to +0.2% on offline between Windows builds that do not touch its path, wider
than the 1% budget. This host has no unprivileged hardware counter (`perf stat -e instructions` is not supported
under WSL2; Windows needs elevation), so the layout-independent read is Valgrind 3.22's cachegrind on Linux:
instructions executed (exact, and the same whatever the linker's layout) plus a simulated cache (I1 32 KiB 8-way, D1
48 KiB 12-way, LL 36 MiB 18-way, as Valgrind detects the i9-14900K), whose misses depend on addresses and are reported.

`diag-overhead.py instructions` (a new subcommand, since (7)'s rendering workload needs the same read; it reuses the
board's record checks, arm rotation, verdict and head, now `board_head`) runs
each binary's off mode at 100 and 300 timed frames after 20 warmup and takes the difference per frame, so setup
cancels; every count is repeated and the interval spans every pairing of the repeats. Two runs of one binary at one
thread gave identical totals in every event; with four jobs threads the totals move by about 0.2% (the scheduler), and
the interval shows it. `test-diag-overhead.py` covers the subcommand with fake cachegrind files (setup cancels, moving
repeats widen to unresolved, a failed run takes the verdict); teeth: dropping the subtraction, keeping one pairing and
dropping the cost each fail it.

The binary pair (5): a scratch configuration, `CRD_LANE_ROOT=~/cerid-profile-scratch scripts/wsl-lane.sh configure
linux-gcc-shipping -DCRD_ENABLE_PROFILING=ON`, which differs from linux-gcc-shipping only in profiling (GCC 13.3,
LTO). Not a preset: a visible preset needs a hosted lane or a diagnostic-tier entry in `.github/ci-tiers.json`, and the
row's hosted evidence needs no Linux disabled lane. SHA-256 prefixes: compiled-out `cf3fb98ffec9`, profiling
`0833b0270012`.

## Instruction boards

| Workload | Compiled-out per frame | Off per frame | Off vs compiled-out: instructions, L1 misses, LL misses | Verdict (1%) |
| --- | ---: | ---: | --- | --- |
| offline | 10,024,275 | 10,032,127 | +0.08%, -1.23%, +0.90% (1 miss a frame) | within |
| jobs, one thread | 4,799,790 | 4,799,798 | +0.00%, -0.02%, -3.57% (under one miss a frame) | within |
| jobs, four threads, 4 repeats | 6,222,837 .. 6,239,554 | 6,224,985 .. 6,237,394 | -0.23% .. +0.23%, -0.56% .. +0.66%, under one miss a frame | within |
| alloc (worst case, reported) | 1,254,438 | 1,324,056 | +5.55%, -0.79%, under one miss a frame | over |

The extra work is the allocator statistics gate: alloc's 69,618 extra instructions over its 8,192 gate executions are
8.5 per gate, offline's 7,852 over its 1,008 (the 2026-10-23 count) 7.8 per gate, and jobs runs no gate. So offline's
disabled residual (3) is not executed work: what the 2026-10-16 and 2026-10-19 Windows pairs showed (+4.6%, and -11.0%
to +0.2% across rebuilds) is the layout floor, and the gate, the only extra work, stays bounded at 0.18% of a frame
(the 2026-10-23 count times cost). alloc's +5.55% agrees with its measured +4.7% to +5.0% time.

## Timing boards on the same pair (the Linux disabled arm, (5))

`diag-overhead.py run --arms compiled-out,off --workloads offline,jobs --iterations 200 --warmup 20 --affinity 5555`:

| Pool, processes per arm | offline: frame time, CPU per frame | jobs: frame time, CPU per frame | Verdicts |
| --- | --- | --- | --- |
| one thread, 48 | -0.35% (-0.92 .. -0.15), -0.52% (-0.95 .. +0.28) | +0.24% (+0.03 .. +0.40), +0.13% (-0.19 .. +0.45) | within, within |
| four threads, 96 | -0.38% (-0.56 .. -0.13), +0.16% (-0.47 .. +0.41) | -0.14% (-0.83 .. +0.60), +0.27% (-0.02 .. +0.74) | within, within |

This pair's layout happens to cost nothing; the instruction boards are what make the verdict independent of that.

## Windows

No instruction counter runs unprivileged on Windows here (the PMU counters are elevation-gated,
`counters-capability.py`, DIAG.6c), and fetching an instrumentation tool is a download the user has not approved.
The Windows disabled verdicts therefore stay on the exact count times the measured cost (offline 0.18%, jobs no
gate), with the Linux board showing that the gate is all the extra work the shared source executes. An instruction
count of the MSVC build is open qualification under DIAG.11c.

## Verification

`test-diag-overhead.py` (19 cases), `check-diag-runbooks.py --build build/win-debug` with the runbook's new command,
the tooling tests, `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`, the ctest guards. No C++
changed.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; each waits for the user's own approval in the
  conversation.
