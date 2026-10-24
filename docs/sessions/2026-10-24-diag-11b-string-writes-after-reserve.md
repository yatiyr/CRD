# DIAG.11b: String writes after a reserve at the GCC link, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [redaction class](2026-10-24-diag-12-redaction-class.md).

## CI read at the start

`gh run list --branch main --limit 15`: the completed runs since the last batch (40120050759, 40125506582) were
cancelled before any job ran. Run 40122902896 (head `a4500838`, still running) has two failed jobs, linux-gcc-release
(120961103848) and linux-gcc-shipping (120961103877). Both stop linking crd-perf-tests:

```
string.hpp:222:16: error: writing 1 byte into a region of size 0 [-Werror=stringop-overflow=]
  inlined from 'asset_text' at test_diag_flight_commands.cpp:93:19
  note: at offset 37 into destination object 'path' of size 32
```

The last run that finished these lanes, 40103197370 (head `6b214076`), linked it; the test is unchanged since. The
DIAG.12 additions to crd-perf-tests moved `asset_text` into another link-time unit.

## Cause and fix

`push_back` reserved, then wrote through `data()`. GCC followed `data()`'s inline branch with the heap-sized length of
a 36-character path. After `reserve(n)` with `n` above the inline capacity, the string is always on the heap, but
nothing in `push_back` stated it. `append` had a one-off guard for the same pattern. `resize` and the in-place
`assign` had the pattern with no guard. `assign` also wrote the terminator at `buf[23]` for a full inline string,
past the array, and then overwrote it in `set_size`.

`String::data_after_reserve(n)` returns `m_heap.data` for any `n` above the inline capacity. It asserts that the
string is on the heap. `push_back`, `resize`, `append` and the in-place `assign` write through it, and `append`'s
guard is deleted. `assign` now terminates a full inline string through `set_size`, as the other writers already did.

## Verification

- linux-gcc-shipping (mirror tree), `crd-perf-tests`: HEAD fails the link with the same diagnostic (offset 34, local
  path). With the fix it links clean. `crd-containers-tests` and the `diag assets`/`flight commands` cases pass.
- New case `String: push_back, assign and resize across the inline boundary` (22 to 23 to 24 characters). It passes
  on win-debug, linux-gcc-debug and linux-gcc-shipping.
- Teeth on win-debug: with `n >= kSsoCapacity` in the helper, the case fails with SIGSEGV. Restored, rebuilt, 122 of
  122 cases pass.
- Guards: ctest `^crd-no-|^crd-check` plus the two diagnostic registries, 15 of 15. Allman passes. clang-format
  finds 13 issues in `string.hpp`, the same count as HEAD, none on an added line. tidy-files passes both files.

Only crd-perf-tests and crd-containers-tests were linked locally. Hosted once pushed: linux-gcc-release,
linux-gcc-shipping and linux-gcc-relwithdebinfo build the whole tree with no `warning:` line.
