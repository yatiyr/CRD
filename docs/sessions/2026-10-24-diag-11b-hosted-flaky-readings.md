# DIAG.11b: four hosted failures the code passed elsewhere, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [device validation cost](2026-10-24-diag-11b-device-validation-cost.md).

## CI read at the start

`gh run list --branch main --limit 15`: 40067359705 (head `9ce438da`) completed with four failed lane jobs, and the
scheduled 40075929537 (head `420aaf1a`, still running) had one failed job. Two were already handled:

- linux-gcc-shipping `crd-repository-tools` (job 120801562603): the detached `git maintenance` in the fixture
  repository, repaired by `b793af76`.
- linux-gcc-asan `assetio: OUR tangents vs the REFERENCE mikktspace.c ORACLE` (job 120801562631): registered
  third-party defect TP-5; the lane's register gate printed `registered TP-5: failed`. That job failed because of
  the ring case below.

## The other four, each a defect

| Job | Case | Cause | Fix |
|---|---|---|---|
| 120819184913 linux-gcc-public-checks | `jobs diag10c: held counters and fibers ...` `fibers[0].high_water 8 == 9` | a pool raises its peak just after the count; `pressure_snapshot()` read the count (9) between the two | every reading reports the high water as at least the count it read: jobs counters, fiber tiers, pinned lanes and the lifetime ledger's `live` |
| 120801562631 linux-gcc-asan | `ring: frame-history reads are free of torn records ...` `copies 0 > 0` | the writer finished its 200,000 frames before the reader thread ran | the writer keeps lapping until the reader has copied, bounded by a 30 s deadline that fails the case; the INFO now precedes the checks |
| 120801562668 linux-clang-tsan | `diag10c: a reservation is one mapped region ...` `Absent: 0 == 1` | see below | `os_range_no_access` |

Teeth, win-debug: a 20 ms delay between the count and the peak raise reproduced the fiber failure (`8 == 9`) at
HEAD's reading and passed with the clamp; temporary cases with the same delay in the counter pool, the pinned push
and the ledger failed without their clamp and passed with it (not committed). A 2 s delay before the reader's first
copy reproduced `copies 0 > 0` with HEAD's loop and passed with the new one.

## The released reservation that read as mapped

The ledger said Balanced, so `munmap` succeeded. Looping the case on the linux-clang-tsan mirror tree with CI's
`TSAN_OPTIONS` failed 1 in 100 runs; probing straight after `vm::release` still failed 1 in 500, and an instrumented
run that failed printed one thread and the range inside a merged `rw-p` anonymous mapping
(`7d367351f000-7d3673a00000`). The kernel puts the process's next anonymous mapping at the top of the gap a release
opens, and the TSan runtime maps memory of its own at instrumented instructions, the probe's own `open` and `read`
included, so no ordering can make "nothing is mapped there" sound. Its owner is not resolvable without `strace`,
which this host lacks; the mapping is accessible, which is the discriminator.

`crd::os_range_no_access` (crd-core, sharing the range walker with `os_range_mapped`) counts only what nothing may
touch: MEM_RESERVE or PAGE_NOACCESS on Windows, `---` in `/proc/self/maps` on Linux. The vm case and the ledger's
range case judge every release with it (a surviving reservation is no-access; a runtime's mapping is not), check
Present and Partial (a committed page is accessible) on held ranges, and the ledger case asks `os_range_mapped`
for Absent at address 4096, which no OS maps. Teeth: skipping `VirtualFree` (win-debug) or `munmap`
(linux-gcc-debug) fails the release check; counting accessible mappings fails the Partial check on both. After the
change: 10,000 TSan runs, 0 failures.

## Verification

win-debug: crd-jobs-tests, crd-perf-tests, crd-vm-tests and crd-core-tests built and passed in full. linux-gcc-debug
(mirror): the same four built and passed; the affected CTests (`diag10c|ring:|lifetime|pressure|^jobs|fiber`, 245)
passed. linux-clang-tsan: the four built, the same selection (246) passed. Tidy (LLVM 20) clean on the changed
files; clang-format findings at HEAD's count except the aligned groups the files already use.

Contract: [runtime diagnostics, DIAG.10c](../design/runtime-diagnostics.md) names `os_range_no_access` and the
high-water rule. Hosted once pushed: the four cases pass on every lane.
