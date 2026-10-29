# 2026-10-30 — SIMD-DISPATCH: the sparse LU factors' crd-jobs levers as forced arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the supernodal Cholesky's levers](2026-10-30-simd-dispatch-jobs-supernodal-picks.md).

## CI first

- No completed run has a failed job. Push runs 40967635565 and earlier were cancelled before any job started (the
  jobs API lists none); push run 40971874176 (45e148d9) is pending. Push run 40929108987 (1ebe9dc4) still runs
  win-asan and linux-gcc-asan; its other 26 jobs passed.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has completed. Nothing
  cancelled or dispatched.

## What landed

The retune's next hesap-direct consumers, the two sparse LU factors and the dense front kernel they share.
`direct_picks.hpp` gains three picks; `SupernodalLU::factorize` is `factorize_arm(SupernodalLuPick{})` and
`MultifrontalLU::factorize` is `factorize_arm(MultifrontalLuPick{})`, so production and the arms share one body.
The job count stays `factorize`'s parameter.

- **`DenseLuPick`** (`factor_front`, `dense_lu_kernels.hpp`): the panel width (128, blocking), the column-split
  U12 solve's cut (256 columns), the trailing Schur gemm's cut (256K m n k, `gemm_parallel_auto`'s, now
  `gemm_parallel` on the pick's job count), the lookahead (off runs the next panel after the gemm instead of beside
  it) and a forced job count. They act only on a front factored on the main thread (`gemm_par`), so no setting nests
  a `parallel_for`; partial pivoting keeps the serial within-front path.
- **`MultifrontalLuPick`**: the small-fill cut (1M L + U: the serial walk), the front-parallel level width (0: half
  the pool, at least 2), the parallel zero fill's and extend-add's cuts (2^18 entries, 256 columns), and `front`.
  The extend-add takes a job count and cut instead of a flag.
- **`SupernodalLuPick`**: the inline foot update's cut against gemm (2^18 flop) and the diagonal block's panel (48),
  both blocking (the gemm path accumulates through the level's kernels, the inline loop term by term).
- `CRD_MF_PANEL` and `CRD_MF_FRONTPAR_K`, documented dev overrides
  ([2026-06-01](2026-06-01-hesap-v5b-3d-node-level-and-adaptive-mc64.md)), go: the forced arms replace them in one
  binary. The stale "num_workers reserved" comments on both factors are corrected.
- **Kept serial (no arm):** both LU solves and their refinement run on the calling thread; `SupernodalLU` has no
  within-supernode parallelism (its cmod gemm and diagonal LU run on the supernode's job).

## Tests

`lu_arms` (`test_lu_arms.cpp`), f64, f32, c64 and c32, on a bordered unsymmetric block matrix (32-column blocks
under a 48-column root) and a 16 x 16 convection-diffusion grid, on a four-worker pool on every host:

- the default pick at 4 to 32 jobs, and six multifrontal scheduling arms (serial walk, level walk, every level
  front-parallel, every front on the main thread with every within-front stage parallel, the same without
  lookahead and on 32 forced front jobs) at 4 to 32 jobs: L, U, their fill and the row permutation bit for bit
  against one job, with and without partial pivoting;
- blocking arms (foot update all gemm and all inline, diagonal panels 1, 8 and 256; front panels 1, 8, 16 and 256
  with every stage parallel, lookahead on and off): each bit for bit at 8 and 32 jobs against its own one job, and
  the raw factor's solve (`apply_inverse`) within 4 n eps;
- specials: -0 and subnormal off-diagonals, a NaN off-diagonal, and a weak diagonal whose natural-diagonal attempt
  falls back to MC64.

The three LU moats now run 1 to 32 workers. win-debug: `lu_arms` 24.5 s, 1,952 assertions.

## Board

`bench_hesap_direct_parallel` gains `board_lu`: both LU factors on convection-diffusion stencils (16³, 24³, 256²)
under AMD, f32 and f64, one job against 6 supernodal and 17 multifrontal arms, bit gates and the raw-solve bound, a
miss exits 1. `run_bench_direct_parallel.sh` runs `lu_arms` with the Cholesky gate first.

## Checks

win-debug builds of crd-hesap-direct-tests and the bench; `lu_arms`, the `[lu]` and `[lu-pp-mf]` cases pass;
strict tidy clean on the ten changed C++ files; clang-format on the added lines; Allman braces; the ctest guards.
No bench ran: the board runs at row close.

## Left

hesap-direct's LDLt, QR and BLR, then hesap-amg, -iterative, -autodiff and -sched.
