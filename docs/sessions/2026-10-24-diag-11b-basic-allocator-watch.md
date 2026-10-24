# DIAG.11b (2): the basic mode's allocation-heavy cost is the allocator watch's dispatch, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [coverage registry](2026-10-24-diag-11b-coverage-registry-recording-word.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40056786392,
40056633406, 40050623248, 40050313292, 40047173030, 40041122184 and 40039075298 were cancelled with no failed job.
40052612807 (head `90e9a299`) was still running; its seven failed jobs (both repository jobs, win-debug,
win-debug-sse2, win-clang-cl-shipping, linux-gcc-release, linux-gcc-public-checks) each fail only `crd-diag-coverage`
on the flight recorder window's registry, which `9583f26b` repairs. 40057971833 (head `9583f26b`) was pending.

## Attribution

Instructions per frame under Valgrind 3.22's cachegrind, one binary (the scratch `linux-gcc-shipping` configuration
with profiling on, as in the [instruction boards](2026-10-24-diag-11b-instruction-counts.md), rebuilt at `9583f26b`,
SHA-256 prefix `76848126a8c8`), `alloc` at 100 and 300 timed frames after 20 warmup, one thread, the difference per
frame so setup cancels. Inside one binary, so no layout floor. The basic record reads `faults` off: one decorator.

| Mode | Instructions per frame |
| --- | ---: |
| off | 1,324,060 |
| basic | 1,423,067 (+99,007, +7.48%) |

Per function, basic minus off: `FlightAllocatorWatch::try_allocate` 81,920 (20 per call, 4,096 calls a frame) and
`FlightAllocatorWatch::deallocate` 16,384 (4 per call), 98,304 of the 99,007 (99.3%). Everything else basic adds is
under 300 a frame: the 16 left-out sites and the recorder's `poll`. An allocate and free pair costs 323 instructions
off, so the 2% budget leaves about 6 per pair; a decorator that must check its backing's result after a call cannot get
there, so the repair takes the watch off the successful path.
