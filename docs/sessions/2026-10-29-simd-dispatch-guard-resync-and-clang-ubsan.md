# 2026-10-29 — SIMD-DISPATCH: the image sweep restarts at a misread function, and Clang's UBSan lane

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §6](../decisions/0134-runtime-cpu-dispatch.md);
follows [board programs and levels](2026-10-29-simd-dispatch-board-links.md).

## CI first

Push run 40826432863 (dd860168) is still running; later push runs were replaced before any job started (0 jobs).
Two of its completed failures were not yet traced, both read from their logs:

- win-release (job 123291442862): the guard's image route finished for the first time (440 s) with 14 findings.
  - 13 were one lone `C4` at a function's third byte. A jump table that abuts the next function ends inside an
    instruction dumpbin guessed, and the guessed instruction swallows the first two bytes of the function's prologue,
    `48 8B C4` (`mov rax,rsp`). Reproduced on a local win-release build of crd-hesap-tensor-cli-tests: `add_timing`
    reads as `00 48 8B` followed by a lone `C4`.
  - 1 was `roundpd` in `__sse4_floor2`, the CRT's SSE4.1 vector floor. Its dispatcher `__vdecl_floor2` compares
    `__isa_available` with 2 before jumping to it (read from msvcrt.lib).
- linux-clang-arm64-asan (job 123291442813), the first Clang UBSan lane:
  - `crd::math::expm1` converted an infinite reduction index to int for a huge negative finite argument. The same
    function returned NaN for x in [1023.5·ln2, ln(max)], where 2^k is 2^1024. `reduce_pio2` converted a huge
    argument's k to `long long`.
  - Clang's UBSan skips null checks on volatile accesses, so the specimens' volatile null stores never reached the
    report the UBSan lanes promise; ASan caught the fault instead.
  - The guard rejected Clang's `___asan_globals_registered`, a data flag, in kernel objects.
  - The trap, FPU and branch-target failures are the ones ed83f242 and baebebea fixed.

DIAG.11c and DIAG.12 stay Needs CI: no completed run carries their hosted evidence.

## What landed

- **The sweep restarts at a function it misread.** A map function start that dumpbin's sweep did not decode as an
  instruction start is decoded again from itself to the next function (`/range`), and that decode replaces the
  sweep's hits there, so a misread can neither invent a hit nor hide one. On the local image this took 6 decodes and
  no added time.
- **CRT vector math.** Reviewed exception `msvc-crt-svml`: the CRT's `svml_*_{sse4,avx,avx2,avx512}` bodies, entered
  only through dispatchers that test `__isa_available`.
- **expm1 at the ends of the finite range.** Above ln(max) it returns +∞ and below −40 it returns −1, the rounded
  value, so k stays in [−58, 1024]. At k = 1024 it returns 2^1024·(1 + expm1(r)) and no longer NaN. In-range bits are
  unchanged; the golden checksum holds.
- **The trig quadrant** is k − 4·⌊k/4⌋ in exact double arithmetic, so a huge k is never converted to an integer.
- **Null stores the sanitizer checks.** The crash-capture and diagnostic-host specimens store through a volatile
  pointer to a plain object. The store still reaches the hardware, and Clang's UBSan now checks it as GCC's does.
- The ASan exception names `___asan_globals_registered` too.

## Checks

- A scratch Clang UBSan probe (float-cast-overflow, no recovery) over every scalar `crd::math` function and hard
  inputs runs clean; before the fix, the ARM64 lane stopped at `expm1`.
- win-debug: crd-math-tests `[tx]`, `[deterministic]` and the every-level case pass, including the new edge case.
  The crash-capture cases and the diagnostic host's wild write pass against the rebuilt specimens.
- `test-repository-tools.py IsaIsolation` (17 cases): the new misread case fails without the fix. Tidy and
  clang-format on the changed C++, and the repository validators.

## Left

- `sin`, `cos` and `tan` of |x| ≫ 1e6 are still wrong: Cody-Waite loses the reduction, and sin(1e300) is +∞. This
  needs Payne–Hanek reduction, the next batch.
- The Boost peers (a download, awaiting the user), the no-peer-header check after them, and the row's other Left
  items.
