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

## Still open in DIAG.3a

- Zero-size: `IAllocator` says `try_allocate(0)` returns nullptr, but linear-style `allocate` asserts `size > 0`, so the
  default `try_allocate` cannot be swept yet.
- Per-allocator exhaustion and reallocation failure that keeps the previous allocation.
- Wrong-allocator free, parent/child arena destruction, external buffers and partial construction.
- `RingAllocator` and `OffsetAllocator`: neither is an `IAllocator`, so each needs its own boundary checks.
