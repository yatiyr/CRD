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

## §15 Amendment (2026-05-21) — v1 sparse substrate decision lock (v1-close)

**Status:** Accepted.

Phase 3.1.6 **v1 (sparse storage substrate + kernels)** is **closed** (v1a–v1g).
§15 locks the sparse-era decisions validated empirically across the cluster,
records the deviations from the §13/§14 scope, and certifies the v1-close gate.
The algorithm posture (§3), determinism contract (§4 / ADR-0063), allocator
discipline (§6), and threading model (§7) hold unchanged.

### v1-close gate (criteria)

- **18-config full sweep PASS** (`scripts/full-sweep.ps1`: 11 Windows + 7 Linux).
- **hesap-sparse suite**: 93 cases / 719,188 assertions (substrate trinity +
  builders + spmv/SELL + conversions + element-wise + structural + spgemm
  (dense-SPA + hash) + spmm + SDDMM + BSR/ELL/DIA + Matrix-Market I/O).
- **Reference-class benchmarking vs Eigen** (gated `CRD_BUILD_HESAP_VS_REFERENCE`):
  assembly 1.03–1.76× `setFromTriplets`; SELL-C-σ spmv 1.21–1.27× DRAM-bound +
  Eigen-MT parity; spgemm **2.32× median on real SuiteSparse** (5/6 win) + wins
  the adversarial stress corpus 2.5–5.5×; spmm 1.3–1.9× heavy-RHS; BSR 3.4–6.7×,
  ELL 4.9–5.2×, DIA 4.8–5.9× vs Eigen scalar-CSR on native patterns; SDDMM wins
  the same-flops kernel race where compute-bound (1.3–1.5×), at the gather wall
  on high-nnz FEM (user-accepted, proven by two signals).

### Locked v1 decisions — D(sparse)-1…9

The eight determinism/design decisions pinned in `docs/systems/hesap-sparse.md`
are now **formally locked**, plus one added at v1g:

- **D(sparse)-1** `topology_hash` = FNV-1a-64, little-endian explicit-byte feed
  over `rows,cols,format,block_size` + canonical per-outer (count + sorted
  indices); slack-invariant (compressed == uncompressed of the same matrix).
- **D(sparse)-2** Structural-query CLI commands (`nnz`/`density`/
  `structural_query`/`inner_indices`) are **type-agnostic** (registered once;
  read structure only). Typed ops keep the ×4 {f32,f64,c32,c64} surface.
- **D(sparse)-3** spmv per-row reduction is **two-rounded** (`mul_add`, NOT
  single-rounded `simd::fma`) and bit-exact across the CSR scalar baseline and
  the SELL SIMD primary; width-independent; compressed-CSR only.
- **D(sparse)-4** SELL-C-σ: slice height `C` per-T (f32→8 / f64→4 / complex→4
  scalar); σ row-length sort (global default; identity fast-path for
  uniform/banded); within-row columns stay ascending → bit-exact with CSR.
- **D(sparse)-5** Element-wise (`add`/`subtract`/`hadamard`): `a OP b`
  left-first single-rounded; topology-hash fast-path + symbolic-union merge.
- **D(sparse)-6** BSR block-spmv uses a **dedicated fully-unrolled small-block
  GEMV** (compile-time b∈{1,2,3,4,6} + runtime fallback), NOT the v0d dense GEMM
  microkernel (sized for large-N tiling; prologue dominates a 3×3 block). No
  hesap-dense dependency. Diverges from the phase-note "reuse v0d microkernel".
- **D(sparse)-7** CSR→BSR zero-pads partial/edge blocks (a block is dense);
  BSR→CSR emits all stored block entries; DIA→CSR / ELL→CSR drop stored zeros
  (round-trips exactly for zero-free matrices).
- **D(sparse)-8** ELL is the **interop/base** regular format (global padding);
  SELL-C-σ is the irregular-matrix performance path. Both ship; ELL is the
  canonical unsorted-unchunked ELLPACK for interchange + uniform patterns.
- **D(sparse)-9 (new at v1g)** spgemm dispatches on `B.cols`: dense per-row SPA
  for cols ≤ `kMaxSpaCols` (4M, the fast path), **open-addressing hash
  accumulator** above it (bounded O(row distinct nnz) memory → arbitrary cols).
  The hash accumulates in the SAME encounter order as the dense SPA + emits
  column-sorted → **bit-exact with the dense path**; per-job hashes are
  pre-sized single-threaded (allocating inside `parallel_for` from the
  non-thread-safe TlsfAllocator is forbidden — it corrupts the heap).

### Cross-cutting fix locked at v1g (containers)

- **`crd::containers::sort` is in-place introsort** (median-of-three quicksort +
  heapsort fallback + insertion base) — **zero allocation**, deterministic +
  cross-platform bit-exact, non-stable. **`stable_sort` takes the caller's
  `IAllocator*` (or a reusable `Array<T>& scratch`)** — never a hidden
  default-malloc. This closed a real defect (merge-sort scratch was malloc-backed
  for N≥32). Validated: 5-config DoD green across all modules (no caller relied
  on stable-sort tie-breaking). See `feedback_no_hidden_default_allocator_malloc`.

### Deviations from the §13/§14 scope (recorded)

| Plan item | v1 outcome | Rationale |
|---|---|---|
| Storage formats incl. CSR5 / Merge-CSR / Sliced-ELL / JDS / SkyLine | **CSR/CSC/COO/SELL-C-σ/BSR/ELL/DIA shipped; CSR5+Merge-CSR → v17 GPU; JDS+SkyLine → consumer-pull follow-on; HYB folded into ELL.** | Shipping CPU storage of GPU/legacy formats with no consumer is the speculative pattern v0f learned to defer; SELL-C-σ already covers the SIMD-spmv niche CSR5/JDS target. |
| spgemm "elite refinement (BRMerge / MAGNUS-locality)" | **Not built — dense-SPA Gustavson wins the adversarial stress corpus 2.5–5.5× vs Eigen-ST; trigger (Eigen ≥ 1.0× on any in-cap case) not met.** Instead the hash-accumulator (D(sparse)-9) lifts the 4M-col capability ceiling (user-directed). | Refinement would gold-plate an already-winning kernel; the real gap was the cols>4M ceiling, which the hash path closes. |
| Matrix-Market I/O + SuiteSparse fixture corpus | **Engine-side `.mtx` reader/writer shipped (in-memory, no platform dep); SuiteSparse fetched at configure via gated `file(DOWNLOAD)` for benches (not committed).** | Caller does file I/O (module stays platform-free); committing fixtures stays deferred per §14's posture. |

### v1 CLI surface

~146 commands registered per-slice (every op × {f32,f64,c32,c64} + 4
type-agnostic structural queries). v1g CLI-completeness audit (grep-diff op-list
vs registered-command-list) closed the residual gaps (`from_csc`, `scale_rows`,
`submatrix`, `to_sell`, `spmv_adjoint`, `inner_indices`).

### Next

**Phase 3.1 eylem v1c+** resumes per `feedback_strategic_execution_plan_2026_05_15`
— consuming geometry + hesap-dense + hesap-sparse from day 1. (FFT / iterative /
direct sparse solvers / eig are later hesap phases.)

§15 ✅ Accepted — v1a–g locked, Phase 3.1.6 v1 (sparse) closed.

## §16 Amendment (2026-05-21) — v2 reorderings decision lock (v2-close)

§16 locks the `crd-hesap-ordering` (v2a–v2e) decisions and the **D(ord)-1..7**
determinism pins, all validated against Eigen/CSparse across the cluster.

**Modules + algorithms.** New sibling module `crd-hesap-ordering` (graph/integer
work on `SparsePattern`, no SIMD floats): `AdjacencyGraph` (symmetrised `A∪Aᵀ`,
diagonal-free, ascending) + `Permutation`/`apply_symmetric`; **RCM** (George-Liu
pseudo-peripheral); **AMD** (faithful `cs_amd` quotient-graph port — approximate
external degree + supervariables + mass elimination + aggressive absorption +
dense-node-last); **full symbolic factorisation** (`cs_etree`/`cs_post`/`cs_counts`
+ `cs_ereach` L-pattern + Liu-Ng-Peyton fundamental supernodes); **nested
dissection** (multilevel: heavy-edge matching coarsening + re-seeding BFS bisect +
Fiduccia-Mattheyses + König min-vertex-separator + node-FM) driven by **CAMD**
(constraint-aware `cs_amd` copy — per-`cmember`-class min-degree on the full graph,
the subdomain↔separator interface fix).

**Validation locked.** AMD fill ≤ 1.05× Eigen-AMD on bcsstk13/24/25 (gate met).
Symbolic L-pattern bit-exact vs `Eigen::SimplicialLLT` factor; symbolic analysis
beats Eigen `analyzePattern` at scale. **ND+CAMD beats Eigen-AMD fill on the FEM
subset** (bcsstk13 0.983×, bcsstk24 0.999×); bcsstk25 (large 3D multi-DOF) deferred
to `v2e-weighted-compression`. **Fill is a downstream-perf knob, never correctness**
(any valid permutation → identical solve; the v5 consumer picks AMD/ND per matrix).

**D(ord)-1..7 pinned** (`docs/systems/hesap-ordering.md`): (1) all tie-breaks →
ascending original-graph index; (2) iterate hash-like structures by sorted key; (3)
structure-derived seed, never RNG; (4) re-sort adjacency ascending before use; (5)
supervariable principal = lowest-index member; (6) aggressive absorption iterates
elements ascending; (7) ND coarse vertices numbered by ascending lowest fine
member + bisection re-seeds from the lowest-index unassigned vertex.

### Next

**v3** — SVD + dense eigenvalue + least squares. (Then v4 iterative, v5 sparse
direct which consumes these orderings + the v2c symbolic factorisation, … v18.)

§16 ✅ Accepted — v2a–e locked, Phase 3.1.6 v2 (reorderings) closed.

## §17 Amendment (2026-05-23) — v3a-3 MRRR decision lock (symmetric eigensolver)

§17 locks the **MRRR** (`dstemr`-class) decisions shipped in `crd-hesap-dense` for
the symmetric eigensolver — eigenvalues (dqds + Sturm bisection + parallel
multisection) and eigenvectors (twisted factorizations + RRR cluster tree + GS
fallback) — and the determinism pins **D(dense-eig)-9..12** plus the faithful-port
divergences. Companion to v3a-1/v3a-2 (which locked D(dense-eig)-1..8 for the QL/QR
+ D&C paths). Phase doc: `docs/phases/phase-3.1.6-hesap.md` § "v3a-3 locked design".

**Determinism pins (D(dense-eig)-9..12):**
- **D(dense-eig)-9 — pivmin Sturm guard is exact + fixed.** `|t| < pivmin ⇒ t = −pivmin`
  in the tridiagonal Sturm recurrence (`detail/sturm_count.hpp`) makes the eigenvalue
  count a deterministic step function of the shift; `pivmin = safmin·max(1, max|eᵢ|²)`
  (block-derived, not host-tuned). This is what makes bisection/multisection
  bit-reproducible.
- **D(dense-eig)-10 — RRR shift selection deterministic** (`dlarrf`): try the cluster
  L/R ends, accept the first whose element growth ≤ MAXGROWTH1·spdiam (then the refined-
  RRR test, then KTRYMAX back-off, then forced-best) — fixed tie-break, no convergence-
  dependent branch.
- **D(dense-eig)-11 — cluster tie-break + GS-fallback trigger fixed** (`dlarrv` loop):
  segment by relative gap `MINRGP = 1e-3`; clusters processed in fixed ascending order;
  the Gram-Schmidt re-orthogonalization fires at the fixed recursion-depth cap (≤8) for
  residual near-multiplicity — committed, not "if it looks bad".
- **D(dense-eig)-12 — dqds (`dlasq2`) fixed-iteration termination**: `dlasq2`'s `N+1`
  outer-while cap + `dlasq3`'s `NBIG = 100·(n0−i0+1)` per-block cap + deterministic
  `dlasq4` shift + IEEE NaN/Inf handling (`DISNAN` branch, deterministic given IEEE-754).
- **Parallel determinism:** the parallel multisection (eigenvalues) and the parallel
  eigenvector dispatch write **disjoint** outputs per worker over a **fixed** index
  partition → bit-identical regardless of worker count / scheduling. The work-stealing
  affects only *which* core runs a task, never the result.

**Faithful-port divergences (numbered, advisor-vetted):**
- **D(dense-eig)-MRRR-Z1base** — the dqds qd-array (`dlasq2/3/4/5/6`) and `dlar1v`/`dlaneg`
  use **1-based** access via a thin `Z1` pointer-minus-one wrapper, so the `4*N0+PP−3`
  ping-pong index arithmetic ports **line-for-line** from the Fortran (the standard
  correct-C-port practice; hand-translating to 0-based is the classic dqds failure mode).
- **D(dense-eig)-MRRR-dqds-ieee-only** — only the `IEEE=.TRUE.` branches of `dlasq5`/`dlasq6`
  are ported (ADR-0063 mandates IEEE-754 — a contract simplification, not a corner-cut).
- **D(dense-eig)-MRRR-dlarrb-per-eigenvalue** — `dlarrb_refine` uses per-eigenvalue
  bisection (via the `dlaneg` LDLᵀ twisted Sturm count) instead of dlarrb.f's linked-list
  multisection — same converged result, simpler control flow (the sharing is a speed-only
  opt; cluster sizes are small so it is invisible).

**Performance (the reference-class gate, measured on i9-14900K AVX2, f64; both Eigen AND
LAPACK per `feedback_always_bench_both_eigen_and_lapack`):**
- Full symmetric eig (`eig_sym`, values+vectors): **1.4–1.95× Eigen, 2.1–3.7× LAPACK
  `dsyev`**; Hermitian **2.2× Eigen / 2.7× LAPACK `zheev`**.
- Eigenvalues-only (parallel multisection): **beats `dsterf` at N≥2048, crushes `dstemr`
  1.6×**.
- Tridiagonal vectors (parallel MRRR): **crushes Eigen `computeFromTridiagonal` 5–64×,
  crushes LAPACK `dstemr` 1.7–2.7×, matches/beats LAPACK `dstedc` (BLAS-3 D&C) 0.84–1.52×**;
  orthogonality `‖VᵀV−I‖ ≤ O(n)·eps` (glued-Wilkinson W₂₁⁺ passes).
- **Honest negative result:** SIMD-across-eigenvectors (`dlar1v_x4`) tried + reverted — the
  O(n) per-eigenvector scratch makes a 4-batch memory-bound (16n footprint blows L2), so it
  nets ~0; the vector path is at the CPU hardware (memory-bandwidth) limit. Next lever = GPU
  (future v17). ([[project_mrrr_perf_win_is_vectors_not_values]].)

§17 ✅ Accepted — v3a-3 (MRRR) locked; beats Eigen + LAPACK on the symmetric eigensolver.

## §18 Amendment (2026-05-23) — v3b-1b SVD decision lock (bidiagonal QR, serial baseline)

Locks the SVD serial foundation: `bidiagonalize` (Golub-Kahan, v3b-1a) →
`detail/bdsqr.hpp::dbdsqr` (Demmel-Kahan implicit-zero-shift QR, faithful LAPACK port)
→ `svd` / `svdvals` drivers + the 4-column benchmark protocol. The crush is **not** in
this leaf by design — v3b-1b is the proven serial baseline that MEASURES the gap so the
next leaves are sequenced on data, not a guess (the v3a-3 lesson: serial predictions were
wrong both ways).

**Determinism + faithful-port pins (D(svd)-1..5):**
- **D(svd)-1 — `dlartg` f90 convention.** The plane rotation uses the Anderson-2017
  `dlartg.f90` contract (`c ≥ 0`, `r = sign(d,f)`, `s = g/r`). dbdsqr's zero-shift sweep
  **chains `r`** across consecutive `dlartg` calls, so the convention is load-bearing —
  `eig_sym.cpp::lartg` (whose `c` carries the sign of `f`) is a *different* rotation and
  is deliberately NOT reused for the SVD path.
- **D(svd)-2 — sign pin.** The largest-magnitude entry of each V column is made positive;
  the matching U column is flipped to preserve `A = U Σ Vᵀ`. RNG-free + reproducible.
- **D(svd)-3 — wide matrices via transpose.** `m < n` is handled by SVD of `Aᵀ`
  (`A = V'ΣU'ᵀ ⇒ U_A = V', V_A = U'`); bidiagonalize always sees `m ≥ n`.
- **D(svd)-4 — `dlasr` RowMajor.** The plane-rotation applier is rewritten for RowMajor
  (`A(r,c)=a[r·lda+c]`) — the transpose of the column-major Fortran inner loops; only
  PIVOT='V' (plane `(k,k+1)`, the only pivot dbdsqr uses) is ported. The 4 WORK rotation
  segments (NM1/NM12/NM13) are kept separate exactly as the Fortran.
- **D(svd)-5 — values-only via `dlasq2`.** `svdvals` feeds B's squared+scaled qd array
  to the dqds engine (`dlasq2`, its native purpose) with dlasq1-style smax scaling, rather
  than dbdsqr's no-rotate branch. (`TOLMUL = max(10,min(100,eps^(−1/8)))` is computed as
  `sqrt(sqrt(sqrt(1/eps)))` to stay clear of `std::pow` + the no-std-math guard.)

**Performance (i9-14900K AVX2, f64; FOUR columns — Eigen JacobiSVD + BDCSVD, LAPACK
`dgesvd` + `dgesdd`):**
- Full SVD (values + thin vectors): beats Eigen **JacobiSVD 3.0–3.4×** everywhere;
  ties/beats LAPACK `dgesvd` at small N (1.30× @128) but **loses to the D&C references at
  scale** — C/BDC 0.14, C/`dgesdd` 0.13 at N=512. Accuracy: val err ~1e-13, recon ~1e-14.
- Values-only (`svdvals`): crushes JacobiSVD **9–15×**; loses to D&C / `dgesdd` at N≥256.
- **Honest finding (the lever for the next leaves):** at N≥256 the dominant cost is the
  **unblocked `dgebd2` bidiagonalization (BLAS-2)**, not dbdsqr. Smoking gun: `svdvals`
  @ N=512 is **156 ms vs `dgesvd`-N 46 ms**, yet `dlasq2` is O(n²) (proven ~free in
  v3a-3.1). So the visible gap to `dgesvd` at scale is the reduction, not the QR sweep.
  This makes **v3b-1a-perf (blocked `dlabrd`, BLAS-3)** likely the *larger* lever than
  v3b-1b-perf (parallel split-block dbdsqr) for full SVD at scale — same shape as blocked
  `dsytrd` carrying v3a-1. Both feed v3b-2 (Gu-Eisenstat D&C vs SVD-via-MRRR). Sequencing
  of the next leaf left to the user. ([[project_serial_iterative_qr_loses_to_dc_reduction_is_bottleneck]].)

§18 ✅ Accepted — v3b-1b (bidiagonal SVD serial baseline) locked; faithful dbdsqr +
driver + CLI + 4-column bench. The crush is deferred to v3b-1a-perf / v3b-1b-perf / v3b-2.

## §19 Amendment (2026-05-23) — v3b-1a-perf blocked bidiagonalization (the reduction crush)

Closes the reduction gap §18 measured. `bidiagonalize` is now blocked `dgebrd`:
`dlabrd_upper` panels (faithful LAPACK `dlabrd`, m≥n upper branch, RowMajor) build the
X/Y panel-update matrices; the driver crushes the trailing block with ONE BLAS-3 rank-2k
update per panel (two `gemm_parallel_auto` GEMMs accumulated into a temp, then one
subtract — the proven blocked-`dsytrd` pattern); the unblocked `dgebd2`
(`bidiag_unblocked`) handles small n (≤ 2·nb) and the final tail. Block width nb=32.

**D(svd)-6 — blocked dgebrd panel form (faithful + two divergences pinned):**
- The reflector storage layout is **identical** to the unblocked path (v-tail in the
  sub-diagonal column, u-tail in the super-diagonal row), so `form_q_bidiag` /
  `form_pt_bidiag` / `dbdsqr` / `dlasq2` are unchanged. CONVENTION divergence: `dlabrd`
  writes the reflector unit heads EXPLICITLY into `A(i,i)=1` / `A(i,i+1)=1` (they are read
  by the same iteration's X/Y matvecs) and the driver restores `A(i,i)=d` / `A(i,i+1)=e`
  after the trailing GEMM — vs the unblocked v[0]=1-implicit convention. Numerically
  equivalent; the heads at the panel/tail boundary are intentionally part of V and the
  U-block during the GEMM.
- **Panel matvec layout (perf-load-bearing):** the Y matvec is computed row-outer into a
  CONTIGUOUS accumulator `yacc` (the column-strided textbook form cache-thrashes at scale
  — measured super-linear blow-up at N=1024); the X matvec is a contiguous row·row dot.
  Both route through `detail/dot_simd.hpp::simd_dot` / `simd_axpy` (single-rounded FMA,
  same as `eig_sym`; ADR-0082 §determinism-relaxation already accepts FMA for
  hesap-numerical, distinct from the two-rounded `mul_add` physics path). `simd_dot` /
  `simd_axpy` promoted to `dot_simd.hpp` as the canonical home (templated f32/f64).

**Performance (i9-14900K AVX2, f64; vs LAPACK `dgesvd`/`dgesdd` + Eigen `BDCSVD`):**
- **`svdvals` (reduction-dominated) now BEATS both LAPACK references at every N≥128:**
  vs `dgesvd` **3.6× @128 · 2.1× @256 · 1.8× @512 · 1.3–1.5× @1024**; vs `dgesdd`
  comparable; vs Eigen `BDCSVD` **2.0–6.5×**. The headline **@512 went 156 ms → ~26 ms
  (≈6×)**, and **@1024 flipped from 0.94× (losing) to 1.3–1.5× (winning)**. The reduction
  is the O(n³) cost (97% at N=1024; `dlasq2` ~3%); the win = SIMD panel + parallel trailing
  GEMM beating LAPACK's serial-SIMD panel. Accuracy: val err ~1e-13.
- **Full SVD (values + vectors): beats `dgesvd` at N=128/256 (1.1–1.5×), still loses to the
  D&C references (`BDCSVD`/`dgesdd`) at scale** (C/`dgesdd` 0.41 @256, 0.15 @512). This is
  NOT the reduction (svdvals proves it wins) — it is the **serial `dbdsqr` O(n³) vector
  sweep**, the exact D&C-class gap MRRR hit vs `dstedc`. Closing it is **v3b-1b-perf**
  (parallel split-block dbdsqr) and **v3b-2** (Gu-Eisenstat D&C `dbdsdc` vs SVD-via-MRRR),
  the next leaves — not this one. Filed `v3b-1a-perf-followon-parallel-panel` (parallelize
  the two big panel matvecs across cores — the LAPACK-serial Amdahl part — for an even
  bigger values crush; deferred, ~40% panel is BLAS-2 serial at N=1024) and
  `v3b-1a-perf-followon-dot_simd-consolidate-eig_sym` (eig_sym's local simd_dot/simd_axpy
  duplicate the now-canonical header; mechanical dedup).

§19 ✅ Accepted — v3b-1a-perf (blocked `dlabrd`/`dgebrd`) closed; the SVD reduction beats
LAPACK + Eigen at all N. Full-SVD-at-scale crush deferred to v3b-1b-perf / v3b-2.

## §20 Amendment (2026-05-23) — v3b-1b-perf vector-path crush via blocked dorgbr

Profiling the full-SVD vector path at N=512 (the gap §19 left open) split the 690 ms total
into: bidiag 22 ms (3%, already blocked), **form_q + form_pt 335 ms (49%)** at ~1.5 GFLOPS
(serial scalar reflector-apply), **`dbdsqr` 337 ms (49%)**. The elite fix for the
forming-half is NOT "parallelize the scalar loop" (P× of a slow memory-bound floor) but
**blocked `dorgbr` (BLAS-3 compact-WY)** — what LAPACK does — mirroring the v3b-1a-perf
reduction. New `detail/orgbr.hpp` (`orgbr_q` builds U=Q from the left reflectors; `orgbr_p`
builds VT=Pᵀ from the right reflectors) + shared `detail/block_reflector.hpp`
(`build_block_t_from_vtv`, the `dlarft` factor). Scalar paths kept for n ≤ 2·nb (nb=32) and
as the test oracle. Parallelism rides the existing MT `gemm_parallel_auto` in the apply.

**D(svd)-7 — forming Q applies (I − V T Vᵀ) with T, not Tᵀ.** `qr.cpp`'s compact-WY apply
uses **Tᵀ** because QR factorization applies `Q_panelᵀ` to reduce A→R; forming the
orthogonal factor itself is the OPPOSITE direction and uses **T**. (Empirically: Tᵀ produced
exactly Qᵀ — the transpose of the intended factor.)

**D(svd)-8 — `build_block_t_from_vtv` zeros the strict-lower triangle.** `dlarft`'s T is
upper-triangular and the routine only writes the upper + diagonal. `qr.cpp` relied on the
strict-lower staying at its zero-init value via a CONSTANT block stride; `orgbr` reuses one
T buffer across blocks of VARYING width (partial last block), so a stale strict-lower would
feed garbage to the `T·W` GEMM (which reads the full nb×nb T). The builder now zeros the
strict-lower explicitly — clean T regardless of caller buffer reuse (harmless for qr).

**D(svd)-9 — `orgbr_p` transpose-trick (reuse the columnwise Q machinery for the rowwise
P).** The right reflector G(g) (unit at column g+1, tail in row g), read as a COLUMN vector,
is identical to a columnwise reflector C(g) with unit at row g+1. So `P = C(0)…C(n−2)` is a
columnwise product: build `M = P` with the exact `orgbr_q` block-WY apply (a +1 row offset,
reading the tail along a matrix row), then `VT = Mᵀ`. No separate rowwise `dorglq` path.

**Parallel `dbdsqr` SKIPPED (scope decision, advisor + user 2026-05-23).** dorgbr alone is
the v3b-1b-perf win: it flips **C/`dgesvd` 0.81 → 1.45** — we now beat LAPACK's dbdsqr-CLASS
routine (the direct algorithmic peer). A per-`dlasr` parallel `dbdsqr` would (a) exhaust the
1 MB per-thread frame arena (~1000–2000 sweeps × 2 `dlasr` × JobDecl allocs, reclaimed only
on `frame_reset`) and (b) at best TIE BDC/`dgesdd` (parallelizing an O(n³) memory-bound
sweep cannot beat an O(n²) D&C). The crush vs the D&C references is **v3b-2** (Gu-Eisenstat
`dbdsdc`); `dbdsqr` stays the small-N fallback inside D&C where its cost is negligible.

**Performance (i9-14900K AVX2, f64, N=512):** form_q+form_pt **335 → 12 ms (28×)**; full SVD
**690 → 378 ms**; **C/`dgesvd` 0.81 → 1.45 (beats LAPACK dbdsqr-class)**, C/BDCSVD 0.17 →
0.24, C/`dgesdd` 0.15 → 0.27. Recon ~2.7e-14, 303 SVD assertions green; blocked dorgbr
validated against the scalar reflector-apply oracle at multi-panel sizes (m=n and m>n).

Filed follow-ons: `v3b-1b-perf-followon-qr-block_reflector-consolidate` (migrate `qr.cpp`'s
local `build_block_t_from_vtv` / `materialize_panel_v` to the shared `block_reflector.hpp` —
the dot_simd-consolidate precedent); `v3b-2-svd-via-mrrr` (the deferred novel fork — form
`J=[[0 Bᵀ][B 0]]` + parallel MRRR, gated on the exact-±σ-multiplicity perfect-shuffle
extraction; pursue only if Gu-Eisenstat D&C does not reach the crush).

§20 ✅ Accepted — v3b-1b-perf closed via blocked dorgbr; full SVD beats LAPACK `dgesvd`.
Parallel dbdsqr skipped (arena hazard + at-best-tie); the BDC/`dgesdd` crush is v3b-2.

## §21 Amendment (2026-05-23) — v3b-2 Gu-Eisenstat D&C SVD: the full-SVD crush

The full-SVD-at-scale gap §20 left (vs Eigen `BDCSVD` / LAPACK `dgesdd`) is closed by a
faithful Gu-Eisenstat divide-and-conquer bidiagonal SVD, replacing the O(n³) serial `dbdsqr`
rotation sweep with O(n²)-merge D&C on parallel BLAS-3. New `detail/svd_secular.hpp` (dlasd5
2×2 + dlasd4 ψ/φ secular root + dlaed6 3-pole cubic) and `detail/svd_dc.hpp` (dlasd2
deflation + dlasd3 secular-solve/vector-assembly + dlasd1 merge + dlasdq base + dlasdt tree +
dlasd0 recursion). Wired into `svd()` at a crossover (n≥64 → D&C, else dbdsqr); internal
recursion stop smlsiz=25. The back-transform `U=Q·U_b`, `VT=VT_b·Pᵀ` and the dlasd3 merge
assembly run on `gemm_parallel_auto` (the cores LAPACK's/Eigen's serial D&C lacks).

**Faithful-port pins (D(svd)-10..14):**
- **D(svd)-10 — dlasd4 ψ/φ split, NOT a d→d² rewrite of dlaed4.** The SVD secular equation
  `f(σ)=1+ρ·Σ z²/(d²−σ²)` uses a monotone-decreasing-left / increasing-right ψ/φ split with
  per-side rational interpolation + a 3-pole (`SWTCH3`) branch via dlaed6; load-bearing for
  accuracy near tight gaps. Ported line-for-line from dlasd4.f (MAXIT=400).
- **D(svd)-11 — column-major D&C, row-major dbdsqr bridged at dlasdq.** The LAPACK dlasd*
  chain is column-major (makes `U(:,j)` contiguous for dlasd4); the proven v3b-1b
  dbdsqr/dlasr are row-major. `dlasdq_upper` is a column-major adapter that runs the SQRE
  rotations + dbdsqr on row-major temps (init identity) and transposes into the column-major
  sub-blocks — reuses the *tested* dbdsqr rather than an untested column-major one.
- **D(svd)-12 — dlasd2 deflation rotates BOTH U and VT.** Equal-pole Givens applied to U
  columns AND VT rows (vs the symmetric-eig dlaed2 which rotates only Q).
- **D(svd)-13 — interleaved-Löwner Z recompute (dlasd3).** The updated weights are formed as
  a single running product (`U(i,j)*VT(i,j)` from dlasd4's delta/work) to avoid the K-factor
  overflow — the same fix as the eig D&C ([[feedback_lowner_product_overflow_interleave]]).
- **D(svd)-14 — column-major GEMM via the swap identity.** `gemm_cm_nn` routes large blocks
  to row-major `gemm_parallel_auto` via `C_cm=A·B ⟺ Cᵀ=Bᵀ·Aᵀ` (col-major buffer viewed
  row-major IS its transpose); tiny leaf merges stay scalar (job-dispatch + arena floor).

**Performance (i9-14900K AVX2, f64; full SVD values+vectors):** beats **every** reference at
**all** N=128–1024 — vs Eigen `BDCSVD` (fair same-compiler gate) **1.59–3.21×**, vs LAPACK
`dgesdd` **1.37–4.55×**, vs `dgesvd` **4.78–10.48×**, vs `JacobiSVD` 11–28×. **@512: full SVD
690 ms → 52.9 ms this session (13×); C/BDCSVD flipped 0.17 (losing 6×) → 1.76 (winning).**
Reconstruction ~1e-14. (LAPACK on MSVC = generic OpenBLAS = accuracy oracle; Eigen `BDCSVD`
is the fair speed gate, beaten at every N.) Validated by reconstruction gates: dlasd0 full
D&C `‖B−UΣVᵀ‖<1e-9` (n=6–50, multi-level) + dlasdq base + dlasd4/dlasd5 secular residual.

§21 ✅ Accepted — v3b-2 D&C SVD beats Eigen + LAPACK on the full SVD at scale. Filed
follow-on `v3b-2-parallel-merges` (parallelize dlasd0's independent same-level merges to
widen the lead). Complex SVD remains v3b-1c.

## §22 Amendment (2026-05-23) — v3b-3 randomized SVD + symmetric eig

`rsvd` (randomized truncated SVD, Halko-Martinsson-Tropp 2011) and `rsyev` (randomized
symmetric eigendecomposition) in `svd.{hpp,cpp}` — both built ENTIRELY on the shipped
deterministic `gemm_parallel_auto` / Householder-QR (`factor_qr`+`apply_q`) / dense `svd` /
`eig_sym`, with NO new numerical kernels. `rsvd`: Gaussian sketch (sum-of-12-uniforms CLT,
avoids std::log/cos + the no-std-math guard) → QR range finder → `power_iters` subspace
iterations (re-orthonormalized) → small dense `svd` of `B=QᵀA` → lift `U=Q·Ũ`, truncate to
rank. `rsyev`: range finder → Rayleigh-Ritz `B=QᵀAQ` → `eig_sym(B)` → lift `V=Q·V_b`, top-k
descending.

- **D(svd)-15 — `rsyev` uses Rayleigh-Ritz, NOT the Nyström `C⁻ᵀ` variant.** Deliberate
  divergence: Rayleigh-Ritz (`QᵀAQ` + dense eig) is more general (any symmetric A, not just
  PSD) and reuses the `eig_sym` we already beat Eigen+LAPACK with, vs the PSD-streaming
  Nyström-Cholesky form. (`feedback_document_paper_divergence_explicitly`.)
- **No head-to-head bench (intentional):** Eigen/LAPACK have no randomized path, so the
  v3b-3 gate is ACCURACY + structural speed (O(mn·ℓ) vs full O(mn·min)), not beat-the-
  reference. Gated: exact rank-r SVD reconstruction `<1e-8` + orthonormal U/V; rank-r PSD eig
  residual `‖Av−λv‖ < 1e-7`. Deterministic given `seed`.

CLI: `hesap.dense.rsvd.{f32,f64}` + `hesap.dense.rsyev.{f32,f64}` (the per-op-CLI rule).
DoD: debug / asan (memory-clean) / shipping (LTO) / tidy green on hesap-dense. Randomized
Nyström-`C⁻ᵀ` PSD-streaming variant filed as the optional follow-on `v3b-3-nystrom-cholesky`.

§22 ✅ Accepted — v3b-3 randomized SVD + symmetric eig shipped. **v3b (SVD) is now
substantively complete: v3b-1 (bidiag+dbdsqr+blocked) + v3b-2 (D&C crush) + v3b-3
(randomized) ✅; remaining v3b-1c (complex SVD).**

## §23 Amendment (2026-05-23) — v3b-1c complex SVD + v3b CLOSE

Complex SVD (`zgesvd`-class) in `detail/svd_complex.hpp` + the complex driver in `svd.cpp`.
A complex Golub-Kahan reduction takes A (complex m×n) to a REAL bidiagonal (d,e) with
complex unitary Q, P, then the **real (d,e) feed the already-shipped real dlasd0/dbdsqr (the
D&C crush, reused verbatim)** and a complex back-transform `U=Q·U_b`, `V=P·V_b` lifts the
vectors. `svd`/`svdvals` dispatch real-vs-complex by `if constexpr (is_complex_v<T>)`; m<n
via A^H (swap U/V). c32/c64 + CLI (`hesap.dense.{svd,svdvals}.c{32,64}`, interleaved [re,im]).

- **D(svd)-16 — bidiagonalize_complex skips the SECOND ZLACGV (un-conjugate).** zgebd2
  conjugates the row before ZLARFG (forces e[i] REAL) then un-conjugates after; we skip the
  un-conjugate so the stored right-reflector tail is the actual w-tail that `form_p_complex`
  reads directly — internally consistent (not LAPACK-storage-compatible, which we don't need),
  removes a conjugation-convention bug surface.
- **D(svd)-17 — complex svdvals routes through the complex driver** (computes vectors then
  returns the spectrum); the values-only dqds-direct path is the perf follow-on
  `v3b-1c-svdvals-dqds-direct`.
- **D(svd)-18 — complex phase pin.** Per singular vector, the largest-|.| entry of each V
  column is made real-positive (rotate U,V columns by conj(phase); A=U S V^H preserved).

Gated: complex Householder unitary (200 trials <1e-12); `A=Q B P^H` reconstruction + Q/P
unitary <1e-11; full `A=U S V^H` reconstruction + U/V unitary + S descending <1e-9 (m≥n,
m<n, and the D&C bidiagonal path n≥64). DoD debug/tidy/asan(memory-clean)/shipping green.
**HONEST perf note:** the complex *bidiagonalization* is UNBLOCKED (the real bidiagonal SVD it
reduces to IS the D&C crush, so moderate-N is competitive); the at-scale complex speed-crush
needs a blocked complex `zgebrd` — filed `v3b-1c-blocked-complex-bidiag` (mirrors the real
v3b-1a-perf path). Correctness + the algorithmic reuse of the crushing real engine are done.

§23 ✅ Accepted — v3b-1c complex SVD shipped + gated. **🎉 v3b (SVD) CLOSED — v3b-1 (bidiag +
blocked dgebrd/dorgbr + dbdsqr) + v3b-2 (Gu-Eisenstat D&C, beats Eigen BDCSVD + LAPACK dgesdd)
+ v3b-3 (randomized rsvd/rsyev) + v3b-1c (complex). Real full SVD crushes Eigen + LAPACK at
scale; complex correct + reuses the crush. Filed perf follow-ons: v3b-1c-blocked-complex-bidiag,
v3b-1c-svdvals-dqds-direct, v3b-2-parallel-merges, v3b-3-nystrom-cholesky.** NEXT = v3c
(least-squares — consumes the SVD).

## §24 Amendment (2026-05-23) — v3c least-squares family (lstsq + pinv + NNLS + TLS) + v3c CLOSE

v3c builds the least-squares family on the shipped factorizations (which already beat Eigen +
LAPACK), adding one genuinely new factorization (column-pivoting QR + COD) and the BLAS-3
reflector-application lever that makes the dense-inverse path crush.

**v3c-1 — `lstsq` + `pinv`.**
- **Column-pivoting Householder QR** (`QRColPiv`/`factor_qr_colpiv`, `dgeqp3`/`dlaqp2` faithful):
  Businger-Golub largest-remaining-column-norm pivot on a TRANSPOSED scratch (each column
  contiguous → SIMD norm/dot/axpy) + the LAPACK partial-column-norm **downdate-with-recompute**
  (recompute when the downdated norm degrades past √eps). Reveals numerical rank from the
  non-increasing |R[k,k]| diagonal.
- **Complete orthogonal decomposition** (`COD`/`factor_cod`/`solve_cod`, LAPACK dgelsy): after
  col-piv QR reveals rank `r`, reduce the r×n upper-trapezoid `[R11 R12]` to a r×r triangular
  `T11` by RZ Householder reflectors from the right (`dtzrzf`/`dlatrz`). `solve_cod` = min-norm
  least-squares: Qᵀb → tri-solve T11 → zero-pad → Zᵀ apply → undo column permutation.
- **`lstsq`** dispatch (D(lstsq)-2): `Auto` → COD for real (rank-revealing + fast, the robust
  default), SVD for complex (the COD fast path is real). `QR` = full-rank fast path (dgels).
  `SVD` = max-accuracy / every shape. Multi-RHS matrix B; lazy residual (`with_residual`);
  rcond default `max(m,n)·eps`; returns {X, rank, residual}.
- **`pinv`** (D(lstsq)-3): `Auto` → COD for real (matches Eigen `pseudoInverse()`), SVD for
  complex / max accuracy. COD path: `A⁺ = P·Zᵀ·[T⁻¹ 0; 0 0]·Qᵀ`.
- **Blocked-reflector-apply attack** (user-directed): BLAS-3 `dlarfb` (`detail/apply_q_block.hpp`,
  all 4 side×transpose modes, compact-WY + gemm, bit-matches the scalar applies) + blocked
  recursive `trtri`. **pinv 0.11× → 1.60× Eigen** (the scalar O(r³) T⁻¹ back-sub with strided
  column access was the large-r bottleneck; the col-piv QR factor was NOT — the COD-solve path
  ties Eigen with the same factor).
- **Bench (i9-14900K AVX2, f64):** CRUSHES LAPACK everywhere (lstsq QR 1.70–8.69× / COD
  1.88–12.19× / SVD 1.07–4.01× vs dgels/dgelsy/dgelsd). vs Eigen: pinv 1.12–1.60× (↑ with n),
  SVD-path 1.08–2.40× (n≥128 beats BDCSVD), COD-default 0.84–1.07× (parity, beats at n=512),
  QR-tall 0.69–0.93× (the ADR-0083 row-major small-n layout-fit accepted in v0e; filed
  `v3c-1-qr-tall-blocked`). Filed `v3c-1-blocked-rz-apply` (rank-deficient Z-apply blocking).

**v3c-2 — NNLS + TLS.**
- **NNLS** (`nnls`, real f32/f64 — x ≥ 0 is meaningless for complex): Lawson-Hanson 1974
  active-set with an INCREMENTAL thin QR of the passive columns (Björck §5.8) — add =
  re-orthogonalised (2-pass) modified Gram-Schmidt; remove = Givens re-triangularisation sweep
  (the "up/downdate", no full refactor per active-set change). **D(lstsq)-1**: the entering
  variable is the largest gradient with ASCENDING-index tie-break (strict `>`); the rank /
  singular cutoff is strict `σ_i > rcond·σ_max`. Gated by KKT optimality (x≥0; x_j>0 ⇒ w_j≈0;
  x_j=0 ⇒ w_j≤0) + exact recovery of a full-rank non-negative ground truth + the 2×2 textbook
  case. (Head-to-head vs Eigen's `unsupported/Eigen/NNLS` filed `v3c-2-nnls-vs-eigen-bench`; the
  solution is unique for full-rank A, so the correctness gates pin it.)
- **TLS** (`tls`, 4 type variants via the shipped complex SVD): augmented `C=[A|B]`, SVD,
  partition the last d right singular vectors `[V12; V22]` ⟹ `X = −V12·V22⁻¹`. `exists=false`
  flags V22 singular (b ⟂ range(A) / σ_n(A) ≤ σ_{n+d}(C)). The d×d V22 inverse uses a
  type-generic Gauss-Jordan (real + complex). Gated by exact recovery on consistent systems
  (smallest σ = 0 ⇒ null vector `[x; −1]`), multivariate (d=2), and complex.
- **CLI:** +14 commands across v3c (`lstsq`/`pinv` × {f32,f64,c32,c64}, `nnls` × {f32,f64},
  `tls` × {f32,f64,c32,c64}) — every op has a command.

**D(lstsq) determinism pins:**
- **D(lstsq)-1** — all least-squares tie-breaks resolve by ascending original column index
  (NNLS entering variable, col-piv QR equal-norm pivot). Rank / singular-value cutoffs are
  strict `value > tol` (closed boundary excluded).
- **D(lstsq)-2** — `lstsq` Auto = COD (real) / SVD (complex); the fast/accurate paths are opt-in
  via `LstSqMethod`. Multi-RHS solves each column independently (bit-identical to single-RHS).
- **D(lstsq)-3** — `pinv` Auto = COD (real) / SVD (complex); the dense inverse is formed via
  blocked `dlarfb` + blocked `trtri`, never per-reflector scalar application.

§24 ✅ Accepted — v3c shipped + gated; 4-config DoD (debug/shipping/tidy/asan) green; full
hesap-dense suite **279 cases / 105 764 assertions**. **🎉 v3c (least-squares family) CLOSED —
lstsq + pinv (col-piv QR + COD + SVD, blocked-apply crush) + NNLS (Lawson-Hanson + Givens
up/downdate) + TLS (SVD). Crushes LAPACK across the board; beats Eigen on pinv + SVD-path, ties
the COD default; NNLS/TLS correctness-gated.** NEXT = v3d (non-symmetric eigensolver: balance +
Hessenberg + Francis double-shift Schur + AED + eigenvectors). 18-config sweep delegated to CI.
