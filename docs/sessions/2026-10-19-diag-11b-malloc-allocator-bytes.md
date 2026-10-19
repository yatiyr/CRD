# DIAG.11b follow-up (p): the default allocator's exact sizes

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b) (DIAG.3a's wrong-allocator policy and the DIAG.11b
> log), [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Found by: [allocator statistics on request](2026-10-19-diag-11b-allocator-statistics-on-request.md#the-defect-found-on-the-way-mallocallocators-bytes).

## CI read at the start

`gh run list --branch main --limit 15` at 10:30 local (2026-10-19). No completed run of a pushed commit had a failure
without a fix already on main:

- [39207693168](https://github.com/yatiyr/CRD/actions/runs/39207693168) (`a38224df`, complete tier): in progress,
  21 jobs running or queued; the preflight, both repository jobs and windows-native finished green.
- [39216534845](https://github.com/yatiyr/CRD/actions/runs/39216534845) (`86f4b900`): pending behind it.
- 39201708392, 39200628155, 39195570971, 39195138375: cancelled while pending, no job started.
- The nightly runs 39108972918 and 39013990584 and push run 39129062998 failed on causes already fixed on main
  (5cbc4c4e, the virtual memory arena's unpoisoning, d88d68ee, 471109f6), as the earlier sessions record.

The previous batch's first follow-up, a complete-tier run for `86f4b900`'s win-shipping-profile clause, waits: a
complete-tier run (39207693168) is in progress, and the rule allows a dispatch only when none is queued or running.
So this batch took the next one, (p).

## What was wrong

`MallocAllocator`, the process-wide `default_allocator()`, counted the request at allocation and
`allocation_size()` at free, and `allocation_size()` asked the platform:

- Windows: 0. The CRT cannot size an `_aligned_malloc` block without the alignment it was made with, and the
  allocator kept none, so a free never lowered `bytes_in_use`. The header comment claimed `_msize`.
- glibc: `malloc_usable_size`, at least the request and usually more, so each free lowered the count by more than its
  allocation raised it, and it wrapped.

The previous batch's uncommitted probe read 1,100 bytes allocated and freed as 1,100 still in use on Windows and as
2^64 - 52 on glibc; the sandbox's `memory.allocators` showed the default allocator at 503,764,378 bytes in use, its
lifetime total.

## The repair

`engine/foundation/memory/src/allocators/malloc_allocator.cpp` now makes every block one C-library `malloc` and aligns
inside it, with a 16-byte header just below the pointer it hands out:

```
raw              user - 16                user                       user + size          raw + total
| leading pad    | header: size, layout  | the caller's size bytes  | trailing pad         |
```

- The header holds the request and a layout word: the offset from `raw` to `user`, with log2 of the alignment in its
  top byte. `allocation_size()` returns the request on every platform, and the statistics count the same bytes at
  allocation and at free.
- `malloc` guarantees `alignof(std::max_align_t)` (16 on glibc, 8 under MSVC's headers). A request aligned at most
  to that needs no padding; a larger alignment reserves `alignment - alignof(std::max_align_t)` bytes, which the
  placement splits between the two pads. The sum is overflow-checked, and alignments above 2^55 are refused, so the
  DIAG.3a boundary sweep (sizes near `SIZE_MAX`, a 2^62 alignment) still gets nullptr. A block below malloc's
  guarantee would be a named fatal rather than an overrun.
- Under AddressSanitizer the header and both pads are poisoned, so a run before or past the caller's bytes is
  reported (use-after-poison where it lands in them, the sanitizer's own redzone beyond). The header is read through
  `CRD_MEM_ASAN_UNCHECKED` (new in `crd/memory/asan_poison.hpp`: `no_sanitize_address` and out of line, plain loads,
  no memcpy), so `allocation_size()` and `deallocate()` report nothing and every other access stays checked. The
  sanitizer's free poisons the whole block, so nothing is unpoisoned before it.
- `deallocate()` and `allocation_size()` check the header first: a non-zero size, an alignment the pointer has, an
  offset inside that alignment's budget and a malloc-aligned block below it. A header that fails is
  `CRD_FATAL("MallocAllocator: block header failed its check")` in every build, never a `free()` of a pointer computed
  from it. That is a corruption check, not ownership: `owns()` stays true for every pointer, and reading below a
  pointer this allocator never handed out may itself fault (the DIAG.3a policy says so).

Alternatives measured against this one:

- Counting the platform's usable size on both sides: Windows still cannot answer, so its bytes stay wrong.
- Keeping `_aligned_malloc`/`aligned_alloc` and adding a prefix of `max(16, alignment)`: correct, but on Windows the
  CRT's own bookkeeping (`alignment - 1 + 8` bytes) comes on top of the prefix, 16 more bytes per default-aligned block
  than this layout.
- The cost of this layout: on Windows one byte more per block than `_aligned_malloc` at every alignment (it reserved
  `alignment - 1 + 8`; this reserves `16 + alignment - 8`); on glibc 16 bytes more per block at alignments up to 16,
  and `alignment` bytes beyond the request at larger ones (glibc's `memalign` had trimmed its own padding). Two stores
  per allocation and two loads and a few compares per free, against a `malloc`/`free` call.

## The real program, before and after

`smoke_memory` (win-debug) requests the default allocator's statistics before anything allocates and prints them at
exit:

| Source | alloc | dealloc | bytes_in_use | peak |
| --- | --- | --- | --- | --- |
| HEAD (`86f4b900`) | 21 | 17 | 789,028 | 789,028 |
| this change | 21 | 17 | 787,520 | 787,779 |

At HEAD the 17 frees lowered nothing (in use equals the peak, the lifetime total); now they return 1,508 bytes. The
remaining 787,520 bytes are blocks the program still holds at exit.

## Tests and teeth

- `test_memory.cpp`: `MallocAllocator: allocation_size is the exact request at every alignment` (every build; sizes
  1 to 4,097 at alignments 1 to 4,096, the bytes of every block kept intact across the others),
  `MallocAllocator: the byte statistics return to their baseline after every free` (requests the statistics before
  the first allocation, so it is exact on the `CRD_DEBUG` lanes and on win-shipping-profile and reads zeros where
  they are compiled out), and the debug statistics case now checks the bytes exactly instead of a lower bound.
- `test_allocator_asan.cpp`: `allocator asan: a malloc heap block is live exactly, its header and padding poisoned`
  (under ASan the caller's bytes are live, the byte past them, the header and the byte below it poisoned; every
  build checks the exact size and the bytes), and the negative control's new `malloc-underrun` mode
  (`crd-diag-allocator-poison-specimen`: a read of the byte before a live default-allocator block must be ASan's
  use-after-poison, exit 42).
- `test_allocator_boundaries.cpp`: `allocator boundaries: a MallocAllocator block whose header was overwritten stops
  at the named fatal`, through the new `malloc-header-clobbered` mode of `crd-diag-alloc-fatal-specimen` (the 16 bytes
  below a block overwritten, then freed; unpoisoned first so an ASan build reaches the free).

Teeth: `malloc_allocator.cpp` set back to HEAD's bytes (everything else unchanged), the lane rebuilt, the cases run,
the source restored byte for byte (`cmp`) and the lane rebuilt and rerun green each time.

| Lane | What failed with HEAD's allocator |
| --- | --- |
| win-debug | exact size (`0 == size`, 25 blocks, both passes), the balanced bytes (`after.bytes_in_use` stayed at 6,100), the exact debug statistics, the ASan-file case's exact size (20 blocks), and the header mode (exit `0xC0000005` in `_aligned_free`, not 42): 5 of 56 selected cases |
| win-asan | the in-process case (exact size on 20 blocks; the byte below a 48-byte block addressable once) and the harness case (`malloc-underrun` exit 43: the report was not use-after-poison) |
| linux-gcc-debug | exact size (`24 == 1` and 22 more), the balanced bytes (`after.bytes_in_use` = 2^64 - 3,252, the wrap), and the header mode (exit 134, glibc's own abort in `free()`): 3 of 9 selected cases |

## Also repaired: the vswhere message

Every `scripts\msvc-env.bat` call from the agent's shells printed "'vswhere.exe' is not recognized" (the previous
batches carried it). The cause: VsDevCmd runs `vswhere.exe` by bare name after a `pushd` into the installer directory,
and these shells set `NoDefaultCurrentDirectoryInExePath=1`, so cmd.exe does not search the current directory. The
side effect was more than noise: `VSCMD_VER` stayed unset. `msvc-env.bat` now puts the installer directory (from the
`vswhere.exe` it already located) on `PATH` before vcvars; a probe printed the message without it, and nothing but
`VSCMD_VER=18.6.3` with it, and no build log of this batch prints it since.

## Verification

- win-debug: crd-memory-tests (182 cases, 824,841 assertions), the two specimens, `crd-fuzz-memory-allocators` and
  `crd-fuzz-perf-capture` and `smoke_memory` built with no warning; the suite, both corpus replays and the 13
  repository guards (`ctest -R "^crd-no-|^crd-check"`) pass.
- win-asan (under vcvars): crd-memory-tests, the two specimens and the two fuzz targets built with no warning;
  crd-memory-tests (182 cases) and both corpus replays pass.
- linux-gcc-debug and linux-gcc-asan (mirror trees through `wsl-lane.sh`): the same five targets built with no
  warning; crd-memory-tests passes 182 cases on both (on linux-gcc-asan the 32 deliberate poison-specimen reports are
  26 use-after-poison and 6 container-overflow, each its mode's declared kind) and both corpus replays pass. That
  round's guards failed `crd-no-malloc-allocator` on both lanes: the new specimen spelled the type in its expected
  fatal. It now matches the message's tail; linux-gcc-debug was rebuilt and its suite, corpus replays and 12 guards
  pass, as do win-debug's.
- `dev.py plan --build build/win-debug` named the routes: `check-headers.py` on the two headers and the include budget
  (both pass, 9 of 9 entries within), and the sanitizer lanes above.
- Strict tidy is clean on the eight changed C++ files (two local constants renamed to lower case). clang-format: no
  file has more findings than at HEAD; the one hunk applied is the specimen-mode table the new entry re-aligns.
- The Allman check, `test-repository-tools.py` (117 tests, none skipped under `msvc-env.bat`) and
  `test-dev-workflow.py` (59) pass, as do `check-master-plan.py` and `check-repository.py`.

## Hosted evidence needed

- crd-memory-tests passes on every lane with the three new cases, and the ASan lanes (win-asan, linux-gcc-asan) with
  the in-process case's poison checks and `malloc-underrun` at exit 42.
- `allocator boundaries: a MallocAllocator block whose header was overwritten stops at the named fatal` passes (exit
  42) on every lane, the shipping and release lanes included.
- `crd-fuzz-memory-allocators-corpus` and every other corpus replay pass (the fuzz harness's budget allocator sits on
  this allocator), and every lane's suites pass with the default allocator's new layout.
- The Windows jobs' logs print no "'vswhere.exe' is not recognized".

## Follow-up for an extra batch

- The complete-tier dispatch for `86f4b900` once 39207693168 finishes (unchanged from the previous batch).
- DIAG.11b (1), (1a) and (2) to (9) as the row lists them; REPO.3's unrouted programs and tracked debug outputs.
