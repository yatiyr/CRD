# DIAG.3b pool and arena poisoning, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.3b](../ROADMAP.md#slice-diag.3b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-3b). Rules: [AGENTS](../../AGENTS.md).

## Goal

Close the three items the 2026-10-06 audit left on DIAG.3b: the intentional use-after-reset negative control, pool
free-list poisoning, and the Linux ASan run. Linear and stack arenas already poisoned their unhanded ranges.

## What changed

- `crd/memory/asan_poison.hpp` documents ASan's 8-byte shadow granularity and adds `asan_is_poisoned()`, which only
  queries the shadow (false without ASan), so tests can assert the poisoned state without touching the memory.
- `PoolAllocator` and `GrowablePoolAllocator` poison a free slot whole, its free-list header included. The allocator
  reads a free slot's link just in time (`read_free_link`: unpoison the 8-byte header, read it, poison it again unless
  it was live). `validate_structure()` walks the list the same way. A live slot is unpoisoned whole. Pages and adopted
  buffers are unpoisoned, exactly their own range, before they go back to the parent or the caller.
- `GrowableLinearAllocator` now follows `LinearAllocator`: each chunk's header stays addressable, the rest is poisoned
  until an allocation unpoisons exactly its bytes, `reset()` poisons every chunk again, and the destructor unpoisons each
  chunk before returning it.
- New negative control `crd-diag-allocator-poison-specimen` (`tests/support/diag/specimens/allocator_poison_specimen.cpp`).
  Each of 15 modes builds a real allocator and performs one intentional bad read: linear reset, `LinearScope` rewind,
  one-byte overrun of a 13-byte and of a 1-byte slice, underrun into the padding before a 64-aligned slice, a child
  arena's slice read one byte past its end after the child is destroyed, a popped stack frame, growable-linear reset,
  freed pool slot (its header byte), pool over- and underrun into a freed neighbour, a freed 4099-byte slot packed at
  an 8-byte stride, an overrun from such a slot, and the growable-pool freed-slot and overrun cases. A death callback
  exits 42 only when ASan's report is `use-after-poison`, so an unrelated crash or a heap-redzone report cannot pass.
  Without ASan the specimen runs the allocator steps, skips the read and reports `InstrumentAbsent`.
- New `tests/foundation/memory/test_allocator_asan.cpp`: the in-process positives (pool, odd 4099-byte and large
  64 KiB 256-aligned slots, growable pool across pages, one-byte and over-aligned linear slices, rewind and reset reuse,
  stack frames, growable linear across chunks, nested arenas three deep with a pool grandchild) assert the shadow state
  under ASan and that the helpers are no-ops without it. The last case runs the 15 specimen modes.

## Decisions

- **A pool's logical allocation is its slot.** The first version unpoisoned only the requested size. The win-asan
  stress suite (`test_allocators_v5_stress.cpp`) then faulted: it requests a random size and fills the whole slot.
  `IAllocator::allocation_size()` reports the slot size as the usable size and the pool keeps no side storage for the
  request, so poisoning the tail would contradict the interface. The live slot is unpoisoned whole, and the existing
  stress test is unchanged. Pool over- and underruns are declared at the slot boundary into a free neighbour. An
  overrun into a live neighbour, and immediate reuse of the same slot, are raw-pointer limits for DIAG.3e generations.
- **The free-list header is poisoned too.** Leaving it addressable would miss a stale read of a freed slot's first
  8 bytes and a one-byte overrun into a free neighbour. The just-in-time link read keeps the bookkeeping usable.
- **A double free stays visible to the structural walker.** `deallocate` unpoisons the header before writing the link.
  For a live slot this is a no-op. For a double free it keeps the existing DIAG.3d behaviour (the walker reports the
  cyclic list, `test_allocator_contracts.cpp`) instead of an ASan report inside the allocator.
- **Granularity.** ASan names a fault in a partly addressable granule after the next granule's shadow. A slice
  followed by poisoned padding reports `use-after-poison`. A slice packed against a live neighbour still faults but
  reports `unknown-crash`, and the last one in a heap block reports `heap-buffer-overflow`. The first specimen version
  read past a 4099-byte slot that was the pool's last (the free list hands out the highest slot first) and got
  `heap-buffer-overflow`, exit 43. The odd-slot modes now use two adjacent slots with the upper one freed.

## Evidence

- win-asan: the memory suite passes, 158 test cases and 824,228 assertions, and all 15 specimen modes end in
  `use-after-poison` (exit 42). Consumers of the changed allocators also pass under win-asan: `crd-stress-tests`
  (18 cases), `crd-scene-tests` (280 cases; archetype chunks use the growable pool) and `crd-ceir-tests` (534 cases;
  the CEIR context, diagnostics and pass manager use the growable linear arena).
- Teeth on win-asan, with the free-slot poison removed from both pools and the reset poison removed from both linear
  arenas: 29 assertions fail. Nine specimen modes exit 0 (`linear-reset`, `glinear-reset`, `pool-freed-slot`,
  `pool-overrun`, `pool-underrun`, `pool-odd-freed`, `pool-odd-overrun`, `gpool-freed-slot`, `gpool-overrun`), and the
  in-process shadow checks fail. An earlier tooth on the first version also bit. After restoring, touching the sources
  and rebuilding win-asan, the suite passes again.
- win-debug: memory suite 158 cases, 824,228 assertions; `crd-stress-tests` 18 cases; the specimen reports
  `InstrumentAbsent` in all 15 modes. win-shipping and win-clang-cl-shipping: 157 cases (the debug-only stats case is
  not registered).
- WSL reference host, one build at a time: linux-gcc-asan passes 158 cases and its log holds 15 `use-after-poison`
  reports, one per mode. linux-gcc-debug and linux-clang-tsan pass 158 cases (`InstrumentAbsent`; a TSan build tags
  the ASan-only class absent).
- Strict `tidy-files.py` is clean on the six changed C++ files. Two findings were fixed: the `CRD_MEM_ASAN` defines
  now carry the repository's macro NOLINT, and the test's ASan flag is an `#if` constant. `clang-format --dry-run
  --Werror` is clean on the new files and adds no violation to the touched allocator sources (their older aligned
  assignments are unchanged). `check-allman-braces.py` passes, and the `crd-no-*`/`crd-check*` CTest guards pass.

## Remaining on DIAG.3b

- Container live ranges: annotate `crd::containers::Array` (and the other contiguous engine containers) with
  `__sanitizer_annotate_contiguous_container`, so unused capacity is poisoned, plus a specimen mode that reads past
  `size()` inside `capacity()`. `array.hpp` is included everywhere, so this needs a win-debug `all` build.
- The other CPU allocators named in the contract: TLSF and growable TLSF free blocks (and the streaming heap built on
  them) and the ring allocator's retired space. Virtual memory already poisons its tail.
- Hosted: the first change-tier run after the push must show the memory suite green on win-asan and linux-gcc-asan,
  with `allocator asan: every poisoned access is a use-after-poison report` passing (15 modes caught), and on the
  non-ASan lanes the same case passing as `InstrumentAbsent`.
