# 2026-10-29 — SIMD-DISPATCH: Payne–Hanek reduction for sin, cos and tan

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
follows [the image sweep resync and Clang's UBSan lane](2026-10-29-simd-dispatch-guard-resync-and-clang-ubsan.md),
whose Left list named this as the next batch.

## CI first

Push run 40826432863 (dd860168) is still running. Its two new completed failures, win-shipping (job 123291442977)
and win-relwithdebinfo (job 123291443152), are each one `roundpd` in the CRT's `__sse4_floor2`
(`MSVCRT:svml_dfloor2_sse4.obj`), the finding win-release had; the `msvc-crt-svml` exception of b9e796e3, after
dd860168, covers that object. The guard took 448 s and 737 s there. Every later push run was replaced before any job
started (0 jobs). DIAG.11c and DIAG.12 stay Needs CI: no completed run carries their hosted evidence.

The same run carries the baseline flip's first green `-at-sse2` arms, read from the job logs: win-debug (job
123291442861) passed `[cpu-level-applied]` and 7,997 tests in 2,805 s, and linux-gcc-debug (job 123291443052) 7,671
tests in 3,209 s, each gate with no failures. The Left clause is closed.

The ISA guard's hosted seconds so far, for the timeout per route: on the object route win-debug 155,
win-debug-scalar 174, win-clang-cl 153, linux-gcc-debug 188, linux-gcc-debug-scalar 75, linux-clang-tsan 92; on the
image route win-clang-cl-shipping 437, win-release 440, win-shipping 448 and win-relwithdebinfo 737 (the last three
failed on the SVML finding). The guard's own clause waits for a run green on every lane.

## What landed

- **Payne–Hanek above Cody-Waite's exact range.** `reduce_pio2` keeps Cody-Waite while |k| ≤ 2^20, where its
  three π/2 parts multiply k exactly (every in-range bit is unchanged; the golden checksum holds). Above, it
  multiplies the 53-bit significand by the 224 bits of 2/π that matter for its exponent, in 32-bit limbs: the bits
  above add only multiples of 4, the dropped ones less than 2^−138. The fraction keeps at least 97 significant bits,
  enough for Muller's worst case (|r| ≈ 2^−61), and becomes r + rt through an exact product with π/2. Integer
  arithmetic and exact conversions only, so every compiler and level gives the same bits. Before, sin(1e300) was +∞.
- **References to the largest double.** The generator adds 1,240 points per function from 1e6 to the largest
  double, both signs: the seam's neighbours, the doubles nearest k·π/2 for k up to 2^1000 + 1, and Muller's worst
  case, at 400 digits; the quadrants of ten huge arguments; and the 37 words of 2/π, which the test compares with the
  engine's table. The existing arrays are unchanged.
- **`crd::math::deterministic` past Cephes's domain.** Its f32 and f64 `sin`, `cos` and `tan` converted the octant
  count to an integer type, undefined behaviour for |x| ≳ 3.4e9 (f32) and 1.5e19 (f64) and for NaN and ±∞, and the
  reduction was wrong past |x| = 8192 (f32) and 2^30 (f64). Those arguments now take `crd::math`'s functions, rounded
  to f32; the Vec8f and Vec4f overloads send such lanes to the scalar function after one compare. In-domain bits are
  unchanged; a new case checks both precisions, NaN and ±∞, mixed vector lanes and the C library within an ulp.
- The quadrant case compares with mpmath's k mod 4. The old oracle, round(x·2/π) mod 4 in doubles, was the wrong
  quadrant once x·2/π no longer rounds to k.

## Checks

- win-debug crd-math-tests: all 178 cases pass, with sin and cos ≤1 ulp and tan ≤2 ulp over the new range, and the
  new range's golden `0xa65cc2f9b1b48d16`.
- Tidy and clang-format on the changed C++, and the repository validators.
- The new golden comes from MSVC; the reduction uses integer operations, exact conversions and `libm::fma` only, so
  a Linux or ARM64 lane that disagrees points at a defect in the double-double tail, never at the constant.

## Left

- The generator does not reproduce its log-spaced grids: numpy's `exp` gives other last bits than the ones
  committed (log, log2, log10, rsqrt). Spacing them through mpmath, as the new range does, adds a point where `log`
  is 2 ulp (x = 0.17705993512268989): `crd_log_reduce` rounds t = (m − 1)/(m + 1) and carries no correction. The next
  batch makes `log` ≤1 ulp with its SIMD twins and regenerates the grids.
