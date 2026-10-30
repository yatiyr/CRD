# 2026-10-30 — SIMD-DISPATCH: the fft row-close board and whole codelets at avx2 and avx512

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the boards, fft first). Before:
[win-asan under its bound](2026-10-30-simd-dispatch-tick-clock-span-win-asan-bound-run.md).
Board: [fft](../bench/2026-10-30-simd-dispatch-fft-board.md).

## CI first

- No completed run has an unhandled failure. Push run 40993784639 (87695de9, in progress) failed linux-gcc-debug,
  -asan, -debug-scalar and -public-checks on test_svd.cpp:1636's `const auto` pair copy, fixed at 2db8e32c.
- 40993784639 predates 585fb406, so its win-asan job is not the bound's green-run evidence; the first run at or after
  585fb406 is (41000702578 was pending; this batch's push replaces it). Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run.
- Housekeeping: an orphan `grep` of an earlier batch held 9.9 GB and was killed; an uncommitted, uncompilable edit of
  `runtime/examples/peers/eigen_peer_dense.cpp` (undefined `CRD_EIGEN_PEER_TEXT`), recorded by no note, was saved to
  the scratchpad and reverted.

## The board method

Pre-row is `2f2aed72`, the parent of the row's first engine commit `809e720c`. Its fft is header-only, so its two
programs and its foundation sources are compiled by hand from a `git archive` export with that commit's
linux-gcc-release flags (the [board](../bench/2026-10-30-simd-dispatch-fft-board.md) lists them); later families
reuse the export. Five passes interleave pre-row and HEAD at every level on one pinned core; the noise is each
binary's range over the passes.

## Finding and fix

The avx2 level was 11% to 22% behind pre-row on f64 128K to 8M (but 1M) and 13% to 48% on f32 64K to 4M, MKL
unchanged in the same processes. A/B builds traced it to the codelet split of 20c9f2a1: every tile body ran as
150-line parts passing live values through a slot array on every level. Whole bodies restored f64; force-inlined parts
did not (the slots stay on f32).

d8475439: `fft_codegen_split.py` writes each codelet twice in its unit, whole under `CRD_FFT_GEN_WHOLE_BODY`
(codelet_lanes.hpp: optimized avx2 and avx512, a lane one register) and split for portable, sse2, neon and unoptimized
builds. Every split edition is byte-identical to before; the hierarchical units were re-split from the 20c9f2a1^
bytes. MSVC: a whole batched unit 136 s and 2.15 GB at avx2, 106 s and 4.19 GB at avx512, so a Ninja pool
(`crd_fft_whole_codelets`, two) bounds them on 16 GB runners.

After: f64 accepted (0.96 to 1.10 of pre-row, inside the noise). f32 not: 64K +13% and 512K +23% outside the noise,
256K to 2M +9% to +14% inside it.

## Checks

linux-gcc-release lane tree: crd-hesap-fft-tests (45 cases, the `[kernels]` level-against-portable bit checks
included) passed. win-debug: crd-hesap-fft-tests built, `[kernels]` passed; the ctest guards passed on a serial rerun
(the first, at four jobs, failed `crd-check-environment-reads` once on "internal error in regular expression
engine", which three reruns and the serial run did not repeat; a GCC segfault compiling an avx512 batched unit at
the first lane build did not repeat either; both recorded as host events). MSVC win-release commands compiled a
twiddle, a hierarchical and an avx512 batched unit clean. Generators `--check` on WSL, check-generated, the tooling
tests, strict tidy and clang-format on codelet_lanes.hpp, Allman braces, the plan and repository checks.

## Left

- f32 at avx2: 64K (ip4) and 512K (deep-split-4) beyond noise; the cause next.
- The method: `MKL_ENABLE_INSTRUCTIONS` per forced level and a glibc without its FMA ifunc on portable, then the
  sse4.2, sse2 and portable columns again.
- A user decision: at sse2 and sse4.2 the planner's algorithms lose to Stockham (1.2x to 1.7x, 1K to 64K) and the
  four-step (1.2x to 1.26x, 128K to 1M); one plan on every level keeps the cross-level bits (ADR-0134 §7), a plan per
  level gives them up.
- The other families' boards.
