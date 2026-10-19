# DIAG.11b clause (1): allocator statistics counted on request

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Board: [2026-10-19 allocator statistics](../bench/2026-10-19-diag-11b-allocator-statistics.md).

## CI read at the start

`gh run list --branch main --limit 15` at 09:05 local (2026-10-19). No completed run of a pushed commit had a failure
without a fix already on main:

- [39189832409](https://github.com/yatiyr/CRD/actions/runs/39189832409) (`471109f6`): 22 of 25 jobs green, three
  still running (`windows-shipping` for win-shipping-profile and win-shipping, `windows` for win-relwithdebinfo); none
  failed.
- [39207693168](https://github.com/yatiyr/CRD/actions/runs/39207693168) (`a38224df`): pending, no job started.
- 39201708392, 39200628155, 39195570971, 39195138375: cancelled while pending by newer pushes, no job started.

## Why this batch, and not the first queued follow-up

The previous batch queued REPO.3's 37 unrouted peer programs first. That item belongs to REPO.3, a row after DIAG in
the strict order, and needs peer libraries on specific hosts; DIAG.11b's own list starts at clause (1) once its (0)
items wait on the user. Clause (1) was also in the same follow-up list ("DIAG.11b clauses (1) to (7)"), so this batch
took it. The REPO.3 item stays queued, now with the three tracked debug outputs under `bench/gpu-compute/`.

## What was wrong

`crd/memory/memory_stats.hpp` updated every allocator's counters whenever `CRD_DEBUG` or `CRD_ENABLE_PROFILING` was
set: three atomic read-modify-writes and a peak-bytes exchange per allocation, two per free, whether or not anything
read them. The runtime-disabled mode of a profiling build (win-shipping-profile, no reader running) therefore paid
+37.7% on allocation-heavy frames ([2026-10-16 board](../bench/2026-10-16-diag-11b-overhead.md), loss 1).

## What changed

| Piece | Where | What it does |
| --- | --- | --- |
| Coverage | `crd/memory/statistics_coverage.hpp` | `StatisticsCoverage`: `Complete`, `NotRequested`, `TooLate`, `Elsewhere`, `CompiledOut`, and `Unrecorded` for a record that carries none; stable names (`complete`, `not_requested`, ...) |
| The gate | `StatisticsGate` in `crd/memory/memory_stats.hpp` | per allocator, one atomic byte: Silent (keeps no counts), Untouched, Uncounted, Refused, Counted. Counted is reachable only from Untouched; the first allocation's exchange closes the door on a later request, and an allocation that loses that exchange to a request counts itself. So a free is counted exactly when its allocation was, with relaxed order (one atomic's modification order decides) |
| Build modes | `CRD_MEMORY_STATS_TRACKING` (compiled in) and `CRD_MEMORY_STATS_ON_REQUEST` | `CRD_DEBUG`: counted from construction, unchanged. Profiling without `CRD_DEBUG`: counted after a request made before the first allocation. Neither: compiled out |
| The hot path | `MemoryStats::on_allocate`, `on_deallocate` | force-inlined: one byte load and two compares per operation; the first allocation's exchange and the counting are out of line |
| Who counts | the 8 allocators that update their own counters (TLSF, pool, growable pool, linear, growable linear, stack, malloc, virtual memory) call `m_stats.keep_counts()` in their constructors | every other allocator (growable TLSF, thread-safe, diagnostic, streaming category, fault and flight decorators, test doubles) reports `Elsewhere` by default, so a new forwarding allocator cannot read as a complete zero |
| The request | `IAllocator::request_statistics()`; `crd::perf::register_allocator` calls it | idempotent, any thread; `Complete`, `TooLate`, `Elsewhere` or `CompiledOut` |
| Readers | `AllocatorSnapshot::coverage`; `AllocatorRecord::stamps` (was `_pad`; the name stamp in bits 0-31 as before, the coverage + 1 in bits 32-39) with `allocator_record_coverage()`; `memory.allocators`' `statistics` field; the panel's Counted column | a capture keeps what its counts covered; an older capture's records read `Unrecorded`, and an unknown value from a file is never cast |
| A move | `GrowablePoolAllocator`'s move constructor and assignment | the destination takes the source's counts and decision (`MemoryStats::take`); before, a moved pool's frees came off zero counts and wrapped `bytes_in_use` on the debug lanes |
| Callers | `sandbox/src/main.cpp`, `runtime/examples/smoke_memory.cpp` | request the default allocator's statistics before anything allocates; whether static initialisation allocated through it first is not known for win-shipping-profile, and the panel's Counted column says `too_late` if it did |

The first form had the gate behind a call: MSVC did not inline `MemoryStats::on_allocate` into
`TlsfAllocator::try_allocate` (read with cdb from the win-shipping-profile binary), so every allocation paid a call
to learn it was not counted. The committed form inlines the load and compares (cdb: `movzx`, `cmp`, `je`, `cmp`, `jne`
in `try_allocate`) and calls out only to count or for the first allocation.

## Tests

- `tests/foundation/memory/test_statistics_coverage.cpp` (11 cases, every build): the gate's rules (a request before
  the first allocation counts everything; after it, refused and nothing counted, the first block's free included;
  `Elsewhere` without `keep_counts`; counting from construction), 300 rounds of a request racing three threads' first
  allocations (every round decides all allocations alike), and each build's own answer on real allocators: requested
  early, requested late, never requested, a decorator and a composite over a counted TLSF, a moved growable pool, the
  names.
- `tests/foundation/perf/test_memory_tracking.cpp`: registration gives `complete` snapshots and records; an allocator
  registered after its first allocation reads `too_late` and zeros on win-shipping-profile, `complete` with both
  allocations elsewhere; a registered decorator reads `elsewhere` while its wrapped heap counts; the record stamps keep
  name and coverage apart (an older capture, a coverage-only stamp, a name-only stamp, an unknown value).
- `test_capture_roundtrip.cpp` (the coverage survives the file), `test_diag_history_labels.cpp` (the fallback case
  now reads "no name stamp" through the helper instead of `_pad == 0`, and the coverage `elsewhere` of its growable
  TLSF), `test_diag_commands.cpp` (`"statistics":"complete"`), `test_world_tlsf.cpp` (the World's heap requests its
  statistics and is checked on every build that compiles them in, not only `CRD_DEBUG`).
- `fuzz_allocator_ops.cpp`: every fuzzed allocator's statistics are requested before its first allocation and must be
  `complete` (`elsewhere` for the growable TLSF and the diagnostic allocator), so its drained and balanced checks hold
  on win-shipping-profile too.
- The overhead harness (`crd-diag-overhead`): `Workload::measured_allocator()` (the alloc workload's TLSF, never the
  host's watch); in every mode with `--check` its coverage must be what the build counts unasked (`complete` on
  `CRD_DEBUG`, `not_requested` on win-shipping-profile, `compiled_out` elsewhere), balanced with every frame counted
  on `CRD_DEBUG`, all zero otherwise; the record gains a `statistics` object.

## Teeth

`build/diag11b-stats/teeth.py` applies each mutation by a byte replace on a unique site, rebuilds the lane, requires the
deciding tests to fail, restores the bytes (and the timestamp), rebuilds and requires them to pass again:

| Tooth | Lane | Failed |
| --- | --- | --- |
| A: the gate removed from allocation and free | win-shipping-profile | `[coverage]` (2 cases), `crd-diag-overhead-alloc-off`, `-basic`, `-full`; rerun against the final inlined form: the same |
| E: registration without a request | win-shipping-profile | 4 snapshot, frame and coverage cases (17 assertions) |
| B: a late request granted | win-debug | 2 coverage cases (the gate's late rule and the race: 300 rounds won by the request) |
| C: an allocation that loses the first exchange left uncounted | win-debug | the race case (48 of 300 rounds won by the request, inconsistent) |
| D: the moved pool without its counts | win-debug | the move case |
| F2: a capture's coverage cast unchecked | win-debug | the record-stamp case (0xEF read as a coverage) |
| G: frame_mark stamping only the name | win-debug | the frame, late-registration, decorator, history-labels and capture round-trip coverage checks |

One mutation does not bite and is recorded as equivalent: decoding the name without masking the low 32 bits (`F`).
`static_cast<u32>(stamps - 1)` keeps the low half, and a coverage-only word decodes to the all-ones invalid id either
way, so the mask is clarity, not behaviour.

## The defect found on the way: `MallocAllocator`'s bytes

A case freeing through a `MallocAllocator` under counting and checking `bytes_in_use` back at its baseline (run on
win-debug and linux-gcc-debug, not committed: it fails):

- Windows: `before in_use=0 mid in_use=1100 after in_use=1100` (100 and 1,000 bytes allocated and freed).
  `platform_allocation_size` returns 0 for an `_aligned_malloc` block (the CRT needs the alignment to size it), so
  `on_deallocate(0)` never lowers the bytes; the header's comment claimed `_msize`. The sandbox's `memory.allocators`
  shows the default allocator at 503,764,378 bytes in use, its lifetime total.
- glibc: `after in_use=18446744073709551564`. The request is counted at allocation and `malloc_usable_size` (1,152) at
  free, so the same pair wraps it to 2^64 - 52.

The existing debug case had checked only the counts. The repair is a 16-byte block prefix (size, alignment) that gives
an exact `allocation_size` on both platforms and balanced bytes, ASan-poisoned and read without instrumentation; it
changes the process-wide default allocator's layout, so it is its own batch, queued as DIAG.11b follow-up (p) with this
case as its tooth.

## Measured

[Board](../bench/2026-10-19-diag-11b-allocator-statistics.md): the allocation-heavy disabled cost fell from +37.7% to
+4.7% to +5.0% (two runs of the inlined form, one of the called form at +5.8%), still over 1%. A third binary with the
gate removed measured +0.12% against win-shipping and +4.98% below the gated one: the remaining cost is the gate
itself, about 0.5 ns per operation of a 10 ns TLSF operation. The same runs moved offline -11.0%, -10.7% and -3.2%
between builds that do not change its hot path, so the two-binary method has a layout floor of several percent; the
earlier board's unattributed offline residual (+4.6%) lies inside it. Both are open on the row, with what decides
them next: the counting chosen at construction (zero per-operation cost), and layout-varied builds or retired
instruction counts before a 1% verdict is read.

## Verification

- win-debug: every target built (1,779 steps after the gate, 1,727 again after its inlining) with no warning;
  crd-memory-tests (178 cases), crd-perf-tests (292), crd-scene-tests `[tlsf]`, the 12 harness CTests, the verdict and sampling-runbook CTests, both corpus replays and
  the 13 repository guards (29 CTests) pass; `smoke_memory` and `crd-sandbox --diag memory.allocators --smoke-test 3`
  print their coverage (`complete`).
- win-shipping-profile (the on-request lane): crd-diag-overhead, crd-memory-tests (177 cases), crd-perf-tests (288)
  and the fuzz target built with no warning; the suites, the 12 harness CTests and both corpus replays pass, the alloc
  record reading `not_requested` with zero counts.
- win-shipping: crd-diag-overhead built (the board's reference).
- linux-gcc-debug (mirror tree, mold): the five touched targets built with no warning; crd-memory-tests (178),
  crd-perf-tests (290) and 28 CTests (harness, corpus replays, guards) pass.
- Strict tidy clean on every changed C++ file (one pre-existing name, `_pad`, became `stamps`; a redundant
  `0 == 0` became `#if` branches; three local constants lower case). clang-format: no file has more findings than at
  HEAD; the lines it flags among this change's are rows of aligned groups the files already had. The Allman check,
  `check-diag-runbooks.py`, `check-master-plan.py` and `check-repository.py` pass.

## Hosted evidence needed

- win-shipping-profile: the three alloc `crd-diag-overhead-*` CTests pass with the heap's statistics `not_requested`
  and zero counts; crd-memory-tests' `[coverage]` cases (`too_late` for the late request) and crd-perf-tests'
  snapshot, frame, late-registration, decorator and record-stamp cases pass; `memory.allocators` reports
  `statistics` `complete`.
- Every other lane: the 12 harness CTests with its own coverage (`complete` on the `CRD_DEBUG` lanes, `compiled_out`
  on win-release, win-shipping, win-clang-cl-shipping, linux-gcc-release and linux-gcc-shipping), the coverage cases,
  `crd-fuzz-memory-allocators-corpus`, `crd-fuzz-perf-capture-corpus` and the history-labels cases pass.
- linux-gcc-shipping, -release and -relwithdebinfo link the changed memory and perf consumers with no `warning:` line.

## Follow-up for an extra batch

- (p) `MallocAllocator`'s byte statistics, as above.
- DIAG.11b (1) and (1a): the counting choice at construction, and the method floor.
- REPO.3: route or retire the 37 unrouted programs, and remove `bench/gpu-compute/dbg_err.txt`, `dbg_out.txt` and
  `san_out.txt`.

**Addendum, 2026-10-19.** (p) is repaired:
[the default allocator's exact sizes](2026-10-19-diag-11b-malloc-allocator-bytes.md).
