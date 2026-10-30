# 2026-10-30 — SIMD-DISPATCH: crd's exact fma bodies where glibc's fma is software

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the follow-up of
[the fft board's method](2026-10-30-simd-dispatch-fft-board-method.md)). ADR-0134 §8 (amended).

## CI first

No completed run has an unfixed failure; the cancelled runs 41033961827, 41021307508, 41019372039 and 41016561958
list no failed job. In-progress push run 41021189146 (`4ab1cd90`): linux-gcc-release and linux-gcc-shipping failed
`the sparse LinearOps' picks keep the serial kernels' bits` on the block op's complex specials, the NaN-payload case
`530d0f4e` fixed (its push run 41034376277 is pending). Needs CI audit: DIAG.11c and DIAG.12 wait on a green
complete-tier run, SIMD-DISPATCH on a green win-asan run; nothing flips.

## The measurement

ns per call, i9-14900K, one pinned core, 2^12 triples, best of seven (scratch board, not kept). Masked: glibc's FMA
and FMA4 hidden by `GLIBC_TUNABLES=glibc.cpu.hwcaps`; MSVC: the UCRT's FMA3 bodies off. glibc's software bodies round
once (no mismatch); the UCRT's do not (2026-10-29).

| | glibc fused | glibc masked | UCRT FMA3 | UCRT off | crd integer f64 | crd round-to-odd f32 |
|---|---|---|---|---|---|---|
| f64, GCC 13 | 1.59 | 119 | | | 10.4 | |
| f32, GCC 13 | 1.26 | 70 | | | | 1.29 inlined |
| f64, Clang 18 | 2.39 | 119 | | | 10.8 | |
| f32, Clang 18 | 1.26 | 69 | | | | 1.09 inlined |
| f64, MSVC | | | 0.58 | 39.7 | 12.9 | |
| f32, MSVC | | | 0.46 | 25.3 | | 1.24 inlined |

glibc 2.39's resolver (`libm.so.6`, disassembled) picks the FMA body on the FMA feature's active bit, the FMA4 body on
FMA4's, else software; hiding AVX2 alone kept it fused.

## What changed

- `crd/math/libm.hpp`: GCC and Clang x64 units without `__FMA__` (portable, sse2, sse4.2, general code) take the
  soft path MSVC had: f64 calls the C library only while `crd::detail::libc_fma_fused`, else the integer body; f32
  always rounds to odd (no slower than glibc's fused fmaf call, 50x faster than its software one). The integer body's
  word helpers use `__builtin_clzll` and `unsigned __int128` there.
- MSVC and clang-cl: f32 now calls the UCRT's fmaf while its FMA3 bodies run (0.46 against the rounding to odd's
  1.24 ns), as f64 did; the same bits.
- `crd::detail::ucrt_fma3_active` is `libc_fma_fused`. On Linux x64 a priority-101 constructor reads glibc's active
  FMA and FMA4 bits through `__x86_get_cpuid_feature_leaf` (glibc 2.33 ABI, weak; `<sys/platform/x86.h>`'s accessors
  return `_Bool`, which Clang refuses in C++); another C library leaves it false.
- The soft-body case runs on every x64 lane: the UCRT's FMA3 fma or glibc's as the oracle over 2^20 hard triples,
  for both bodies and `libm::fma`; on GCC the flag is checked against `CPU_FEATURE_ACTIVE`.

## Checks

win-debug: crd-math-tests built, the case at detected and sse2 and `[simd-levels]` passed. linux-gcc-release:
crd-math-tests built, the case passed with glibc fused (flag 1) and masked (flag 0), `[simd-levels]` masked passed;
Clang 18 compiled `cpu_features.cpp` and a `libm.hpp` unit with `-Wpedantic -Werror`. bench_fft_f32 at portable with
glibc masked: the 4M forward 115.8 ms (before: 3,357; unmasked 109). Tidy (4 files), clang-format, Allman, the 19
ctest guards, plan and repository checks.

## Left

- fft: the user's decision on one plan for every level.
- The other families' boards; a green win-asan run.
