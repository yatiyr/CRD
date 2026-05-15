# Phase 3.1.6 — `crd-hesap` numerical computing substrate

**Status:** ⏳ Planned (post-Phase-3.1; ships after eylem v9 closes).
**Estimated duration:** ~6–8 months across 18 slices.
**Locked architecture:** ADR-0065.
**Research:** `docs/research/cerid-hesap.md` + pre-existing
`docs/research/sparsematrices.md`.

## Why this phase exists

The MATLAB-class numerical substrate. Every Cerid domain (games +
robotics + medical + cinematic + DAW + scientific tool) pulls on it
differently; per-domain rolls would fragment 50–100 KLOC of solvers
into incompatible sub-implementations.

ADR-0065 locks the decisions; this file is the slice plan.

## Phase posture

**AMENDED 2026-05-15 per Strategic Execution Plan** (`docs/ROADMAP.md` §
Strategic Execution Plan): `crd-hesap-dense` **v0 ships BEFORE Phase
3.1 eylem v1c resume**, not after the entire eylem phase completes.
This is the first concrete artifact of the engineering-platform pivot
(Pathway E).

**v0 minimum scope:** BLAS L1/L2/L3 (axpy / dot / nrm2 / scal /
copy / swap / asum / iamax / gemv / trmv / trsv / ger / gemm / trmm /
trsm) + LAPACK-class direct (Cholesky `potrf/potrs`, LU `getrf/getrs`,
QR `geqrf/orgqr`) — minimum to unblock eylem v7 FEM (Cholesky on the
mass matrix) + future CFD / FEA / estimation+control (Cholesky / LU /
QR everywhere).

**Sized:** ~3–4 weeks calendar for v0 (1 slice). The full
`crd-hesap` phase (18 slices over 6–8 months) continues to ship after
eylem CLOSE per the unchanged-sub-slice plan below — but eylem v7
FEM **ships hesap-consuming from day 1** rather than the original
narrow-PCG-then-refactor pattern. This obsoletes the
"narrow-version-then-refactor" precedent for eylem v7 the same way
ADR-0076 §12 obsoleted it for eylem v1c/v1d.

**Sequencing after the 2026-05-15 amendment:**

1. Phase 3.1.7 geometry CLOSE (full 49 slices)
2. **Phase 3.1.6 v0 hesap-dense (NEW EARLY SLOT — ~3–4 weeks)**
3. Phase 3.1 eylem v1c+ resume (consuming geometry + units + hesap-dense)
4. Eylem v1c → v9 ships in full
5. Phase 3.1.5 sdf (interleaved between eylem v2 and v3 — unchanged)
6. **Phase 3.1.6 v1–v17 (rest of hesap)** ships after eylem v9 close — sparse / iterative / direct / eig / opt / ode / fft / dsp / stats / tensor / autodiff / gpu / repl
7. Phase 3.1.8+ domain substrates (brep / cad-feature / cfd / etc.) — unchanged

---

### Original phase posture (pre-amendment 2026-05-15)

- **Sequential successor to Phase 3.1.** Eylem v0–v9 ships first (with
  eylem v7 FEM using a narrow internal PCG); `crd-hesap` ships after.
  Eylem v7's internal PCG is refactored to use `crd-hesap-iterative`
  once the substrate exists (see eylem phase plan v7 dependency note).
- **Phase 3.1.6** in the numbering scheme. The fractional `3.1.X`
  series is foundational substrates (3.1 eylem, 3.1.5 sdf, 3.1.6
  hesap); Phase 3.2+ are consumer phases.
- **18 active slices** (v0–v17). v0–v8 build the dense + sparse +
  iterative + direct + eig substrate. v9–v13 add optimisation + ODE +
  FFT + DSP + stats + interpolation + tensor + autodiff. v14–v15 add
  N-dim tensors + autodiff. v16 mirrors to GPU. v17 ships the REPL +
  plug-in C ABI.

## Slice structure

| Slice | Topic | LOC | Tests | Duration |
| :---: | --- | :---: | :---: | :---: |
| v0 | Substrate + dense BLAS L1 (axpy/dot/nrm2/scal/copy/swap/asum/iamax) | ~1500 | ~30 | ~1.5 wk |
| v1 | Dense matrix + BLAS L2 (gemv/gbmv/ger/syr/hemv/trmv/trsv) | ~2000 | ~28 | ~2 wk |
| v2 | Dense BLAS L3 (gemm cache-blocked, syrk, trmm, trsm) + dense direct (LU, Cholesky, QR, LDLT) | ~3000 | ~40 | ~3 wk |
| v3 | SVD + dense eigenvalue (symmetric + non-sym) + least squares | ~2500 | ~30 | ~2 wk |
| v4 | Sparse storage (COO/CSR/CSC/BSR/ELL/HYB) + spmv + spmm + spgemm | ~2500 | ~36 | ~2 wk |
| v5 | Fill-reducing reorderings (AMD, RCM, METIS-class nested dissection) + symbolic factorisation | ~2000 | ~24 | ~1.5 wk |
| v6 | Iterative solvers (CG, PCG, BiCGSTAB, GMRES, MINRES, LSQR, IDR(s)) + Jacobi/IC(0)/ILU(0) preconditioners | ~3000 | ~40 | ~2.5 wk |
| v7 | Sparse direct (supernodal Cholesky, left-looking LU, multifrontal QR) + AMG preconditioner + block-Jacobi + additive Schwarz | ~4000 | ~36 | ~3 wk |
| v8 | Sparse eigenvalue (Lanczos, Arnoldi, IRA, LOBPCG) | ~2000 | ~24 | ~2 wk |
| v9 | Optimisation: unconstrained (gradient, Newton, L-BFGS, trust-region) + line search + QP (OSQP-style ADMM) + LP (revised simplex + interior point) + NLP (Mehrotra interior point) | ~4500 | ~40 | ~3 wk |
| v10 | ODE/DAE: explicit (DOPRI5/8 + Cash-Karp + adaptive step) + implicit (BDF 1–6 + Newton-Krylov) + Rosenbrock + DAE Pantelides | ~3000 | ~32 | ~2.5 wk |
| v11 | FFT (Cooley-Tukey mixed-radix + Bluestein) + RFFT + 2D/3D + DCT/DST/Hartley + convolution | ~2500 | ~28 | ~2 wk |
| v12 | DSP: FIR (windowed sinc + Parks-McClellan) + IIR (bilinear-transform Butterworth/Cheb/Elliptic/Bessel) + biquad + resampling (polyphase) + spectral analysis (Welch, Bartlett) | ~2500 | ~28 | ~2 wk |
| v13 | Statistics: distributions (CDF/PDF/quantile/sample for 20+ types) + statistical tests (t, chi², KS, Mann-Whitney, ANOVA) + special functions (gamma, beta, erf, Bessel J/Y/I/K, Legendre, Hermite, Chebyshev) + splittable PCG + Xoshiro256** | ~3000 | ~36 | ~2 wk |
| v14 | Polynomial / interpolation (linear, cubic spline, Akima, Hermite, monotone, Chebyshev, barycentric, RBF) + quadrature (Gauss-Legendre, Gauss-Hermite, Gauss-Laguerre, Clenshaw-Curtis, adaptive Simpson, Romberg) + numerical differentiation | ~2000 | ~24 | ~1.5 wk |
| v15 | N-dim tensors + broadcasting + einsum + reductions + reshape/transpose/slice/gather/scatter | ~2500 | ~32 | ~2 wk |
| v16 | Autodiff: forward (dual/Jet) + reverse (tape-based) + higher-order + sparse Jacobian + Hessian via forward-over-reverse + every BLAS op differentiable | ~3500 | ~40 | ~3 wk |
| v17 | GPU mirror: dense BLAS L1/L2/L3 + sparse spmv/spmm + GPU FFT + GPU iterative (CG/PCG/GMRES) + GPU autodiff via existing `UploadHandle`/`Fence` | ~4000 | ~32 | ~3 wk |
| v18 | REPL + notebook (`.cnb` `'CNBK'` format) + plot integration + plug-in C ABI (per ADR-0034 pattern) + ImGui frontend panel | ~2500 | ~24 | ~2 wk |

**Total: ~52 KLOC, ~580 tests, ~38 weeks (~8 months) for the full
substrate.** Comparable in scope to eylem itself.

> v17 (GPU) and v18 (REPL) are independently shippable. If schedule
> pressure surfaces, they can defer to a later "Phase 3.1.6 follow-up"
> without blocking the CPU substrate's usefulness.

---

## v0 — Substrate scaffolding + dense BLAS L1 (~1.5 weeks)

**Goal:** `crd-hesap` modules exist and link; dense `Vector<T>` works
correctly; BLAS Level 1 (vector-vector) ops ship deterministic +
SIMD-accelerated.

**Public surface:**

```cpp
namespace crd::hesap::dense
{
    template <typename T>
    class Vector
    {
    public:
        Vector(IAllocator* alloc, usize n);
        Vector(IAllocator* alloc, std::initializer_list<T>);

        [[nodiscard]] usize size() const noexcept;
        [[nodiscard]] T*    data() noexcept;
        [[nodiscard]] const T* data() const noexcept;
        T&       operator()(usize i) noexcept;
        const T& operator()(usize i) const noexcept;
    };

    // BLAS L1 — all const-correct, all return value or write to out-param.
    template <typename T> void  axpy (T alpha, const Vector<T>& x, Vector<T>& y);  // y += alpha*x
    template <typename T> T     dot  (const Vector<T>& x, const Vector<T>& y);
    template <typename T> T     nrm2 (const Vector<T>& x);                          // L2 norm
    template <typename T> void  scal (T alpha, Vector<T>& x);                        // x *= alpha
    template <typename T> void  copy (const Vector<T>& src, Vector<T>& dst);
    template <typename T> void  swap (Vector<T>& x, Vector<T>& y);
    template <typename T> T     asum (const Vector<T>& x);                          // sum |x_i|
    template <typename T> usize iamax(const Vector<T>& x);                           // argmax |x_i|
}
```

**Backends:** SIMD via `crd-math::Vec8f` / `Vec4f` for `f32`,
scalar fallback + AVX2 path for `f64`. Pairwise reduction tree for
sums (`dot`, `nrm2`, `asum`) — bit-reproducible across SIMD widths
under ADR-0063 contract.

**Tests (~30):**
- Numerical accuracy: each op vs hand-computed reference for sizes 1,
  3, 7, 8, 15, 16, 17, 31, 32, 100, 1000.
- SIMD/scalar bit-exact parity (ADR-0063 contract: same input → same
  output across SSE2 / AVX2 / NEON / scalar).
- `axpy` with `alpha = 0` is identity; `alpha = 1` is `y += x`.
- `nrm2` of zero vector is exactly 0; of `[1, 1, ..., 1]` is `sqrt(n)`
  to within `n * eps`.
- `iamax` ties broken by first index (stability).

**Smoke test:** `smoke_hesap_blas1` — random vector, run all 8 ops,
RMSE within `n * eps` of reference.

**Out of scope:**
- Matrix types (v1).
- Complex numbers (reserved; real-valued first).

**Definition of done:** all six configs green + `smoke_hesap_blas1`
green + bit-exact SIMD/scalar parity test green.

---

## v1 — Dense matrix + BLAS L2 (~2 weeks)

**Goal:** `Matrix<T>` (row-major + col-major variants) + BLAS Level 2
(matrix-vector) ops + strided sub-matrix views.

**Public surface:**

```cpp
namespace crd::hesap::dense
{
    enum class Layout : u8 { RowMajor = 0, ColMajor = 1 };

    template <typename T, Layout L = Layout::RowMajor>
    class Matrix
    {
    public:
        Matrix(IAllocator*, usize rows, usize cols);

        T&       operator()(usize i, usize j) noexcept;
        const T& operator()(usize i, usize j) const noexcept;
        [[nodiscard]] usize rows() const noexcept;
        [[nodiscard]] usize cols() const noexcept;
        [[nodiscard]] usize ld()   const noexcept;  // leading dimension
        [[nodiscard]] containers::ConstSpan<T> data() const noexcept;

        [[nodiscard]] MatrixView<T, L> view(usize r0, usize c0, usize nr, usize nc) noexcept;
    };

    // BLAS L2.
    template <typename T> void gemv(T alpha, const Matrix<T>& A, const Vector<T>& x,
                                     T beta,  Vector<T>& y);                          // y = alpha*A*x + beta*y
    template <typename T> void gbmv(...);  // banded
    template <typename T> void ger (T alpha, const Vector<T>& x, const Vector<T>& y,
                                     Matrix<T>& A);                                   // A += alpha*x*y^T
    template <typename T> void syr (T alpha, const Vector<T>& x, Matrix<T>& A);       // symmetric rank-1
    template <typename T> void trmv(...);  // triangular matrix-vector
    template <typename T> void trsv(...);  // triangular solve (vector RHS)
    // ... hemv (Hermitian), hbmv (Hermitian banded)
}
```

**Tests (~28):**
- `gemv` with α=1, β=0 = matrix-vector product; α=0, β=1 = identity on
  y; full mixed case.
- `trsv` round-trip: solve `Ux = b`, verify `Ux == b`.
- `ger` rank-1 update preserves rank-1 structure; eigenvalue check.
- Strided view: 5×5 sub-block of a 100×100 matrix gives correct
  arithmetic.
- Row-major vs col-major: same logical operation, byte-exact same
  output.

**Smoke test:** `smoke_hesap_blas2` — `gemv` 1000×1000 random matrix,
RMSE check vs reference.

**Definition of done:** all six configs green + `smoke_hesap_blas2`
green + row-major/col-major parity test green.

---

## v2 — Dense BLAS L3 + Direct dense solvers (~3 weeks)

**Goal:** `gemm` (cache-blocked) + dense direct solvers (LU, Cholesky,
QR, LDLT) + linear solve from factorisation.

**`gemm` implementation:** Goto/BLIS-style layered:

```
loop nc:        outer panel of B (cache C)
  loop kc:      inner panel of A and B (cache K)
    loop mc:    micro-panel of A (cache L2)
      loop nr:  micro-panel of B (cache L1)
        loop mr: microkernel — fully SIMD-unrolled
```

Microkernel: 8×8 (AVX2) / 4×8 (NEON) / 4×4 (SSE2) / 1×1 (scalar).
Tunable block sizes (`mc`, `kc`, `nc`) — defaults from
benchmark-on-target-hardware fixtures.

**Public surface:**

```cpp
namespace crd::hesap::dense
{
    // BLAS L3.
    template <typename T> void gemm (T alpha, const Matrix<T>& A, const Matrix<T>& B,
                                      T beta,  Matrix<T>& C);                          // C = alpha*A*B + beta*C
    template <typename T> void syrk(...);
    template <typename T> void trmm(...);
    template <typename T> void trsm(...);  // triangular solve (matrix RHS)

    // Direct factorisations (LAPACK-class).
    template <typename T>
    struct LU
    {
        Matrix<T>           LU_packed;   // L (below diag) + U (on/above diag)
        containers::Array<i32> piv;       // pivot indices
        [[nodiscard]] bool singular() const noexcept;
        Vector<T> solve(const Vector<T>& b) const;
        Matrix<T> solve(const Matrix<T>& B) const;
    };
    template <typename T> [[nodiscard]] LU<T> lu(IAllocator*, const Matrix<T>& A);

    template <typename T> [[nodiscard]] auto cholesky (const Matrix<T>& A) -> Cholesky<T>;
    template <typename T> [[nodiscard]] auto qr       (const Matrix<T>& A) -> QR<T>;
    template <typename T> [[nodiscard]] auto ldlt     (const Matrix<T>& A) -> LDLT<T>;

    // Convenience.
    template <typename T> Vector<T> solve(const Matrix<T>& A, const Vector<T>& b);  // dispatches LU
}
```

**Tests (~40):**
- `gemm` accuracy: random matrices, RMSE vs naïve triple-loop within
  `n * eps`.
- `gemm` performance: ≥80% of theoretical peak `f32 gemm` on AVX2
  hardware (validated as a benchmark, not a unit test).
- LU pivoting: matrix with permuted identity factorises to the
  identity LU exactly.
- Cholesky symmetric-positive-definite check; rejects indefinite
  matrices.
- QR: orthogonality `Q^T*Q = I` within `n * eps`; `A == Q*R`.
- Solve round-trip: solve `Ax = b`, verify `Ax == b` within
  `cond(A) * eps`.
- Iterative refinement improves residual when A is ill-conditioned.

**Smoke test:** `smoke_hesap_dense_solvers` — random 500×500 SPD
matrix, Cholesky factorise, solve, residual norm logged.

**Definition of done:** all six configs green + microkernel performance
benchmark logs ≥70 % of theoretical peak on win-release + GPU/window
smokes still green.

---

## v3 — SVD + dense eigenvalue (~2 weeks)

**Goal:** SVD (Golub-Reinsch), symmetric eigenvalue (Householder
tridiag + QR with shifts), non-sym eigenvalue (Schur via QR with
double-shift), pseudo-inverse, least squares.

**Public surface:**

```cpp
namespace crd::hesap::dense
{
    template <typename T> struct SVD { Matrix<T> U; Vector<T> S; Matrix<T> V; };
    template <typename T> [[nodiscard]] SVD<T> svd(IAllocator*, const Matrix<T>& A);

    template <typename T> struct EigSym  { Vector<T> values; Matrix<T> vectors; };
    template <typename T> [[nodiscard]] EigSym<T>  eig_sym (const Matrix<T>& A);

    template <typename T> struct EigNon  { Vector<std::complex<T>> values;
                                           Matrix<std::complex<T>> vectors; };
    template <typename T> [[nodiscard]] EigNon<T>  eig     (const Matrix<T>& A);

    template <typename T> Matrix<T>  pinv       (const Matrix<T>& A, T rcond = T(0));  // Moore-Penrose
    template <typename T> Vector<T>  lstsq      (const Matrix<T>& A, const Vector<T>& b);  // least squares via SVD or QR
}
```

**Tests (~30):**
- SVD reconstruction: `A == U*diag(S)*V^T` within `n * eps * |A|`.
- Singular values are non-negative + sorted descending.
- Eigenvalues of diagonal matrix = the diagonal entries (in some order).
- Eigenvectors are orthogonal for symmetric input.
- Pseudo-inverse: `A * pinv(A) * A == A` within tolerance.
- Least squares: solution minimises `|Ax - b|` (verify with random
  perturbation).

**Definition of done:** all six configs green + numerical-accuracy
benchmark fixtures green.

---

## v4 — Sparse storage + spmv + spmm + spgemm (~2 weeks)

**Goal:** `SparseMatrix<T, Layout>` for COO/CSR/CSC/BSR/ELL/HYB
formats + format conversions + sparse matrix-vector / matrix-matrix
multiplication.

**Public surface:**

```cpp
namespace crd::hesap::sparse
{
    enum class Layout : u8 { COO = 0, CSR = 1, CSC = 2, BSR = 3, ELL = 4, HYB = 5 };

    template <typename T, Layout L>
    class SparseMatrix { /* allocator-managed, layout-specialised storage */ };

    template <typename T> SparseMatrix<T, Layout::CSR> from_triplets(
        IAllocator*, usize rows, usize cols,
        containers::ConstSpan<i32> ri, containers::ConstSpan<i32> ci,
        containers::ConstSpan<T>   v);

    template <typename T, Layout L>
    SparseMatrix<T, Layout::CSR> to_csr(const SparseMatrix<T, L>& src);
    // ... and converters between every pair.

    template <typename T, Layout L>
    void spmv(T alpha, const SparseMatrix<T, L>& A,
              const dense::Vector<T>& x, T beta, dense::Vector<T>& y);

    template <typename T> SparseMatrix<T, Layout::CSR> spgemm(
        const SparseMatrix<T, Layout::CSR>& A,
        const SparseMatrix<T, Layout::CSR>& B);
}
```

**Tests (~36):**
- Format conversion round-trip: `csr → coo → csr` is identity.
- spmv: dense product matches sparse product within `n * eps`.
- spmv parallel reduction: byte-exact with serial under ADR-0063
  contract.
- spgemm pattern correctness: produced sparsity matches symbolic
  prediction.
- BSR (block sparse) gives 3-4× faster spmv than CSR for block-structured
  matrices (benchmark, not unit test).

**Smoke test:** `smoke_hesap_sparse` — load 5-point Laplacian on
1000×1000 grid, run 1000 spmv, validate against dense.

**Definition of done:** all six configs green + format-conversion
round-trip test green + spmv parallel-determinism test green.

---

## v5 — Reordering + symbolic factorisation (~1.5 weeks)

**Goal:** Approximate Minimum Degree (AMD), Reverse Cuthill-McKee
(RCM), METIS-class nested dissection (pure-C++ port of the algorithm —
not a wrap of METIS the library) + symbolic factorisation (predict
fill-in before numeric factorisation).

**Public surface:**

```cpp
namespace crd::hesap::sparse
{
    enum class Reorder : u8 { Natural = 0, AMD = 1, RCM = 2, NestedDissection = 3 };

    [[nodiscard]] containers::Array<i32> compute_permutation(
        const SparseMatrix<f64, Layout::CSR>& A, Reorder strategy);

    template <typename T, Layout L>
    SparseMatrix<T, L> permute(const SparseMatrix<T, L>& A,
                               containers::ConstSpan<i32> perm);

    [[nodiscard]] usize predict_fill(const SparseMatrix<f64, Layout::CSR>& A,
                                     containers::ConstSpan<i32> perm);
}
```

**Tests (~24):**
- AMD on canonical FEM test matrices: fill ratio ≤ documented bound.
- RCM bandwidth reduction: bandwidth(permuted) ≤ bandwidth(original).
- Permutation is a true permutation (every row/col index appears
  exactly once).
- Symbolic-fill prediction matches actual factorisation fill exactly.

**Definition of done:** all six configs green + fill-ratio benchmark
green (logs ratio against natural ordering).

---

## v6 — Iterative solvers + simple preconditioners (~2.5 weeks)

**Goal:** CG, PCG, BiCGSTAB, GMRES (with restart), MINRES, LSQR, IDR(s)
+ Jacobi, IC(0), ILU(0) preconditioners.

**Public surface:**

```cpp
namespace crd::hesap::iterative
{
    enum class Preconditioner : u8 { None = 0, Jacobi = 1, IC0 = 2, ILU0 = 3 };

    template <typename T>
    struct SolverResult
    {
        bool      converged;
        usize     iterations;
        T         residual_norm;
        Vector<T> x;
    };

    template <typename T>
    [[nodiscard]] SolverResult<T> cg(const sparse::SparseMatrix<T, sparse::Layout::CSR>& A,
                                      const dense::Vector<T>& b,
                                      T tol = T(1e-8), usize max_iter = 1000);

    template <typename T>
    [[nodiscard]] SolverResult<T> pcg(const sparse::SparseMatrix<T, sparse::Layout::CSR>& A,
                                       const dense::Vector<T>& b,
                                       Preconditioner pc, T tol, usize max_iter);

    // ... bicgstab, gmres, minres, lsqr, idr_s
}
```

**Tests (~40):**
- CG on SPD Laplacian: convergence ≤ predicted iterations from
  condition number.
- PCG with Jacobi: faster convergence than plain CG on diagonally-
  dominant systems.
- GMRES on non-symmetric: solves linear system within tolerance.
- BiCGSTAB on non-symmetric.
- MINRES on symmetric indefinite (CG would fail; MINRES converges).
- Residual norm decreases monotonically (or near-monotonically for
  BiCGSTAB).
- Determinism: parallel `dot` and `axpy` inside the solver give bit-
  exact same iterations across SIMD widths.

**Smoke test:** `smoke_hesap_iterative` — solve 100×100 Laplacian,
log iteration count.

**Definition of done:** all six configs green + convergence-rate
fixture tests green.

---

## v7 — Sparse direct + AMG + advanced preconditioners (~3 weeks)

**Goal:** Sparse Cholesky (supernodal, CHOLMOD-class), sparse LU
(left-looking Gilbert-Peierls), sparse QR (multifrontal), AMG
preconditioner, block-Jacobi, additive Schwarz.

**Public surface:**

```cpp
namespace crd::hesap::direct
{
    template <typename T>
    struct SparseCholesky
    {
        // factor stored internally with permutation
        Vector<T> solve(const Vector<T>& b) const;
    };

    template <typename T>
    [[nodiscard]] SparseCholesky<T> sparse_cholesky(
        const sparse::SparseMatrix<T, sparse::Layout::CSR>& A,
        sparse::Reorder reorder = sparse::Reorder::AMD);

    template <typename T>
    [[nodiscard]] SparseLU<T> sparse_lu(
        const sparse::SparseMatrix<T, sparse::Layout::CSR>& A,
        sparse::Reorder reorder = sparse::Reorder::AMD);
}

namespace crd::hesap::iterative
{
    enum class AdvancedPC : u8 { AMG = 10, BlockJacobi = 11, AdditiveSchwarz = 12 };
    // PCG / GMRES gain advanced PC parameter.
}
```

**Tests (~36):**
- Sparse Cholesky on FEM stiffness: solves Ax=b within tolerance.
- AMG accelerates CG convergence on Poisson problems by ≥10× vs
  Jacobi on 1000+ unknowns.
- Multifrontal QR on tall-skinny least-squares matches dense reference.
- Determinism: same input → byte-exact same factorisation across runs
  (parallel symbolic + parallel numeric phases both deterministic).

**Definition of done:** all six configs green + AMG convergence
benchmark green.

---

## v8 — Sparse eigenvalue (~2 weeks)

**Goal:** Lanczos (symmetric, with restart), Arnoldi (non-symmetric),
implicitly restarted Arnoldi (IRA, ARPACK-class), LOBPCG.

**Public surface:**

```cpp
namespace crd::hesap::eig
{
    enum class Which : u8 { LargestMag = 0, SmallestMag = 1, LargestReal = 2, SmallestReal = 3 };

    template <typename T>
    struct SparseEigResult
    {
        Vector<T>           values;
        Matrix<T>           vectors;
        bool                converged;
        usize               iterations;
    };

    template <typename T>
    [[nodiscard]] SparseEigResult<T> lanczos(
        const sparse::SparseMatrix<T, sparse::Layout::CSR>& A,
        usize n_eig, Which which, T tol = T(1e-6), usize max_iter = 1000);

    // ... arnoldi, ira, lobpcg
}
```

**Tests (~24):**
- Lanczos on SPD with known eigenpairs: converges to top-k within
  tolerance.
- IRA on non-symmetric matrix: complex eigenvalues recovered.
- LOBPCG with preconditioner outperforms plain Lanczos on stiff
  problems.

**Definition of done:** all six configs green + eigenpair-accuracy
fixture green.

---

## v9 — Optimisation (~3 weeks)

**Goal:** Unconstrained (gradient descent, Newton, L-BFGS,
trust-region) + line search + QP (OSQP-style ADMM) + LP (revised
simplex + Mehrotra interior point) + NLP (Mehrotra interior point).

**Public surface:**

```cpp
namespace crd::hesap::opt
{
    template <typename T>
    struct OptResult
    {
        Vector<T> x;
        T         f_value;
        Vector<T> grad;
        bool      converged;
        usize     iterations;
    };

    // Unconstrained: caller supplies f and grad_f.
    template <typename T, typename FnF, typename FnG>
    [[nodiscard]] OptResult<T> lbfgs(IAllocator*, const Vector<T>& x0,
                                      FnF f, FnG grad_f,
                                      LbfgsOptions<T> opts = {});

    // QP: 0.5*x'Qx + c'x s.t. l <= Ax <= u
    template <typename T>
    [[nodiscard]] OptResult<T> qp_osqp(
        const sparse::SparseMatrix<T, sparse::Layout::CSR>& Q,
        const Vector<T>& c,
        const sparse::SparseMatrix<T, sparse::Layout::CSR>& A,
        const Vector<T>& l, const Vector<T>& u,
        QpOptions<T> opts = {});

    // LP / NLP / MIP-basic — see ADR-0065 §3.
}
```

**Tests (~40):**
- L-BFGS on Rosenbrock, Beale, Powell, Booth — converges to known minima.
- Wolfe line search satisfies sufficient-decrease + curvature conditions.
- QP solver on randomized SPD Q — solution satisfies KKT within tolerance.
- LP simplex correctness on textbook problems.
- NLP interior-point on QP-equivalent problems matches QP solver.

**Definition of done:** all six configs green + benchmark suite of 20
canonical optimisation problems green.

---

## v10 — ODE / DAE solvers (~2.5 weeks)

**Goal:** Explicit (DOPRI5/8 with FSAL + adaptive step) + implicit
(BDF 1–6 with Newton-Krylov) + Rosenbrock + DAE Pantelides.

**Public surface:**

```cpp
namespace crd::hesap::ode
{
    enum class Method : u8 { DOPRI5 = 0, DOPRI8 = 1, BDF = 2, Rosenbrock = 3, RK4 = 4, EulerImplicit = 5 };

    template <typename T, typename FnRhs>
    struct IvpResult { containers::Array<T> t; containers::Array<Vector<T>> y; bool converged; };

    template <typename T, typename FnRhs>
    [[nodiscard]] IvpResult<T> solve_ivp(IAllocator*, FnRhs rhs,
                                          T t0, T t1, const Vector<T>& y0,
                                          Method method = Method::DOPRI5,
                                          T rtol = T(1e-6), T atol = T(1e-9));
}
```

**Tests (~32):**
- Conservation: simple harmonic oscillator energy preserved over
  10000 periods (DOPRI5 vs RK4 — DOPRI5 better).
- Stiff: Van der Pol with μ=1000 — only BDF/Rosenbrock converge in
  reasonable steps.
- Robertson chemical kinetics — BDF handles 12-orders-of-magnitude
  timescale spread.
- Event detection: `solve_ivp` with `events=...` triggers at zero
  crossings.

**Definition of done:** all six configs green + Hairer-Wanner test
suite (10 reference problems) green.

---

## v11 — FFT (~2 weeks)

**Goal:** Cooley-Tukey mixed-radix + Bluestein for prime sizes + RFFT
+ 2D / 3D + DCT/DST/Hartley + convolution.

**Public surface:**

```cpp
namespace crd::hesap::fft
{
    template <typename T>
    void fft (containers::Span<std::complex<T>> data, bool inverse = false);

    template <typename T>
    void rfft(const dense::Vector<T>& real_in, dense::Vector<std::complex<T>>& freq_out);

    template <typename T>
    void fft_2d(MatrixView<std::complex<T>> data, bool inverse = false);

    template <typename T> void dct(containers::Span<T> data, DctType type = DctType::II);
    template <typename T> void dst(containers::Span<T> data, DstType type = DstType::II);

    template <typename T> dense::Vector<T> convolve(const dense::Vector<T>& a, const dense::Vector<T>& b);
}
```

**Tests (~28):**
- FFT size 2, 3, 4, 5, 7, 8, 16, 100, 1024, 1031 (prime), 4096.
- IFFT(FFT(x)) == x within `n * eps`.
- Parseval's theorem holds.
- Convolution via FFT matches direct convolution.

**Definition of done:** all six configs green + numerical-accuracy
suite green.

---

## v12 — DSP filters + resampling + spectral (~2 weeks)

**Goal:** FIR (windowed sinc + Parks-McClellan Remez) + IIR (bilinear
transform Butterworth/Cheb1/Cheb2/Elliptic/Bessel) + biquad cookbook
EQ + polyphase resampling + Welch/Bartlett spectral analysis.

**Public surface:**

```cpp
namespace crd::hesap::dsp
{
    enum class Window : u8 { Hann = 0, Hamming = 1, Blackman = 2, Kaiser = 3, Rectangular = 4 };
    enum class FilterType : u8 { LowPass = 0, HighPass = 1, BandPass = 2, BandStop = 3 };

    template <typename T> dense::Vector<T> design_fir_window(usize order, T cutoff_normalized,
                                                              FilterType type, Window w = Window::Hann);
    template <typename T> dense::Vector<T> design_fir_remez (usize order, FilterType type, ...);

    template <typename T>
    struct IirCoeffs { dense::Vector<T> b; dense::Vector<T> a; };

    template <typename T>
    IirCoeffs<T> design_iir_butterworth(usize order, T cutoff_normalized, FilterType type);
    // ... cheb1, cheb2, elliptic, bessel

    template <typename T>
    struct Biquad { T b0, b1, b2, a1, a2; T z1, z2; };
    template <typename T> Biquad<T> design_biquad(BiquadType, T fs, T f0, T Q, T gain_db);
    template <typename T> T process_sample(Biquad<T>&, T x);

    template <typename T> dense::Vector<T> resample_polyphase(const dense::Vector<T>& x, i32 up, i32 down);

    // Spectral.
    template <typename T> dense::Vector<T> welch_psd(const dense::Vector<T>& x, usize nfft, Window w, f32 overlap);
}
```

**Tests (~28):**
- FIR design: response matches specified cutoff within tolerance.
- IIR Butterworth: monotonic magnitude response.
- Elliptic IIR: order matches design spec for given ripple.
- Biquad: peaking EQ at f0 has gain == design.
- Resample 44.1k → 48k round-trip RMSE bound.

**Definition of done:** all six configs green + filter-design accuracy
suite green.

---

## v13 — Statistics + Special functions + RNG (~2 weeks)

**Goal:** Splittable PCG + Xoshiro256** + 20+ distributions
(CDF/PDF/quantile/sample) + statistical tests + special functions.

**Public surface:**

```cpp
namespace crd::hesap::stats
{
    class PcgRng {
    public:
        explicit PcgRng(u64 seed);
        PcgRng split() noexcept;  // splittable
        u32 next() noexcept;
        f64 uniform() noexcept;
    };

    template <typename T>
    struct Normal { T mean, stddev; };
    template <typename T> T cdf(const Normal<T>&, T x);
    template <typename T> T pdf(const Normal<T>&, T x);
    template <typename T> T quantile(const Normal<T>&, T p);
    template <typename T> T sample  (const Normal<T>&, PcgRng&);

    // Same surface for: Uniform, Exponential, Gamma, Beta, ChiSquared, StudentT, F,
    //                   LogNormal, Cauchy, Weibull, Poisson, Binomial, Geometric,
    //                   NegativeBinomial, Hypergeometric, Multinomial, MultivariateNormal.

    // Tests.
    template <typename T> T t_test_one_sample(const dense::Vector<T>& x, T mu0);
    template <typename T> T chi_squared_test(const dense::Vector<T>& observed, const dense::Vector<T>& expected);
    template <typename T> T ks_test(const dense::Vector<T>& x, const dense::Vector<T>& y);
    template <typename T> T mann_whitney(const dense::Vector<T>& x, const dense::Vector<T>& y);
}

namespace crd::hesap::special
{
    template <typename T> T gamma   (T x);
    template <typename T> T lgamma  (T x);
    template <typename T> T beta    (T a, T b);
    template <typename T> T erf     (T x);
    template <typename T> T erfc    (T x);
    template <typename T> T erfinv  (T p);
    template <typename T> T bessel_j(i32 n, T x);
    template <typename T> T bessel_y(i32 n, T x);
    template <typename T> T legendre_p(i32 n, T x);
    template <typename T> T hermite_h (i32 n, T x);
    template <typename T> T chebyshev_t(i32 n, T x);
    // ... plus polygamma, hypergeometric, etc.
}
```

**Tests (~36):**
- PCG: splittable streams are statistically independent (chi-squared
  on 1M samples).
- Distributions: CDF/PDF/quantile round-trip within tolerance.
- Special functions: numerical accuracy vs reference at canonical
  points (gamma(1)=1, gamma(0.5)=sqrt(π), etc.).
- Statistical tests: known p-values for textbook examples.

**Definition of done:** all six configs green + special-function
accuracy suite green.

---

## v14 — Polynomial + Interpolation + Quadrature (~1.5 weeks)

**Goal:** Polynomial roots / evaluation / arithmetic + interpolation
methods + quadrature methods + numerical differentiation.

**Tests (~24):** Roots on known polynomials, interpolation passes
through nodes, quadrature converges at expected rate, numerical
differentiation accurate to documented order.

**Definition of done:** all six configs green + Romberg/Gauss
quadrature accuracy suite green.

---

## v15 — N-dim tensors + broadcasting + einsum (~2 weeks)

**Goal:** `Tensor<T, N>` arbitrary-rank with strided views +
NumPy-style broadcasting + Einstein-summation `einsum`.

**Public surface:**

```cpp
namespace crd::hesap::tensor
{
    template <typename T, usize Rank>
    class Tensor { /* shape + strides + data + allocator */ };

    template <typename T, usize R> Tensor<T, R> broadcast_to(const Tensor<T, R>& src, ShapeSpec target);

    // Einsum: einsum("ij,jk->ik", A, B) == matmul.
    template <typename T> Tensor<T, ?> einsum(containers::StringView spec, ...);

    // Reductions.
    template <typename T, usize R> Tensor<T, R-1> sum  (const Tensor<T, R>& src, i32 axis);
    template <typename T, usize R> Tensor<T, R-1> max  (const Tensor<T, R>& src, i32 axis);
    template <typename T, usize R> Tensor<T, R-1> min  (const Tensor<T, R>& src, i32 axis);
    template <typename T, usize R> Tensor<T, R-1> mean (const Tensor<T, R>& src, i32 axis);
    template <typename T, usize R> Tensor<T, R-1> var  (const Tensor<T, R>& src, i32 axis);
}
```

**Tests (~32):**
- Broadcasting matches NumPy semantics for documented shape pairs.
- Einsum equivalence to manual loops for matmul, trace, transpose-mul.
- Reduction across axis preserves correct shape.
- Strided view doesn't alias data wrongly.

**Definition of done:** all six configs green + NumPy-parity test
suite green.

---

## v16 — Autodiff (forward + reverse) (~3 weeks)

**Goal:** Forward mode (dual numbers / Jet types) + reverse mode
(tape-based) + higher-order derivatives + Hessians via
forward-over-reverse + sparse Jacobian support.

**Public surface:**

```cpp
namespace crd::hesap::autodiff
{
    // Forward mode.
    template <typename T> struct Dual { T value; T deriv; };

    // Reverse mode.
    template <typename T>
    class Var
    {
    public:
        Var(T value);
        T value() const noexcept;
        // operator+, -, *, /, sin, cos, exp, log, sqrt, etc.
    };

    template <typename FnLoss>
    auto value_and_gradient(FnLoss loss, dense::Vector<f64>& x)
        -> std::pair<f64, dense::Vector<f64>>;

    template <typename FnLoss>
    auto value_grad_hessian(FnLoss loss, dense::Vector<f64>& x)
        -> std::tuple<f64, dense::Vector<f64>, dense::Matrix<f64>>;
}
```

**Tests (~40):**
- Forward-mode Dual: dx/dx = 1; chain rule on composed functions.
- Reverse-mode tape: gradient of `sum(x^2)` matches `2*x`.
- Higher-order: Hessian of `0.5*x'Qx` matches `Q`.
- Gradient checking: AD vs finite differences within `1e-6`.
- Sparse Jacobian: gradient of large-output function uses sparse
  storage automatically.

**Definition of done:** all six configs green + gradient-check
fixture green.

---

## v17 — GPU acceleration mirror (~3 weeks)

**Goal:** Mirror dense BLAS L1/L2/L3, sparse spmv/spmm, FFT, and
iterative solvers to GPU via Vulkan compute + the existing
`UploadHandle` / `Fence` infrastructure (ADR-0061).

**Public surface:**

```cpp
namespace crd::hesap::gpu
{
    template <typename T>
    class GpuVector { /* device-side, opaque handle */ };

    template <typename T>
    class GpuMatrix { /* device-side */ };

    template <typename T>
    UploadHandle upload_to_gpu(const dense::Vector<T>& cpu_v, GpuVector<T>& gpu_v);

    template <typename T>
    Vector<T>     download_from_gpu(const GpuVector<T>& gpu_v);

    template <typename T> void  axpy(T alpha, const GpuVector<T>& x, GpuVector<T>& y);
    template <typename T> void  gemm(T alpha, const GpuMatrix<T>& A, const GpuMatrix<T>& B,
                                      T beta, GpuMatrix<T>& C);
    template <typename T> void  spmv(T alpha, const GpuSparseMatrix<T>& A,
                                      const GpuVector<T>& x, T beta, GpuVector<T>& y);
    template <typename T> SolverResult<T> gpu_pcg(const GpuSparseMatrix<T>& A,
                                                    const GpuVector<T>& b, ...);
    template <typename T> void  gpu_fft(GpuVector<std::complex<T>>& data, bool inverse = false);
}
```

**Tests (~32):**
- CPU-GPU parity: every op produces matching results within `f32`/`f64`
  precision tolerance.
- Performance: ≥10× CPU on suitable problem sizes (benchmark logged,
  not asserted).
- `UploadHandle` lifecycle works correctly for long-running solves.

**Smoke test:** `smoke_hesap_gpu` (GPU/window) — GPU CG solve on
Laplacian, residual norm verified.

**Definition of done:** all six configs build clean + `smoke_hesap_gpu`
green on machines with Vulkan + display.

---

## v18 — REPL + notebook + plug-in C ABI (~2 weeks)

**Goal:** Interactive REPL + file-backed notebook (`.cnb` `'CNBK'`
format) + plot integration via crd-renderer + plug-in C ABI per
ADR-0034.

**Public surface:**

```cpp
namespace crd::hesap::repl
{
    class Repl
    {
    public:
        Repl(IAllocator*);
        void execute(containers::StringView line);
        void render_imgui();  // ImGui frontend
        void load_notebook(containers::StringView path);
        void save_notebook(containers::StringView path);
    };
}

extern "C"
{
    // Plug-in C ABI (ADR-0034 pattern). Stable; never changes shape.
    struct CrdHesapApi
    {
        u32 version;
        // Function pointers for every public op...
        void* (*matrix_create)(usize rows, usize cols, u32 dtype);
        void  (*matrix_destroy)(void* m);
        // ... gemm, lu, cholesky, fft, ...
    };

    CrdHesapApi* crd_hesap_get_api();
}
```

**Tests (~24):**
- REPL: basic arithmetic + matrix construction + assignment + indexing.
- Notebook: save → reopen → execute → matches.
- Plug-in: external DLL loads, calls 10 representative ops, results
  match in-process.

**Definition of done:** all six configs green + REPL roundtrip test
green + plug-in C-ABI test green.

---

## Cross-module integration touchpoints

By the time Phase 3.1.6 ships, these consumers can plug in:

- **Eylem v7 FEM** — refactor the internal PCG (shipped during eylem
  v7) to use `crd-hesap-iterative::pcg` + `crd-hesap-direct::sparse_cholesky`.
  Determinism contract preserved (both inherit ADR-0063).
- **Eylem v9 differentiable** — refactor to use
  `crd-hesap-autodiff::reverse` for the gradient pass.
- **`crd-audio` (Phase 3.4)** — `crd-hesap-dsp` provides every filter,
  resampler, and spectral primitive the audio module needs.
- **`crd-renderer` (Phase 3.5+)** — large sparse problems for
  GI baking + IBL precompute; uses `crd-hesap-iterative` for the
  sparse linear systems.
- **MATLAB-class tool** — `crd-hesap-repl` is the entry point. ImGui
  in sandbox / editor surfaces the panel; standalone CLI tool available
  via the C ABI.
- **Future robotics module (Phase 8)** — `crd-hesap-opt` for motion
  planning; `crd-hesap-ode` for system dynamics integration.
- **Future medical sim (Phase 8)** — `crd-hesap-ode` (stiff biological
  systems), `crd-hesap-stats` (uncertainty quantification),
  `crd-hesap-eig` (modal analysis).

## Determinism contract

Every slice obeys ADR-0063 (eylem determinism contract) and
ADR-0065 §4. Same-input → byte-exact-output across compilers /
platforms / SIMD widths. Pairwise summation as the default reduction
tree; Kahan summation reserved for high-precision use.

The 9-config replay-hash CI matrix (eylem v9b) extends to also hash a
representative `crd-hesap` benchmark (CG solve on a fixed Laplacian) —
catches any cross-platform divergence in the substrate immediately.

## Test budget

| Slice cluster | Tests | Smokes |
| --- | :---: | --- |
| v0–v3 (dense LA) | ~128 | smoke_hesap_blas1 / blas2 / dense_solvers |
| v4–v8 (sparse + iterative + direct + eig) | ~160 | smoke_hesap_sparse / iterative |
| v9 (optimisation) | ~40 | smoke_hesap_opt |
| v10 (ODE) | ~32 | smoke_hesap_ode |
| v11–v12 (FFT + DSP) | ~56 | smoke_hesap_fft / dsp |
| v13–v14 (stats + interpolation) | ~60 | smoke_hesap_stats |
| v15–v16 (tensors + autodiff) | ~72 | smoke_hesap_tensor / autodiff |
| v17 (GPU) | ~32 | smoke_hesap_gpu (GPU/window) |
| v18 (REPL) | ~24 | smoke_hesap_repl |
| **Total** | **~604** | **~10 headless + 1 GPU/window** |

Adds ~+70 % to the post-Phase-3.1 test baseline (which itself is
~1500 by then). Numerical-accuracy benchmarks are a separate test
tier (run on CI but not gating most builds — own CTest label).

## Open questions (to resolve in-flight)

- **Complex number support depth.** v0 ships real-only; complex types
  woven through v1+ (`std::complex<T>`). Evaluate whether to bind a
  Cerid `Complex<T>` with bit-exact ops vs trusting `std::complex`.
- **Distributed memory (MPI-style).** Reserved out of v0–v18. Pickup
  when the scientific-computing tool ambition meets a cluster-scale
  workload (probably Phase 8+).
- **Jit-compiled kernels.** Like NumPy/JAX with XLA — runtime
  specialisation of `gemm` / `spmv` for hot shapes. Reserved for a
  later optimisation pass.
- **Symbolic computation (CAS).** Out of `crd-hesap` scope; if
  needed, would land as `crd-sembol` (Turkish: "symbol") — separate
  module with its own ADR.

## References

See ADR-0065 for the curated reference list. Additional sources:

- Eigen — header-only C++ template library reference (especially the
  `Block` / view semantics + sparse storage layout).
- LAPACK — Anderson et al. (1999); the canonical dense LA reference.
- SuiteSparse (CHOLMOD, UMFPACK, KLU, SPQR, AMD) — Davis et al. — the
  canonical sparse-direct reference suite.
- Saad (2003) — *Iterative Methods for Sparse Linear Systems*.
- Trilinos / PETSc — large-scale scientific computing reference
  architectures.
- SUNDIALS (CVODE, IDA, ARKODE) — ODE/DAE reference.
- IPOPT — interior-point NLP reference.
- OSQP — operator-splitting QP reference.
- FFTW — Frigo & Johnson "fastest FFT in the West" reference.
- Stan — Carpenter et al. — modern reverse-mode AD reference.
- JAX — Bradbury et al. — modern AD + tensor reference.
- MATLAB — closed-source but well-documented; user ergonomics
  reference.
