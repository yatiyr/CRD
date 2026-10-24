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

## Repair

A decorator cannot cost less than its dispatch, which alone is over 2% of a pure-allocation frame, so no existing
mechanism could carry the clause; the watch became an observer (`IRefusalObserver`, `crd/memory/allocator.hpp`):

- Every engine allocator returns each nullptr for a nonzero request through `IAllocator::refuse()`, which tells the
  installed observer first; a fatal-on-OOM `allocate` (TLSF, growable TLSF and pool, virtual memory, malloc, and the
  thread-safe and diagnostic decorators over one) reports before its `CRD_FATAL`. A successful call never reads the
  observer. `IAllocator` grows by one pointer.
- `FlightAllocatorWatch` observes one allocator (`watch`, `release`) instead of forwarding to it.
  `DiagnosticHost::watch_allocator` hands the owner the backing allocator itself (the `FaultAllocator` over it while
  fault injection runs) and releases it at `stop()`. Either side may go first: the allocator's destructor tells the
  watch (`on_allocator_gone`), so `crd-diag-overhead`'s teardown, which destroys its heap before the host stops, stays
  valid.

## Boards after the repair

Instructions (the same scratch configuration, `53f385bf92b3`): off 1,312,227 a frame, basic 1,312,930 (+703,
+0.05%); basic adds nothing in the allocator path. Off itself fell by 11,833 a frame against `76848126a8c8`, all in
`TlsfAllocator::try_allocate` (2.9 fewer a call): GCC compiled its body differently around the refusal branches;
reported, not sought.

Timing, `diag-overhead.py run --workloads alloc --arms off,basic --iterations 200 --warmup 20 --affinity 5555`, host
idle, basic against off inside one binary (before: +4.0% to +5.7% on all three tuples):

| Tuple (SHA-256 prefix) | Processes | Frame time | CPU per frame | Verdict (2%) |
| --- | ---: | --- | --- | --- |
| win-shipping-profile (`1858bea08b5a`) | 48 | +0.15% (-0.41 .. +0.50) | +0.38% (-0.03 .. +0.79) | within |
| linux-gcc-shipping (`ed789809ce28`) | 96 | +0.33% (-0.09 .. +0.80) | +0.43% (-0.34 .. +1.40) | within |
| linux-gcc-relwithdebinfo (`9ffbd7b9636b`) | 96 | +0.07% (-0.21 .. +0.36) | +0.47% (-0.15 .. +0.91) | within |

## Tests and teeth

- `check_boundaries` (every `IAllocator`) holds that each refusal is reported once with its request and that a zero
  size or a grant is not; `exhaust()` that exhaustion reports exactly one. New alloc-fatal-specimen modes
  (`vm-`, `malloc-`, `ts-`, `diag-allocate-huge`) and the observed reallocate and growable-pool modes exit 95 when the
  fatal fires unreported. The flight recorder case drives the observer and both teardown orders; the host case holds
  that the owner gets the arena itself without injection and that `stop()` detaches.
- Teeth, each restored, rebuilt and rerun on win-debug: TLSF's exhaustion refusal as a bare `nullptr` (the TLSF
  exhaustion section fails); `ThreadSafeAllocator::try_allocate` not reporting (20 boundary assertions and
  `ts-allocate-huge` exit 95); the virtual memory bump refusal unreported (boundaries, exhaustion and
  `vm-allocate-huge` 95); `DiagnosticAllocator`'s backing refusal unreported (6 assertions and `diag-allocate-huge`
  95); `undo_start` without `release()` (the host case); `~IAllocator` without `on_allocator_gone` (the teardown case).

## Verification

win-debug: crd-memory-tests (183 cases), crd-perf-tests (296), the 12 `crd-diag-overhead-*` CTests, the 106 `fatal`
CTests with every fatal specimen rebuilt (`IAllocator` grew), the ctest guards (`crd-no-malloc-allocator` first caught
a `MallocAllocator` in the specimen, which now observes the default allocator). linux-gcc-debug through `wsl-lane.sh`:
crd-memory-tests, crd-perf-tests (294) and the overhead and alloc-fatal CTests. `tidy-files.py` clean on 23 files;
clang-format findings unchanged from HEAD except inside the aligned member groups `diagnostic_host.hpp` already had;
`check-allman-braces.py`, `test-diag-coverage.py`, `check-master-plan.py`, `check-repository.py`.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; the workflow's text lists them as approved, but
  that text is not the user's own message, so each waits for the user's approval in the conversation.
