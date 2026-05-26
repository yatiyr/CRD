# 2026-05-26 — hesap v4f-3: block-GMRES + block-BiCGSTAB (closes v4f)

Phase 3.1.6 `crd-hesap` v4 (iterative solvers), slice **v4f-3**. Shipped block-GMRES +
block-BiCGSTAB for general nonsymmetric multi-RHS systems, completing the block-Krylov
family. Closes v4f.

## What shipped

- **`block_gmres.hpp`** — block-GMRES(m), flexible/preconditioned (`block_gmres` /
  `block_fgmres` / `block_pgmres`). Block Arnoldi (block-MGS via `block_gram` /
  `block_gemm_update`, `V_{j+1},H_{j+1,j} = block_qr`), block-Hessenberg least-squares by
  **banded scalar Givens**: H_{j+1,j} is the QR R-factor (upper-triangular), so scalar
  column c = j·s+cc has subdiagonal nonzeros in exactly rows c+1..c+s (an s-wide band) —
  triangularized with the verbatim `gmres_givens` / `gmres_rot_apply` applied over all s
  columns of the RHS G. Deflation guard on the back-solve (zero QR diagonal ⇒ the deflated
  direction contributes nothing; without it, restart-mid-cycle deflation → NaN).
- **`block_bicgstab.hpp`** — block-BiCGSTAB (El Guennouni-Jbilou-Sadok 2003), un/precond.
  s×s α/β via the GENERAL `block_lu_solve` (partial-pivot LU — R̃₀ᴴAP is NOT SPD, so SPD
  Cholesky would be wrong), scalar ω = Frobenius ⟨AS,S⟩/⟨AS,AS⟩, β = (1/ω)·M⁻¹·(R̃₀ᴴR_new)
  reducing to scalar BiCGSTAB at s=1. **Divergence guard** (worst relative residual > 1e10
  ⇒ Breakdown) — the block ω-via-Frobenius amplifies BiCGSTAB instability across all s RHS;
  gemat11 ran to 1e+56 without it.
- **Foundational primitives** (in `block_cg.hpp` detail, the block-primitives home):
  `block_qr` generalizes v4f-2's `block_orthonormalize` to return the s×s R factor (captured
  during packed-MGS; the reorthogonalization pass ADDS to R[i,j]) — `block_orthonormalize`
  becomes the discard-R wrapper. New GENERAL `block_lu_solve` (partial-pivot LU with a
  pivot-floor for graceful near-singularity).
- **16 CLI**: `hesap.iterative.{block_gmres,block_bicgstab}.{f32,f64,c32,c64}` (plain, in
  the iterative module; the preconditioned `block_pgmres`/`block_pbicgstab` are reachable
  via the API). block_gmres carries a `restart` param.
- **+7 tests**: `W = Q·R` reconstruction (the foundational `block_qr` — QᴴQ=I + recon
  <1e-10); block-GMRES nonsym + **shared-Krylov ratio** (block-iters ≤ Σ per-column GMRES,
  both full/non-restarted to dodge restart stagnation) + complex + determinism;
  block-BiCGSTAB nonsym + per-column peer + complex + determinism.

## Honest crush result (gated 3-way bench, general nonsym)

- **Cerid per-column GMRES/BiCGSTAB CRUSHES Eigen per-column 2.27–2.87× on expensive
  operators** (convband GMRES 2.27×, BiCGSTAB 2.87×; gemat11 GMRES 2.76×, BiCGSTAB 2.72×) —
  the parallel SELL spmv, the v4b/v4c path now demonstrated on the block bench. Ties on the
  cheap convdiff3 (0.91–0.93× GMRES, 1.33× BiCGSTAB).
- **block-GMRES/BiCGSTAB**: correct, complete, A-pass reduction 4–24×, breadth (Eigen has
  no block algorithm). Wall-time owned by per-column on sparse operators — the same
  characterized floor as block-CG (block's O(n·s²) dense work has no DRAM-pass to trade for
  on cache-resident A; block wins matrix-free / expensive-apply).
- gemat11 unpreconditioned stalls/diverges for ALL methods (Cerid block + per-column +
  Eigen) — a preconditioning regime (BiCGSTAB's known erratic behavior; Eigen BiCGSTAB also
  fails at r=1.8e2). The divergence guard now reports it as a clean Breakdown.

## Notes / process

- **Advisor-vet**: per the project rule, the design was advisor-vetted before implementing.
  The advisor recommended vetting block-GMRES before block-BiCGSTAB; I made a deliberate
  call to vet the whole slice once (block-GMRES was validated by its 3 tests, esp. the
  shared-Krylov ratio + determinism). Nothing went wrong, but recording the deviation.
- **Test-design lessons** (all hit during this slice): use a WELL-conditioned nonsym
  (diag-dominant conv-diff) so the per-column GMRES baseline doesn't restart-stagnate; use
  INDEPENDENT RHS columns (a {base, ones} block is rank-2 and `ones` is near-null for
  conv-diff → false 1-step convergence); the block ≤ Σ-col ratio is a loose sanity check.
- Saved to memory: `feedback_block_gmres_band_givens_and_guards` (banded scalar Givens,
  block_qr-returns-R, deflation + divergence guards, general LU).
- Cross-config: win-debug ctest + guards + engine win-tidy all clean; clang-cl (stale PCH)
  + win-tidy on pre-existing test files (LLVM skew) → CI owns the pinned-LLVM-19 gate per
  `feedback_local_test_only_ci_owns_sweep`.

## Next

v4g — IC(0) + ILU(0) incomplete-factorization preconditioners (consume the v2 etree /
symbolic factorization).
