# DIAG.3b container live ranges, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.3b](../ROADMAP.md#slice-diag.3b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-3b). Rules: [AGENTS](../../AGENTS.md).
> Earlier batches the same day: [pool and arena poisoning](2026-10-07-diag-3b-pool-and-arena-poisoning.md),
> [TLSF and ring poisoning](2026-10-07-diag-3b-tlsf-and-ring-poisoning.md).

## Goal

Close the last local item on DIAG.3b: container live ranges. A read of `Array` storage past `size()` but inside
`capacity()` touches memory the allocator handed out, so neither core ASan nor the allocator poison could see it.

## What changed

- `crd/memory/asan_poison.hpp` adds `asan_annotate_live_range(storage, capacity_bytes, old_live, new_live)`, a
  wrapper over `__sanitizer_annotate_contiguous_container`. It marks only the whole granules inside the storage
  (start rounded up, end rounded down, both boundaries clamped into that range) and skips null or empty ranges. Older
  sanitizer runtimes (the GCC 13 libsanitizer among them) abort on an unaligned start, and a granule shared with a
  neighbour cannot be split without marking the neighbour, so a packed buffer never touches the bytes around it.
  Without ASan it is a no-op.
- `crd::containers::Array` keeps its unused capacity `[size, capacity)` marked. Every mutator moves the boundary:
  `push_back`, `emplace_back`, `try_push_back` and `insert` open a slot before constructing it; `resize`,
  `resize_uninitialized`, the copy constructor and copy assignment open the target range before constructing it;
  `pop_back`, `erase`, `swap_remove`, `clear` and shrinking `resize` close slots after destroying them. A new buffer is
  marked once the elements are relocated (`adopt_buffer`), and `free_buffer` lifts the marking before the buffer goes
  back to its allocator, so an arena slice, pool slot or TLSF block returns exactly as it was handed out. Moves carry
  the buffer and its marking together.
- The negative control `crd-diag-allocator-poison-specimen` gains six `array-*` modes (31 in all): `array-past-size`
  (element 3 of 3 inside a 16-element capacity), `array-pop`, `array-clear`, `array-shrink` (`resize(10)` then
  `resize(2)`), `array-odd-bytes` (byte 13 of a 13-byte `Array<u8>`, inside a partly live granule) and `array-arena`
  (an `Array<u64>` over a `LinearAllocator` slice). The death callback now checks the kind the mode declares:
  `container-overflow` for the array modes, `use-after-poison` for the 25 allocator modes, which are unchanged. A
  buffer that moved or a misaligned byte buffer exits 95. The specimen links `crd-containers`.
- Consumers found by the full ASan runs (below). `tests/numerics/hesap-dense/test_svd.cpp` and
  `test_mrrr_eigenvalues.cpp` built their buffers with `Array(n, &alloc)`, which only reserves capacity, and filled
  them through `data()`; 33 tests now build them with a local `sized<T>(n, alloc)` helper (`resize(n)`). A
  `bugprone-misplaced-widening-cast` finding in those touched lines was fixed with the casts on the operands.
- A real engine defect: `dlasd1` (`engine/numerics/hesap-dense/include/crd/hesap/dense/detail/svd_dc.hpp`) sized
  `coltyp` to `n`, but `dlasd2` fills it with the four `CTOT` counts on exit, so every merge with `n == 3` wrote one
  `int` past the live range (inside the capacity, so nothing faulted before). It is now `max(n, 4)`, as in LAPACK,
  where `COLTYP` lives in a larger integer workspace.
- `tests/foundation/memory/test_allocator_asan.cpp` gains two in-process cases that assert the shadow under ASan (and
  its absence elsewhere): the boundary after every mutator, growth, copy construction and assignment, a move and
  `shrink_to_fit`; an arena-backed array whose slice returns fully addressable after the array is destroyed while
  the arena's tail stays poisoned; and an `Array<u8>` packed at byte 3 of an arena with live neighbours on both sides
  that never marks them.

## Decisions

- **Only `Array` is annotated.** `String` (an inline buffer, then a heap buffer with its NUL slot) and `FixedArray`
  (inline storage copied with its owner) are not; the row and the design declare the limit.
- **Whole granules, clamped.** Rather than rely on the newer runtimes' unaligned-container support, which the GCC 13
  runtime on the Linux lanes may lack, the wrapper keeps every call inside whole granules. Detection is byte-exact
  when the buffer starts on a granule and its byte size is a multiple of 8, which the default allocator gives for
  8-byte and larger elements.
- **`data()` past `size()` is now a report.** A caller that writes into reserved capacity before growing the size is
  reported; grow with `push_back`, `resize` or `resize_uninitialized` first. The full ASan suites found such callers
  only in two hesap-dense test files, plus the `dlasd1` workspace defect.

## Evidence

- win-asan: the memory suite passes, 162 test cases and 824,434 assertions; all 31 specimen modes end in their
  declared report (25 `use-after-poison`, 6 `container-overflow`, exit 42). The whole tree was rebuilt and its full
  CTest run (7,223 tests, 68 minutes, `-j 8`) failed 34 tests: 33 hesap-dense tests on the container-overflow above
  and `crd-developer-workflow`, a Python tooling test whose temporary-directory cleanup hit a Windows file lock while
  the WSL build ran beside it; it passes alone. After the fixes `crd-hesap-dense-tests` passes whole under win-asan
  (349 cases, 359,508 assertions); before the `dlasd1` fix its smoke test reported the `coltyp` write at
  `svd_dc.hpp:420`. `svd_dc.hpp` is reached only through `crd-hesap-dense` (`src/svd.cpp`) and `test_svd.cpp`; no
  other suite in the full run reported the `coltyp` write. `CEIR-31b-3-c-ii` reports Skipped as before.
- Teeth on win-asan: with `asan_annotate_live_range` returning early, the six array modes exit 0 and 3 test cases
  with 29 assertions fail (the specimen case and the two new in-process cases). Restored (rewritten, so the mtime
  moved), rebuilt and rerun: the suite passes again.
- linux-gcc-asan on the WSL reference host: the whole tree built and its full CTest (6,894 tests, `-j 8`) failed 54:
  the same 33 hesap-dense tests, the registered third-party UBSan defect TP-5 in the mikktspace oracle, and 20
  CUDA cases (19 `crd-kir-cuda-tests`, one `crd-ceir-gpu-cuda-tests`) where WSL's `/usr/lib/wsl/lib/libcuda.so.1`
  leaks from `cuInit` with no CUDA device (LeakSanitizer; the hosted runner has no such driver). No
  container-overflow outside hesap-dense. After the fixes the memory suite passes 162 cases with 25
  `use-after-poison` and 6 `container-overflow` reports, and `crd-hesap-dense-tests` passes 349 cases.
- win-debug: the whole tree builds; memory 162 cases (the specimen reports `InstrumentAbsent` in all 31 modes),
  hesap-dense 349. win-shipping and win-clang-cl-shipping: memory 161 cases (the debug-only stats case is not
  registered), hesap-dense 349, all green. linux-gcc-debug and linux-clang-tsan: memory 162, hesap-dense 349, no
  TSan report.
- Strict `tidy-files.py` is clean on the seven changed C++ files. `clang-format --dry-run --Werror` adds no
  violation (`svd_dc.hpp` goes from 102 older findings to 101; the two hesap tests keep 67 and 47, as at HEAD).
  `check-allman-braces.py`, the `crd-no-*`/`crd-check*` CTest guards, `check-master-plan.py` and
  `check-repository.py` pass.

## Remaining on DIAG.3b

Hosted only. The first change-tier run after the push must show, on win-asan and linux-gcc-asan, the memory suite
green with `allocator asan: every poisoned access is the declared ASan report, or reported absent` passing (31
modes: 25 `use-after-poison`, 6 `container-overflow`) and the full CTest green on both lanes; on the non-ASan lanes
the same case passes as `InstrumentAbsent`.
