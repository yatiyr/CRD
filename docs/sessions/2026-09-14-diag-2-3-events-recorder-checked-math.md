# DIAG.2a/2b/3a — typed events, lifecycle recording, checked allocator math

<!-- doc-role: historical -->

Dated evidence for [DIAG.2a](../ROADMAP.md#slice-diag.2a), [DIAG.2b](../ROADMAP.md#slice-diag.2b)
and [DIAG.3a](../ROADMAP.md#slice-diag.3a). Contract:
[runtime-diagnostics](../design/runtime-diagnostics.md); direction:
[ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). One autonomous DIAG-loop
iteration; all proof is `win-debug`.

## DIAG.2a — typed events, identities, policy (`crd/perf/diagnostics.{hpp,cpp}`)

- **Schema + taxonomy.** `kDiagnosticSchemaVersion`; `Severity` separates Info / Warning /
  RecoverableError / DeveloperAssertion / InstrumentFailure / FatalInvariant (DG13's "don't
  collapse into one error bucket"). `is_fatal` isolates the required-validation class.
- **Identities + clock.** `SourceIdentity` (borrowed file/symbol/line), `GenerationKey` (u64,
  wrap made explicit), monotonic `diagnostic_now_ns` (steady clock, never wall-clock).
- **Authorable bounded policy (DG18).** `DiagnosticPolicyStore::load` validates a candidate
  against hard bounds and the schema version; only a valid candidate commits and advances the
  generation. A malformed / oversized / unknown-version / zero-bound candidate is rejected with
  an explicit `PolicyLoadStatus` and leaves **both** the active policy and the generation
  untouched — malformed input can never move the active generation (last-known-good).
- **Shared context.** `to_json` emits a deterministic, escaped, single-line object; `to_log_line`
  the human form. Message truncation to the policy's byte bound sets `message_truncated` — never
  silent.
- **Cross-module bridge without an upward dependency.** `EventCode` is a `u32` with a documented
  foundation range `[0, 0xFFFF]`; other modules call `register_code_range(name, lo, hi)` from
  their own translation unit, so crd-perf includes no CEIR header. `owning_module` resolves the
  tag; overlapping / inverted / into-foundation registrations are refused. A CEIR-coded and a
  foundation-coded event serialise through the identical path, differing only in the module tag
  (the "same structured context through CLI/JSON" acceptance).
- **Always-on validation.** `check_invariant` / `CRD_DIAG_INVARIANT` build a `FatalInvariant`
  event and route it to a settable handler (tests install a non-terminating one; the default
  reports via `CRD_FATAL` and aborts). It is present in every build — not a `CRD_ASSERT` that
  vanishes in release.

70 assertions across 8 cases (`[diag][events|policy|json|bridge|fatal]`).

## DIAG.2b — lifecycle recording + emergency record (`crd/perf/diagnostic_recorder.{hpp,cpp}`)

- **Preallocated bounded ring.** `init(capacity)` reserves the ring once; `record` never
  allocates and overwrites the oldest slot when full while keeping every retained record valid
  (owned message strings intact). `snapshot` copies oldest-first; `count`/`total_recorded` expose
  live vs monotonic totals.
- **Reader lifetime tokens.** `register_reader` hands back a generation-tagged token;
  `deregister_reader` retires the slot and bumps its generation, so a double-deregister or a
  token for a since-reused slot is rejected — no double-free, no dangling reader.
- **Independent emergency record.** `capture_emergency` writes a fixed-size record under its own
  lock, never the ring mutex, so it succeeds even while another thread holds the ring/log lock
  (the "crash while the log mutex is held still yields a record" case). Async-signal safety from
  a real handler remains DIAG.5b's job.
- **Lifecycle.** Every entry point is a benign no-op before `init` and after `shutdown`;
  re-init after shutdown is legal. DG08's concern is met by reading and writing the registry under
  the same lock rather than trusting a writer mutex to protect readers.

56 assertions across 6 cases, including a 6-thread × 4000 concurrent-record stress that stays
bounded to capacity with every record accounted for. The TSan proof rides DIAG.1b's lane.

## DIAG.3a (core) — checked allocator arithmetic (`crd/memory/checked_math.hpp`)

Constexpr, allocation-free overflow-checked helpers: `checked_add`, `checked_mul`,
`checked_array_size` (count×stride), `checked_align_up` (refuses the `(value + align - 1)` wrap
the naive form suffers within `align-1` of `SIZE_MAX`, and non-power-of-two alignments),
`checked_page_count`, `checked_padded_array_size`. Each returns success/overflow rather than
wrapping to a too-small size (the classic allocator integer-overflow bug), and adds no
per-allocation heap cost. 31 assertions incl. compile-time `static_assert`s and exact
near-address-width boundary cases.

**Not yet done for 3a:** wiring the helpers through every allocator's size path and the full
per-allocator ownership / zero-size / exhaustion / realloc-failure contract sweep — later
iterations of the loop.

## Regression

`crd-perf-tests`, `crd-memory-tests` and `crd-jobs-tests` build clean under `/W4 /WX
/permissive-`; the repository and master-plan validators pass.

## DIAG.3a (wired) / 3b / 3d / 3e — second loop iteration

Continuing the same programme; all proof is `win-debug` unless noted.

### DIAG.3a wired into real size paths

`checked_math.hpp` was a header nothing called; it is now wired where overflow actually reaches an
allocation: `PoolAllocator` pads `slot_size` and multiplies by `slot_count` through
`checked_align_up`/`checked_mul` (CRD_FATAL on overflow — always-on, not a debug assert), and
`LinearAllocator::allocate` computes the aligned offset + size through `checked_align_up`/
`checked_add` so a near-`SIZE_MAX` request fails like exhaustion (nullptr) and leaves the prior
allocation intact. Contract test (`test_allocator_contracts.cpp`) proves the overflowing request
returns nullptr and the next in-bounds allocation still succeeds.

### DIAG.3b — ASan boundaries, proven under real win-asan

`asan_poison.hpp` (`CRD_MEM_ASAN`, mirroring `CRD_JOBS_ASAN`) wraps
`__asan_poison/unpoison_memory_region`, compiling to nothing without ASan. Wired into
`LinearAllocator`: the whole buffer is poisoned on construct and on `reset`, each logical
allocation is unpoisoned as it is handed out (padding between allocations stays poisoned to catch
over-reads), `reset_to` re-poisons the rewound tail (nested-arena/scope reuse), and destruction
unpoisons so external buffers come back clean. Proof: the full `crd-memory-tests` suite runs
**19/19 green under the `win-asan` preset** with poisoning active — correct usage stays clean;
Windows MSVC ASan is real ASan. The intentional use-after-reset negative-control specimen (which
would ASan-abort, so it cannot be an ordinary ctest) and the pool intrusive-free-list poisoning
are owed.

### DIAG.3d — structural walkers, double/interior/wrong-owner free

`PoolAllocator::validate_structure()` walks the intrusive free list and returns false on an
out-of-range link, an off-slot-boundary link, a cycle, or a length disagreeing with
`slots_free()` — the exact shapes a double-free produces; the test induces a real double-free and
confirms detection. `is_slot_aligned()` separates a genuine slot pointer from an interior one
(the pool's `owns()` is already slot-strict), and a wrong-owner free is rejected by the other
pool's `owns()`. 18 assertions.

### DIAG.3e — generation-checked handles

`SlotMap<T>` (`crd/containers/slot_map.hpp`): a dense store addressed by `Handle{index,
generation}`. get/contains/erase validate the generation; erase bumps it (explicit null-generation
wrap guard) so every outstanding handle and derived borrow goes stale at once, a reused slot never
resurrects an old handle, and a double-erase is refused. Needs only a movable T; a move-only,
resource-owning payload is released on erase. 57 assertions. Threading it through the real
resource-manager/reload consumers (DG04) is the remaining half.

### Regression

`crd-memory-tests`, `crd-containers-tests`, `crd-perf-tests`, `crd-jobs-tests` build clean under
`/W4 /WX /permissive-`; `crd-memory-tests` additionally passes under `win-asan`; both validators
pass.

## DIAG.3d / 3e / 1b completion — third loop iteration

Closing the follow-on halves the second iteration left open, so the Needs-CI claims are true before push.

### DIAG.3d — TLSF structural walker + model-based sweep

`TlsfAllocator::validate_structure()` (declared in the header, defined in `tlsf_allocator.cpp`) is the
TLSF counterpart to the pool walker. Pass 1 walks the physical block chain from the start sentinel to
the end sentinel: bounds, `prev_phys_block` back-links, the `kPrevFreeBit` flag reconciled against the
real predecessor's free state, the coalescing invariant (never two physically-adjacent free blocks),
and exact region closure by the end sentinel. Pass 2 walks every FL/SL free list: each listed block is
free and in-bounds, doubly-linked `next_free`/`prev_free` are consistent and acyclic, each block is
filed in the bucket its size maps to (`mapping_insert`), the FL/SL bitmaps are set iff their list is
non-empty, and the physically-free count reconciles exactly with the listed count. A caught subtlety:
the true minimum free-block payload is `kBlockMinUserSize` (16, the `next_free`/`prev_free` overlay),
not `kBlockMinSize` (32) — an alignment leading-remainder (`trim_free_leading`) is legitimately that
small, so the walker checks the former (an over-strict first cut false-positived on the 32-byte-aligned
path in the sweep).

`test_allocator_contracts.cpp` adds: a healthy-heap acceptance across alloc/free/realloc; a 4000-op
randomized alloc/realloc/free sweep against an independent live-range model that, after **every**
operation, requires the walker clean, live ranges non-overlapping, and each block's byte pattern intact
(catching any metadata scribble into user memory), draining to a fully-recovered heap; and a seeded
metadata corruption (a header size field stomped past the region via a caller-provided buffer) confirmed
as a named walker failure, not a hang or silent pass. Full `crd-memory-tests`: 820112 assertions / 129
cases green on win-debug.

### DIAG.3e — real reload consumer follows the SlotMap lifetime

`test_hot_reload.cpp` gains a `[resources][hot_reload][diag]` case proving the real `ResourceManager`
control block enforces the same generation-checked lifetime `SlotMap<T>` formalizes: a generation
snapshot taken before a `reload_mount_now` while a live handle is held goes stale when the reload bumps
the control-block generation — the "reload with pending work" acceptance — and the identical contract is
asserted in the formal container (a handle rejected after `erase`, a reused slot returning the same
index with a new generation so the old handle stays dead). This complements the file's existing
generation-bump / failed-reload-last-good / subscribe / unsubscribe cases, which already thread the
lifetime through the real consumer. `crd-resources-tests`: 14471 assertions / 117 cases green.

### DIAG.1b — negative-control specimen

Recorded in the DIAG.1a session doc: `data_race_specimen.cpp` through the DIAG.0 harness, driven by
`test_tsan_race_specimen.cpp`, asserting SanitizerCaught under TSan / InstrumentAbsent otherwise.
`crd-jobs-tests`: 29393 assertions / 107 cases green (4 `[tsan]` cases). The jobs test module now
declares `support/diag` in its `crd_module()` TESTS list so the harness link matches the build graph.

### Regression

`crd-memory-tests`, `crd-jobs-tests`, `crd-resources-tests` all build clean under `/W4 /WX /permissive-`
and pass on win-debug; the repository and master-plan validators pass.

## DIAG.3c — provenance, redzones, quarantine, sampling (fourth loop iteration)

`crd/memory/diagnostic_allocator.{hpp,cpp}` — `DiagnosticAllocator`, an `IAllocator` decorator over
any backing allocator. What landed, mapped to the acceptance clauses:

- **Provenance / stale-access sites.** Each tracked allocation carries size, alignment, an
  `AllocationTag` (owner/task/generation), and interned **allocation and free** call-stack ids. PCs are
  captured cheaply (`RtlCaptureStackBackTrace` on Windows, `backtrace` where `<execinfo.h>` exists) and
  interned by FNV-1a hash into a bounded table; `stack_frames()` returns the raw PCs for **offline**
  symbolization (never symbolized inline). `provenance_of(p)` reports a live *or* quarantined block, so
  an intentional stale access names both its allocation and free sites.
- **Redzones (guards).** Every allocation is padded with pattern-filled guard bytes on both sides;
  `verify_redzones` runs on free and via `check_all_redzones()`, reporting `RedzoneUnderrun` /
  `RedzoneOverrun` with the corrupted byte offset — isolated to the allocation, attributable to its site.
- **Bounded quarantine.** With a non-zero cap, freed blocks are poisoned and held FIFO up to a byte
  budget before the backing free runs, so a use-after-free write is caught (`QuarantineUseAfterFree`)
  against a block whose sites are still recorded. Eviction is oldest-first; `quarantined_bytes()` never
  exceeds the cap. Double-free is a named verdict (the quarantined record is found), not a re-free.
- **Metadata saturation is safe and loud.** The record/stack/quarantine arenas are fixed-size and carved
  once from an **independent** metadata allocator (asserted `!= backing`, so diagnosis never recurses into
  the diagnosed allocator). When the record table fills, the allocation still succeeds (the redzoned block
  is released and re-served as a plain passthrough — no interior-pointer corruption), a `MetadataSaturation`
  violation fires in mandatory mode (never silent), and `metadata_saturation_count()` advances.
- **Guarded sampling.** Only a configured 1-in-N fraction over an eligible `[min,max]` size window captures
  a full stack; `sampling_report()` states the period, window, eligible/sampled/total counts and retained
  live history — it never implies exhaustive detection.
- **Leak/retention summary.** `report_leaks()` visits every still-live (non-freed) record with its size and
  allocation site.
- **Cost.** `per_allocation_overhead()` is exact (front redzone aligned up + rear redzone) and asserted
  against the backing block size.

`tests/foundation/memory/test_diagnostic_allocator.cpp`: 11 cases covering each clause. Green on
win-debug and under the real `win-asan` preset (24 `[diag]` cases / 155 assertions each); the software
redzone reads stay within the backing block, so it composes with ASan rather than fighting it. Full
`crd-memory-tests`: 820203 assertions / 140 cases. Both validators pass.

Mechanism note: under/overrun detection is redzone-based (checked on free + on demand), the "guards" the
design enumerates; an eager page-protection (fault-on-touch) variant is a possible future hardening, not
required by the feature set and not claimed here.

## DIAG.3f — qualified sanitizer detector routes (fifth loop iteration)

DIAG.3f is fundamentally a sanitizer-lane slice: its acceptance is "every claimed error class has a
seeded subprocess detector test, symbolized site and nonzero failure," with partial-instrumentation
dependencies and tool absence made explicit and no route converted into a skip-pass. Detectors landed
through the DIAG.0 specimen harness (`crd-diag-harness`), consumed by
`tests/foundation/perf/test_diag_harness.cpp` (`[diag][harness][diag3f]`). Each specimen **self-reports
its route** via the `SANITIZER=` tag and the test judges purely on that tag — so a lane where a route is
absent yields InstrumentAbsent, never a false failure and never a skip-pass. `specimen_common.hpp` gained
an `#ifndef CRD_DIAG_SPECIMEN_SANITIZER` guard so a specimen can pre-declare its route unavailable even
when a sanitizer is linked.

- **heap-use-after-free** (`use_after_free_specimen.cpp`) — a temporal-safety class core AddressSanitizer
  catches on every ASan lane. **Proven on win-asan**: the child aborts with a symbolized
  `heap-use-after-free ... in main` report → SanitizerCaught. Without a sanitizer → InstrumentAbsent.
- **memory leak** (`leak_specimen.cpp`) — allocates, drops the only reference in a separate frame, then
  `__lsan_do_recoverable_leak_check()` + `abort()` turns LeakSanitizer's finding into a crash the harness
  classifies (LSan's normal at-exit path is a plain non-zero exit the harness would not treat as a catch).
  Qualified on linux-gcc-asan; the route is self-declared absent where no functional LSan exists — MSVC
  ships `<sanitizer/lsan_interface.h>` but has no working LeakSanitizer, so MSVC is excluded from the route.
- **stack-use-after-return** (`use_after_return_specimen.cpp`, the design's named "MSVC use-after-return")
  — gcc/clang ASan implement the fake-stack route and catch it (linux-gcc-asan). MSVC ASan does **not**:
  verified empirically that even with `/fsanitize-address-use-after-return` + `detect_stack_use_after_return`
  it flags the address-escape as a spurious `stack-buffer-underflow` (it aborts with the post-return read
  removed), so it is not a genuine UAR catch. The specimen therefore self-declares the route unavailable
  under MSVC (`SANITIZER=none` → InstrumentAbsent) — the acceptance's partial-instrumentation dependency
  made explicit, not a weakened check.

**Fiber coverage.** The full `crd-jobs-tests` suite — fiber create/switch/retire through
`sanitizer_fibers.hpp`'s `__asan_start/finish_switch_fiber` model — runs **107/107 clean under the real
win-asan preset**. (Building it under win-asan surfaced a real bug in the DIAG.1b `data_race_specimen`,
which had tagged itself `asan` under win-asan; its route needs ThreadSanitizer, so it now pre-tags the
route absent under any non-TSan build and reports InstrumentAbsent there — fixed, jobs back to 107/107.)

**Explicitly not claimed** (no qualified route in the pinned toolchain / no CI lane, which the acceptance
permits — a missing route remains unqualified): UBSan, an MSan/Memcheck initialized-memory closure,
use-after-scope, and scoped-static lifetime/ownership via clang-tidy
(`cppcoreguidelines-avoid-non-const-global-variables` is disabled and enabling it would be a repo-wide
config sweep, which is out of scope). None is converted into a pass.

`[diag3f]` is green on win-debug (all three InstrumentAbsent) and win-asan (UAF SanitizerCaught, UAR + leak
InstrumentAbsent by self-tag); full `crd-perf-tests` 414 assertions / 114 cases.
