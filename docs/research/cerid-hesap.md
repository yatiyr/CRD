# Cerid — `crd-hesap` numerical substrate research

**Date:** 2026-05-10
**Locks:** ADR-0065 (`crd-hesap` numerical computing substrate).
**Phase plan:** `docs/phases/phase-3.1.6-hesap.md`.
**Companion research:** `docs/research/sparsematrices.md` (pre-existing
Cerid sparse-matrix background).

> Source-of-truth for the *why* behind every algorithm /
> data-structure / API choice in `crd-hesap`. ADR-0065 cites this file.
> Phase plan implements against it.

---

## 1. Why a MATLAB-class numerical substrate (per-domain)

| Domain | Pulls | Without `crd-hesap` |
| --- | --- | --- |
| Games — runtime | small dense LA (already in `crd-math`); minor stats | OK, no need |
| Games — content tools | SVD for asset compression, optimization for layout | per-tool bespoke |
| Eylem v3 (XPBD soft) | small Jacobi iterations inside constraint solve | inline; no substrate |
| Eylem v6 (Featherstone) | dense 6×6 spatial inertia operations | inline in eylem |
| Eylem v7 (FEM) | sparse CG/PCG, sparse Cholesky, AMG preconditioner | **needs internal solver** ← see eylem v7 dependency note |
| Eylem v9 (differentiable) | reverse-mode autodiff, gradient propagation | bespoke per-feature; brittle |
| Robotics motion planning | constrained optimization (SQP, IPOPT-class), QP, ODE | external library + plumbing |
| Robotics control | LQR, MPC (real-time QP), state-space methods | external + brittle |
| Medical visualization | DICOM volume processing, 3D segmentation | external + per-pipeline |
| Medical biological sim | stiff ODE/DAE (cellular biochem timescale spread) | external library |
| Cinematic VFX | numerical sim (fluids, large deformation), large sparse | external library |
| Cinematic — colour pipeline | LUT processing (interpolation), tone-mapping curves | inline OK |
| DAW — DSP | FIR/IIR filters, biquad, FFT, convolution, resampling, spectral | external library + plumbing |
| DAW — analysis tools | spectral analysis, statistical tests on signals | external |
| MATLAB-class tool | **everything above** + REPL + plug-in API + notebook | impossible without substrate |
| Scientific computing user (researcher writing a tool atop Cerid) | full surface | impossible without substrate |

The multi-domain mandate (CLAUDE.md §1) makes `crd-hesap` non-negotiable
unless we accept that several of those domains run on per-feature
bespoke math — fragmenting the engine and breaking the "elite-tier"
quality bar.

---

## 2. Industry landscape

### 2.1 Dense linear algebra (BLAS / LAPACK class)

- **LAPACK** (Anderson et al. 1999, ongoing). The reference dense LA
  library since 1992. Fortran source; massively tested over 30 years.
  The canonical choice of algorithms (LU with partial pivoting,
  Householder QR, Golub-Reinsch SVD, QR algorithm with shifts for
  eigenvalues) is what every production library converges to.
- **BLAS** (Lawson et al. 1979, Dongarra et al. 1990). Three levels:
  L1 (vector-vector), L2 (matrix-vector), L3 (matrix-matrix). The
  decomposition isolates the hot path (gemm) where 90% of dense LA
  work lives.
- **Goto/BLIS** (Goto-van de Geijn 2008; van Zee-van de Geijn 2015).
  Modern `gemm` architecture: outer loops cache panels of A and B in
  L2/L3, microkernel runs on L1-resident sub-panels with full SIMD
  unrolling. **The right choice for Cerid** — modular, portable,
  ~80–90 % of vendor BLAS perf with disciplined microkernel work.
- **Intel MKL** (closed source). Vendor-tuned, ~95 % theoretical peak
  on Intel CPUs. Reference for "maximum performance possible." Sets
  the bar.
- **OpenBLAS** (open). Auto-tuned BLAS; the open standard. Performance
  gap to MKL is small.
- **Eigen** (header-only C++). Excellent ergonomics + expression
  templates; performance competitive with hand-tuned BLAS for small
  fixed-size + good for general dense. Reference for **API ergonomics**.
- **cuBLAS** (NVIDIA). The GPU-side reference. `crd-hesap-gpu`
  patterns mirror it.

### 2.2 Sparse linear algebra

- **SuiteSparse** (Tim Davis, ongoing). CHOLMOD (sparse Cholesky),
  UMFPACK (sparse LU), KLU (sparse LU for circuit simulation), SPQR
  (sparse QR), AMD (approximate minimum degree), COLAMD, METIS
  interface. **The canonical sparse-direct reference suite.**
- **PETSc** (Argonne, ongoing). The reference scientific-computing
  toolkit; PDE focus. Distributed memory + iterative solvers + AMG.
- **Trilinos** (Sandia, ongoing). Massive scientific computing
  framework: Epetra/Tpetra (distributed sparse), Belos (Krylov
  iterative), Anasazi (eigensolvers), AztecOO. Reference for
  "scientific computing at HPC scale."
- **MUMPS** (multifrontal massively parallel sparse direct).
- **SuperLU** (sparse LU, multiple variants: serial, multithreaded, dist).
- **PARDISO** (Intel MKL). Dense + sparse direct + iterative.
- **Hypre** (LLNL). AMG + structured grids. The reference AMG impl.
- **Saad (2003)** — *Iterative Methods for Sparse Linear Systems*.
  The canonical textbook for CG, GMRES, BiCGSTAB, MINRES, IDR(s),
  preconditioners. **Cerid's iterative-solver chapter is built from
  this book.**
- **Eigen-Sparse**. Solid header-only C++ sparse + iterative; weaker
  than SuiteSparse for direct.

### 2.3 Eigenvalue solvers

- **LAPACK eigenvalue routines** — dense reference (`dsyev`, `dgeev`,
  `dsyevr`).
- **ARPACK** (sparse eigenvalue). Implicitly restarted Arnoldi (IRA);
  the canonical sparse non-symmetric eigensolver. Used by SciPy.
- **SLEPc** (Scalable Library for Eigenvalue Problem Computations;
  PETSc-based). Distributed-memory.
- **LOBPCG** (Locally Optimal Block Preconditioned CG). For sparse
  symmetric eigenvalue with preconditioning.
- **FEAST** (Polizzi). Contour-integral eigensolver for spectrum
  slices. Reserved for specialty workloads.

### 2.4 Optimization

- **IPOPT** (Wächter-Biegler 2006). Interior-point filter line search
  for large-scale nonlinear programming. **The reference NLP solver**;
  used in robotics, energy, scientific computing.
- **OSQP** (Stellato et al. 2020). Operator-splitting QP solver.
  Tight, modern, embedded-friendly. **The right choice for QP in
  Cerid** — used by MPC, robotics, control.
- **L-BFGS** (Liu-Nocedal 1989). Limited-memory quasi-Newton; the
  unconstrained workhorse. Used everywhere.
- **Nocedal-Wright** (textbook). The canonical numerical optimization
  reference.
- **Clarabel** (Goulart-Chen 2024). Modern interior-point conic
  solver in Rust. Reference for clean modern conic IPM.
- **HiGHS** (Huangfu et al.). Open LP/MIP solver. Modern reference for
  simplex + interior-point LP.
- **CVXOPT / CVXPY** (Boyd group). Convex optimization
  reference (modelling layer; Cerid's REPL facade can take cues here).
- **CasADi** (Andersson et al.). Algorithmic differentiation +
  optimization. Reference for AD-aware optimisation.

### 2.5 ODE / DAE / PDE

- **SUNDIALS** (Hindmarsh et al. 2005). CVODE (non-stiff + stiff ODE),
  IDA (DAE), ARKODE (additive RK + IMEX), KINSOL (nonlinear systems).
  **The canonical scientific ODE/DAE reference.** Used by every serious
  scientific computing tool.
- **DifferentialEquations.jl** (Rackauckas-Nie 2017). Julia ecosystem;
  modern reference for ODE solver completeness — 200+ algorithms,
  callbacks, sensitivity analysis.
- **Hairer-Wanner** textbooks (1993, 1996). Canonical reference for
  ODE algorithms (Vol I non-stiff, Vol II stiff). The DOPRI5 / DOPRI8
  / Cash-Karp / BDF algorithm choices come from here.
- **Boost.Odeint** (Ahnert-Mulansky). Header-only C++ ODE solvers.
- **Trilinos Tempus** — for embedded scientific computing.

### 2.6 FFT

- **FFTW** (Frigo-Johnson 2005). "Fastest FFT in the West." Self-tuning
  via run-time profiling; multiple algorithms (Cooley-Tukey radix-2
  / mixed-radix, Bluestein for primes, Rader, prime-factor). **The
  reference FFT design.**
- **Pocketfft** (Reinecke). Modern, header-only, compact.
- **Kissfft** (Borgerding). Simple embedded FFT.
- **Intel MKL FFT**. Vendor-tuned reference.
- **cuFFT**. GPU reference.

### 2.7 DSP

- **Oppenheim-Schafer** (textbook, 3rd ed. 2010). Canonical DSP
  reference. FIR/IIR filter design, windowing, spectral analysis.
- **Parks-McClellan / Remez exchange** for optimal FIR design.
- **Audio Cookbook (RBJ EQ Cookbook)**. Canonical biquad EQ formulas
  used by every DAW + audio framework.
- **JUCE / WDL-OL / iPlug2 / RtAudio**. Reference DSP frameworks.
  `crd-hesap-dsp` API takes cues from JUCE's clean separation of
  filter design vs filter processing.
- **PFFFT, KFR**. Fast modern DSP libraries.

### 2.8 Statistics + RNG

- **Boost.Math** + **Boost.Random**. Canonical C++ statistics +
  distributions. Reference for API ergonomics + numerical accuracy.
- **GSL** (GNU Scientific Library). Comprehensive, but C-style API.
- **PCG** (O'Neill 2014). Splittable counter-based RNG family.
  **Cerid's primary RNG** — splittable streams, deterministic, good
  statistical properties, minimal state.
- **Xoshiro256\*\*** (Blackman-Vigna 2018). Fast non-splittable; default
  for non-splitting needs.
- **Stan-math**. Reference for statistical distributions + reverse-mode
  AD over them — exactly the pattern `crd-hesap-stats` +
  `crd-hesap-autodiff` follow.

### 2.9 Special functions

- **Boost.Math.SpecialFunctions**. Canonical C++ reference.
- **Cephes** (Stephen Moshier). Canonical numerical-special-function
  library (since 1980s; many libs vendor it). Gamma, beta, erf, Bessel,
  hypergeometric, etc.
- **AMOS** (Bessel functions). Reference Bessel implementation.

### 2.10 N-dim arrays + tensor + autodiff

- **NumPy** (Harris et al. 2020). Reference for N-dim array
  ergonomics, broadcasting semantics, indexing, einsum.
- **JAX** (Bradbury et al. 2018). Modern reference for functional AD +
  JIT. Reference for AD over BLAS at scale.
- **PyTorch ATen / autograd**. Tape-based reverse-mode AD reference.
- **Stan-math**. Statistical AD reference; clean operator-overloading
  reverse mode.
- **Adept**, **autodiff**, **CppAD**. C++ AD libraries.
- **Enzyme** (Moses-Churavy 2020). LLVM-based AD via compiler IR.
  Reference for advanced AD; not a Cerid pattern (compiler integration
  is too invasive).
- **Taichi** (Hu et al. 2019). Differentiable physics + AD; reference
  for physics-AD integration.

### 2.11 GPU acceleration

- **cuBLAS / cuSPARSE / cuFFT / cuSOLVER**. NVIDIA's reference GPU
  numerical stack. Mirror in `crd-hesap-gpu`.
- **rocBLAS / rocSPARSE / rocFFT / rocSOLVER**. AMD parallel.
- **MAGMA** (UTK). GPU + CPU hybrid LA. Reference for cross-CPU/GPU
  algorithms.
- **Kokkos** (Trott et al.). Performance-portable kernel framework
  (CPU/GPU); reference for cross-backend abstraction.
- **CUB / Thrust**. GPU primitives (scan, reduce, sort).

### 2.12 Whole-stack reference systems

- **MATLAB** (MathWorks, 1984+). Closed source but well-documented;
  the reference for **user ergonomics** — backslash solver, structured
  bindings on factorisations, plot integration, REPL flow. Cerid's
  Layer-B (MATLAB-class facade) takes cues from MATLAB syntax directly.
- **Octave** (open-source MATLAB clone). Reference for "MATLAB
  syntax in C++".
- **NumPy + SciPy** (Python ecosystem). The Python-side reference;
  combined with Matplotlib for plots, IPython for REPL, Jupyter for
  notebook. Cerid's REPL takes cues from this stack.
- **Julia LinearAlgebra + SparseArrays + DifferentialEquations.jl +
  Optim.jl + JuMP.jl**. Modern reference for **type-aware numerical
  ergonomics** in a language-level numerical stack. Cerid's typed
  C++ API (Layer A) takes cues from Julia's multiple-dispatch design.

---

## 3. Algorithm choices — dispositions with rationale

### 3.1 Dense `gemm`

| Algorithm | Year | Cerid disposition |
| --- | :---: | --- |
| **Goto/BLIS layered (microkernel + outer cache panels)** | 2008/2014 | **CHOSEN — v2.** Most modular performant `gemm` design. Microkernel can be SIMD-tuned per ISA without changing outer loops. ~80–90 % of vendor BLAS perf. |
| Strassen recursion | 1969 | Reserved for very large; `n^2.807` complexity but worse constant + numerical instability. Strassen-Winograd ditto. |
| Naïve triple loop | n/a | Reference baseline only. |
| Auto-tuned (ATLAS) | 2001 | Reserved; Goto-style covers 95 % of need. |

### 3.2 Dense direct solvers

| Problem | Algorithm | Reference | Reserved alternatives |
| --- | --- | --- | --- |
| LU | Doolittle + partial pivoting | LAPACK `dgetrf` | Recursive blocked LU (Sandhu-Chee 2018) |
| Cholesky | Right-looking blocked | LAPACK `dpotrf` | Left-looking |
| QR | Householder reflections | LAPACK `dgeqrf` | Givens rotations (banded), Modified Gram-Schmidt (caution) |
| LDLT | Bunch-Kaufman pivoting | LAPACK `dsytrf` | Aasen's algorithm |

### 3.3 Dense eigenvalue + SVD

- **Symmetric eigenvalue:** Householder tridiagonalization → QR
  algorithm with Wilkinson shift. Standard. (`dsyev`-class.)
- **Non-symmetric eigenvalue:** Hessenberg reduction → Schur
  decomposition via QR with double-shift. Standard. (`dgeev`-class.)
- **SVD:** Householder bidiagonalization → bidiagonal QR algorithm
  (Golub-Kahan). Standard. Randomized SVD reserved for very large
  systems.

### 3.4 Sparse storage formats

| Format | When | Notes |
| --- | --- | --- |
| **COO** (Coordinate) | construction-time | easy to assemble from triplets; convert to CSR for ops |
| **CSR** (Compressed Sparse Row) | default operational | best for spmv; standard in most libraries |
| **CSC** (Compressed Sparse Col) | for column-oriented ops | LU factorization scratch |
| **BSR** (Block Sparse Row) | for blocked structure | 3–4× faster spmv on FEM matrices with natural block structure |
| **ELL** (Ellpack) | uniform row sparsity | GPU-friendly |
| **HYB** (Hybrid) | mixed sparsity | ELL + COO overflow |

### 3.5 Iterative solvers — the canon (Saad 2003)

- **CG** (Hestenes-Stiefel 1952): symmetric positive-definite. The
  workhorse.
- **PCG**: CG + preconditioner. Most useful in practice.
- **BiCGSTAB** (Van der Vorst 1992): non-symmetric. Smoother
  convergence than BiCG.
- **GMRES** (Saad-Schultz 1986): non-symmetric. Restart for memory.
  Classical Gram-Schmidt + reorthog for stability.
- **MINRES** (Paige-Saunders 1975): symmetric indefinite. CG would
  fail here.
- **LSQR** (Paige-Saunders 1982): least squares.
- **IDR(s)** (Sonneveld-van Gijzen 2008): induced dimension
  reduction; modern non-symmetric alternative.

### 3.6 Preconditioners

- **Jacobi** (diagonal). Trivial, often surprisingly effective.
- **IC(0) / IC(k)** (Incomplete Cholesky). For SPD systems.
- **ILU(0) / ILU(k)** (Incomplete LU). General-purpose.
- **AMG** (Algebraic Multigrid). The heavy hitter for elliptic PDE
  systems (Poisson, elasticity). Hypre / Trilinos ML reference.
  Smoothed-aggregation AMG (Vaněk et al.) is the modern variant.
- **Block-Jacobi** + **Additive Schwarz**. For block-structured
  problems and FEM-on-mesh assembly.

### 3.7 Sparse direct solvers

- **Sparse Cholesky:** supernodal (CHOLMOD-class). The canonical
  approach.
- **Sparse LU:** left-looking (Gilbert-Peierls) for general sparse;
  supernodal LU for performance.
- **Sparse QR:** multifrontal (SuiteSparseQR-class).
- **Reordering:** AMD (Approximate Minimum Degree) for fill reduction;
  RCM for bandwidth; nested dissection (METIS-class) for very large
  systems.

### 3.8 Optimization

| Class | Algorithm | Reference |
| --- | --- | --- |
| Unconstrained | L-BFGS + Wolfe line search | Liu-Nocedal 1989; Nocedal-Wright 2006 |
| Trust region | Steihaug-Toint CG | Steihaug 1983 |
| Newton (when Hessian available) | Modified Newton + line search | Nocedal-Wright |
| QP | OSQP-style ADMM | Stellato et al. 2020 |
| LP | Revised primal simplex + Mehrotra interior-point | textbook |
| NLP | Mehrotra interior-point with filter line search | Wächter-Biegler 2006 (IPOPT model) |
| MIP-basic | Branch-and-bound (LP relaxation) | reserved scope |

### 3.9 ODE / DAE

| Class | Algorithm | Reference |
| --- | --- | --- |
| Non-stiff explicit | DOPRI5 (5th order, FSAL) + DOPRI8 + Cash-Karp | Hairer-Nørsett-Wanner 1993 |
| Step control | PI step controller | Gustafsson 1994 |
| Stiff implicit | BDF (1–6) with Newton-Krylov inner | SUNDIALS CVODE |
| Stiff explicit | Rosenbrock W-methods | Hairer-Wanner 1996 |
| DAE index-1 | BDF | SUNDIALS IDA |
| DAE index-3 | Pantelides reduction | Pantelides 1988 |
| Symplectic (Hamiltonian) | Verlet, Runge-Kutta-Nyström | Hairer-Lubich-Wanner |

### 3.10 FFT

| Algorithm | Cerid disposition |
| --- | --- |
| **Cooley-Tukey radix-2 + mixed-radix** | **CHOSEN — v11.** Standard. Fast for power-of-2 + composite sizes. |
| **Bluestein** for prime sizes | **CHOSEN.** Handles arbitrary size including primes via convolution. |
| Stockham | Reserved alternative — better cache behaviour, more memory. |
| Rader | Reserved — alternative to Bluestein for primes. |

### 3.11 DSP filter design

- **FIR design:** Windowed sinc (low-effort) + Parks-McClellan/Remez
  (optimal equiripple). Standard.
- **IIR design:** Bilinear transform of analog Butterworth / Chebyshev
  I / Chebyshev II / Elliptic / Bessel. Standard.
- **Biquad cookbook** (RBJ): peaking EQ, low/high shelf, low/high
  pass, band-pass, notch, all-pass. Used by every DAW.

### 3.12 RNG

- **PCG family** (O'Neill 2014). Splittable, counter-based, ~16 bytes
  state, period ≥ 2^128, excellent statistical properties.
- **Xoshiro256\*\*** (Blackman-Vigna 2018). Faster than PCG when
  splitting isn't needed, ~32 bytes state, period 2^256-1.

### 3.13 Autodiff

- **Forward mode:** Dual numbers / Jet types (Pearlmutter-Siskind
  references; Stan-math implementation reference).
- **Reverse mode:** Tape-based with operator overloading. Stan-math is
  the cleanest C++ reference; PyTorch / JAX establish the modern
  patterns. Operator-overloading is the right approach for Cerid
  (vs source transformation a la Tapenade or compiler integration a
  la Enzyme — both invasive).
- **Higher-order:** Hessian via forward-over-reverse (cheap when
  output is scalar; cubic complexity in input dim otherwise).

---

## 4. API ergonomics study

### 4.1 The two-layer choice (recap from ADR-0065)

**Layer A (Eigen-class typed API)** — primary, used by engine code.
Reference: Eigen, modern Julia LinearAlgebra.

**Layer B (MATLAB-class facade)** — opt-in syntactic sugar for the
REPL + scripting layer. Reference: MATLAB, Octave, NumPy.

### 4.2 Why two layers, not one

- Engine production code wants typed C++ (compile-time errors,
  predictable perf, IDE autocomplete).
- MATLAB-class tool users (researchers, scientific tool authors) want
  ergonomic notation (`x = A \ b`).
- Forcing one camp to use the other's interface creates friction.
- The two layers share storage + algorithms; only the calling syntax
  differs.

### 4.3 Reference patterns for matrix solve

| Library | Syntax |
| --- | --- |
| LAPACK (Fortran) | `dgesv(n, 1, A, n, ipiv, b, n, info)` |
| Eigen (C++) | `x = A.lu().solve(b)` |
| Julia | `x = A \ b` (auto-dispatches by `A` type) |
| NumPy | `x = np.linalg.solve(A, b)` |
| MATLAB | `x = A \ b` |
| **Cerid Layer A** | `auto x = solve(A, b)` (free function; auto-dispatches LU/Cholesky/QR by matrix tag) |
| **Cerid Layer B** | `x = A \ b` (operator overload in `crd::hesap::matlab` namespace) |

---

## 5. Performance posture

### 5.1 Targets

| Workload | Target | Reference |
| --- | --- | --- |
| Dense `f32 gemm` (1024×1024) | ≥80 % theoretical peak on AVX2 hardware | OpenBLAS / BLIS |
| Sparse spmv (1M unknowns) | bandwidth-bound; ≥85 % theoretical bandwidth | reference benchmarks |
| CG on Poisson (128³ grid) | converges in ≤200 iter at tol 1e-8 | textbook condition number |
| FFT (size 4096) | ≥70 % FFTW perf | FFTW reference |
| L-BFGS on Rosenbrock (100D) | converges in ≤50 iter | Nocedal-Wright benchmark |

### 5.2 What we do not chase

- **Last 5 % of vendor BLAS perf.** Goto/BLIS-style gives us 80–90 %.
  The remaining gap is instruction-scheduling vs vendor-specific tuning;
  not worth the complexity at v0.
- **AVX-512 microkernel-tuned.** Add only when AVX-512 hardware
  becomes a Cerid CI target (currently AVX2 + NEON + scalar).
- **Distributed memory.** Cerid is single-node. MPI-style scaling
  reserved for Phase 8+ if tooling consumer arises.

### 5.3 Determinism vs performance trade-offs

ADR-0063 determinism contract bans:
- Fast-math / `-ffp-contract=on` (would let the compiler reorder FMA).
- `std::sin/cos/atan2` (different on different platforms).
- Non-commutative reductions (varies across thread counts).

These cost roughly 5–15 % perf vs unrestricted code. Accept the cost.
Determinism is non-negotiable for replay-hash CI + scientific
reproducibility.

---

## 6. Numerical accuracy posture

### 6.1 Backward error guarantees

For dense direct solvers, target backward error within `O(n × eps)`:
- LU: `||A - L*U|| ≤ n * eps * ||A||` (with partial pivoting; growth
  factor caveat).
- Cholesky: `||A - L*L^T|| ≤ n * eps * ||A||` (for SPD A).
- QR: `||A - Q*R|| ≤ n * eps * ||A||`.

### 6.2 Forward error for iterative solvers

For PCG on SPD systems with κ = condition number:
- Iteration count ≈ `0.5 * sqrt(κ) * log(2/tol)` (worst case).
- Forward error ≤ `tol * κ`.

### 6.3 Special functions

Special functions (gamma, beta, erf, Bessel) target ulp-error ≤ 5
across documented argument ranges. Cephes-style coefficient tables for
each region.

### 6.4 Test discipline

Numerical-accuracy tests are a separate CTest label. Run on every CI
build but logged as benchmarks rather than asserted as pass/fail above
documented tolerances.

---

## 7. Plug-in / scripting / REPL — design notes

### 7.1 The MATLAB-tool ambition

The MATLAB-class tool is a real project that Cerid underwrites. It is
not "MATLAB rewritten in C++" — it is "every numerical primitive
MATLAB has, exposed via a clean C++ API + an opt-in MATLAB-syntax
facade + a REPL surface."

A user can:

1. Open a `.cnb` notebook in the sandbox / editor.
2. Type `x = A \ b`.
3. See the result printed.
4. Type `plot(x)` and see the plot inline.
5. Save the notebook; reopen later; identical results (determinism).
6. Export the notebook as a standalone CLI script that runs without
   the editor (via the C ABI).

### 7.2 Plug-in C ABI

`crd-hesap-repl` ships a stable C ABI. External code (Lua, Python,
custom DLLs) calls `crd::hesap` through it. Pattern matches ADR-0034
hot-reload C++ scripting.

### 7.3 Plot routing

In the engine: `plot(x)` populates a `crd::hesap::repl::PlotPanel`
which the host (sandbox / editor) draws via ImGui + a thin GPU
rasteriser.

In the standalone CLI tool: `plot(x)` emits a PNG / SVG via a CPU
rasteriser sub-slice (small, no Vulkan dependency).

---

## 8. Open research notes

Topics surfaced during research that aren't in v0–v18 but should be
revisited later:

- **Compiler-integrated AD (Enzyme).** Enzyme runs at LLVM IR level
  and can autodiff arbitrary code. More general than operator
  overloading but invasive. Reserved.
- **JIT-specialised kernels (XLA / NumPy@nopython).** Run-time
  kernel specialisation for hot shapes. Reserved.
- **Probabilistic programming (Stan-class).** A whole framework on top
  of AD + sampling. Reserved as a future module (`crd-stan`?).
- **Symbolic / CAS layer.** Out of `crd-hesap` scope; potential
  separate module `crd-sembol`. Reserved.
- **GPU direct sparse solvers.** Sparse LU/Cholesky on GPU is a
  research frontier; reserved for v17 follow-up.
- **Communication-avoiding linear algebra.** For very large systems
  on bandwidth-bound clusters. Reserved.
- **Mixed-precision iterative refinement** (FP16/FP32 for hot work,
  FP64 for accuracy). Promising for ML-adjacent workloads; reserved.

---

## 9. References (curated)

### 9.1 Books
- Anderson et al. (1999) — *LAPACK Users' Guide* (3rd ed.). SIAM.
- Davis (2006) — *Direct Methods for Sparse Linear Systems*. SIAM.
- Saad (2003) — *Iterative Methods for Sparse Linear Systems* (2nd ed.). SIAM.
- Nocedal & Wright (2006) — *Numerical Optimization* (2nd ed.). Springer.
- Hairer, Nørsett & Wanner (1993) — *Solving ODEs I (non-stiff)*. Springer.
- Hairer & Wanner (1996) — *Solving ODEs II (stiff)*. Springer.
- Trefethen & Bau (1997) — *Numerical Linear Algebra*. SIAM.
- Golub & Van Loan (2013) — *Matrix Computations* (4th ed.). JHU Press.
- Press, Teukolsky, Vetterling & Flannery (2007) — *Numerical Recipes* (3rd ed.). Cambridge. (Caveat: take with reservation; pedagogical reference.)
- Oppenheim & Schafer (2010) — *Discrete-Time Signal Processing* (3rd ed.). Pearson.
- Boyd & Vandenberghe (2004) — *Convex Optimization*. Cambridge.

### 9.2 Papers
- Goto & van de Geijn (2008) — *Anatomy of high-performance matrix multiplication*. ACM TOMS.
- van Zee & van de Geijn (2015) — *BLIS: A framework for rapidly instantiating BLAS functionality*. ACM TOMS.
- Amestoy, Davis & Duff (1996) — *An Approximate Minimum Degree ordering algorithm*. SIAM J. Matrix Anal.
- Karypis & Kumar (1998) — *METIS: A software package for partitioning unstructured graphs*. (Reordering reference.)
- Hindmarsh et al. (2005) — *SUNDIALS: Suite of Nonlinear and Differential/Algebraic Equation Solvers*. ACM TOMS.
- Wächter & Biegler (2006) — *On the implementation of an interior-point filter line-search algorithm for large-scale nonlinear programming*. Math. Prog.
- Stellato et al. (2020) — *OSQP: an operator splitting solver for quadratic programs*. Math. Prog. Comp.
- Frigo & Johnson (2005) — *The design and implementation of FFTW3*. Proc. IEEE.
- O'Neill (2014) — *PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation*.
- Blackman & Vigna (2018) — *Scrambled linear pseudorandom number generators*.
- Carpenter et al. (2017) — *Stan: A Probabilistic Programming Language*. JSS.
- Bradbury et al. (2018) — *JAX: composable transformations of Python+NumPy programs*.
- Lubin et al. (2023) — *JuMP 1.0*. Math. Prog. Comp.
- Rackauckas & Nie (2017) — *DifferentialEquations.jl*. JORS.
- Pantelides (1988) — *The consistent initialisation of differential-algebraic systems*. SIAM J. Sci. Stat. Comput.
- Sonneveld & van Gijzen (2008) — *IDR(s): a family of simple and fast algorithms for solving large nonsymmetric systems of linear equations*. SIAM J. Sci. Comput.
- Moses & Churavy (2020) — *Enzyme: high-performance automatic differentiation of LLVM*. NeurIPS.

### 9.3 Software (algorithm reference, not source)
- LAPACK / BLAS — Netlib. Reference dense LA.
- SuiteSparse (CHOLMOD, UMFPACK, KLU, SPQR, AMD) — Tim Davis.
- PETSc / Trilinos — DOE national labs. Scientific computing toolkits.
- SUNDIALS (CVODE, IDA, ARKODE) — LLNL. ODE/DAE.
- IPOPT — open NLP solver.
- OSQP — open QP solver.
- HiGHS — open LP/MIP.
- Clarabel — modern Rust conic IPM.
- FFTW — Frigo-Johnson FFT.
- Eigen — header-only C++ template LA.
- ARPACK / SLEPc — sparse eigenvalue.
- Hypre — AMG reference.
- MATLAB / Octave — user-ergonomics reference.
- NumPy / SciPy / Matplotlib / IPython — Python reference.
- Julia LinearAlgebra / SparseArrays / DifferentialEquations.jl /
  Optim.jl / JuMP.jl — modern reference.
- Stan-math — reverse-mode AD reference.
- JAX — modern functional AD reference.
- PyTorch ATen — tensor + autograd reference.
- CasADi — AD + optimisation reference.
- Boost.Math / Boost.Random — C++ statistics reference.
- JUCE — DSP API ergonomics reference.

### 9.4 Cerid-internal cross-references
- ADR-0034 — C++ hot-reload DLL scripting (C ABI pattern reused).
- ADR-0061 — Async GPU upload contract (`UploadHandle` + `Fence`
  reused for `crd-hesap-gpu`).
- ADR-0062 — Eylem physics architecture (sibling substrate; same
  posture).
- ADR-0063 — Eylem determinism contract (inherited wholesale).
- ADR-0064 — `crd-sdf` substrate (sibling substrate; same posture).
- ADR-0065 — `crd-hesap` numerical computing substrate (locks
  decisions).
- `docs/research/sparsematrices.md` — pre-existing Cerid sparse-matrix
  background research.
