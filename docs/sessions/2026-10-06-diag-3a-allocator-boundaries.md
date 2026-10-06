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

## Part 4: wrong-allocator free, parent/child destruction, external buffers and partial construction

Seven more cases at the end of `test_allocator_boundaries.cpp`, plus seven modes in `crd-diag-alloc-fatal-specimen`.

**Wrong-allocator free.** Two instances of every `IAllocator` give every ordered pair, including two allocators of the
same type. Each receiver frees a block from each source. The receiver must not write into the block, must keep its
own blocks, must never hand the block out again, and its structural walkers must stay clean. The source then frees the
block normally. `deallocate(nullptr)` is a no-op with no assert on all 12. What each receiver does:
- pool, growable pool, TLSF, growable TLSF, virtual memory, the thread-safe wrapper and the streaming category view
  refuse the pointer in every build, after one assert where asserts are compiled in;
- linear, stack and growable linear ignore it, because their `deallocate` is a no-op for every pointer;
- the diagnostic decorator reports an `UnknownPointer` violation;
- `MallocAllocator` is left out: `owns()` is true for every pointer, so it cannot tell, and the C runtime would get the
  foreign block. Wrap it in the diagnostic decorator to diagnose such frees.

Defects found and fixed:
- `PoolAllocator` and `GrowablePoolAllocator` asserted `owns(p)` and then linked the pointer into the free list. In a
  release build, the next `allocate` returned the other allocator's memory. Both now refuse. The growable pool's
  `owns()` scanned every page and accepted interior pointers. Its page table is now kept sorted, so `owns()` is a
  binary search, and it checks the slot boundary as the fixed pool does. Its `slot stride * slots_per_page` is now
  checked.
- `TlsfAllocator::deallocate` asserted and then read a header in front of the foreign pointer and coalesced it. It now
  refuses anything that cannot be a payload (outside the first payload and the end sentinel, or not 16-byte aligned)
  and an immediate double free (the header is already free). An interior pointer that happens to land on a valid
  header, and a stale free after the block was reused, are still not detectable here. They need the decorator or the
  generations of DIAG.3e.

**Parent/child destruction.** Every child is built on a diagnostic decorator over a TLSF heap. After the child is
destroyed, the decorator must hold no live block, must have seen no unknown or double free, and must find no
redzone damage, and the heap's walker must be clean. The children: linear, stack, pool, TLSF, ring, offset (two node
arrays), growable linear across six chunks plus an oversized one, growable pool across six pages (the page table grows
from 4 to 8 entries), growable TLSF across several chunks, a TLSF, linear and stack chain torn down innermost first,
a diagnostic decorator whose metadata comes from the parent, and a growable pool that is move-constructed and then
move-assigned over a pool with its own page. No defect was found. Mutations show the case bites: a ring that does not
free its buffer leaves one live block, and a move that keeps the source's parent frees the pages twice.

**External buffers.** Linear, stack, pool and TLSF arenas over a caller's buffer with 64 guard bytes on each side are
filled to exhaustion. Every block lies inside the buffer, the guards are untouched, and after the arena is destroyed
the buffer is still its owner's single live block and can be written in full. Under AddressSanitizer that write also
proves that linear and stack unpoisoned what they had poisoned. No defect was found.

**Partial construction.** Two kinds:
- An arena whose parent refuses its backing memory. An exhausted arena returns nullptr from `allocate`, and the linear,
  stack, pool, ring and offset constructors then built themselves on null: they handed out offsets from address 0, or
  wrote the free list through null. TLSF asserted and then did the same in release. Each owning constructor now calls
  `CRD_FATAL("<arena>: parent refused ...")`, the out-of-memory policy of the interface, in every build. The specimen
  proves all six in a bounded child, including the offset allocator whose node array is granted and whose free-node
  array is refused. `GrowablePoolAllocator` asked its parent with `allocate`, so a refusing parent left it writing
  through null. It now uses `try_allocate`: `try_allocate` returns nullptr and the pool stays usable (a freed slot is
  reused, and it grows again once the parent grants), and `allocate` reaches its own out-of-memory fatal (the seventh
  specimen mode).
- Objects constructed into allocator memory. `construct` placed the object at nullptr when the allocator refused, and
  `allocate_array` computed `sizeof(T) * count` unchecked, so a huge count allocated too little. Both now return
  nullptr. A constructor that throws now returns the storage, and in `construct_array` the objects already built are
  destroyed in reverse order first. The `try`/`catch` is compiled only where exceptions are enabled.

Teeth: with the engine sources put back to their pre-batch versions, the pool adopts a malloc block (the block's bytes
change, the walker fails and the next `allocate` returns it); the TLSF double free crashes; the growable pool trips its
null-page assert; `construct` on a refused allocation crashes; and the specimen's linear and stack modes exit 0 (the
arena was built on null and returned) while the pool mode dies with an access violation. A second mutation run took out
only the growable pool's refusal: it then adopted the foreign block too. After restoring and rebuilding win-debug, the
memory suite passes again.

Evidence: the memory suite passes 153 test cases on win-debug and win-asan, and on WSL clang TSan, gcc debug and gcc
ASan. win-shipping and win-clang-cl-shipping pass 152, without the debug-only case. A full win-debug `all` build is
clean, because `construct.hpp` is widely included. The consumers' suites also pass on win-debug: scene (the archetype
chunks sit on the growable pool), stress, CEIR, hesap-opt and scene-render. Strict tidy is clean on every changed source. `clang-format
--dry-run --Werror` is clean on the new code: the touched files report only the violations they already had.

## Still open in DIAG.3a

- Nothing local. The row waits for a hosted run that shows the memory suite green on every lane, including the
  complete tier's `linux-clang-tsan`.
