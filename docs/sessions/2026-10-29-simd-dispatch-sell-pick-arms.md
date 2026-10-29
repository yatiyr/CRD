# 2026-10-29 — SIMD-DISPATCH: the SELL board's pick arms and complex gathers

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left items 3 and 4, the SELL policy and Delta16 boards).
CI first this batch: [TickClock](2026-10-29-tick-clock-counter-behind.md).

## What landed

- **SellPick** (`crd/hesap/sparse/sparse_kernels.hpp`): each SELL entry takes `{sweep, gather, pair_width}` and
  returns whether it ran the pick. `SellGather::Policy` is the level's own choice; `Loads` and `Gather` force a body's
  scalar-load or vector-gather form, and `pair_width` forces the pairing (0 is the policy's, `kSellNoPairs` none).
  A `Gather` pick is refused, y untouched, where the level has no gather form (portable, sse2) or x is wider than the
  body's index reaches; Policy and Loads are never refused. Production passes `{sweep, Policy, 0}` through
  `detail::sell_spmv_range`, whose forced overload serves the board.
- **Complex gather forms** on avx2 and avx512: a complex f32 value gathers as one 64-bit lane (reach 2^31 columns);
  a complex f64 value as two, indices 2 col and 2 col + 1 (reach 2^30). Masked columns duplicate each lane's bit for
  the pair. Each gathering body now carries its own `kGatherColumns`. The policies keep their 2026-10-27 picks.
- The pair width is a run-time argument of the driver, so the arms add no instantiation per width.

Every pick moves the same bits through the same two-rounding chain, so all are bit-exact with the CSR spmv.

## Checks

- crd-hesap-sparse-tests built on win-debug. `[kernels]` passes at the detected level, forced portable, sse2 and
  avx2, and under SDE `-spr` for the spmv cases (avx512 included). `[sell]` passes.
- New cases: every forced pick (three gather forms at pair widths 0, 1, 8, 32 and none) over two ranges and two
  alpha/beta cases of every matrix and sweep, with the refusals checked; a fourth sweep sets x past 2^30 columns, so a
  complex f64 gather is refused where the others still gather.
- Strict tidy is clean on the four changed C++ files; Allman braces, the plan and repository checks pass.

## Left

- The job cut at a core's L2 share (`l2_share_bytes`), the Delta16 Auto cut's arm, the one-binary board driver with
  the complex Eigen peer, then the board itself in the row-close pass.
