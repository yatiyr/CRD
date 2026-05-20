# ADR-0065 — `crd-hesap` numerical computing substrate

**Date:** 2026-05-10
**Status:** Accepted
**Tags:** [arch] [hesap] [math] [solvers] [autodiff] [optimization] [dsp] [gpu] [scripting]

## Context

Cerid's multi-domain mandate (games + robotics + medical + cinematic +
DAW + scientific computing tools) demands a **MATLAB-class numerical
substrate**. Every domain pulls on a different part of this substrate:

| Domain | Pulls |
| --- | --- |
| Games | small dense LA (already in `crd-math`); minor stats |
| Eylem v7 (FEM) | sparse CG/PCG, sparse Cholesky, AMG preconditioner |
| Eylem v9 (differentiable) | reverse-mode autodiff, gradient propagation |
| Robotics | constrained optimization (SQP, IPOPT-class), QP, ODE solvers, motion planning |
| Medical | ODE/DAE (stiff biological systems), PDE primitives, statistics |
| Cinematic VFX | numerical sim (fluids, deformation), FFT, large sparse |
| DAW | DSP (FIR/IIR filters, biquad, FFT, convolution, resampling, spectral) |
| MATLAB-class tool | **all of the above** + REPL + plug-in API + notebook surface |

Currently `crd-math` is a small primitive layer (Vec/Mat/Quat/Transform
+ SIMD wrappers + deterministic stdlib). Pushing 50–100 KLOC of
sparse / iterative / direct / eig / opt / ODE / FFT / stats / autodiff
into it would:

1. Bloat every dependent module's compile time (RHI, renderer, scene,
   eylem, sdf, app — all pull `crd-math`).
2. Violate module isolation (CLAUDE.md §7) — a DAW build doesn't need
   GMRES; a pure-game build doesn't need eigenvalue solvers.
3. Conflate two very different testing tiers — `crd-math` correctness
   vs `crd-hesap` numerical accuracy benchmarks (convergence rates,
   condition-number stress, residual norms vs LAPACK reference).
4. Make the MATLAB-class tool ambition harder to surface as a
   first-class engine pillar.

`docs/research/sparsematrices.md` (existing Cerid research) already
established the sparse-matrix background. ADR-0065 extends that into a
full substrate decision.

## Decision

Cerid ships **`crd-hesap`** (Turkish: *computation / calculation /
reckoning*) as a standalone numerical substrate — peer of
`crd-math`, `crd-jobs`, `crd-eylem`, `crd-sdf`. The name follows the
existing Turkish-named-module pattern (Eylem = action/physics, Öbek =
cluster/prefab, Hesap = computation/numerics).

**The substrate IS the interface.** No third-party wrap. Reference
implementations (LAPACK/BLAS, SuiteSparse, FFTW, Eigen, MKL, SUNDIALS,
IPOPT, OSQP, Stan, JAX) inform algorithm choice — not source code.

### 1. Module split

```
crd-hesap                                ← this ADR locks
  └─ engine/hesap/
       include/crd/hesap/                — public umbrella headers
       src/                               — top-level glue
       sub-modules (each compiles independently, opt-in link):
         crd-hesap-dense                  — BLAS L1/L2/L3 + LAPACK-class dense LA
         crd-hesap-sparse                 — CSR/CSC/BSR/COO/ELL/HYB + spmv/spmm/spgemm
         crd-hesap-iterative              — CG/PCG/BiCGSTAB/GMRES/MINRES/LSQR/IDR(s)
         crd-hesap-direct                 — sparse LU/Cholesky/QR/LDLT (multifrontal)
         crd-hesap-eig                    — eigenvalue solvers (dense + sparse)
         crd-hesap-opt                    — L-BFGS / SQP / IPOPT-class / OSQP / LP / MIP
         crd-hesap-ode                    — ODE / DAE / PDE primitive solvers
         crd-hesap-fft                    — FFT + DCT/DST/Hartley
         crd-hesap-dsp                    — FIR/IIR filters, biquad, resample, spectral
         crd-hesap-stats                  — distributions, RNG, statistical tests
         crd-hesap-tensor                 — N-dim arrays + broadcasting + einsum
         crd-hesap-autodiff               — forward + reverse mode AD over BLAS
         crd-hesap-gpu                    — GPU dense + sparse + FFT + iterative
         crd-hesap-repl                   — interactive REPL + plug-in C ABI surface
```

Each sub-module is a separate CMake target. Consumers link only the
parts they need. `crd-hesap` (the umbrella) is a meta-target that
pulls the common foundation; downstream code typically links one or
two sub-modules, not the whole stack.

### 2. Module dependencies (one-way, no cycles)

```
crd-hesap-* depend on:
  crd-core        (types, asserts, platform)
  crd-memory      (IAllocator, GrowablePool)
  crd-containers  (Array, HashMap, sort)
  crd-math        (Vec/Mat/Quat — small fixed-size; SIMD wrappers from Phase 3.1 v0)
  crd-jobs        (parallel_for for parallel BLAS, parallel CG, parallel reductions)

crd-hesap-gpu also depends on:
  crd-rhi         (compute shader dispatch + buffer upload)
  crd-renderer    (UploadHandle / Fence reuse from ADR-0061)

crd-hesap-repl also depends on:
  crd-imgui       (debug REPL panel; Phase 7 editor takes over later)
  crd-platform    (file watcher for notebook reload)

crd-hesap does NOT depend on:
  crd-scene, crd-eylem, crd-sdf
  (those depend on us)
```

`crd-math` stays lean. `crd-hesap` is the heavy substrate.

### 3. Algorithm choice posture (closed)

For each problem class, ADR-0065 commits to one canonical algorithm
with documented alternatives reserved. The phase plan
(`docs/phases/phase-3.1.6-hesap.md`) implements them.

| Problem | Chosen algorithm | Reference | Reserved alternatives |
| --- | --- | --- | --- |
| Dense `gemm` | Goto/BLIS layered (microkernel + outer panels) | BLIS 2014, OpenBLAS | Strassen for very large; intrinsics-tuned |
| Dense LU | Doolittle + partial pivoting | LAPACK `dgetrf` | Recursive blocked LU |
| Dense Cholesky | Right-looking blocked | LAPACK `dpotrf` | Left-looking |
| Dense QR | Householder | LAPACK `dgeqrf` | Givens (banded), randomized |
| Dense SVD | Golub-Reinsch (Hou + bidiag QR) | Golub & Van Loan | Jacobi (small); randomized SVD (large) |
| Symmetric eig | QR with Wilkinson shift after Householder tridiag | LAPACK `dsyevr` | Divide-and-conquer; bisection |
| Non-sym eig | Schur via QR with double-shift | LAPACK `dgeev` | Hessenberg form; Krylov |
| Sparse CG | Saad classic | Saad 2003 | Pipelined CG; chronopoulos-Gear |
| Sparse PCG | Jacobi / IC(0) / ILU(0) preconditioners | Saad 2003 | block-Jacobi, AMG (own slice) |
| Sparse BiCGSTAB | Van der Vorst 1992 | — | TFQMR |
| Sparse GMRES | Restarted GMRES(m) + classical Gram-Schmidt + reorthog | Saad 2003 | Pipelined GMRES |
| Sparse Cholesky | Supernodal (CHOLMOD-class) | Davis 2006 | Multifrontal (own variant) |
| Sparse LU | Left-looking (Gilbert-Peierls) | Davis 2006 | Supernodal LU |
| Sparse QR | Multifrontal | Davis 2011 | Householder+SuiteSparseQR-class |
| Fill-reduce ordering | AMD (approximate minimum degree) | Amestoy 1996 | Nested dissection (METIS-class own port); RCM |
| Sparse symmetric eig | Lanczos with restart | Saad 2011 | LOBPCG; FEAST |
| Sparse non-sym eig | Implicitly restarted Arnoldi (IRA) | ARPACK | Krylov-Schur; Jacobi-Davidson |
| Unconstrained opt | L-BFGS + Wolfe line search | Nocedal-Wright | trust-region Steihaug; Newton |
| QP | OSQP-style ADMM | OSQP 2020 | Active-set; interior-point |
| NLP | Mehrotra interior-point | IPOPT model | SQP |
| LP | Revised primal simplex | — | Mehrotra interior-point |
| Stiff ODE | BDF (1–6) with Newton-Krylov inner | SUNDIALS CVODE | Rosenbrock |
| Non-stiff ODE | DOPRI5 + DOPRI8 (FSAL) | Hairer-Wanner | Cash-Karp; PI step controller |
| DAE | BDF with Pantelides index reduction | SUNDIALS IDA | RADAU5 |
| FFT | Cooley-Tukey mixed-radix + Bluestein for primes | Frigo-Johnson FFTW | Stockham; Rader |
| Real FFT | RFFT via half-size complex + post-processing | FFTPack | Split-radix |
| FIR design | Windowed sinc + Parks-McClellan (Remez) | Oppenheim-Schafer | Frequency sampling |
| IIR design | Bilinear transform + analog Butterworth/Cheb/Elliptic | Oppenheim-Schafer | Impulse invariance |
| RNG | PCG splittable (counter-based) + Xoshiro256** for fast non-splittable | O'Neill 2014 / Blackman-Vigna 2018 | Philox; ChaCha20 |
| Reverse-mode AD | Tape-based with operator overloading | Stan-math model | Source transformation; expression templates |
| Forward-mode AD | Dual numbers (Jet types) | Hyper-dual for second-order | — |
| GPU dense | Compute-shader BLIS-style microkernels | cuBLAS pattern | Tensor cores when Vulkan exposes |
| GPU sparse | Per-row CSR spmv with warp-reduce | cuSPARSE pattern | Block-CSR; ELLPACK on GPU |
| GPU FFT | Stockham radix-mix on GPU | cuFFT pattern | Bluestein on GPU |
| GPU iterative | CG/PCG with GPU spmv + GPU axpy/dot | — | Communication-avoiding variants |

### 4. Determinism contract (inherits ADR-0063)

Every `crd-hesap` algorithm obeys ADR-0063 (eylem determinism contract):

- `-ffp-contract=off` / `/fp:precise` — no fused multiply-add reordering.
- No fast-math; no x87.
- Cerid-internal trig / exp / sqrt via `crd::math::deterministic`
  (Cephes-style; bit-exact across platforms / compilers / SIMD widths).
- `crd::containers::sort` for any sort (no `std::sort`).
- FNV-1a 64 for any hash (no `std::hash`).
- Cross-thread reductions are commutative + associative (Kahan
  summation in parallel reductions; pairwise summation as the
  default reduction tree).
- Splittable RNG with deterministic seed → identical output across
  platforms.
- `same input → same output`, byte-exact, on the 9-config replay-hash
  CI matrix.

This is non-negotiable for two reasons:

1. **Eylem v7 FEM** refactors to use `crd-hesap-iterative` (sparse PCG)
   inside its deterministic physics step. Non-determinism in the
   solver breaks replay-hash CI.
2. **Differentiable physics (eylem v9)** + **MATLAB-class tool**
   both rely on bit-reproducible numerics for trust.

Numerical accuracy is a separate axis from determinism. ADR-0065
commits to:

- Backward error within `O(n × eps)` for direct dense solvers (`eps =
  machine epsilon`).
- Forward error within tolerance × condition number for iterative.
- Convergence rate tests as separate test tier (not just unit tests
  but **numerical benchmark tests** with documented tolerances).

### 5. API ergonomics — Eigen-class core + MATLAB-class facade

Two layers, same data:

**Layer A — Eigen-class typed C++ API** (primary, used by engine code):

```cpp
// crd::hesap::dense
auto A = Matrix<f64>::random(alloc, 1000, 1000);
auto b = Vector<f64>::random(alloc, 1000);
auto x = solve(A, b);                              // LU under the hood
auto qr = qr_factorize(A);                          // returns QR object
auto y  = qr.solve(b);                              // re-use factorization

// crd::hesap::sparse
SparseMatrix<f64, CsrLayout> K = build_stiffness(...);
auto pcg = PCG<f64>{}
  .matrix(K)
  .preconditioner(Preconditioner::Jacobi)
  .tolerance(1e-8)
  .max_iter(1000);
auto u = pcg.solve(f);

// crd::hesap::ode
auto sol = solve_ivp(rhs_fn, /*t_span=*/{0, 10}, /*y0=*/{1, 0},
                     IntegratorMethod::DOPRI5, /*rtol=*/1e-6);

// crd::hesap::autodiff (reverse-mode)
auto loss = [](const Vector<Var>& x) -> Var {
    return sum(x * x) + sin(x[0]);
};
auto [value, grad] = value_and_gradient(loss, x);
```

**Layer B — MATLAB-class facade** (for the REPL + scripting layer):

```cpp
// crd::hesap::matlab (thin sugar over the Eigen-class API)
using namespace crd::hesap::matlab;
A = randn(1000, 1000);                              // double-precision dense
b = randn(1000);
x = A \ b;                                          // backslash = solve
[U, S, V] = svd(A);                                 // structured binding
y = fft(signal);
plot(t, y);                                         // routes to crd-renderer in tool mode
```

The MATLAB-class facade is **opt-in syntactic sugar**, not the
canonical API. It exists for the MATLAB-tool use case and for
prototyping inside the REPL. Engine production code uses Layer A.

### 6. Memory + allocation discipline

Every container takes `IAllocator*` at construction (CLAUDE.md hard
rule). No owning STL containers anywhere. `crd::containers::Array`,
`crd::containers::HashMap`, `crd::containers::String` for storage;
`crd::containers::ConstSpan` for views.

Sparse matrix storage uses a compact two-array `(rowptr, colind,
values)` layout (CSR) plus optional per-row sort (CSC variant). All
allocations route through the user's `IAllocator` so a `TlsfAllocator`
can host a giant sparse system in a single arena.

### 7. Threading model

`crd-hesap` integrates into `crd-jobs` with the same posture as
`crd-eylem` and `crd-sdf` (ADR-0062 §7, ADR-0064 §10): **never spawns
its own threads**. Every parallel operation fans out via
`jobs::parallel_for` / `run_and_wait`. From the schedule's
perspective, a `gemm` or `pcg.solve()` call is one atomic phase
boundary; internally it's saturated parallel.

This is the critical reason `crd-hesap` is not "another OpenMP-style
library":

- A user calling `solve(A, b)` from a system inside the eylem
  `Physics` schedule phase fans out across the whole job pool for
  that solve, then yields the cores back to the next system.
- No oversubscription (no separate `crd-hesap` thread pool competing
  with `crd-jobs`).
- No nested OpenMP-style parallelism issues.

### 8. C ABI + plug-in surface (per ADR-0034 hot-reload pattern)

`crd-hesap-repl` exposes a stable **C ABI** so external code can:

- Load `crd-hesap` from a DLL / .so / .dylib at runtime.
- Call the full numerical surface from Lua / Python / external tools.
- Hot-reload during a long-running tool session.

Pattern matches ADR-0034 (C++ hot-reload scripting). The C ABI is a
narrow envelope: handle-based factory + opaque function pointers + a
typed dispatch table. The C++ API is the source of truth; the C ABI
is a generated-or-handwritten wrapper.

### 9. REPL + notebook surface

`crd-hesap-repl` ships an interactive surface so the MATLAB-class
tool ambition has a concrete entry point:

- **Inline REPL** — line-by-line execution; reads MATLAB-class
  facade syntax (Layer B above); prints results with auto-formatting
  (column-width-aware matrix print, scalar with sig figs).
- **File-backed notebook** — `.cnb` file (Cerid notebook; CRDR-format
  with `'CNBK'` FourCC). Cells are typed: code, markdown, plot.
- **Plot integration** — when running inside the engine (sandbox or
  editor), `plot()` / `imshow()` / `surf()` route to a `crd-renderer`
  panel via ImGui's per-frame widget. Outside the engine (headless
  CLI use), they emit PNG / SVG via a CPU rasteriser sub-slice.
- **Inspector** — runtime introspection of variables (matrix size,
  sparsity, condition number estimate).

The full editor experience (syntax highlighting, debugger,
breakpoints in numerical scripts) lands in Phase 7.

### 10. GPU surface

`crd-hesap-gpu` mirrors the CPU API on GPU via Vulkan compute shaders
+ the existing `UploadHandle` / `Fence` infrastructure (ADR-0061). No
new RHI primitives needed.

```cpp
auto x_gpu = upload_to_gpu(x_cpu);              // returns GpuVector<f64>
auto y_gpu = gpu::pcg_solve(K_gpu, f_gpu, ...); // solver runs on GPU
auto y_cpu = download_from_gpu(y_gpu);
```

The contract: any algorithm with a `crd::hesap::xxx` CPU
implementation has (eventually) a `crd::hesap::gpu::xxx` mirror.
The GPU sub-module is opt-in (one of the heaviest by LOC); a CPU-only
build leaves it unlinked.

### 11. Scope clarifications

**In scope** (Phase 3.1.6):

- Dense + sparse linear algebra, all storage formats, all standard
  factorisations.
- Iterative + direct solvers + AMG preconditioner.
- Eigenvalue solvers (dense + sparse).
- Optimisation: unconstrained, constrained, QP, LP.
- ODE / DAE solvers (stiff + non-stiff).
- FFT (any size) + DCT/DST/Hartley.
- DSP: FIR/IIR filter design + biquads + resampling + spectral
  analysis.
- Statistics: distributions + sampling + tests.
- Special functions: gamma, beta, erf, Bessel, polynomials.
- Polynomial / interpolation / quadrature.
- N-dim tensors with broadcasting + einsum.
- Forward + reverse mode autodiff.
- GPU acceleration mirror.
- REPL + notebook + plug-in C ABI.

**Reserved** (post-Phase-3.1.6):

- Symbolic computation (CAS — separate concern; potential `crd-sembol`).
- Distributed memory parallelism (MPI-style — Phase 8+ when scientific
  computing tool needs cluster scaling).
- Mixed-integer programming (MIP) beyond basic branch-and-bound —
  full MIP needs cuts + heuristics + warm-start; reserve as a v17+
  extension.
- Stochastic programming, robust optimisation — reserved.
- Geometric algebra primitives — reserved.

### 12. Versioning + format freeze

`.cnb` notebook format and any cooked-artifact data carry (FourCC,
schema_version, payload_size). Same pattern as Phase 3.0 v1p freeze:

- `'CNBK'` notebook v1: cell list with typed payloads.
- `'HSPS'` solver-state snapshot v1: factorisation snapshot for
  long-running solvers (resume from checkpoint).
- `'HEDV'` densely-stored matrix v1: row-major dense matrix on disk.
- `'HSPM'` sparse-matrix v1: CSR storage with sorted column indices.

Bumps require a deliberate schema break visible in source; loader
rejects mismatched versions with `LoadState::Failed`.

## Consequences

**Positive:**

- One numerical substrate consumed by **every** Cerid domain (games +
  robotics + medical + cinematic + DAW + scientific tool).
- MATLAB-class tool ambition gets a first-class engine pillar — not a
  bolt-on.
- Eylem v7 (FEM), eylem v9 (differentiable), audio v? (DSP), robotics
  motion planning (opt + ODE), medical sim (ODE), cinematic VFX (FFT,
  large sparse) all draw from one well.
- Determinism contract (ADR-0063) inherits cleanly — replay-hash CI
  catches regressions in the solvers automatically.
- Plug-in C ABI (per ADR-0034 pattern) makes Cerid's numerical layer
  callable from external tools / Lua / Python.

**Negative:**

- **~6–8 months of work for the full substrate.** Comparable in scope
  to eylem itself. Justified by the breadth of consumers but not a
  small commitment.
- **Numerical testing is hard.** Convergence-rate tests, condition-
  number stress, residual-norm benchmarks — all require careful
  reference data (we generate against LAPACK / SuiteSparse / FFTW
  reference outputs and pin them as fixtures).
- **GPU sub-module doubles the surface.** Each CPU algorithm needs
  a GPU mirror. Mitigated by sequencing GPU as a late slice (v16);
  the CPU substrate ships first and proves itself.
- **Algorithm coverage is wide.** No one engineer holds full mental
  context across BLAS + sparse + iterative + direct + eig + opt +
  ODE + FFT + autodiff. Mitigated by sub-module isolation — each
  sub-module is independently maintainable.

**Insertion point:**

`crd-hesap` ships as **Phase 3.1.6** — after Phase 3.1 (eylem) closes,
before Phase 3.2 (animation) begins. Phase ordering rationale:

- Phase 3.1 (eylem v0–v9) **does not block** on `crd-hesap`. Eylem v7
  (FEM) ships its own narrow PCG solver internally — ~1500 LOC, scoped
  to FEM's exact need; refactored to use `crd-hesap-iterative` once
  the substrate lands. (See `phase-3.1-eylem.md` v7 dependency note.)
- Phase 3.2+ (animation, font, audio, PBR) consumes `crd-hesap` as
  needed but doesn't strictly block on it (animation = small dense LA
  in `crd-math`; font = no numerical solver; audio = wants
  `crd-hesap-dsp` but can ship first slices without).
- The MATLAB-class tool becomes feasible once Phase 3.1.6 reaches
  ~v10 (roughly half the substrate).

Phase plan: `docs/phases/phase-3.1.6-hesap.md`.
Research: `docs/research/cerid-hesap.md`.

## References

- Goto & van de Geijn (2008) — *Anatomy of high-performance matrix multiplication*. ACM TOMS.
- van Zee & van de Geijn (2015) — *BLIS: A framework for rapidly instantiating BLAS functionality*. ACM TOMS.
- Anderson et al. (1999) — *LAPACK Users' Guide* (3rd ed.). SIAM.
- Davis (2006) — *Direct Methods for Sparse Linear Systems*. SIAM. (CHOLMOD, UMFPACK, KLU foundation.)
- Saad (2003) — *Iterative Methods for Sparse Linear Systems* (2nd ed.). SIAM.
- Amestoy, Davis & Duff (1996) — *An Approximate Minimum Degree ordering algorithm*. SIAM J. Matrix Anal.
- Karypis & Kumar (1998) — *METIS: A software package for partitioning unstructured graphs*. (Reordering reference.)
- Hairer, Nørsett & Wanner (1993) — *Solving Ordinary Differential Equations I (non-stiff)* / *II (stiff)*. Springer.
- Hindmarsh et al. (2005) — *SUNDIALS: Suite of Nonlinear and Differential/Algebraic Equation Solvers*. ACM TOMS. (CVODE, IDA, ARKODE.)
- Nocedal & Wright (2006) — *Numerical Optimization* (2nd ed.). Springer.
- Wächter & Biegler (2006) — *On the implementation of an interior-point filter line-search algorithm for large-scale nonlinear programming*. (IPOPT.)
- Stellato et al. (2020) — *OSQP: an operator splitting solver for quadratic programs*. Math. Prog. Comp.
- Frigo & Johnson (2005) — *The design and implementation of FFTW3*. Proc. IEEE.
- Oppenheim & Schafer (2010) — *Discrete-Time Signal Processing* (3rd ed.). (FIR/IIR filter design canon.)
- O'Neill (2014) — *PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation*.
- Blackman & Vigna (2018) — *Scrambled linear pseudorandom number generators*. (Xoshiro family.)
- Carpenter et al. (2017) — *Stan: A Probabilistic Programming Language* — reverse-mode autodiff over BLAS reference.
- Bradbury et al. (2018) — *JAX: composable transformations of Python+NumPy programs* — modern AD reference.
- Lubin et al. (2023) — *JuMP 1.0*. Math. Prog. Comp. (Algebraic modelling reference.)
- ADR-0034 — C++ hot-reload DLL scripting (C ABI pattern reused for plug-in API).
- ADR-0061 — Async GPU upload contract (`UploadHandle` + `Fence` reused for `crd-hesap-gpu`).
- ADR-0063 — Eylem determinism contract (inherited wholesale).
- ADR-0064 — `crd-sdf` substrate (sibling substrate; same architectural posture).
- `docs/research/cerid-hesap.md` — full industry survey + algorithm rationale.

---

## §13 Amendment (2026-05-19) — Elite-tier refinement + agent-native plumbing

**Status:** Accepted alongside ADR-0081 (Proposed).

Per user direction 2026-05-19, `crd-hesap` goes elite-and-big (same
precedent as Phase 3.1.7 `crd-geometry`). Locks the following design
decisions on top of the original 2026-05-10 architecture.

Research dossier: `docs/research/cerid-hesap-2026-update.md`.
Companion vision: `docs/research/cerid-agent-native-engine.md`.

### Locked decisions D1-D17 (this amendment)

- **D1 (matrix-type catalog).** Hesap ships ~30 matrix types from v0,
  not the original 7. Dense: `Matrix<T,Layout>`, `MatrixView<T,L>`,
  `DiagonalMatrix`, `IdentityMatrix`, `PermutationMatrix`,
  `TriangularMatrix<T,Uplo,Diag>`, `SymmetricMatrix<T,Uplo>`,
  `HermitianMatrix<Complex<T>,Uplo>`, `BandedMatrix<T>`,
  `BlockDiagonalMatrix<T>`, `BlockTridiagonalMatrix<T>`,
  `ToeplitzMatrix<T>`, `HankelMatrix<T>`, `CirculantMatrix<T>`,
  `VandermondeMatrix<T>`. Sparse: CSR, CSC, BSR, COO, ELL, HYB, DIA,
  CSR5 (Liu-Vinter 2015), Merge-CSR (Merrill-Garland 2016), Sliced
  ELL, JDS, SkyLine. Hierarchical: HSS (Chandrasekaran 2006),
  H-matrix (Hackbusch 1999), BLR (Amestoy 2017).

- **D2 (complex from v0).** `crd::hesap::Complex<T>` value type
  (own — not `std::complex<T>` — for determinism + GPU layout
  control). Every BLAS / LAPACK op has 4 instantiations: f32, f64,
  Complex32, Complex64. `dot` becomes `dotu` (un-conjugated) +
  `dotc` (conjugated). Cholesky becomes Hermitian-positive-definite.
  Eigenvalue spectra are inherently complex for non-symmetric
  matrices. The original "real-valued first" pin is **explicitly
  reversed**.

- **D3 (LinearOp abstraction from v0).** `LinearOp<T>` abstract base
  + 6 concrete materialisations (Dense, Sparse, Function, Composition,
  Sum, Scaled). Every Krylov solver / iterative method accepts
  `LinearOp<T>`, enabling matrix-free FEM / PDE / Hessian-vector
  for opt. Reference: PETSc `Mat`, Trilinos `Tpetra::Operator`.

- **D4 (task-DAG scheduling via crd-jobs).** `gemm` and other L3 ops
  use **tile-based task-DAG dispatch** over `crd::jobs::parallel_for`,
  NOT fork-join BLAS. New sub-module `crd-hesap-sched` owns the
  task-graph builder. Each tile op is a `crd::jobs` task; deps form
  a DAG; scheduler load-balances across cores. Reference: PLASMA
  (Buttari 2009), PaRSEC (Bosilca 2013), StarPU (Augonnet 2011).

- **D5 (mixed-precision iterative refinement).** Direct dense solvers
  (`lu`, `cholesky`, `qr`) ship an IR variant: factorize in f32,
  refine in f64. Reference: LAPACK `dsgesv` / `dsposv`, HPL-AI.
  **3-4× perf at full f64 accuracy.**

- **D6 (modern hardware support).** Microkernel specialization for
  AVX2 (existing) + **AVX-512** + **NEON** + **SVE/SVE2** (ARM HPC,
  Apple M-series) + **scalar fallback**. Runtime CPU dispatch.
  Reserve: Apple AMX + Intel AMX (matrix coprocessors) when stable
  reverse-engineered ABIs land. Microkernel pattern from BLIS
  (van Zee-van de Geijn 2015).

- **D7 (modern preconditioners).** Original Jacobi / IC(0) / ILU(0)
  remain. **Add**: SPAI (sparse approximate inverse, Grote-Huckle
  1997), ILUPACK multilevel ILU (Bollhöfer-Saad 2006),
  smoothed-aggregation AMG (Vaněk 1996), AGMG (Notay 2010), bootstrap
  AMG (Brannick 2010), polynomial preconditioners (Chebyshev).

- **D8 (Krylov subspace recycling).** GCRO-DR, M-CG (Parks-de
  Sturler-Mackey-Miller 2006) ship in v6 iterative substrate. **For
  eylem time-stepping** (sequence of related linear systems) and
  optimization inner solves. 2-5× speedup measured on real workloads.

- **D9 (modern eigenvalue solvers).** Original `dsyevr` (MRRR) +
  `dgeev` (QR + double-shift) remain. **Add**: LOBPCG (Knyazev 2001),
  FEAST (Polizzi 2009), Jacobi-Davidson (non-symmetric), IRLBA
  (Baglama-Reichel 2005). Reserve: ELPA distributed.

- **D10 (randomized linear algebra).** Halko-Martinsson-Tropp 2011 +
  Tropp 2019. Randomized SVD, randomized QR, randomized range-finder.
  10-100× speedup for rank-k approximations of large matrices.
  Ships in v3 SVD/eig as `_randomized` variants alongside classical.

- **D11 (operator-level AD).** Original tape-based reverse-mode AD
  (Stan pattern) augmented with **per-op custom VJP / JVP rules**
  (JAX pattern). `solve(A, b)`'s VJP is `solve(A^T, dy)` — never AD
  through LU. Reference: JAX (Bradbury 2018), Enzyme (Moses-Churavy
  2020). Sparse Jacobian via Curtis-Powell-Reid coloring.

- **D12 (modern ODE methods).** Original DOPRI5/8 + BDF + Rosenbrock
  + Pantelides remain. **Add**: SDIRK (singly-diagonally-implicit
  RK), Verner methods (higher-order embedded), Tsitouras 2011,
  **symplectic integrators** (Verlet, Yoshida 4/6/8) for conservative
  Hamiltonian systems (orbital mechanics, molecular dynamics),
  Implicit-Explicit (IMEX) methods, **sensitivity analysis**
  (forward + adjoint, CVODES / IDAS pattern; bridges to autodiff).

- **D13 (NUFFT + sparse FFT).** Original Cooley-Tukey + Bluestein
  + DCT/DST/Hartley remain. **Add**: NUFFT (Greengard-Lee 2004) for
  irregular sample grids (MRI / interferometry / particle methods),
  sparse FFT (Hassanieh 2012) for k-sparse signals (niche but elite).

- **D14 (modern optimization).** Original L-BFGS + OSQP + IPOPT-class
  + simplex remain. **Add**: SCS (O'Donoghue 2016 — splitting conic),
  trust-region Steihaug (1983), Adam (Kingma-Ba 2014), AdaGrad,
  AdamW (Loshchilov-Hutter 2019), Lion (Chen 2023). Stochastic
  optimization is essential for ML / agent training workflows.

- **D15 (benchmark substrate `crd-hesap-bench`).** New sibling
  sub-module tracks regression vs MKL / OpenBLAS / LAPACK /
  SuiteSparse / FFTW reference baselines. Per-slice CI gate.
  Property-based + reference-fixture replay + convergence-rate +
  numerical-accuracy + determinism + replay tiers (~100-200 tests
  per slice, not the original ~30).

- **D16 (CLI protocol plumbing from v0).** Per ADR-0081 (Proposed) +
  user direction 2026-05-19. Every hesap entry point registers a
  typed `CommandSchema` via static-init `cli::register_module_commands`
  hook. Structured output (typed result + diagnostics + provenance)
  is the C++ return shape from day 1. MCP tool descriptors
  auto-generated. **No actual CLI parser / REPL / RPC in hesap** —
  those ship with `crd-cli` substrate in Phase 4.0. Protocol-first,
  parser-later (same pattern as ADR-0076 §12 for geometry-before-eylem).

- **D17 (C++ scripting via hot-reload — the ONLY scripting path).**
  Per ADR-0081 + user direction 2026-05-19. The original ADR-0034
  C ABI plug-in plan is **subsumed by ADR-0081**'s broader
  agent-native substrate. Cerid scripts (including notebook cells)
  ARE C++ files (`.crds.cpp`) compiled into hot-reloadable DLLs.
  **No Lua / Python / GDScript embedded interpreter.** AI agents
  emit C++ → `script.compile` → hot-reload DLL → call → inspect
  typed output → next iteration. The original `crd-hesap-repl` MATLAB-
  class facade is **replaced** by a C++-cell notebook surface on
  top of `crd-cli` (later, Phase 4.0+2). The MATLAB-class facade
  remains a stretch goal as a C++-source-transformer (parse MATLAB
  syntax → emit C++ snippet → compile + hot-reload) but is NOT in
  scope for Phase 3.1.6.

### Updated slice plan

The full 18-slice plan structure remains, but every slice gains:

1. Matrix-type catalog coverage where applicable.
2. Complex variants throughout (real + complex bake into the same
   slice, not deferred).
3. CLI protocol plumbing (typed `CommandSchema` registration).
4. Test scope expanded to 100-200 cases per slice (was ~30).
5. Benchmark fixtures committed (LAPACK / SuiteSparse / FFTW
   reference outputs).
6. Determinism + replay tests inherit ADR-0063 contract.

**v0 sub-slice plan** (was 1.5 wk; refined to ~5 wk):

- **v0a substrate** (~3 days): module skeleton + `Complex<T>` +
  matrix-type catalog headers + `LinearOp<T>` abstraction + `MatrixId`
  / `VectorId` typed handles + format-pin (`'HDV0'` CRDR FourCC).
- **v0b BLAS L1** (~3 days): 8 L1 ops × 4 type variants;
  Kahan-pairwise reduction; bit-exact SIMD/scalar parity; CLI
  registration.
- **v0c BLAS L2** (~5 days): L2 ops (gemv / ger / trmv / trsv / sym
  / herm / banded / triangular); CLI registration.
- **v0d BLAS L3 + task-DAG gemm** (~7 days): tile-based gemm via
  `crd-hesap-sched`; AVX2 / AVX-512 / NEON / SVE2 / scalar
  microkernels; syrk / herk / trmm / trsm; mixed-precision dispatch
  helper.
- **v0e dense direct** (~7 days): LU / Cholesky / QR / LDLT with
  iterative refinement + mixed-precision variants; `LinearOp<T>` view
  output from each factor; condition estimation; CLI registration.
- **v0f bench substrate + system doc** (~5 days): `crd-hesap-bench`
  sub-module; LAPACK reference fixtures committed; property-based
  test framework; `docs/systems/hesap-dense.md`.
- **v0-close**: ADR-0065 §14 amendment (locking v0a-f decisions) +
  18-config full sweep.

v1-v17 inherit the per-slice patterns. Schedule shifts from "6-8
months" → "~10-12 months elite-tier" honest scope.

### Sub-module additions

In addition to the original 14 sub-modules:

- **`crd-hesap-sched`** — task-DAG builder over `crd::jobs`.
- **`crd-hesap-bench`** — benchmark + reference-fixture replay
  substrate.

Total sub-module count: 16.

### Determinism notes

ADR-0063 contract continues unchanged. Complex variants follow the
same bit-exact-across-SIMD-widths discipline. Mixed-precision IR
preserves determinism (f32 factor is deterministic; f64 refinement
is deterministic; the iteration count is deterministic given input
matrix + initial-guess).

### Scope adjustments

**Reserved** (post-Phase-3.1.6) — unchanged:
- Symbolic computation (CAS).
- Distributed memory parallelism (MPI).
- Full MIP optimization.
- Stochastic / robust optimization.
- Geometric algebra primitives.

**Added to reserved** (post-Phase-3.1.6):
- HSS-augmented sparse direct (STRUMPACK pattern) ships in v5
  refinement but the full STRUMPACK feature set defers.
- Tensor Train / Tucker decomposition (Oseledets 2011) reserves.
- BLR-augmented sparse direct (MUMPS-BLR pattern) reserves.

### Cluster cross-validation

When Phase 3.1.6 closes:

- 18-config full sweep PASS.
- Eylem v1f-articulation (Featherstone) integration smoke
  cross-validates `crd-hesap-dense` factorizations.
- Reference baselines pinned: gemm ≥ 70% AVX2 / AVX-512 peak; dpotrf
  ≤ 1.5× LAPACK accuracy on Hilbert(N); pcg convergence within 10%
  of theory.

### Filed follow-on slices (consumer-pull)

Not blocking; tracked in `docs/debt.md`:

- HSS sparse-direct full integration (STRUMPACK).
- BLR sparse-direct full integration (MUMPS-BLR).
- Tensor Train + Tucker tensor decompositions.
- Apple AMX + Intel AMX matrix-coprocessor specialization.
- Distributed parallelism (MPI) when scientific-computing tool needs
  cluster scaling.

### References (2024-2026 SOTA additions)

See `docs/research/cerid-hesap-2026-update.md` for the full
bibliography (~50 citations covering mixed-precision IR, randomized
LA, task-DAG scheduling, Krylov recycling, modern AMG, ILUPACK,
SPAI, JAX-style AD, modern hardware, NUFFT, sparse FFT, modern
optimization, modern ODE, AD-at-operator-level).

ADR-0065 §13 ✅ Accepted alongside ADR-0081 Proposed.
- `docs/research/sparsematrices.md` — pre-existing Cerid sparse-matrix research.

## v0e decisions (queued for §14 lock at v0-close)

Locked in code + measured during v0e (dense direct solvers, 2026-05-20).
To be formally numbered (D5x) in the §14 amendment at v0-close (after v0f):

- **v0e-D1 — LU**: right-looking blocked LU, partial pivoting (LAPACK
  xGETRF), bs=64; trailing update via `gemm_parallel` (first real
  consumer). f32/f64 RowMajor.
- **v0e-D2 — Cholesky**: SPD; unblocked SIMD per-row-dot for n≤256,
  right-looking blocked + packed parallel `syrk_lower_minus` (+ parallel
  trsm) for n>256.
- **v0e-D3 — LDLT**: Bunch-Kaufman (ALPHA=(1+√17)/8), 1×1 + 2×2 pivots,
  D as block storage; SIMD row-restructured trailing update.
- **v0e-D4 — QR**: blocked compact-WY Householder (LAPACK xGEQRT, nb=32),
  transposed-panel SIMD factor + block-reflector trailing update via two
  `gemm_parallel` calls.
- **v0e-D5 — LinearOp + condition**: `MatrixLinearOp`/`SymmetricLinearOp`
  over the v0a `crd::hesap::LinearOp<T>`; Hager 1-norm κ₁ estimator
  (xLACON pattern).
- **v0e-D6 — iterative refinement**: Wilkinson same-precision IR
  (`refine_lu`/`refine_cholesky`); mixed-precision HPL-AI filed `v0e-f2`.
- **v0e-D7 — packed register-tiled `syrk_lower_minus`**: reuses the GEMM
  microkernel + BLIS packing, lower-triangle-only, parallel over
  block-rows. Reusable primitive for future eig/sparse/optimization.
- **v0e-D8 — row-major storage** (promoted to standalone **ADR-0083
  Accepted**): keep row-major; small-N factor gap vs Eigen is a
  layout-fit issue (column-major would fit factorization's column
  access); per-factor column-major escape hatch reserved.

Filed v0e follow-ons (consumer-/hardware-gated, not blocking):
`v0e-a2` (LU complex), `v0e-b-hpd` (Hermitian Cholesky), `v0e-c-blocked`
(LDLT gemm trailing), `v0e-d-colpiv` (rank-revealing QR), `v0e-e2`
(LU/LDLT/QR condition estimators), `v0e-f2` (mixed-precision IR).

v0e shipped 7 sub-slices (a–g) in 1 day + a perf-attack session; 172
hesap-dense cases / 65,098 assertions; 5-config DoD green.

---

## §14 Amendment (2026-05-20) — v0a–f implementation-validated decision lock (v0-close)

**Status:** Accepted.

Phase 3.1.6 v0 (dense foundation: substrate + BLAS L1/L2/L3 + dense direct
solvers + property/bench infrastructure) is **closed**. §13 set the elite
*plan* (D1–D17); §14 locks what was actually built and measured across v0a–f,
records every deviation from the §13 plan, and certifies the v0-close gate.

This is a **decision lock**, not a re-litigation: the algorithm-choice posture
(§3), determinism contract (§4), allocator discipline (§6), and threading model
(§7) hold unchanged. §14 only pins the v0-era implementation choices that were
validated empirically and the scope deltas taken along the way.

### v0-close gate (criteria met)

- **18-config full sweep PASS** (`scripts/full-sweep.ps1`: 11 Windows + 7 Linux).
- **hesap-dense suite**: 176 cases / 66,703 assertions (was 172 / 65,098 at
  v0e-close; v0f added 5 property cases).
- **Reference-class benchmarking** (continuous-benchmarking policy): GEMM 10/10
  WINS over Eigen-MT; LU WINS N≥128; small-N dense-factor residual traced to
  root cause (ADR-0083). Guards green (no-non-ascii / no-std-math /
  simd-emission). Determinism: factors bit-identical across worker counts.

### Locked v0 decisions

**v0a–d (substrate + BLAS):**

- **L50 (substrate from day 1).** `Complex<T>` (own value type, not
  `std::complex`), the ~30-type matrix catalog headers, `LinearOp<T>` + 6
  materialisations, `MatrixId`/`VectorId` handles, `'HDV0'` CRDR format pin, and
  CLI protocol scaffolding (`CommandRegistry` + `CommandSchema` + structured
  output + MCP-descriptor emit) all ship in v0a — realising §13 D1/D2/D3/D16.
  **Sparse catalog deferred to v1** (the storage formats are v1's whole job).
- **L51 (BLAS L1 reduction).** Kahan-Babuška-Neumaier pairwise reduction is the
  default reduction tree (satisfies §4 determinism). SIMD path: 8-accumulator
  `simd_dot`/`simd_sumsq` via single-rounded `simd::fma()`.
- **L52 (BLAS L2 SIMD shape).** Single-pass BLAS symv (each lower-half element
  touched once, updates both `y[i]` and `y[k]`) + 8-row tiled gemv +
  16-wide-unroll symv with 4 dot accumulators + gated `_mm_prefetch(_T1)` for
  `n > 512`. Residual 5–25% gaps vs Eigen filed `vs-ref-blas2-followups`
  (need asm or AVX-512).
- **L53 (BLAS L3 = Goto/BLIS layered gemm, intrinsics microkernel).** 5-loop
  layered gemm + scalar/AVX2 microkernel hot-swap behind
  `CRD_HESAP_MICROKERNEL_BACKEND`; **per-µarch asm deferred indefinitely**
  behind ADR-0082's three-condition revisit gate (not triggered).
  `gemm_parallel` = BLIS outer-loop parallelism over `crd::jobs::parallel_for`
  (NOT a separate thread pool — §7). `gemm_parallel_auto` heuristic dispatch +
  `gemm_mixed<f32,f64>` + `crd-hesap-sched::DependencyGraph` (per-tile
  RAW/WAW/WAR). **Single-rounded `simd::fma()`** (distinct from two-rounded
  `mul_add`) is the GEMM accumulation primitive and the key Eigen-MT-beating
  lever.

**v0e (dense direct solvers):** the eight decisions **v0e-D1…v0e-D8 above are
now formally locked** (LU right-looking blocked; Cholesky small-N per-row-SIMD +
large-N packed parallel `syrk_lower_minus`; LDLT Bunch-Kaufman SIMD-restructured;
QR blocked compact-WY transposed-panel; LinearOp + Hager κ₁; Wilkinson IR;
reusable packed `syrk_lower_minus`; **row-major storage promoted to standalone
ADR-0083 Accepted**).

**v0f (test + bench infrastructure):**

- **L54 (property-based test framework).** Seeded `RandomMatrix` factory
  (`tests/hesap-dense/random_matrix.hpp`) is the consolidated source of test
  matrices (general / diag-dominant / SPD / symmetric-indefinite /
  ill-conditioned-SPD); property tests assert each factorisation's defining
  algebraic identity across seeds × sizes. Realises §13 D15's property tier.
- **L55 (vs-reference bench dedup).** `crd_add_hesap_vs_ref_bench()` CMake
  helper is the single registration path for every Eigen/OpenBLAS shootout
  target. **Validated only with `CRD_BUILD_HESAP_VS_REFERENCE=ON`** — the gate
  keeps fetch-heavy Eigen/OpenBLAS out of the routine sweep.

### Deviations from the §13 plan (recorded)

| §13 plan item | v0 outcome | Rationale |
|---|---|---|
| D15 `crd-hesap-bench` **sub-module** + committed LAPACK/SuiteSparse/FFTW reference binaries (v0f) | **Deferred to FFT/sparse slices (v1, v5, v10).** Shipped instead: in-tree `RandomMatrix` factory + property tests + a CMake bench-registration helper. No reference binaries committed today. | Committing FFTW/SuiteSparse fixtures before any consumer exists is speculative; ship the substrate (factory + helper) proactively, defer the consumer-specific fixtures. The continuous Eigen+OpenBLAS shootout already covers dense regression. |
| `bench_common.hpp` shared C++ harness | **Deferred to the third vs-reference bench.** | `time_loop` couples to `jobs::frame_reset`, but 2 of 4 vs-ref benches don't link `crd-jobs`; abstracting over a 2/2 split is premature — sparse/FFT supplies the real third data point. |
| D6 microkernel AVX-512 / NEON / SVE2 specialisation | **Filed hardware-gated** (`v0d-microkernel-{avx512,neon,sve2}`); scalar + AVX2 shipped. | The dev/CI hardware is AVX2-only (i9-14900K, no AVX-512); other ISAs need their target silicon to validate against. |
| D5 mixed-precision IR (f32 factor → f64 refine) | Same-precision Wilkinson IR shipped; **HPL-AI mixed-precision filed `v0e-f2`.** | Needs a cross-precision solve wrapper; same-precision IR proves the refinement loop first. |
| §13 "~30 tests/slice → 100–200" | v0 slices landed **38–94 cases each** (lower count, far higher assertion density — e.g. v0e-a alone is 37k assertions). | Property + parametric tests trade case count for assertion volume; the coverage intent (D15) is met. |

### Filed follow-ons (consumer-/hardware-gated, NOT blocking v0)

`v0d-microkernel-{avx512,neon,sve2,blocks-empirical-sweep}`, `v0d-asm-microkernel`
(ADR-0082 gate), `v0e-a2` (LU complex), `v0e-b-hpd` (Hermitian Cholesky),
`v0e-c-blocked`, `v0e-d-colpiv`, `v0e-e2`, `v0e-f2`, `v0e-g-eigen-mt-config`,
`vs-ref-blas2-followups`, `v0d-small-gemm-fastpath`, `crd-hesap-bench` sub-module
+ reference-fixture replay (FFT/sparse-gated), `bench_common.hpp` (third-bench-gated).

### Next

**v1** — sparse storage substrate (CSR/CSC/BSR/COO/ELL/HYB/DIA/CSR5/Merge-CSR/
Sliced-ELL/JDS/SkyLine) + spmv/spmm/spgemm + format-conversion graph + sparse
`LinearOp` + complex variants + CLI registration. The strategic sequencing
(`feedback_strategic_execution_plan_2026_05_15`) then resumes **Phase 3.1 eylem
v1c+**, which consumes geometry + hesap-dense from day 1.

§14 ✅ Accepted — v0a–f locked, Phase 3.1.6 v0 closed.
