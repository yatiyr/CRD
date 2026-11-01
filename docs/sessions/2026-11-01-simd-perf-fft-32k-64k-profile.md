# 2026-11-01 — SIMD-PERF: the f32 32K and 64K forwards against MKL, profiled

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at its widest
avx512 gaps after the 1K change: 64K (0.76x MKL aligned) and 32K (0.83x), both planned IP4-AoS. Board:
[2026-11-01-simd-perf-fft-32k-64k-profile](../bench/2026-11-01-simd-perf-fft-32k-64k-profile.md).

## CI first

- Every push run on main is still cancelled by the next push (`c591984a`'s, 41284087995, among them). Manual
  dispatch 41274380605 (`de5e10b0`) had passed win-vs, win-arm64-debug and the ARM64 Linux lanes when read: MSVC and
  ARM64 have compiled `b0640d0c`'s `transpose8x8_halves`. Its three failures are not this track's: win-clang-cl and
  win-clang-cl-shipping fail to link `crd-perf-tests` on `__udivti3` and `__divti3` (128-bit division in
  `tests/foundation/perf/test_instruction_access.cpp`, the serial workflow's open HW-RESILIENCE file), and
  linux-gcc-release fails `test_multivariate.cpp:125`'s `-Wmaybe-uninitialized`, fixed on main at `76330344`, which
  the dispatch predates. win-tidy was still running; no completed lane has built `bcec86fd`'s avx512 stage-1 store yet.

## Profile first

- **The shape.** MKL runs 32K as three passes of 32 and 64K as 32·32·64. The planned IP4-AoS runs 1.70x and 1.66x
  MKL's instructions (core bound 30% and 44%); the deep-split-3, Cerid's three-pass shape at MKL's sizes, runs 1.34x
  MKL's at 32K, memory bound 63%, and ties IP4-AoS.
- **The 16K lever at 32K (not kept).** The in-place three passes need equal outer passes (pass 3 writes its group
  where it read it only when A = C), so with the 16-point outer passes 32K is 16·128·16: a generated 128-point pass
  2. It ran 0.964x IP4-AoS aligned [0.946-0.972] and 0.967x off a line [0.951-0.979]; the 128-point pass 2 took 41%
  of the samples and the transform stored 2.9x MKL's with L1d fills equal to MKL's. Reverted (the 260,000 lines of
  regenerated codelet units with it).
- **The bound.** With `l2_rqsts.all_rfo`, `resource_stalls.sb` and `l1d_pend_miss.fb_full` at 32K: the deep-split-3
  and the candidate match MKL's RFOs and fill-buffer stalls, so their out-of-place or in-place layout costs no extra
  line traffic, but they store 41,900 and 49,700 times a transform against MKL's 17,300 (three passes need about
  12,300) and the store buffer is full for 51% and 54% of their cycles against MKL's 35%. The 32·32·32 shape moved
  into the caller's buffer would therefore win nothing; the leaves spill. A step of a Cerid leaf is 16 columns with
  real and imaginary parts in their own registers: a 32-point leaf keeps 64 vectors live in 32 registers.
- **64K's arms.** The deep-split-4 ran 1.029x IP4-AoS aligned [1.017-1.044] and 1.101x off a line, the deep-split-3
  1.046x [1.001-1.064] and 1.071x: unresolved on a 64-byte boundary after 15 repetitions, so the plan stays (either
  would move 64K from 0.75x to about 0.78x MKL).

## Checks

- No engine change landed: the candidate (`INPLACE_MIDS` with 128, `forward_builds` and `planned_forward` to 32K,
  `execute_in_place3`'s 128-point case and the tests' 32K expectations) was built on the machine and reverted.
- Locally the plan, repository and Allman checks.
- The machine ran 0.35 h in two starts and is stopped.

## Left

- The lever this profile names: a leaf step whose live set fits the 32 registers, so the three-pass shapes stop
  spilling (a 32-point leaf over 8 complex columns a step, real and imaginary parts in one register's halves, with
  the half swap its twiddle products and the ±i rotations need), first as the deep-split-3's 32-point passes at 32K
  and the 32·64·32 or 32·32·64 shape at 64K, then the 8K and 16K passes. The 1K profile's 128 stack stores a step of
  the 32-point stage 1 are the same cause.
- The rest of the stage-1 session's list: the f64 `Vec8d` groups' edition, the 128K to 1M residual, 1K's last 5%,
  8K's and 16K's pass-3 transposition, the sse2 and portable Default rows on this machine, avx512 2K's two-pass
  shape, ARM64's canonical plans on ARM hardware, the i9 re-board of the avx2 plan rows, the jobs retune's boards per
  level, torch and onnxruntime pinned and boarded.
