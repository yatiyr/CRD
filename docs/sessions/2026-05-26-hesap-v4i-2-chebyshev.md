# 2026-05-26 — hesap v4i-2: Chebyshev polynomial preconditioner

Phase 3.1.6 `crd-hesap` v4, slice **v4i-2** (second of three: v4i = SPAI / Chebyshev / Schwarz).
The matrix-free, GPU-mappable, perfectly-parallel polynomial preconditioner — and the smoother
AMG will reuse at v4k.

## What shipped

- **`ChebyshevPreconditioner<T>`** (`chebyshev.hpp`) — `M⁻¹ ≈ p_deg(A)`, the degree-`deg`
  Chebyshev polynomial approximating `A⁻¹` on `[λmin, λmax]` of an SPD/HPD A. apply = the
  three-term Chebyshev recurrence: `deg-1` **matrix-free spmv** + axpy/scal — **NO
  factorization, NO triangular solve**. `apply_adjoint == apply` (real polynomial of a
  Hermitian operator).
- **Spectral bounds** estimated by a deterministic power iteration: **non-uniform seed**
  (alternating-sign × ramp — a uniform vector lies in the near-nullspace of Laplacian-like
  operators and collapses λmax), fixed iteration count, Rayleigh quotient. `hi = 1.05·λmax`;
  `lo = hi·lo_ratio` (default 1/30, caller may override either bound).
- **Determinism moat held**: power iteration (fixed seed/iters, sequential Rayleigh dot, KBN
  nrm2) + apply (parallel-SELL spmv bit-exact across threads + KBN blas1) ⇒ thread-count
  independent.
- 8 CLI (`hesap.precond.chebyshev.{f32,f64,c32,c64}` + `chebyshev` in the PCG/MINRES/SYMMLQ
  selectors with `degree`/`cheb_lo_ratio` knobs).
- +4 tests (converge + beat unpreconditioned CG; degree-monotone strength; complex HPD;
  determinism). Iterative suite **126 cases / 110866 assertions**.

## Honest bench (gated, vs Eigen IncompleteCholesky-CG)

- **lap2d-160 (n=25600, well-conditioned SPD): Cerid Chebyshev(8)-PCG = 51 it / 35.7 ms (true
  6.8e-10) CRUSHES Eigen IncompleteCholesky-CG = 247 it / 71.2 ms (9.4e-9) — 2.0× faster at a
  tighter residual, MATRIX-FREE (no factorization, no tri-solve, GPU-mappable).** Eigen ships
  no polynomial preconditioner at all (breadth). Degree-monotone: deg4 87 it → deg8 51 →
  deg16 34.
- **bcsstk13 (κ≈10¹², badly scaled): stalls** — `lo=λmax/30 = 1e11` brackets the low spectrum
  catastrophically wrong (true λmin ≪ 1e11). HONEST + textbook: Chebyshev standalone needs a
  good spectral bracket; the degree to resolve κ≈1e12 scales with √κ. It is fundamentally the
  matrix-free parallel **smoother** (high-mode damper) whose killer app is AMG (v4k, where the
  coarse grid handles the low modes), not an ill-conditioned standalone preconditioner.

## DoD

win-debug build + ctest guards + 4 new cases green; engine win-tidy clean (tidy-checks
chebyshev.hpp via cli_register); win-clang-cl clean; gated bench compiles + runs. gcc + 18-config
sweep → CI per `feedback_local_test_only_ci_owns_sweep`.

## Next

**v4i-3 — Additive + Restricted Schwarz** (overlapping domain decomposition; AS + RAS, local
dense solves) closes v4i.
