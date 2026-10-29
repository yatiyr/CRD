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
under a 48-column root), a 16 x 16 convection-diffusion grid and a 96-row cyclic matrix, on a four-worker pool:

- the default pick at 4 to 32 jobs, and six multifrontal scheduling arms (serial walk, level walk, every level
  front-parallel, every front on the main thread with every within-front stage parallel, the same without
  lookahead and on 32 forced front jobs) at 4 to 32 jobs: L, U, their fill and the row permutation bit for bit
  against one job, with and without partial pivoting;
- blocking arms (foot update all gemm and all inline, diagonal panels 1, 8 and 256; front panels 1, 8, 16 and 256
  with every stage parallel, lookahead on and off): each bit for bit at 8 and 32 jobs against its own one job, and
  the raw factor's solve (`apply_inverse`) within 4 n eps;
- specials on the bordered matrix and the grid: -0 and subnormal off-diagonals, a NaN off-diagonal and a
  1e-12-scaled diagonal (perturbed pivots); and a near-zero-diagonal cyclic matrix whose natural-diagonal growth
  passes the bound mid-walk, so every arm aborts and falls back to MC64, with and without partial pivoting.

The three LU moats now run 1 to 32 workers. win-debug: `lu_arms` 32.7 s, 3,696 assertions.

## Two defects the arms found (`multifrontal_lu.cpp`)

The first version claimed a 1e-6-scaled diagonal made the natural-diagonal attempt fall back to MC64. A temporary
print of each attempt's growth showed otherwise: the bordered matrix's natural attempt bails before its numeric (its
fill passes a fifth of dense), the grid's is accepted, and the claimed fallback never ran. Probing it found:

- **The growth ignored NaN.** Its running max (`am > fmax`) skips a NaN entry, so a natural factor holding one read
  as stable, against the comment's "NaN / Inf / over-threshold ⇒ unstable". `growth_max` makes NaN absorbing (still
  commutative and associative, so the moat holds); the grid with a NaN now falls back like the comment says.
- **An early abort with partial pivoting indexed out of bounds.** The level walk's growth abort `break`s out and
  ran the pivoting finalize, whose `invperm[m_rowperm[c]]` read the entries of fronts never factored: the NaN grid
  with pivoting on a level-walk arm hit the array bound assertion (stack from cdb). The abort now returns +inf before
  the finalize, like the pre-numeric bail. Both bails run only when the MC64 re-run follows (`may_abort`): under
  `CRD_MF_NO_MC64` they had returned an unfinished factor as the result.

The cyclic matrix (the fallback moat's own fixture) now carries the fallback case: its natural growth reads 5.8e12
to 1.5e15, or NaN, on every arm.

## Board

`bench_hesap_direct_parallel` gains `board_lu`: both LU factors on convection-diffusion stencils (16³, 24³, 256²)
under AMD, f32 and f64, one job against 6 supernodal and 17 multifrontal arms, bit gates and the raw-solve bound, a
miss exits 1. `run_bench_direct_parallel.sh` runs `lu_arms` with the Cholesky gate first.

## Checks

win-debug builds of crd-hesap-direct-tests and the bench; `lu_arms`, the `[lu]` and `[lu-pp-mf]` cases pass;
strict tidy clean on the eleven changed C++ files; clang-format on the added lines; Allman braces; the ctest guards.
No bench ran: the board runs at row close.

## Left

hesap-direct's LDLt, QR and BLR, then hesap-amg, -iterative, -autodiff and -sched.
