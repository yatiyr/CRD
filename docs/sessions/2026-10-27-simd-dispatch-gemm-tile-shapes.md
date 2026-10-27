# 2026-10-27 — SIMD-DISPATCH: per-level GEMM tile shapes

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run has a new failed job. Complete run 40515770381 (`31e2b9ce`) is still running; its four failed jobs
  were fixed by `b306e2ea` and `14840123`. Push runs 40560579462 back to 40521536075 were cancelled with no failed
  job; 40561980649 (`f992a90d`) is pending. Nothing dispatched (a complete run is in progress).
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI;
  no complete run at or after `14840123` has finished.

## The defect first: the small-GEMM path summed the depth in one group

`gemm()` sums the depth in groups of `kGemmKc` = 256 (each group's sum from +0, merged into C in order), but
`gemm_parallel`'s small path ran the whole depth as one block. For k > 256 one worker (`gemm()`) and several workers
gave different bits, and with a per-level tile the path choice (`m % MR`) would have made the bits level-dependent.
The grouping is now declared arithmetic (`dense_kernels.hpp`, ADR-0134 §7): C takes beta C, then each group's sum,
on every path (gemm, its parallel and small paths, syrk; the supernodal walks' depths are at most 192 and 64, one
group). Small-path bits for k > 256 change from HEAD.

## Per-level shapes

- `DenseTypeKernels<T>` gains `gemm_shape` (`GemmShape{mr, nr}`) and the `pack_a` and `pack_b` entries on a plain
  strided source (`GemmSource`): packed panels are per-call scratch, so each level packs for its own tile (ADR-0082's
  backend-coupled packing, now realised); `GemmTraits` is gone. The interleaved pack bodies live once in the level
  namespace (`gemm_block.hpp`), and general code runs them for the complex types' 8 by 8 tile (HEAD's shape, no fused
  merge: the LTCG C1001 trap).
- Tiles: one `SixRowTile` over the level's registers replaces four hand tiles: twelve accumulators, two B registers
  and one A broadcast (15 of 16): avx2 f64 6 by 8 (unchanged), avx2 f32 6 by 16 (it had eight accumulators), sse2
  f64 6 by 4 and f32 6 by 8 in one pass (they ran two halves). portable is a register-blocked 2 by 4 `PortableTile`
  with a fused merge (14 of 16 scalar registers; it spilled 6 by 8 and 8 by 8 arrays).
- The prefetch of the next B panel's line forms an integer address, never a pointer past the buffer.
- The small path reads op(A)'s whole row panels in place whenever op(A) is row-major (both layouts, any lda) and
  packs only the last partial panel, so it needs no `m % mr` or tight-stride condition; gemm, gemm_parallel and the
  small path each allocate one buffer; the three beta passes are one helper.
- Consumers read the shape at run time: blas3, syrk, the supernodal TRSM walk (its 96-row block rounds down to whole
  panels of the level's mr), the fatal specimen.

Verification: `test_dense_kernels.cpp` ([kernels]) runs each level's block on panels of its own shape against the
declared arithmetic from the logical operands (the bound check against the two-rounding sum), each level's packers
against a scalar model from three stride pairings with a NaN guard word, and every GEMM path (gemm, the small and
packed parallel paths at 1, 2 and 3 workers, both layouts and transposes, syrk) at k = 9 and 300 and m off the tile
against the grouped model bit for bit. win-debug: the dense tests, direct tests and fatal specimen build; the
[kernels] cases pass at the detected level and forced to sse2 and portable; the dense fatal modes, include budget and
`crd-no-*` guards pass; `llvm-nm` on the three kernel objects shows no symbol outside the level namespace. Tests on
hosted CI; the one-thread board (6 by 16 against OpenBLAS and Eigen, portable 2 by 4 against 4 by 4) in the row-close
pass.
