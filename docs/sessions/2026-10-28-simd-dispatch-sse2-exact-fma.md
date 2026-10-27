# 2026-10-28 — SIMD-DISPATCH: the SSE2 and SSE4.2 exact fma and two-lane log and exp

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `60ebef5f`.

## CI first

- Push run 40581144513 (`59c79f2f`, still running): its eleven failed GCC and Clang jobs are the `GemmBlock` `beta`
  initializers that `90a87169` fixed (`syrk_microkernel.hpp:80` sets it). The later push runs were cancelled before
  any job failed. No completed run has another failed job.
- Needs CI audit: no complete-tier run finished since `ae7bdc23` (40578927592 stopped on billing), so DIAG.11c and
  DIAG.12 stay Needs CI. Complete-tier run 40592200636 (`d57b11cd`) was dispatched after the first push.

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

## Two-lane log and exp, Student-t and the gamma batches

- `crd_log_x2` and `crd_exp_x2` (`simd/transcendental.hpp`) are the SSE2 and SSE4.2 twins of `crd_log1` and
  `crd_exp1`: the AVX2 twins' steps on `__m128d`, each FMA through `fma_pd`, and the scalar core on abnormal lanes.
  SSE2 has no `floor`, so the exp twin floors by truncation and a compare (exact for the in-range lanes, |y| < 1100;
  SSE4.2 uses `roundpd`), and builds 2^k from the low bits of `kf + 2^52 + 1023`.
- hesap-stats: the sse2 Student-t score keeps lanes 0-1 and 2-3 in two registers, so element i still adds to lane
  i % 4 and the (l0 + l1) + (l2 + l3) reduction is unchanged.
- hesap-special builds an sse2 level. Its lgamma and tgamma bodies are one vector body over the level's width (four
  lanes on AVX2, two on SSE2), with the same half-plane test and scalar lanes.

## Tests

- `simd types: every level's fused multiply-add rounds once on hard cases` (`test_simd_levels.cpp`): 2^20 correlated
  triples of each type through the seven fused probe operations, every level the host runs, portable included,
  against the C library bit for bit (NaN payloads aside). Lanes: c cancelling RN(ab) within two units,
  (1 + 2^-j)(1 +- 2^-j) products at c's midpoints (the f32 double-rounding traps), addends within three binades and
  far apart, subnormal and overflowing products and results, both sides of every f64 region bound, infinities, NaNs
  and the signed-zero combinations.
- The probe's `exp/log vector twins` runs each level's twins (the scalar cores on portable and NEON) over the raw
  inputs, a [-750, 750] spread and the inputs' magnitudes; every level must give portable's bits. The Student-t and
  gamma differentials already loop over every built level, so they now compare sse2 with the scalar bodies.
- Local, win-debug: `[simd-levels]` passes at portable, sse2, sse4.2 and avx2; `[kernels]` of crd-hesap-stats-tests
  and crd-hesap-special-tests passes. Hosted CI runs the `-sse2` lanes and every `-kernels-at-sse2` and
  `-kernels-at-sse4.2` arm, where fft, geometry, dense and every other fused user now run the emulation.

## Board candidates (row-close pass)

- The fused users at sse2 and sse4.2 against HEAD's per-lane libm calls; the sse2 Student-t and gamma bodies against
  portable (each keeps its level only if it wins). On GCC and Clang lanes glibc's `fma` dispatches to the hardware
  instruction on an FMA host, so HEAD's column is faster there than on a real SSE2-only CPU; the board records that.
