# DIAG.3a allocator boundary sweep, 2026-10-06

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.3a](../ROADMAP.md#slice-diag.3a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-3a). Rules: [AGENTS](../../AGENTS.md).

## What the sweep checks

`tests/foundation/memory/test_allocator_boundaries.cpp` runs one contract against all 12 `IAllocator`s: malloc, linear,
stack, pool, growable linear, growable pool, TLSF, growable TLSF, virtual memory, the thread-safe wrapper, a streaming
category view and the diagnostic decorator. For each one:
- `try_allocate` returns nullptr for `SIZE_MAX`, `SIZE_MAX - 8`, `SIZE_MAX - 4095` and `SIZE_MAX / 2 + 1`, and for an
  alignment of 2^62, instead of wrapping into a small allocation;
- the allocator stays usable afterwards, and an earlier allocation keeps its bytes;
- a foreign pointer is never owned, and the allocator's own block is owned.

Two allocators document a fixed `owns()` answer, and the test records it rather than changing them:
`MallocAllocator` answers true for every non-null pointer, and `StreamingCategoryAllocator` answers false (ownership
belongs to the `StreamingAllocator`).

## What it found

The first Windows run failed on 8 of the 12 allocators, and Linux found a ninth:

| Allocator | Defect | Fix |
| --- | --- | --- |
| TLSF | `size + kAlignSize` and the alignment gap wrapped, so `SIZE_MAX` returned a valid block | `checked_align_up`/`checked_add`; refuse anything larger than the pool |
| Growable TLSF | The chunk size `size + size/32 + alignment + ...` wrapped into a small chunk | bound size and alignment by the chunk cap first, then `checked_add` |
| Growable linear | Aligning the cursor and `off + padding + size` wrapped back inside the chunk; the chunk request `need + header` could too | checked align and add; refuse instead of growing; ask the parent with `try_allocate`, so a fatal-on-exhaustion parent cannot abort a refusal |
| Pool, growable pool | `try_allocate` inherited `allocate`, which asserts `size <= slot_size` | `try_allocate` overrides that refuse a request no slot can hold |
| Thread-safe wrapper | Inherited the TLSF wrap | fixed with TLSF |
| Streaming category | The budget check `cur + size <= limit` wrapped | `cur <= limit - size`; a request larger than the whole budget refuses without evicting anything |
| Malloc (POSIX only) | `aligned_alloc` needs a size rounded up to the alignment; the unchecked `align_up` wrapped, so `SIZE_MAX` returned a valid block on gcc and clang | `checked_align_up`; refuse on overflow |
| Diagnostic decorator | `align_up(redzone, alignment)` truncated to 0 in a `u32`, and `front + size + redzone` wrapped; no non-fatal path | checked sizing; a `try_allocate` that asks the backing allocator with `try_allocate`. Both paths share `allocate_from`, so a sampled allocation stack now has one more internal frame |

## Evidence

- win-debug: all 12 sections pass, and the whole memory suite passes (141 test cases, 820,357 assertions).
- Teeth: with the old TLSF and growable-linear sources put back, TLSF, growable TLSF and the thread-safe wrapper fail,
  and growable linear traps. After restoring the fixes, the rebuilt lane passes again.
- win-shipping and win-clang-cl-shipping: the memory suite passes (140 test cases; the 141st, "MemoryStats tracks
  alloc/dealloc in debug builds", is registered only in debug builds).
- A full win-debug `all` build is clean (three widely included headers changed), and on win-asan the memory suite
  passes; MSVC's AddressSanitizer reads the same default-options hook.
- Strict `tidy-files.py` is clean on all 10 changed sources, and `clang-format --dry-run --Werror` is clean.
- Linux, on the WSL reference host: the first run (clang TSan and gcc debug) failed only the malloc section, on both.
  Under TSan it also aborted with `allocation-size-too-big`. Sanitizer allocators abort on a request above their cap
  (1 TB here) unless `allocator_may_return_null=1`, even when the request is well formed (for example a 2^62
  alignment). The new
  `tests/foundation/memory/sanitizer_options.cpp` sets that default for the memory test executable only. It restores
  libc's null return, which is the contract under test, and leaves every error check on. After the fix, clang TSan,
  gcc debug and gcc ASan each pass all 141 memory test cases.

## Part 2: zero size, exhaustion, reallocate failure and the fatal paths

Three more cases in the same test file, plus a zero-size check in the shared sweep:
- **Zero size.** `try_allocate(0)` must return nullptr on all 12. The default `IAllocator::try_allocate` called
  `allocate`, whose linear-style precondition asserts `size > 0`. It now refuses a zero size and a non-power-of-two
  alignment first.
- **Exhaustion.** Linear, stack, pool, TLSF and virtual memory are filled with `try_allocate` until they refuse. The
  refusal must come, the first block keeps its bytes, and for pool and TLSF a freed block is reusable afterwards.
- **Reallocate failure.** For the allocators whose `allocate` returns nullptr (linear, stack, growable linear),
  `reallocate` to an impossible size must return nullptr and leave the old block untouched, like C `realloc`. The
  default `IAllocator::reallocate` copied into the null result and then freed the old block. It now returns nullptr
  first.
- **Fatal paths.** TLSF `reallocate(p, 64, SIZE_MAX)` rounded the size up to 0 and took the shrink-in-place branch,
  splitting the live block. It now uses `checked_align_up` and reaches `CRD_FATAL`. A fatal cannot run in-process, so
  the new `crd-diag-alloc-fatal-specimen` runs it as a bounded child through the DIAG.0 harness, as the observer-swap
  control does. Its assert handler turns the expected fatal into exit 42. Three modes cover the TLSF overflow, a TLSF
  request too large for the pool, and a growable-TLSF request too large for any chunk. The memory module now lists
  `support/diag` among its test directories, as core, perf and jobs do.

Teeth, with `allocator.cpp` and `tlsf_allocator.cpp` put back to their pre-batch versions:
- the zero-size check asserts in `LinearAllocator::allocate`;
- the linear reallocate case dies with an access violation (the copy into nullptr);
- `tlsf-realloc-overflow` exits 0 because `reallocate` returned, so the fatal case fails.

After the fixes were restored and the lane rebuilt, win-debug passes all 11 contract cases and the whole memory suite
(144 test cases). So do win-asan, and on WSL clang TSan, gcc debug and gcc ASan (144 each). win-shipping and
win-clang-cl-shipping pass 143, without the debug-only case. A full win-debug `all` build is clean, and strict tidy and
`clang-format --dry-run --Werror` are clean on every changed source.

## Part 3: the ring and offset allocators

Neither is an `IAllocator`, so each gets its own case in the same file.

`OffsetAllocator` (the GPU sub-allocation kernel, 32-bit offsets):
- **Node pool.** The header promises an invalid allocation when the node pool is exhausted. Instead, the remainder
  split called `insert_node_into_bin`, which only asserts when no node is left. With asserts compiled out, it reads
  `m_free_nodes[0xFFFFFFFF]`. `allocate` now refuses before changing anything when a split would need a node and none
  is left.
- **Foreign handles.** `free` indexed `m_nodes[metadata]` with no range check. Its double-free check was a debug
  assert that then carried on and corrupted the bins. `free` now refuses, in every build, any handle whose node index
  is out of range, whose node is not live, or whose offset lies outside that node's region. Where asserts are on, it
  asserts first. A stale handle whose node was reissued at the same offset still looks valid; telling those apart
  needs generations, which is DIAG.3e.
- **Bad arguments.** A non-power-of-two alignment now returns an invalid allocation instead of asserting.

`RingAllocator`: `try_claim` with a non-power-of-two alignment, or one above the buffer's cache-line alignment, now
returns nullptr instead of asserting. Exhaustion and recovery were already correct: the ring stays full until its
epoch retires, then reuses the space. The test now covers both.

The tests count asserts through a scoped assert handler that lets execution continue, so each refusal path runs
in-process. They expect one assert per refusal where asserts are compiled in, and none where they are not.

Teeth, with both sources put back to their pre-batch versions: three of the cases break on an assert (the alignment of
3, the node pool, and the ring alignment). The foreign-handle case dies with an access violation at `m_nodes[9999]`.

With the fixes restored and the lane rebuilt, the memory suite passes 146 test cases on win-debug and win-asan, and on
WSL clang TSan, gcc debug and gcc ASan. win-shipping and win-clang-cl-shipping, where asserts are compiled out, pass
145. A full win-debug `all` build is clean, and strict tidy and `clang-format --dry-run --Werror` are clean.

## Still open in DIAG.3a

- Wrong-allocator free across the `IAllocator` family, parent/child arena destruction, external buffers and partial
  construction.
