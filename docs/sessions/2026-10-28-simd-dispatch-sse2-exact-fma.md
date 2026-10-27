# 2026-10-28 — SIMD-DISPATCH: the single-rounded fma on the SSE2 and SSE4.2 backends

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `60ebef5f`.

## CI first

- Push run 40581144513 (`59c79f2f`, still running): its eleven failed GCC and Clang jobs are the `GemmBlock` `beta`
  initializers that `90a87169` fixed (`syrk_microkernel.hpp:80` sets it). The later push runs were cancelled before
  any job failed. No completed run has another failed job.
- Needs CI audit: no complete-tier run finished since `ae7bdc23` (40578927592 stopped on billing), so DIAG.11c and
  DIAG.12 stay Needs CI. A complete-tier run is dispatched after this batch's push.

## What changed

- `crd/math/simd/fma_exact.hpp` (level namespace, SSE2 and SSE4.2 backends only): `fma_ps` and `fma_pd` compute
  RN(a*b + c) in registers. Vec4f, Vec8f and Vec4d `fma`, `fnmadd` and `fmaddsub` use them; before, each lane called
  the C library's fma. NEON and portable keep the libm call (NEON parity is its own Left item).
- f32: widen to f64, where the product is exact and lies in [2^-298, 2^256], round the f64 sum to odd, then convert.
  Rounding to odd keeps the sticky bit, so the one f32 rounding is correct (53 >= 24 + 2); a nearest f64 sum would
  round twice. Every lane takes this path, specials included (their TwoSum error is NaN and fails the ordered tests).
- f64: Boldo and Melquiond 2008, Algorithm 5.4: Veltkamp split and Dekker product, TwoSum(c, uh), RO(tl + ul), one
  final rounding. The proved region is finite operands, |a| and |b| in [2^-1022, 2^995), |ab| in [2^-900, 2^1020)
  and |c| < 2^1020; a zero a or b gives c + ab directly. A result below 2^-1022 inside the region is a Sterbenz
  cancellation, so it is exact before its rounding. Other lanes call libm::fma (one `movemask` test per pair).
- Round to odd is the sum truncated toward zero with bit 0 set: an inexact sum whose error has the opposite sign
  steps one unit down in magnitude, then every inexact sum sets bit 0. SSE2 has no 64-bit compare or arithmetic
  shift, so the sign test broadcasts each lane's high dword.

## Tests

- `simd types: every level's fused multiply-add rounds once on hard cases` (`test_simd_levels.cpp`): 2^20 correlated
  triples of each type through the seven fused probe operations, every level the host runs, portable included,
  against the C library bit for bit (NaN payloads aside). Lanes: c cancelling RN(ab) within two units,
  (1 + 2^-j)(1 +- 2^-j) products at c's midpoints (the f32 double-rounding traps), addends within three binades and
  far apart, subnormal and overflowing products and results, both sides of every f64 region bound, infinities, NaNs
  and the signed-zero combinations.
- Local, win-debug: `[simd-levels]` passes at portable, sse2, sse4.2 and avx2. Hosted CI runs the `-sse2` lanes and
  every `-kernels-at-sse2` and `-kernels-at-sse4.2` arm, where fft, geometry, dense and every other fused user now run
  the emulation.

## Board candidates (row-close pass)

- The fused users at sse2 and sse4.2 against HEAD's per-lane libm calls. On GCC and Clang lanes glibc's `fma`
  dispatches to the hardware instruction on an FMA host, so HEAD's column is faster there than on a real SSE2-only
  CPU; the board records that.
