# Cerid `crd-hesap` — 2026 SOTA Update + Matrix Types + Complex + Elite Refinement

**Date:** 2026-05-19
**Status:** Research dossier — refines ADR-0065 + phase-3.1.6-hesap.md
to truly elite tier per user mandate 2026-05-19.
**Companion:** `cerid-agent-native-engine.md` (vision); `cerid-hesap.md`
(original 2026-05-10 research, ~602 lines, mostly still valid).

> **Mandate.** Push `crd-hesap` from "MATLAB-class numerical substrate"
> to **truly elite engineering-and-math substrate**: 2024-2026 SOTA
> algorithms, every matrix type that matters, full complex-number
> support, agent-native CLI surface from v0. The substrate is the
> source of truth — REPL/notebook/UI layer on top later.

---

## 1. Where the original plan stands (mature; needs SOTA refresh + scope additions)

The 2026-05-10 plan (ADR-0065 + phase doc + research dossier) is solid:

- ✅ Module split clean (14 sub-modules, each independently linkable).
- ✅ Determinism contract via ADR-0063 (bit-exact across SIMD widths).
- ✅ Algorithm canon (52-row table) covers LAPACK, SuiteSparse, Saad,
  SUNDIALS, OSQP, IPOPT, FFTW, Stan.
- ✅ Eigen-class C++ API + MATLAB-class facade.
- ✅ Allocator discipline + threading via `crd-jobs`.
- ✅ Plug-in C ABI + REPL + GPU mirror.

What it misses to reach truly elite:

1. **Matrix-type catalog is sparse.** Plan ships dense (row/col) +
   CSR/CSC/BSR/COO/ELL/HYB. Missing: banded, triangular, symmetric,
   Hermitian, Toeplitz, Hankel, circulant, Vandermonde, diagonal,
   block-diagonal, block-tridiagonal, hierarchical (HSS, BLR, H-matrices),
   sparse-skyline, JDS, Sliced ELL, modern-GPU formats (CSR5, Merge-CSR).
2. **No complex-number support.** Plan says "real-valued first."
   That breaks FFT (inherently complex), eigenvalue (non-symmetric is
   complex), QM / EM simulations, audio analysis, control theory.
   **Complex MUST be first-class from v0.**
3. **No linear-operator abstraction.** Every Krylov solver in the plan
   takes a concrete `Matrix<T>`. Matrix-free methods (FEM K-application
   without forming K, Hessian-vector for opt) are foundational; PETSc
   `Mat` and Trilinos `Tpetra::Operator` set the pattern.
4. **2024-2026 SOTA omissions** — listed in §3 below.
5. **No CLI surface mention.** Per user mandate 2026-05-19, every op
   ships with a CLI command from v0.
6. **Test scope light** (~30/slice). Cerid pattern is 100-200/slice.
   Property-based + reference-fixture + convergence-rate tests needed.
7. **No benchmark substrate.** `crd-hesap-bench` should be a sub-module
   tracking regression vs MKL/OpenBLAS reference + LAPACK accuracy
   baseline.

---

## 2. The matrix-type catalog (full coverage)

Every category gets a typed C++ representation + storage format + ops
+ converters between formats. **Multi-format storage is what makes a
substrate elite** — naive "dense + CSR" implementations force users
to convert, paying allocation cost on every conversion.

### 2.1 Dense matrix types

| Type | Storage | Use |
| --- | --- | --- |
| `Matrix<T, RowMajor>` | row-major `T[rows*cols]` | C/C++ canonical; cache-friendly for row-by-row access |
| `Matrix<T, ColMajor>` | column-major | LAPACK/Fortran canonical; required for many LAPACK-compatible BLAS calls |
| `MatrixView<T, L>` | strided view (rows, cols, ld) | sub-matrix; never owns memory |
| `DiagonalMatrix<T>` | 1D `Vector<T>` of diagonal entries | O(n) storage; many BLAS specializations |
| `IdentityMatrix<T>` | scalar size only | zero allocation; constexpr identity ops |
| `PermutationMatrix` | `Array<u32>` of permutation indices | LU pivots, QR column orderings, AMD/RCM reorderings |
| `TriangularMatrix<T, Uplo, Diag>` | dense storage (upper/lower; unit/non-unit) | Cholesky/LU factors; back-substitution |
| `SymmetricMatrix<T, Uplo>` | dense, half-stored | symmetric LA; Cholesky candidate |
| `HermitianMatrix<Complex<T>, Uplo>` | complex symmetric (conjugate-transposed) | quantum mech; signal covariance |
| `BandedMatrix<T>` | LAPACK band-storage `T[ld * cols]`, ld = kl+ku+1 | tridiagonal, pentadiagonal, ODE Jacobians, FDM matrices |
| `BlockDiagonalMatrix<T>` | array of blocks | sub-system decoupling; preconditioner structures |
| `BlockTridiagonalMatrix<T>` | array of (D_i, U_i, L_i) triples | FEM 1D problems, control state transitions |
| `ToeplitzMatrix<T>` | first column + first row (2n-1 elements) | linear convolution, time series |
| `HankelMatrix<T>` | first column + last row | signal processing, hidden-Markov decoders |
| `CirculantMatrix<T>` | first column (n elements) | FFT-diagonalizable; convolution; modular cyclic structures |
| `VandermondeMatrix<T>` | generator vector | polynomial interpolation, signal generation |

### 2.2 Sparse matrix types (general)

| Format | Layout | Best for |
| --- | --- | --- |
| **COO** (Coordinate) | `(rows[], cols[], values[])`, unsorted | construction, matrix-market I/O |
| **CSR** (Compressed Sparse Row) | `(row_ptr[N+1], col_ind[nnz], values[nnz])` | spmv hot path; FEM stiffness |
| **CSC** (Compressed Sparse Column) | transposed CSR | sparse LU column ops; left-looking factorization |
| **BSR** (Block Sparse Row) | block-of-blocks CSR; dense blocks of size BxB | FEM with vector unknowns per node; preconditioners with block structure |
| **ELL** (ELLPACK) | rectangular `(maxnz_per_row, N)`; padded | GPU-friendly; regular sparsity |
| **HYB** (Hybrid ELL + COO) | ELL part + COO overflow | mixed regular/irregular |
| **DIA** (Diagonal) | diagonals only | banded / structured-grid PDEs |
| **JDS** (Jagged Diagonal) | sorted-by-row-length permutation + COO | vector machines; rare modern use |
| **SkS / SkyLine** | profile storage | symmetric semi-bandwidth |
| **CSR5** (Liu-Vinter 2015) | partition-tile CSR for GPU | best-performing GPU spmv as of 2020 |
| **Merge-CSR / Merge-spmv** (Merrill-Garland 2016) | row-permutation by nnz | balanced GPU spmv on irregular sparsity |
| **Sliced ELLPACK** | banded ELL slices | GPU-friendly; modern variant |

### 2.3 Hierarchical / data-sparse matrix types (2024-2026 SOTA)

These are the "elite" category — dense matrices that compress well via
low-rank sub-blocks. **Used in: FEM K^-1, integral equations, Schur
complements, Hessian approximations, modern preconditioners.**

| Type | Reference | Use |
| --- | --- | --- |
| **HSS** (Hierarchically Semi-Separable) | Chandrasekaran 2006 | direct sparse Cholesky preconditioners; STRUMPACK |
| **H-matrix** (Hierarchical) | Hackbusch 1999, Bebendorf 2008 | integral equation discretizations; HLib reference |
| **H²-matrix** | Hackbusch-Khoromskij 2000 | tighter low-rank structure; near-linear complexity |
| **BLR** (Block Low-Rank) | Amestoy 2017 (MUMPS) | sparse direct factorization speedup |
| **Lossy Compressed Sparse** (LCS) | Anzt 2019 | low-precision representation in sparse |
| **TT** (Tensor Train) | Oseledets 2011 | high-dim PDEs, quantum simulation, NN compression |

### 2.4 Linear operator abstractions (PETSc `Mat` / Trilinos `Tpetra` pattern)

Not a storage format — a **function-pointer / closure / matrix-free
abstraction**. Every Krylov solver / iterative method should accept
this.

```cpp
template <typename T>
class LinearOp
{
public:
    virtual ~LinearOp() = default;
    virtual usize rows() const = 0;
    virtual usize cols() const = 0;
    virtual void apply(const Vector<T>& x, Vector<T>& y) const = 0;       // y = A * x
    virtual void apply_transpose(const Vector<T>& x, Vector<T>& y) const  // y = A^T * x
    {
        CRD_ASSERT(false && "transpose not provided");
    }
    // For preconditioners: y = M^-1 * x (M ≈ A; cheap to apply).
    virtual void apply_preconditioner(const Vector<T>& x, Vector<T>& y) const
    {
        y = x; // default: identity preconditioner.
    }
};

// Concrete materialisations:
//   DenseLinearOp<T>(Matrix<T>)
//   SparseLinearOp<T>(SparseMatrix<T>)
//   FunctionLinearOp<T>(std::function<void(const Vec&, Vec&)>)
//   CompositionLinearOp<T>(LinearOp* A, LinearOp* B)  // A * B
//   SumLinearOp<T>(LinearOp* A, LinearOp* B)          // A + B
//   ScaledLinearOp<T>(LinearOp* A, T alpha)           // alpha * A
//   PreconditionedLinearOp<T>(LinearOp* A, LinearOp* M)
```

Krylov methods consume `LinearOp<T>`, never concrete matrices:

```cpp
auto pcg = PCG<T>{}
    .operator_(op)                      // any LinearOp<T>: dense, sparse, matrix-free.
    .preconditioner(op_M)               // optional LinearOp<T>: any preconditioner.
    .tolerance(1e-8);
auto x = pcg.solve(b);
```

This lets the user pass a **closure that computes `A*x` without ever
forming A**:

```cpp
auto K_apply = [&fem_mesh, &E](const Vector<f64>& u, Vector<f64>& Ku) {
    fem_mesh.assemble_action(E, u, Ku);  // matrix-free assembly
};
auto K_op = make_function_op(N, N, K_apply);
auto u   = solve_pcg(K_op, f);            // solver doesn't know K is matrix-free
```

---

## 3. SOTA algorithm refresh (2024-2026)

### 3.1 Dense linear algebra (BLAS / LAPACK class)

**What's new since the 2003 LAPACK canon:**

1. **Mixed-precision iterative refinement (IR).** Factorize in `f32`,
   refine in `f64`. **3-4× speedup with full f64 accuracy.** Standard
   in LAPACK 3.10+ (`dsgesv`, `dsposv`), cuSOLVER, MAGMA. HPL-AI top500
   benchmark exists *because* of this technique (Higham et al. 2018+).
   Cerid v0 should support this from day 1 as `solve(A, b, IRConfig{})`.

2. **Communication-avoiding (CA-) algorithms.** Demmel-Grigori 2008+.
   CA-QR, CA-CG, CA-GMRES minimize data movement (which dominates
   modern hardware). Relevant for multi-socket NUMA, GPU, distributed.
   Tile-skinny-QR (TSQR) is the canonical CA primitive.

3. **Randomized linear algebra (RandNLA).** Halko-Martinsson-Tropp
   2011; Tropp 2019 survey. **Randomized SVD/QR/range-finder give
   provably-near-optimal results in O(n²k) for rank-k approximations
   vs O(n³).** Standard in scikit-learn, JAX, Julia. NIST RandLAPACK
   (2023+) packages the canonical implementations.

4. **Tile-based parallel LA (PLASMA / PaRSEC / DPLASMA pattern).**
   Buttari-Langou-Kurzak-Dongarra 2009+. Each tile op is a task;
   task-DAG scheduler load-balances across cores. **Fundamentally
   different model from fork-join BLAS** — composes naturally with
   `crd-jobs`. PaRSEC + StarPU + taskflow + OpenMP-tasks all share the
   pattern.

5. **Modern eigenvalue solvers.** Beyond LAPACK `dsyevr` / `dgeev`:
   - **MRRR** (Multiple Relatively Robust Representations, Dhillon
     1997) — `dsyevr` already uses this; current SOTA for symmetric.
   - **ELPA** (Marek 2014) — for very large symmetric, distributed.
   - **FEAST** (Polizzi 2009) — contour-integral-based; good for
     interior eigenpairs (band-pass filtering).
   - **TRLan / IRLBA** (Baglama-Reichel 2005) — Lanczos with implicit
     restart; canonical sparse partial eigenvalue.
   - **LOBPCG** (Knyazev 2001) — locally optimal block PCG for sparse
     symmetric; widely used in DFT, modal analysis.
   - **Davidson / Jacobi-Davidson** — for non-symmetric sparse.

6. **Modern SVD.**
   - Classical Golub-Reinsch is fine for small-to-medium.
   - **Randomized SVD** for large (Halko 2011).
   - **Truncated SVD via Lanczos bidiagonalization** (sparse case).
   - **Jacobi SVD** for highest accuracy (small matrices).

### 3.2 Sparse linear algebra

1. **Modern direct solvers.** SuperLU_DIST, PARDISO (Schenk), MUMPS,
   STRUMPACK (Ghysels 2016 — adds HSS compression), Pastix. Patterns
   to study: supernodal (CHOLMOD), multifrontal (MUMPS), block low-rank
   (MUMPS-BLR), HSS-augmented (STRUMPACK).

2. **AMG variants** (algebraic multigrid):
   - **Classical (Ruge-Stüben) AMG** — Cleary 2000. Robust for M-matrix
     class problems.
   - **Smoothed-Aggregation AMG (SA-AMG)** — Vaněk 1996. Better for
     vector-unknown systems (elasticity, electromagnetics).
   - **AGMG** (Notay 2010) — aggregation-based; commercial-grade.
   - **BootCAMG** (Brannick 2010) — bootstrap; adaptive AMG.
   - **Reference**: Hypre, AMGCL, PyAMG, ML/Trilinos.

3. **Krylov subspace recycling.** Parks-de Sturler-Mackey-Miller 2006;
   GCRO-DR, M-CG, recycled GMRES. **For sequences of related linear
   systems** (eylem time-step, optimization inner solves, time-dep PDE),
   reusing Ritz vectors from prior solves gives 2-5× speedup.
   Eylem v7 FEM is the canonical consumer.

4. **Modern preconditioners.**
   - **SPAI** (sparse approximate inverse, Grote-Huckle 1997) —
     embarrassingly parallel; no triangular solve in apply phase.
   - **ILUPACK / multilevel ILU** (Bollhöfer-Saad 2006) — for very
     ill-conditioned, indefinite, non-symmetric.
   - **Block-Jacobi with overlap** / **Additive Schwarz** — domain
     decomposition class.
   - **Polynomial preconditioners** — Chebyshev acceleration.
   - **Approximate factorization preconditioners** — incomplete
     Cholesky/LU with threshold dropping.

5. **Fast spmv (sparse matrix-vector).** Liu-Vinter 2015 CSR5; Merrill-
   Garland 2016 Merge-spmv; the gap between naive CSR and tuned spmv
   is 3-5× on irregular sparsity. Critical for iterative methods.

### 3.3 Optimization

1. **Modern QP**: OSQP (locked in plan) ✓. Also: SCS (Splitting Conic
   Solver, O'Donoghue 2016), qpOASES (active set, MPC-canonical),
   piqp (proximal interior point).

2. **NLP**: IPOPT (locked) ✓. Also: KNITRO (closed-source ref), SQP
   (more options: SNOPT-style), Augmented Lagrangian (LANCELOT-style).

3. **Trust-region** for unconstrained: Steihaug 1983; not in v9.
   L-BFGS is OK but trust-region handles non-convex better.

4. **Stochastic optimization**: Adam (Kingma-Ba 2014), AdaGrad,
   AdaDelta, NAG, Lion (Chen-2023). **Essential for ML / agent training
   workflows**; not in original plan.

5. **Algebraic modeling**: JuMP, CasADi, AMPL pattern. Eylem v9
   differentiable + opt connection benefits hugely.

6. **Discrete optimization**: not in scope (per ADR-0065 §11 reserved),
   correct.

### 3.4 ODE / DAE

1. **Modern stiff**: BDF (locked) ✓; also Rosenbrock-Wanner, SDIRK
   (singly-diagonally-implicit RK; Hairer-Wanner). RADAU5 reserved
   (locked). All good.

2. **DOPRI** non-stiff (locked) ✓. Also Verner methods (higher order
   embedded), Tsitouras (modern; superior to DOPRI5 for some classes).

3. **Symplectic integrators**: Verlet, Yoshida 4/6/8. For
   conservative/Hamiltonian systems (orbital mechanics, molecular
   dynamics). **Not in original plan; needed for aerospace / cinematic
   astrodynamics**.

4. **Implicit-Explicit (IMEX) methods**: for stiff-nonstiff splittings
   (advection-diffusion).

5. **Adaptive step + error control**: PI step controller (Gustafsson),
   sensitivity analysis.

6. **Sensitivity analysis** (CVODES / IDAS pattern): forward + adjoint
   for ODE/DAE. Bridges to autodiff.

### 3.5 FFT / DSP / spectral

Largely unchanged since FFTW3 (2005). What's modern:

1. **GPU FFT**: cuFFT / Stockham radix-mix on GPU; reference for
   `crd-hesap-gpu`.
2. **Sparse FFT** (Hassanieh 2012): sub-linear for k-sparse signals.
   Niche but elite.
3. **NUFFT** (Non-Uniform FFT, Greengard-Lee 2004): for irregular
   sample grids. Critical for MRI / interferometry / particle methods.
4. **Spectral element methods** (SEM): polynomial bases on each
   element; bridges PDE solvers + FFT.

### 3.6 Autodiff (v16 in original plan)

**Major SOTA shift since 2018 (JAX) + 2020 (Enzyme):**

1. **Operator-level AD (JAX pattern)**: register custom VJPs (vector-
   Jacobian products) per op rather than tracing through every
   multiply. `solve(A, b)`'s VJP is `solve(A^T, dx)` — never AD through
   LU! Lighter memory, faster, more accurate.

2. **Source-transformation AD (Enzyme, Moses-Churavy 2020)**: LLVM-IR
   level AD. Differentiates compiled binaries. Out of scope for v1
   but worth noting.

3. **Forward-over-reverse** for Hessians: `f''(x) v` via FOR mode.
   Cheaper than direct Hessian.

4. **Reverse-over-reverse** for Hessian-vector products in opt.

5. **Sparsity-aware AD**: Curtis-Powell-Reid coloring; chemical
   compression of Jacobians. Essential for FEM-class sparse Jacobians.

6. **Checkpointing** (Griewank 1992): for very long computations
   (training a NN), trade compute for memory. Out of scope for v1.

### 3.7 Statistical computing

1. **Modern distributions**: Stan-math reference (full PDF / CDF /
   quantile / sample for ~50 distributions). MCMC samplers (HMC, NUTS)
   reserved for v18+.

2. **Modern RNGs**: PCG (locked) ✓, Xoshiro256** (locked) ✓. Also:
   counter-based Threefry / Philox (essential for parallel-deterministic
   sampling — JAX's pattern).

3. **Statistical tests** (locked): t / chi² / KS / Mann-Whitney /
   ANOVA. Add: Wilcoxon signed-rank, Friedman, Kruskal-Wallis. Add
   bootstrap / jackknife resampling.

4. **Gaussian processes**: exact GP (cubic), sparse GP (Titsias 2009),
   variational GP. Not in plan; might be v17+ extension.

---

## 4. Complex-number support (first-class from v0)

**The original plan explicitly defers complex.** This is the biggest
single substrate gap. Adding complex from v0 is non-trivial but
not large — most BLAS/LAPACK routines have complex variants (`s/d` →
`c/z`).

### 4.1 Complex type

```cpp
namespace crd::hesap
{

template <crd::math::MathScalar T>
struct Complex
{
    T re;
    T im;

    constexpr Complex() noexcept = default;
    constexpr Complex(T r, T i = T{}) noexcept : re(r), im(i) {}
    constexpr Complex(const Complex&) noexcept = default;

    // Arithmetic: same algebra as std::complex but without std
    // dependency (per Cerid no-STL-containers rule applied broadly
    // to no-STL-value-types where possible).
    constexpr Complex operator+(const Complex& rhs) const noexcept
    { return {re + rhs.re, im + rhs.im}; }
    // ... operator-, operator*, operator/
    // ... conj(), abs(), arg(), polar(), exp(), log(), sqrt()
    // All via crd::math::deterministic primitives -- bit-portable across
    // CPU/GPU + SIMD widths.
};

using Complex32 = Complex<f32>;
using Complex64 = Complex<f64>;

}
```

**Why not `std::complex<T>`?** Cerid's broader posture is "implement
substrate types, don't wrap std." But `std::complex` is a value type
not a container; arguably fine to use. Decision: ship `crd::hesap::
Complex<T>` for full control over determinism + format + GPU layout
compatibility. Bridge from `std::complex` is a one-liner.

### 4.2 Complex BLAS variants

Every BLAS op has 4 precision/type variants matching the LAPACK
naming:

```
saxpy -> daxpy -> caxpy -> zaxpy
sgemm -> dgemm -> cgemm -> zgemm
```

Cerid's generic `template <typename T> axpy(...)` template
instantiates over `f32 / f64 / Complex32 / Complex64`. Special
care:

- `dot` becomes two ops: `dotu` (un-conjugated `x^T * y`) and `dotc`
  (conjugated `x^H * y`).
- `nrm2` computes `|x|` via `sqrt(sum |x_i|²)` — same for real/complex.
- `gemm` for complex doubles flop count.
- Symmetric vs Hermitian distinction matters (`syrk` vs `herk`,
  `symv` vs `hemv`).
- LU partial pivoting works unchanged; QR via Householder works
  unchanged (Householder reflectors are unitary in the complex case).
- Cholesky: SPD (real) becomes Hermitian-positive-definite (complex).

### 4.3 Complex-essential operations

| Op | Why complex |
| --- | --- |
| **FFT** | Inherently complex (forward FFT of real → complex spectrum) |
| **Eigenvalue (non-symmetric)** | Eigenvalues of real non-symmetric matrices are complex |
| **Schur decomposition** | Always complex (for general matrices) |
| **Hermitian eig** | Eigenvalues real but eigenvectors complex (in the complex case) |
| **QR with shifts** | Wilkinson-shift QR uses complex shifts in pairs |
| **Polynomial roots** | All complex (companion-matrix eigenvalue) |
| **DSP transforms** | DFT, DTFT, z-transform — all complex |
| **Control / Riccati** | State-space methods over complex when poles are complex |
| **Quantum simulation** | Inherently complex (state vectors) |
| **Electromagnetics** | Phasors (frequency domain) |

### 4.4 Phase / cycle counts

Splitting v0 / v1 / v2 / v3 into real-only is a false economy. Adding
complex variants doubles surface but adds ~30% LOC (most code is
templated on `T`; the four typedefs are essentially free). **Ship
complex from v0.**

---

## 5. Task-DAG architecture (the elite scheduling layer)

The original plan ships fork-join BLAS via `crd::jobs::parallel_for`.
Modern SOTA is **tile-based task-DAG**: each tile op is a job; deps
form a DAG; the scheduler load-balances.

### 5.1 Task-DAG primitives

```cpp
namespace crd::hesap::sched
{

// A task in the DAG: produces some output tiles, consumes some input tiles.
struct TaskNode
{
    StringView                   name;          // e.g. "gemm-tile(3,7)" for debugger
    Span<const TileHandle>       inputs;        // read deps
    Span<const TileHandle>       outputs;       // write deps
    std::function<void()>        kernel;        // the actual op
};

// Build the DAG, then dispatch via crd::jobs.
class TaskGraph
{
public:
    TaskHandle add(TaskNode node);
    void       commit_to_jobs();   // hands off to crd::jobs scheduler.
};

}
```

### 5.2 Tile-based `gemm`

Instead of:
```cpp
parallel_for(0..M*N tiles, [&](tile){ compute_tile(tile); });
```

Use:
```cpp
// Tile A (mtA × ntA), B (mtA × ntB), C (mtA × ntB)
TaskGraph dag;
for (i, j, k) in (mt, nt, kt):
    dag.add({
        .inputs  = {A[i,k], B[k,j]},
        .outputs = {C[i,j]},
        .kernel  = [&]{ gemm_tile(A[i,k], B[k,j], C[i,j]); }
    });
dag.commit_to_jobs();
```

This composes naturally with the rest of the engine: eylem's
`Physics` schedule phase issues a tile of work; the scheduler
balances across cores; no nested parallelism / oversubscription.

### 5.3 Reference patterns

- **PLASMA / DPLASMA** (Buttari 2009+): dense LA over task-DAGs.
- **PaRSEC** (Bosilca 2013): distributed task-DAG engine.
- **StarPU** (Augonnet 2011): heterogeneous task scheduling.
- **OpenMP-tasks / taskflow / TBB**: similar pattern in different APIs.

Cerid's `crd-jobs` is the task scheduler; `crd-hesap-sched` is the
LA-specific task-graph builder on top.

---

## 6. CLI protocol plumbing for hesap (sequencing locked 2026-05-19)

Per `cerid-agent-native-engine.md` + user direction 2026-05-19, hesap
ships **CLI protocol plumbing from v0 — but NOT an actual CLI parser
or REPL.** The `crd-cli` substrate (the parser + REPL + RPC server +
MCP layer) ships later in Phase 4.0. The plumbing-first approach
means crd-cli inherits hesap's full command surface "for free" when
it lands.

**What plumbing means concretely:**

1. **Every hesap operation has a `CommandSchema` declaration** (typed
   params, output, capability, version, deprecation status) registered
   via a static-init `cli::register_module_commands` hook. The hook
   simply stores the schemas in a `CommandRegistry` singleton at
   module-load time. No parser is wired yet.
2. **Structured output is the C++ return shape from day 1.** Every
   hesap entry point returns a `CommandResult`-shaped tagged union
   (or its typed C++ equivalent). This means today's C++ callers
   get the structured output, and tomorrow's CLI just renders it
   via TTY / JSON / YAML.
3. **MCP tool descriptors are auto-generated from schemas.** A static
   `export_mcp_descriptors()` function walks the registry and emits
   the MCP tool catalog (JSON). Today nobody reads it; tomorrow the
   `crd-rpc` MCP server reads it directly.
4. **No parser, no REPL, no RPC server in hesap.** Those are all
   `crd-cli` / `crd-rpc` substrate concerns. Hesap is content with
   "schemas registered + structured output produced."

This is the **protocol-first / substrate-later** discipline that
ADR-0076 §12 set when geometry shipped before eylem v1c.

### 6.1 v0 command surface (registered as schemas; CLI parser arrives in Phase 4.0)

```
# Matrix construction
hesap.dense.matrix.create --name M --rows N --cols M [--type real|complex] [--precision f32|f64] [--layout row|col]
hesap.dense.matrix.from-data --name M --data <json|csv|npy|crdv>
hesap.dense.matrix.fill --matrix M --value V
hesap.dense.matrix.random --matrix M --distribution uniform|normal|spd|orthogonal --seed S
hesap.dense.matrix.eye --name M --size N
hesap.dense.matrix.diag --name M --values <vec>
hesap.dense.matrix.from-blocks --name M --blocks <json>
hesap.dense.matrix.load --name M --from <path>
hesap.dense.matrix.save --matrix M --to <path>

# Vector
hesap.dense.vector.create --name v --size N
hesap.dense.vector.from-data --name v --data <json>
...

# BLAS L1
hesap.blas1.axpy --alpha A --x VX --y VY --inplace true|false [--out V_OUT]
hesap.blas1.dot --x VX --y VY [--complex-form u|c]
hesap.blas1.nrm2 --x VX
hesap.blas1.scal --alpha A --x VX
hesap.blas1.iamax --x VX

# BLAS L2
hesap.blas2.gemv --alpha A --M M --x VX --beta B --y VY [--trans none|trans|conjtrans]
hesap.blas2.ger --alpha A --x VX --y VY --M_inout M
hesap.blas2.trsv --M M --x VX [--uplo upper|lower] [--unit-diag false|true]
...

# BLAS L3
hesap.blas3.gemm --alpha A --A MA --B MB --beta B --C MC [--trans-a none|trans|conjtrans] [--trans-b ...]
hesap.blas3.syrk --alpha A --A MA --beta B --C MC --uplo upper|lower
hesap.blas3.trsm --M M --B MB [--side left|right] [--uplo upper|lower]
...

# Direct solvers
hesap.solver.lu --matrix M --name LU
hesap.solver.cholesky --matrix M --name CHOL [--mode upper|lower]
hesap.solver.qr --matrix M --name QR
hesap.solver.ldlt --matrix M --name LDLT
hesap.solver.solve --factor LU|CHOL|QR|LDLT --rhs B --name X
hesap.solver.solve_iter_refine --A M --b VB --name X --ir-precision f32|f64

# Inspection
hesap.dense.matrix.info --matrix M  # rows, cols, layout, type, dtype, density, conditioning estimate
hesap.dense.matrix.condition --matrix M --norm 1|inf|2|fro
hesap.dense.matrix.norm --matrix M --norm 1|inf|2|fro
hesap.dense.matrix.rank --matrix M --tolerance T
hesap.dense.matrix.print --matrix M [--max-rows R] [--max-cols C] [--precision P]

# Reordering / structure
hesap.dense.transpose --matrix M --name MT [--conjugate true|false]
hesap.dense.permute_rows --matrix M --perm P
hesap.dense.submatrix --matrix M --rows R0:R1 --cols C0:C1 --name SUB
```

### 6.2 Structured output format

Every CLI command returns JSON (over RPC) or text (TTY):

```json
{
    "ok": true,
    "result": {
        "kind": "matrix-factor",
        "factor_id": "LU_42",
        "n_pivots": 3,
        "condition_estimate": 1.2e7
    },
    "diagnostics": [
        {"level": "info", "msg": "Used 4 thread tiles; total flops 1.6e9"},
        {"level": "warn", "msg": "Condition estimate > 1e6; consider iterative refinement"}
    ],
    "provenance": {
        "session_id": "ses_2026_05_19_103",
        "command_id": 47,
        "parent_command_id": 46
    }
}
```

### 6.3 Notebook / REPL syntax (Phase 4.0+2, after `crd-cli`)

Cerid notebook cells contain **C++ snippets** (locked 2026-05-19:
C++ hot-reload via DLL is the only scripting language). Cell eval
compiles the snippet into a hot-reload DLL, runs it, captures the
structured output, renders the result.

```cpp
// CerScript cell. Compiled to a hot-reload DLL on eval.
auto A = hesap::dense::random_matrix<f64>(1000, 1000, hesap::dist::Normal);
auto b = hesap::dense::random_vector<f64>(1000);
auto x = hesap::solve(A, b);  // dispatches LU + iterative refinement.
plot(x);                       // routes to a renderer panel via the CLI layer.
```

The cell engine cooks the snippet (via the same `crd-resources` +
`crd-script` cooker chain), hot-reloads the resulting DLL, calls the
entry point, captures the typed result, renders it (table for matrix,
chart for vector, scalar for number, ...).

This is the same loop AI agents run: agent emits C++ → cooker compiles
→ hot-reload DLL → call → inspect typed output → next iteration. No
separate scripting language. One language for everything.

---

## 7. Test scope — elite numerical verification

Each slice needs ~100-200 tests across these tiers:

### 7.1 Tier 1: example-based correctness

Specific inputs with hand-computed reference outputs. ~20-40 tests per
slice. Covers boundary cases (size 0, 1, 2; ones; zeros; identity;
non-conformable).

### 7.2 Tier 2: property-based / invariant tests

Generate random inputs satisfying constraints; verify mathematical
invariants:

- Cholesky on random SPD matrix → `L * L^T == A` within `n * eps`.
- QR on random matrix → `Q^T * Q == I` within `n * eps`; `Q * R == A`.
- SVD → singular values descending, `U * S * V^T == A`.
- Solve `Ax = b` → residual norm `|A*x - b| < cond(A) * eps * |b|`.
- BLAS L3 `gemm(alpha, A, B, beta, C)` → bit-exact match for
  `(alpha, beta) = (1, 0), (0, 1), (1, 1)`.

Tools: Catch2 generators, custom `RandomMatrix<T>` factory.

### 7.3 Tier 3: reference-fixture replay

Generate against LAPACK / SuiteSparse / FFTW once; commit binary
fixtures; replay-compare in CI.

```
tests/fixtures/lapack/dgesv_random_100x100.crdv
                       (input A, b)
tests/fixtures/lapack/dgesv_random_100x100_ref.crdv
                       (LAPACK output x)
```

CI: `crd-hesap-test` reads fixture, calls `solve(A, b)`, compares to
ref. Catches regressions reliably.

### 7.4 Tier 4: convergence-rate tests

For iterative methods: verify ε-vs-iteration matches theoretical rate.

- PCG with Jacobi preconditioner on Poisson matrix → 30 iterations
  for 1e-8 tolerance on 1000² grid.
- L-BFGS on Rosenbrock → super-linear convergence rate.
- BDF on stiff Robertson → asymptotic step-size convergence.

### 7.5 Tier 5: numerical accuracy benchmarks

Separate from unit tests; tracked over time:

- `gemm` flops/sec ≥ 70% of theoretical peak on AVX2.
- `dpotrf` accuracy ≤ 1.5× LAPACK on Hilbert(N) matrices.
- `pcg` convergence: iterations within 10% of theory across 100
  random SPD matrices.

CI gate: regression > 10% → fail.

### 7.6 Tier 6: determinism + replay

Per ADR-0063: same input → same output bit-exact across:
- SSE2 / AVX2 / AVX-512 / NEON / SVE2 / scalar.
- 6 build configs (debug / asan / shipping / release / clang-cl / tidy).
- Linux + Windows.

Replay-hash test: run BLAS L1 → BLAS L3 → direct solve, hash final
state, verify across CI configs.

---

## 8. Benchmark substrate (`crd-hesap-bench`)

A separate sub-module tracking regression vs reference. NOT a unit
test — runs separately:

```
crd-hesap-bench --suite blas3.gemm --size 1000 --type f32 --backend auto
crd-hesap-bench --suite pcg-poisson --size 100x100x100 --preconditioner jacobi
crd-hesap-bench --suite fft1d --size 2^20 --type complex32
```

Output: JSON with achieved flops / accuracy / iteration count. CI
tracks trends; regression > 10% fails.

Reference baselines:
- `gemm` vs OpenBLAS / MKL (target ≥ 80% MKL).
- `pcg` vs PETSc / SuiteSparse-AMG.
- `fft` vs FFTW (target within 20%).
- `lu / cholesky / qr` vs LAPACK.

---

## 9. Refined v0 plan (elite tier)

The locked phase doc has v0 = "module skeleton + dense BLAS L1" in
~1.5 weeks. Refined elite-tier plan splits into 5-6 sub-slices over
~4-5 weeks (matching Phase 3.1.7's v0-cluster pattern). User mandate
2026-05-19: matrix types + complex + CLI from day 1.

### 9.1 v0a — Substrate scaffolding (~3 days)

- `crd-hesap` module skeleton (CMakeLists, anchor TU, include layout).
- `crd-hesap-dense` sub-module skeleton.
- `crd::hesap::Complex<T>` value type (with arithmetic, `crd::math::
  deterministic` transcendentals; bit-portable).
- `Vector<T>` + `Matrix<T, Layout>` (real + complex; f32/f64).
- `MatrixView<T, L>` strided view.
- `DiagonalMatrix<T>`, `IdentityMatrix<T>`, `PermutationMatrix`,
  `TriangularMatrix<T, Uplo, Diag>`, `SymmetricMatrix<T, Uplo>`,
  `HermitianMatrix<Complex<T>, Uplo>`, `BandedMatrix<T>`.
- `LinearOp<T>` abstract base + 6 concrete materialisations.
- `MatrixId` / `VectorId` strong-typed handles (for CLI / RPC
  state).
- Allocator discipline + format-pinning (CRDV `'HDV0'` for dense
  matrix on disk).

### 9.2 v0b — BLAS L1 (~3 days)

- All 8 L1 ops (axpy / dot{u,c} / nrm2 / scal / copy / swap / asum
  / iamax).
- f32 / f64 / Complex32 / Complex64 instantiations.
- SIMD via Vec4f/Vec8f for real, vector-of-complex for complex.
- Kahan-pairwise reduction for bit-exact across SIMD widths.
- Property-based + reference-fixture tests; ~80 cases / ~400
  assertions.
- CLI surface: 8 ops × 4 variants exposed as `hesap.blas1.*`.

### 9.3 v0c — BLAS L2 (~5 days)

- All L2 ops (gemv / gbmv / hemv / hbmv / symv / sbmv / ger / geru
  / gerc / syr / her / syr2 / her2 / trmv / trsv / tbmv / tbsv).
- f32 / f64 / Complex32 / Complex64.
- Banded / triangular / symmetric / Hermitian variants.
- Strided matrix views.
- ~80 cases / ~400 assertions.
- CLI surface.

### 9.4 v0d — BLAS L3 + task-DAG `gemm` (~7 days)

- Tile-based `gemm` over `crd-jobs` (task-DAG dispatch).
- Microkernel: AVX2 8x8 + AVX-512 16x16 + NEON 4x8 + SVE2-runtime + scalar.
- syrk / herk / syr2k / her2k / trmm / trsm.
- f32 / f64 / Complex32 / Complex64.
- Benchmark target: ≥ 70% AVX2 / AVX-512 peak.
- Mixed-precision dispatch helper (`f32`-factor → `f64`-refine).
- ~100 cases / ~600 assertions.
- CLI surface.

### 9.5 v0e — Dense direct solvers (~7 days)

- LU with partial pivoting + iterative refinement + mixed-precision
  variant.
- Cholesky (real SPD + complex Hermitian-PD).
- QR via Householder (with column-pivoting QR option).
- LDLT for symmetric/Hermitian indefinite.
- `LinearOp<T>` produced by each factorization (for downstream
  Krylov coupling).
- Condition estimation (1-norm, inf-norm).
- Mat-Mat `trsm` for batched RHS.
- ~120 cases / ~700 assertions covering condition sweeps + iterative
  refinement + complex variants.
- CLI surface: `hesap.solver.*`.

### 9.6 v0f — Benchmark substrate + system doc (~5 days)

- `crd-hesap-bench` sub-module: GEMM / Cholesky / LU benchmark
  harness vs OpenBLAS / LAPACK reference.
- Reference fixtures committed (binary `.crdv` files in `tests/
  fixtures/hesap/`).
- Property-based test framework (`RandomMatrix<T>` factory).
- System doc `docs/systems/hesap-dense.md`.

### 9.7 v0-close

- ADR-0065 §13 amendment locking v0 decisions (complex + matrix-type
  catalog + task-DAG `gemm` + mixed-precision IR + CLI surface).
- 18-config full sweep.

**Total v0 elite scope: ~5 weeks, ~5000 LOC engine + ~3500 LOC tests
+ ~500 LOC CLI registration + ~400 LOC bench harness.** vs original
~1500 LOC in 1.5 weeks.

This is the price of elite. Worth it because every downstream slice
(v1 sparse, v6 iterative, v7 sparse direct, v8 eig, v9 opt, v16 AD)
benefits from the v0 foundation being right.

---

## 10. The full refined 18-slice plan (summary deltas)

Highlights of the deltas vs the locked phase doc:

| Slice | Original | Refined elite |
| --- | --- | --- |
| v0 | dense L1 only (~1.5 wk) | substrate + BLAS L1+L2+L3 + dense direct + CLI + bench (~5 wk) |
| v1 | dense L2 (~2 wk) | **sparse storage + spmv** (moved from original v4); BLAS L2 already in v0 |
| v2 | dense L3 + direct (~3 wk) | **fill-reduce reorderings (AMD/RCM/Nested-Dissection)** + symbolic factorization; v0 already shipped BLAS L3 + direct |
| v3 | SVD + eig (~2 wk) | **SVD + dense eig + randomized variants** + complex variants throughout |
| v4 | sparse storage + spmv (~2 wk) | **iterative solvers** (CG, PCG, BiCGSTAB, GMRES, MINRES, LSQR, IDR(s)) + **Krylov subspace recycling** (M-CG, GCRO-DR) + modern preconditioners (SPAI, ILUPACK-style multilevel ILU, polynomial) |
| v5 | reorderings (~1.5 wk) | **sparse direct** (supernodal Cholesky, left-looking LU, multifrontal QR) + **AMG variants** (classical, SA-AMG, AGMG, BootCAMG) + **HSS / BLR compression** + **STRUMPACK pattern** |
| v6 | iterative solvers (~2.5 wk) | **sparse eig** (Lanczos with restart, LOBPCG, IRA, FEAST) + dense randomized eig |
| v7 | sparse direct + AMG (~3 wk) | **opt v1** — unconstrained (L-BFGS, trust-region Steihaug, Newton, BFGS) + line search + **Adam/AdaGrad/Lion** for ML class |
| v8 | sparse eig (~2 wk) | **opt v2** — constrained (QP via OSQP + SCS, LP simplex + interior-point, NLP IPOPT-class, SQP) |
| v9 | opt (~3 wk) | **ODE/DAE** — DOPRI5/8 + BDF1-6 + Rosenbrock + SDIRK + Verner + Tsitouras + symplectic Verlet/Yoshida; IMEX; sensitivity (forward + adjoint) |
| v10 | ODE/DAE (~2.5 wk) | **FFT** (Cooley-Tukey, Stockham, Bluestein, Rader; NUFFT; sparse FFT; complex + RFFT; multidim) + DCT/DST/Hartley |
| v11 | FFT (~2 wk) | **DSP** (FIR, IIR, biquad, polyphase resampling, spectral analysis, Welch PSD, Bartlett) |
| v12 | DSP (~2 wk) | **stats + distributions + RNG + statistical tests + special functions** |
| v13 | stats (~2 wk) | **polynomial / interpolation / quadrature** (cubic/Akima/Hermite/Chebyshev splines; Gauss-Legendre/Hermite/Laguerre/Lobatto/Radau quadrature; adaptive Simpson; Romberg; Clenshaw-Curtis) + **special functions** (gamma, beta, erf, Bessel, Legendre, Hermite, Chebyshev) |
| v14 | interp + quad (~1.5 wk) | **N-dim tensors** + broadcasting + einsum + reductions + reshape/slice/gather/scatter |
| v15 | tensors (~2 wk) | **autodiff v1** — forward mode (dual / Jet) + per-op manual VJP definitions for entire BLAS surface |
| v16 | autodiff (~3 wk) | **autodiff v2** — reverse mode (tape OR JAX-style structural AD via expression graph); sparse Jacobian (Curtis-Powell-Reid coloring); checkpointing |
| v17 | GPU mirror (~3 wk) | **GPU mirror** for dense + sparse + iterative + FFT + autodiff via `crd-rhi-compute` |
| v18 | REPL + notebook (~2 wk) | **REPL + notebook** (`.cnb` CRDR-format) + plot integration + plug-in C ABI + `crd-cli` consumer surface |

**Total elite scope: ~52 KLOC engine + ~600 tests + ~38 weeks** (same
schedule as original; surface is fuller per slice but slices fit the
same duration windows because the v0 expansion concentrates the
foundation work upfront).

---

## 11. Connection to the agent-native vision

Every slice ships its CLI surface alongside the C++ API (per
`cerid-agent-native-engine.md`):

- v0: `hesap.dense.*`, `hesap.solver.*`, `hesap.bench.*`.
- v1: `hesap.sparse.*`.
- v4: `hesap.solve.{cg,pcg,bicgstab,gmres,minres,lsqr,idr}`.
- ...
- v15: `hesap.autodiff.{forward,reverse,grad,hess,vjp,jvp}`.
- v17: `hesap.gpu.*`.
- v18: REPL launcher.

By the time v18 ships, the entire numerical surface is agent-driveable
end-to-end. **Cerid is the first engine where an AI agent can author,
simulate, analyze, optimize, and visualize a robotics / aerospace /
cinematic / scientific workflow without any GUI.**

---

## 12. Open questions / decisions needed

1. **Complex type — own or std::complex<T>?** Recommendation: own,
   for determinism control + GPU layout pinning. Bridge from std is
   one-liner.
2. **Random matrix factories for tests** — `RandomMatrix<T>` API.
   Should support distribution control (normal, uniform, SPD,
   orthogonal, Hilbert, Vandermonde) + condition-number control.
3. **Reference fixture format** — `.crdv` (CRDR with `'HDV0'` FourCC)
   or `.npy` (NumPy compatibility)? Both? `.crdv` for determinism;
   `.npy` for tooling interop.
4. **Task-DAG scheduler — own or external?** Plan said `crd-jobs`
   directly; refinement: thin task-DAG layer on top of `crd-jobs` in
   `crd-hesap-sched` sub-module.
5. **CLI module split** — `crd-hesap-cli` sub-module, or per-sub-module
   CLI registration? Per-sub-module via static-init hook is cleaner +
   composes with other modules' CLI.
6. **Float16 / BFloat16 support?** Mixed-precision IR exploits this.
   Modern hardware (NVIDIA Hopper, Apple AMX, Intel AMX) has native
   `bfloat16` / `fp16` paths. Out of scope for v0 but reserve.
7. **Distributed parallelism (MPI)** — out of scope per ADR §11. Keep
   reserved.

---

## 13. Action items

- **Mint ADR-0065 §13 amendment** locking the v0 elite-tier decisions
  + matrix-type catalog + complex from v0 + task-DAG `gemm`.
- **Refine `phase-3.1.6-hesap.md`** to incorporate the deltas in §10.
- **Mint ADR-0081** (agent-native engine; companion).
- **Update PRINCIPLES.md** with the agent-native cornerstone.
- **Add to ROADMAP.md**:
  - Phase 3.1.6 v0 elite tier (~5 wk) — slot before eylem v1c.
  - New Phase 4.0 = `crd-cli` + `crd-rpc` substrate (~12 wk).
  - Cross-cuts: per-module CLI back-fill.

## References (2024-2026 SOTA additions vs original)

- Higham (2018+) — mixed-precision iterative refinement papers.
- HPL-AI Top500 entries (2019+) — mixed-precision dense LU on Summit / Frontier.
- Tropp (2019) — *Randomized numerical linear algebra: foundations
  and algorithms*. Acta Numerica.
- Halko, Martinsson, Tropp (2011) — randomized SVD.
- Demmel-Grigori (2012+) — communication-avoiding algorithms.
- Buttari, Langou, Kurzak, Dongarra (2009+) — PLASMA tile-LA.
- Bosilca et al. (2013) — PaRSEC distributed task-DAG.
- Augonnet et al. (2011) — StarPU heterogeneous task scheduling.
- Parks, de Sturler, Mackey, Miller (2006) — recycled GMRES.
- Soodhalter (2014+) — Krylov subspace recycling surveys.
- Notay (2010) — AGMG aggregation-based AMG.
- Vaněk, Brezina, Mandel (1996) — smoothed-aggregation AMG.
- Brannick et al. (2010) — bootstrap AMG.
- Grote, Huckle (1997) — sparse approximate inverse SPAI.
- Bollhöfer, Saad (2006) — ILUPACK multilevel ILU.
- Liu, Vinter (2015) — CSR5 GPU spmv.
- Merrill, Garland (2016) — Merge-spmv.
- Hackbusch (1999) — hierarchical matrices.
- Bebendorf (2008) — *Hierarchical Matrices*. Springer.
- Amestoy, Buttari, L'Excellent, Mary (2017) — block low-rank (MUMPS-BLR).
- Ghysels et al. (2016) — STRUMPACK (HSS-augmented direct).
- Chandrasekaran et al. (2006) — HSS structure.
- Marek et al. (2014) — ELPA eigenvalue.
- Polizzi (2009) — FEAST.
- Knyazev (2001) — LOBPCG.
- Baglama, Reichel (2005) — IRLBA.
- Bradbury et al. (2018) — JAX (operator-level AD via expression graphs).
- Moses, Churavy (2020) — Enzyme (LLVM source-transformation AD).
- Kingma, Ba (2014) — Adam.
- Chen et al. (2023) — Lion.
- Loshchilov, Hutter (2019) — AdamW.
- Hairer, Wanner (1996) — *Solving Ordinary Differential Equations II* (Stiff + DAE).
- Tsitouras (2011) — modern RK methods.
- Hassanieh, Indyk, Katabi, Price (2012) — sparse FFT.
- Greengard, Lee (2004) — non-uniform FFT.
- O'Donoghue, Chu, Parikh, Boyd (2016) — SCS conic optimization.
- Anzt, Cojean, Quintana-Ortí (2019) — lossy compressed sparse.
- Oseledets (2011) — tensor train decomposition.
