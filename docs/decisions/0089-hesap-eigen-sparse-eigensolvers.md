# ADR-0089 — crd-hesap-eigen: sparse eigensolvers + module edges (Phase 3.1.6 v6)

**Status:** Accepted (2026-06-07)

**Tags:** arch, hesap, eigen, eigensolver, svd, determinism, module-edges

## Context

Phase 3.1.6 v6 adds the sparse eigenvalue substrate (`crd-hesap-eigen`): a few eigenpairs / singular triplets of
a large sparse operator, matrix-free over `crd::hesap::LinearOp<T>`. This is the backbone for FEM modal/buckling
(`K·x = λ·M·x`), spectral methods, model reduction, and sparse SVD/PCA. ADR-0065 §v6 reserved the slice; this ADR
records the design decisions and the new module edges that v6 introduces.

The gold standards are mature, decades-tuned libraries: ARPACK (scipy `eigsh`/`svds`), PRIMME (state-of-art
symmetric), FEAST, Spectra, SLEPc. They share the BLAS/LAPACK kernel ceilings; none carries cross-thread
bit-determinism.

## Decision

1. **Matrix-free over `LinearOp<T>`.** The solver sees only `A·x` (+ optionally `B·x` / `Aᵀ·x`). Rayleigh-Ritz on
   the small projected problem reuses the dense eigensolvers (`crd-hesap-dense`). No dense matrix is formed.

2. **The crush axis is ALGORITHMIC, not kernel.** Plain Krylov (Lanczos/Arnoldi/IRLBA) is shipped + reported as
   **parity + the determinism moat** (the honest ceiling — sharing the BLAS/LAPACK ceilings of ARPACK/PRIMME).
   The genuine crush is converging in *fewer matvecs* via preconditioned/iterative methods: shift-invert (v6-d),
   LOBPCG (v6-e), Jacobi-Davidson (v6-f), FEAST (v6-g). **Proven (v6-z bench, 3D model Poisson, matched
   accuracy):** AMG-LOBPCG **crushes DIRECT shift-invert ARPACK** (9.5× wall-clock + 40× memory, both growing —
   iterative-beats-direct in the 3D fill-in regime) and is **at PARITY with state-of-art PRIMME** at f64. The
   crush-vs-direct + parity-vs-same-class + the moat is the win; a speed-win over PRIMME+AMG is NOT claimed.

3. **Determinism is the differentiator.** Deterministic counter-RNG start (`EigenOptions::seed`, SplitMix64) +
   fixed-order reorthogonalization + pinned sign/order conventions (largest-|component| positive; coupled SVD
   signs from the dense `svd`) + deterministic dense Rayleigh-Ritz ⇒ eigenpairs **bit-identical across
   {1..16} workers** (the only parallel step is the bit-exact spmv + the moat-proven v5 factor build). Tested
   `[moat]` for every method.

4. **Restart-method SUBSTITUTION (determinism-driven).** The implicit-restart bulge-chase of IRAM/IRLM is
   ordering-sensitive and not deterministic. We ship the mathematically-equivalent, stabler, moat-safe variants:
   **thick-restart Lanczos ≡ IRLM** (v6-b), **Krylov-Schur ≡ IRAM** (v6-c), **augmented thick-restart ≡ IRLBA**
   (v6-h, Baglama-Reichel).

5. **Real-symmetric focus; complex where inherent.** The symmetric methods are real (f32/f64) — the simulation
   eigenvalue targets. Arnoldi/Krylov-Schur emit complex eigenvalues + eigenvectors for a real nonsymmetric A.
   Hermitian-complex eigensolvers + complex SVD are consumer-driven follow-ons (no current consumer), per the v5
   complex-family discipline.

6. **New module edges (acyclic, one-way):** `crd-hesap-eigen` → `crd-hesap` (LinearOp) · `crd-hesap-dense`
   (Rayleigh-Ritz) · `crd-hesap-sparse` (SparseLinearOp / ParallelSpmvLeastSquaresOp) · **`crd-hesap-direct`**
   (v6-d shift-invert + v6-g FEAST: the v5 multifrontal LU factor of `A−σI` / `z_kI−A`) · **`crd-hesap-iterative`**
   (v6-f Jacobi-Davidson: FGMRES for the correction equation) · `crd-jobs`. The hesap-eigen→hesap-direct and
   hesap-eigen→hesap-iterative edges follow the proven v5f-c2 hesap-direct→hesap-iterative direction (direct +
   iterative are lower siblings; no cycle).

## Consequences

- A complete sparse-eigensolver family (Lanczos, thick-restart, Arnoldi/Krylov-Schur, shift-invert, LOBPCG
  [+precond +generalized], Jacobi-Davidson, FEAST, IRLBA) with a determinism moat no gold standard carries.
- The honest standing: **crush vs direct factorization; parity vs the state-of-art same-class peers**; the moat +
  (often) better accuracy / only-correct-solver is the differentiator — consistent with the v5 sparse-direct
  posture. Reported without parallel-vs-serial asterisks (the FULL-VICTORY honesty discipline).
- The hesap-direct + hesap-iterative reuse makes shift-invert / FEAST / JD a v5↔v6 bridge rather than new kernels.

See `docs/systems/hesap-eigen.md` (the method set + the crush verdict) and `docs/phases/phase-3.1.6-hesap.md`
(v6 row + v6-a…z sub-rows).
