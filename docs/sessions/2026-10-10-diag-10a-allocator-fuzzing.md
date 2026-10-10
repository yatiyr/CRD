# DIAG.10a allocator operation-sequence fuzzing, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9c observer effect](2026-10-10-diag-9c-observer-effect.md).

## Goal

The first slice of DIAG.10a: the contract's "extend the existing bounded fuzz framework to allocator operation
sequences", with real production entry points, a reference oracle, the acceptance's seeded malformed lengths,
overflow, truncation and invalid generations, findings minimized or adopted and replayed in CI, and every fuzz
target registered with an owner. Committed locally, not pushed.

## What changed

- `tests/foundation/memory/fuzz/fuzz_allocator_ops.cpp` (new) with its `CMakeLists.txt` and committed corpus:
  `crd-fuzz-memory-allocators`, built by `crd_fuzz_target()` (the replay executable and the
  `crd-fuzz-memory-allocators-corpus` CTest on every lane; the libFuzzer executable under `CRD_ENABLE_FUZZER`). The
  first input byte picks one of eleven subjects (TLSF, pool, linear, stack, growable linear, growable TLSF, growable
  pool, offset allocator, epoch ring, `DiagnosticAllocator` over TLSF, `SlotMap<u64>`) and its configuration; the
  rest is at most 2,048 operations. A cursor reads zero past the end, so a stream cut mid-operation is a valid
  shorter stream. The memory module's test glob excludes `fuzz/`, and the module declares `support/fuzz` among its
  test directories.
- Contract-valid inputs only for raw allocators: allocation through `try_allocate`; frees name live blocks; a TLSF
  or growable TLSF reallocate runs only after a probe shows an allocation of the new size succeeds now (its fallback
  is the fatal allocate); ring fences increase by one and never exceed K closed, unretired epochs; stack and linear
  rewinds name a marker no older than the top. Malformed requests are inside the contract and generated on purpose:
  size 0, a raw 32-bit length, `SIZE_MAX - k`, past-the-arena sizes, the top-bit alignment and alignment 24.
- Hostile operations go only to the checked layers: the `DiagnosticAllocator` gets a double free of the last free
  (reported `DoubleFree` while quarantined, `UnknownPointer` once evicted), foreign and interior frees
  (`UnknownPointer`, refused), a byte written inside the rear redzone (`RedzoneOverrun` from `check_all_redzones`,
  then repaired) and a byte written into a quarantined block (`QuarantineUseAfterFree`, then repaired), each checked
  to be the only violation raised and against the pointer it names; record-table saturation is checked to give an
  untracked passthrough block reported once. The `SlotMap` gets stale handles, double erases, handles to reused
  slots and forged index and generation pairs.
- The model: a live table of pointer, size, pattern and tag per block. After every operation no live block
  overlaps another, each is aligned and still holds its pattern, `owns()` and `allocation_size()` agree, the exact
  capacity models predict success and refusal (a pool succeeds iff a slot is free; a bump arena iff its aligned
  offset plus size fits, and `used()` lands on the model's offset), `validate_structure()` passes, and the offset
  allocator's bins hold exactly its free storage. At the end everything is released, `bytes_in_use` is zero and the
  allocation and deallocation counts balance (where statistics are tracked), the bump allocators' `bytes_in_use`
  equals their use throughout, the offset span is one free region again and answers a whole-span request as a fresh
  allocator does, the ring is empty, and the parent gets every byte back. Each input runs twice and must produce
  the same outcome trace; the parent is page aligned so placement does not depend on where the system allocator put
  an arena.
- `tests/support/fuzz`: `BudgetAllocator::try_allocate` refused a request by summing it with the live total, which
  wraps for a near-`SIZE_MAX` size (a growable allocator asking for a chunk past every limit) and would hand back a
  small block; it now compares without a sum and refuses a non-power-of-two or over-budget alignment. Its payload is
  found by pointer arithmetic, not an integer turned back into a pointer. The replay driver calls
  `set_assert_headless(true)` and, on Windows, turns off the abort dialog before the first input: the first replay
  of the new seeds hung on an engine assertion's dialog until the timeout.
- `scripts/fuzz.py` names `perf-bundle` (it had no entry since DIAG.5d) and `memory-allocators`.
  `test-repository-tools.py` gains `test_every_registered_fuzz_target_has_a_driver_entry`: every
  `crd_fuzz_target()` in `tests/` has exactly one entry naming its corpus and build-relative executable, and no
  entry is stale.

## Findings, each fixed and adopted as a corpus input

Found by the oracles on the written seeds (win-debug), before any engine change:

1. `TlsfAllocator::try_allocate` asserted `is_pow2(alignment)` where every other `try_allocate` refuses: an alignment
   of 24 stopped the process. It now returns null (seed `tlsf-malformed-lengths`).
2. TLSF in-place reallocation (shrink with a split, grow into a free neighbour) changed the block's size without
   changing `bytes_in_use`, so the statistic drifted by every in-place resize and never returned to zero. A
   byte-only `stats_resize` now follows the block; the counts and the real-time sentinel are untouched, since the
   allocation is the same one (seeds `tlsf-alloc-free-realloc`, `tlsf-truncated-mid-op`).
3. The linear, stack and growable linear allocators charged `size` on allocation but released the consumed offset on
   reset, so alignment padding drove `bytes_in_use` past zero. They now charge what they consume, and `bytes_in_use`
   equals `used()` (`bytes_used()` for the growable one) (seeds `linear-padding-rewind`, `stack-markers`,
   `growable-linear-oversize`).
4. The `DiagnosticAllocator` sent any pointer without a record to its backing allocator whenever the backing range
   check accepted it. An interior pointer of a live block reached `TlsfAllocator::deallocate` (its own check
   asserted), and a double free after quarantine eviction freed a free TLSF block (an access violation). Only a
   live untracked passthrough block goes through now (a new count); anything else is `UnknownPointer` and refused
   (seeds `diag-double-free-overrun-uaf`, `diag-evicted-double-free-interior`).
5. Its `MetadataSaturation` report named the redzoned block it was about to discard instead of the passthrough
   block the caller receives (seed `diag-saturation-passthrough`).

Found by libFuzzer (`linux-clang-fuzz`, clang 18 ASan+UBSan, 4 workers):

6. A TLSF pool whose capacity is not a multiple of 16 put its end sentinel, and every block later split off the
   tail, at a misaligned address: UBSan "member access within misaligned address" in `init_pool` for a 16,556-byte
   pool. `init_pool` now rounds the capacity down to the 16-byte block granularity (the odd tail is never used,
   walked or poisoned); this also covers growable TLSF chunks. Adopted as `2f74360a...`.
7. `OffsetAllocator::reset()` reset the bins and the free-node stack but not the nodes, so the node reused for the
   whole span kept the neighbour links of its previous region: freeing a whole-span allocation after a reset
   coalesced with regions that no longer existed, and a 4,096-byte span reported a free region of 4,441 bytes. A
   handle issued before the reset also still looked issued. `reset()` now returns every node to its initial state.
   Three artifacts of this class adopted (`01bf0567...`, `1425b453...`, `ab9f79d2...`).

Two later libFuzzer reports were the harness's own. First, its oracle overreached: a drained offset allocator could
not hand out its whole span. The allocator rounds a request up to its size bin, so a span whose capacity no bin represents exactly
(462,848 bytes) cannot be handed out whole even when fresh; that is the design's bounded internal fragmentation. The
oracle now asks a fresh allocator of the same span the same question and requires the same answer, and the input is
adopted as coverage (`91152fe2...`). The artifacts were already 16 to 133 bytes and no longer crash after the fixes,
so they were adopted as found rather than minimized. Second, while an untracked saturation passthrough block is
live, the `DiagnosticAllocator` cannot tell a freed-and-evicted pointer from it, so the harness's evicted double free
reached the backing TLSF (its double-free and ownership assertions fired). That is the saturation limit, not a defect:
the harness no longer issues that operation in that state, the decorator's header now states the limit, and the
smallest of the four reports (`7d961f94...`, 42 bytes) is adopted as coverage of the state.

libFuzzer rounds (`scripts/fuzz.py run`, 4 workers, the documented limits; RSS is summed over the workers):

| Round | Wall | Executions | Working corpus | Artifacts | Outcome |
|---|---|---|---|---|---|
| 1 | 2.6 s | 441 | 17 to 125 | 4 | findings 6 and 7 |
| 2 | 1.1 s | 360 | 21 to 156 | 4 | the whole-span oracle overreach |
| 3 | 45.6 s | 11,381 | 22 to 866 | 4 | the passthrough precondition gap |
| 4 | 586.8 s | 256,345 (424/s) | 23 to 3,181 | 0 | clean; peak RSS 2,038 MiB |

The committed corpus is the 17 written streams and the 6 adopted inputs (23); the working corpora's coverage units
were not merged, so the replay set stays the deterministic seeds and the findings.

## Teeth

Twelve engine breaks, each built and observed failing on its seeds, then restored, rebuilt and replayed green (the
teeth script restores by rewriting the file, so the restored build recompiles):

- TLSF `try_allocate` asserting on a non-power-of-two alignment: `tlsf-malformed-lengths` stops on the assertion;
- TLSF in-place resize without `stats_resize`: 2 seeds fail `bytes_in_use == 0`;
- the linear, stack and growable linear allocators charging `size` only: 1 seed each fails `bytes_in_use == used()`;
- the `DiagnosticAllocator` sending unknown pointers to the backing heap: the interior free asserts in TLSF and the
  evicted double free is an access violation;
- the saturation report naming the discarded block: `diag-saturation-passthrough` fails;
- `SlotMap::erase` not bumping the generation: an old handle equals a new one on slot reuse;
- the ring's retire excluding its own fence: the ring's K-in-flight assertion fires;
- the offset allocator never coalescing with its next neighbour: the drained span is not one region;
- `OffsetAllocator::reset()` as before the fix (the file at HEAD): the three adopted artifacts fail;
- `init_pool` without the rounding: the adopted UBSan input fails on `linux-clang-fuzz` (MSVC has no UBSan, so the
  Windows lanes replay it clean either way; the hosted `linux-gcc-asan` lane runs UBSan without recovery).

And one registry break: `fuzz.py` without its `perf-bundle` entry fails the new registry test.

## Verification

- win-debug: the whole tree built after the engine changes; the full ctest ran once: 7,520 of 7,521 passed, the
  one failure `crd-simd-emission-check`, which needs `dumpbin` on PATH and passes inside vcvars. After the last
  edits: `crd-fuzz-memory-allocators` replays 23 inputs; crd-memory-tests 162 cases; the 14 `crd-no-*`,
  `crd-check*` and `crd-fuzz-*` CTests pass (every fuzz corpus replay, the bundle one included).
- win-shipping and win-clang-cl-shipping (reconfigured; both targets rebuilt on the final source, clang-cl linked
  first time): the replay 23 inputs, crd-memory-tests 161 cases.
- win-asan (inside vcvars): the replay 23 inputs, crd-memory-tests 162 cases; the only sanitizer reports are the
  existing DIAG.3b poison and container-overflow specimens', which run as child processes on purpose.
- WSL, one lane at a time, each reconfigured by the build: linux-gcc-debug, linux-gcc-asan (UBSan without
  recovery) and linux-clang-tsan (`setarch -R`): the replay 23 inputs, crd-memory-tests 162 cases and the
  `crd-fuzz-memory-allocators-corpus` CTest pass on each, with no sanitizer report.
- `linux-clang-fuzz` (new tree under `~/cerid-build`): the replay and libFuzzer executables; the four rounds above.
- Strict tidy is clean on all 10 changed C++ files (the touched harness header's integer-to-pointer casts became
  pointer arithmetic, `LLVMFuzzerTestOneInput` carries a NOLINT for its ABI name, and `ViolationKind` is now
  `u8`). clang-format differs on changed lines only in the repository's hand alignment, include grouping and one
  short accessor it would put on one line. The Allman check passes.
- test-repository-tools 65 tests (3 skipped), test-dev-workflow, check-master-plan and check-repository pass.

## Remaining in DIAG.10a

- Capture, policy, flight-record and run-record readers; command decoding; resource reload; the compiled and
  reference CEIR comparison; the virtual-memory and streaming allocators; reinvestigating the unknown-origin CEIR and
  CKIR artifacts on the strengthened harness.
- The hosted run, as the row lists it.
