# DIAG.12: allocation and free sites start at the application's call, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [String writes after a reserve](2026-10-24-diag-11b-string-writes-after-reserve.md).

## CI read

Run 40122902896 (head `a4500838`, still running) also failed win-clang-cl-shipping (120961103886). The case `diagnostic
host: the application's write after free names both sites, and the repaired one runs clean` read the same program
counter for the allocation site and the free site (`CHECK( stale.value_a != stale.value_b )`, both `0x7ff6bea087ae`).

## Cause

`DiagnosticAllocator` skipped a fixed number of its own frames before it kept a stack. That count assumed every
internal function was its own frame:
- `allocate` reached `allocate_from` through `allocate_tagged`, so on win-debug the first allocation frame was inside
  `allocate_tagged`, not the caller.
- On clang-cl thin-LTO the internal functions were inlined, so the skip went past the application's frame. Both
  sites became the specimen's one call in `main`.

The specimen case only asked that the two sites differ, so the debug lanes passed with a wrong allocation site.

## Fix

- Each public entry is `CRD_NOINLINE`: `allocate`, `try_allocate`, `allocate_tagged` and `deallocate`. Each passes its
  own return address (`_ReturnAddress()`, or `__builtin_return_address(0)`) down.
- `capture_pcs` captures from its own frame, finds that address and keeps the frames from it outward. Inlining and tail
  calls below the entry no longer change the result. If the capture misses the address, the address is kept alone.
- `allocate` now calls `allocate_from` directly.

## Verification

- New case `diag-alloc: every entry's stack starts at the application's call`. Each entry is called from its own
  line of one helper, and the helper from one line of another. The first frames must all differ and the second frames
  must all match.
- At HEAD's allocator the case fails on win-debug: the second frames differ, and two first frames are equal.
- At HEAD's allocator on win-clang-cl-shipping, both the new case and the hosted specimen case fail. That reproduces
  the hosted failure here.
- With the fix, both cases pass. So do the 17 application and `diag-alloc` CTests and crd-memory-tests `[diag]`
  (54 cases), on win-debug, win-clang-cl-shipping, linux-gcc-debug and linux-gcc-shipping. The two Linux builds print
  no `warning:` line.
- Guards: 15 of 15. Allman passes. clang-format finds no more issues than HEAD in any touched file. tidy-files
  passes all three files.

Hosted once pushed: the use-after-free application case and the `diag-alloc` cases pass on every lane, the
clang-cl and sanitizer lanes included.
