# crd-hesap-eigen — Sparse Eigenvalue Substrate (Phase 3.1.6 v6)

> Matrix-free eigensolvers + sparse SVD over `crd::hesap::LinearOp<T>`. The differentiator no gold standard
> carries: **cross-thread bit-determinism** — eigenpairs identical across {1,2,4,8,16} workers. ADR-0065 §v6,
> ADR-0089 (module edges).

## Purpose

`crd-hesap-eigen` computes a few eigenpairs (or singular triplets) of a large sparse operator without forming a
dense matrix — the substrate for FEM modal/buckling analysis (`K·x = λ·M·x`), spectral methods, model reduction,
PCA/SVD, and graph spectra. The solver sees only `A·x` (and optionally `B·x` / `Aᵀ·x`) through `LinearOp<T>`, so
any matrix representation (CSR, matrix-free PDE operator, …) plugs in.

Two load-bearing truths (advisor-locked, the design compass):

1. **The crush axis is ALGORITHMIC, not kernel.** Plain Krylov (Lanczos/Arnoldi/IRLBA) shares the BLAS/LAPACK
   ceilings of ARPACK/PRIMME → the honest ceiling there is **parity + the determinism moat**. The genuine crush
   is *converging in fewer matvecs* via preconditioned / iterative methods (shift-invert, LOBPCG, JD, FEAST).
2. **The determinism moat is the differentiator.** Deterministic counter-RNG start + fixed-order
   reorthogonalization + pinned sign/order conventions + deterministic dense Rayleigh-Ritz ⇒ the eigenpairs are
   **bit-identical across worker counts**. No ARPACK / PRIMME / SLEPc / PROPACK / scipy carries this.

## Method set

| Method | Function (header) | Problem | Notes |
|---|---|---|---|
| **Lanczos** | `eigs_sym` (`lanczos.hpp`) | symmetric, extreme | full-reorthog, no restart (well-separated extremes) |
| **Thick-restart Lanczos** | `eigs_sym_tr` (`thick_restart.hpp`) | symmetric, extreme | Wu-Simon ≡ IRLM (deterministic); bounded ncv, clustered |
| **Arnoldi + Krylov-Schur** | `eigs_nonsym`, `eigs_nonsym_ks` (`arnoldi.hpp`, `krylov_schur.hpp`) | NONsymmetric | complex eigenvalues + eigenvectors; Stewart restart ≡ IRAM |
| **Shift-invert** | `eigs_sym_shift_invert` (`shift_invert.hpp`) | symmetric, INTERIOR (near σ) | first algorithmic-crush lever; v5↔v6 bridge (v5 LU factor of A−σI) |
| **LOBPCG** | `eigs_sym_lobpcg`, `eigs_sym_gen_lobpcg` (`lobpcg.hpp`) | symmetric / GENERALIZED `K·x=λM·x`, smallest | block; optional preconditioner = the crush hook |
| **Jacobi-Davidson** | `eigs_sym_jd` (`jacobi_davidson.hpp`) | symmetric, extreme/clustered | correction eqn via FGMRES; preconditioned crush hook |
| **FEAST** | `eigs_sym_feast` (`feast.hpp`) | symmetric, INTERVAL `[lo,hi]` | contour integration → complex shift-invert (v5); the interval specialist |
| **IRLBA (sparse SVD)** | `svds` (`svds.hpp`) | LARGEST singular triplets | Golub-Kahan-Lanczos bidiagonalization + thick restart |

The spec is shared: `EigenOptions<T>` (nev/ncv/which/tol/seed/max_restarts) + `EigenResult<T>` (complex values +
real/complex vectors + residuals + nconv). The SVD has its own `SvdResult<T>` (σ + U m×k + V n×k + residuals).

## The crush verdict (v6-z gold-standard bench, 2026-06-07)

Honest, matched-accuracy comparison on the 3D model Poisson (smallest 4, tol 1e-7), **all peers run in one
Linux/gcc-release environment** for fair wall-clock; **eigenvalues identical across every peer** (the accuracy
gate). Captured to `build/v6z_bench/*.txt`. Peers: Cerid AMG-LOBPCG (`bench_hesap_eigen_lobpcg.cpp`), direct
shift-invert ARPACK (`scripts/eigen_ref_scipy.py`), pyamg-LOBPCG floor (`scripts/eigen_floor_pyamg.py`), PRIMME
(`scripts/eigen_ref_primme.py`, no-precond + pyamg-AMG-preconditioned).

**@ n = 43,680:**

| peer | metric | result vs Cerid AMG-LOBPCG (0.795 s, 21 it, AMG 31 nnz/row) |
|---|---|---|
| **direct shift-invert ARPACK** | wall-clock + memory | **CRUSH — 9.5× faster (7.56 s) + 40× less memory (factor 1241 nnz/row)**, both growing with n (iterative-beats-direct in the 3D fill-in regime) |
| **PRIMME (state-of-art)** | wall-clock / matvecs | **PARITY** — PRIMME-no-precond 0.58 s (Cerid solve-only 0.46 s beats it; total 0.80 s incl. AMG setup ⇒ amortization-dependent); PRIMME+AMG 55 mv ≈ Cerid ~21 block-iters |
| **pyamg-LOBPCG (identical algorithm)** | iterations | **PARITY** — Cerid 21 vs pyamg 19 |
| **all** | determinism | **the {1..16} cross-thread moat none of them carries** |

**Honest claim (every qualifier load-bearing):** Cerid AMG-LOBPCG **crushes DIRECT methods** (the fill-in regime
— the real win for 3D/FEM) and is **at parity with the state-of-art PRIMME** at f64 — matching a mature,
decades-tuned C library *plus* a determinism moat it lacks. This is **not** a speed-win over PRIMME+AMG (which
would be dishonest to claim), and it is the win the design pre-committed to. The matvec metric is caveated: a
LOBPCG iteration applies A to a *block* of nev vectors while PRIMME's JDQMR/GD+k apply per inner step — so
PRIMME+AMG is "competitive with the state-of-art" and the **pyamg-LOBPCG floor (identical algorithm AND precond)
is the clean parity claim**. Deferred peers (named, no new conclusion): Spectra (header-only, no-precond = same
story as unpreconditioned ARPACK) and FEAST-lib (MKL build cost). Plain-Krylov *eigen* methods (Lanczos/Arnoldi)
are parity + moat by design — the honest ceiling, reported as such.

**Sparse SVD verdict (IRLBA, v6-z bench):** on the 2D rectangular-grid edge-node incidence matrix (largest 4
singular triplets, matched tol 1e-7, all peers one Linux/gcc-release env; `bench_hesap_svds.cpp` +
`scripts/svds_ref.py`; **singular values bit-identical across Cerid / scipy.svds / primme.svds** = the accuracy
gate). Wall-clock is competitive-to-favorable (Cerid ≤ `primme.svds` at all tested sizes; ≤ `scipy.svds` up to
n≈4.3K; `scipy.svds` ~1.6× ahead at n=9504, where its ARPACK-on-normal-equations scales better on the
clustered-largest and Cerid's thick-restart cycles grow to 84). **⚠ HONEST CAVEAT: the Python peers pay a
per-matvec reverse-communication overhead a native caller would not (primme.svds ran 654–1422 matvecs), so the
wall-clock wins OVERSTATE the pure-algorithm gap.** The RELIABLE claims: **matched accuracy + the {1..16}
determinism moat the peers lack**; algorithmically it is PARITY (all three are Krylov-bidiagonalization). Honest
standing: a correct, competitive sparse SVD with the moat — not a clean wall-clock crush of the gold standard.

## Complex-completeness audit

| Method | f32 | f64 | complex (c32/c64) | note |
|---|---|---|---|---|
| Lanczos / thick-restart / shift-invert / LOBPCG / JD / FEAST | ✓ | ✓ | — | REAL symmetric (the eigenvalue moat target; the simulation use cases) |
| Arnoldi / Krylov-Schur | ✓ | ✓ | complex eigenvalues + eigenvectors **emitted** for a real nonsym A | the nonsymmetric path is inherently complex-valued |
| IRLBA (`svds`) | ✓ | ✓ | — | real rectangular; complex-input SVD = follow-on |

The symmetric methods are real by design (Hermitian-complex eigensolvers are a follow-on consumer-driven slice,
like the v5 complex families — no current consumer). The nonsymmetric Arnoldi/Krylov-Schur already produce
complex output from real input (`EigenResult::vectors` + `vectors_im`).

## Determinism moat

Every method ships a `[moat]` test asserting eigenpairs **bit-identical across {1,2,4,8,16} workers** on a
well-separated spectrum (clustered/degenerate cases test subspace identity / values-only, since the eigenvectors
are then non-unique). The only parallel step is the operator's spmv (`ParallelSparseLinearOp` /
`ParallelSpmvLeastSquaresOp`, bit-exact across thread counts) and the v5 factor build (shift-invert / FEAST,
itself moat-proven); everything else (RNG start, reorthog, dense Rayleigh-Ritz, restart, FGMRES) is deterministic
serial. → end-to-end {1..16} determinism, the v6 differentiator.

## Module edges (acyclic; ADR-0089)

`crd-hesap-eigen` depends on (one-way): `crd-hesap` (`LinearOp<T>`), `crd-hesap-dense` (Rayleigh-Ritz via
`eig_sym`/`eig_nonsym`/`svd`), `crd-hesap-sparse` (`SparseLinearOp` / `ParallelSpmvLeastSquaresOp`),
`crd-hesap-direct` (v6-d shift-invert + v6-g FEAST: the v5 multifrontal LU factor of `A−σI` / `z_kI−A`),
`crd-hesap-iterative` (v6-f Jacobi-Davidson: FGMRES for the correction equation), `crd-jobs` (parallel spmv).
The hesap-eigen→hesap-direct and hesap-eigen→hesap-iterative edges mirror the proven v5f-c2
hesap-direct→hesap-iterative direction (iterative + direct are lower siblings).

## Integration notes

- **FEM modal/buckling** (`K·x = λ·M·x`): `eigs_sym_gen_lobpcg(K, M, …, precond)` with an AMG/IC0 preconditioner
  on the stiffness K (T≈K⁻¹). The smallest generalized pairs are the natural modal target.
- **Interior bands** (a frequency window): `eigs_sym_feast(A, lo, hi, m0, …)` — all eigenvalues in `[lo,hi]`.
- **Interior point** (near a shift σ): `eigs_sym_shift_invert(A, σ, …)`.
- **Sparse SVD / PCA / model reduction**: `svds(A, …)`.

## Status

v6-a…h shipped (the full method set) + the symmetric crush verdict + the SVD verdict (both above) + the {1..16}
all-methods moat + CLI `hesap.eigen.*`. v6-z closed pragmatically (the 18-config full sweep is left to CI).
