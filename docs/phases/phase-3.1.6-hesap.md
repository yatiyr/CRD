# Phase 3.1.6 — `crd-hesap` numerical computing substrate

**Status:** 🔄 IN PROGRESS (ships BEFORE eylem v1c resumes — re-amended 2026-05-19, see Phase posture). v0 (dense BLAS+solvers) ✅, v1 (sparse) ✅, v2 (reorderings) ✅, v3a (symmetric/Hermitian eig: QL/QR + D&C + MRRR) ✅, **v3b (SVD) ✅ CLOSED** — v3b-1 (bidiag + dbdsqr + blocked dgebrd/dorgbr) + **v3b-2 (Gu-Eisenstat D&C — beats Eigen BDCSVD + LAPACK dgesdd at all N)** + v3b-3 (randomized rsvd/rsyev) + v3b-1c (complex). **v3c (least-squares) + v3d (non-sym eig) + v3e (close) ✅. v4 (iterative: Krylov + block-Krylov + preconditioners + AMG) ✅ CLOSED.** **v5a (sparse-direct: supernodal Cholesky, CHOLMOD-class): FACTOR + multi-RHS SOLVE beat CHOLMOD on hood/ldoor — v5a-4 ✅ (the gap was the SERIAL SYMBOLIC, not per-thread BLAS-3).** **NEXT = v5a-5 (finish the solve: single-RHS x1 + bmwcra x16 scaling) → v5b (LU) → v5c (QR) → v5d (LDLᵀ) → v5e (HSS+BLR) → v5f (mixed-prec IR) → v5z; then eylem v1c.**
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

**RE-AMENDED 2026-05-19 per user direction "make hesap truly elite
like we did geometry"** (ROADMAP.md § Strategic Execution Plan
Revision 2026-05-19). The 2026-05-15 amendment "ship v0 narrow, defer
the rest to after eylem" is **reversed**: per the elite-completeness
mandate (`feedback_elite_only_no_shortcuts`,
`feedback_reference_implementations_are_the_floor`), `crd-hesap`
ships **full v0-v17 elite scope** before eylem v1c resumes. Same
precedent that ADR-0076 §27 set for `crd-geometry`'s full 49-slice
ship before consumer pull.

### Elite-tier amendments (locked 2026-05-19, see ADR-0065 §13)

1. **Matrix-type catalog: ~30 types from v0** (not 7). Dense
   (Matrix / View / Diagonal / Identity / Permutation / Triangular /
   Symmetric / Hermitian / Banded / BlockDiagonal / BlockTridiagonal
   / Toeplitz / Hankel / Circulant / Vandermonde) + sparse (CSR /
   CSC / BSR / COO / ELL / HYB / DIA / CSR5 / Merge-CSR / Sliced
   ELL / JDS / SkyLine) + hierarchical (HSS / H-matrix / BLR).

2. **Complex-number support from v0.** `crd::hesap::Complex<T>`
   value type; every BLAS / LAPACK op has 4 type instantiations
   (f32 / f64 / Complex32 / Complex64). `dotu` + `dotc`; Hermitian
   Cholesky; complex eigenvalue. Original "real-valued first" pin
   **reversed**.

3. **`LinearOp<T>` abstraction from v0.** Matrix-free / function-
   based / closure-based linear operators consumable by every
   Krylov solver. PETSc `Mat` / Trilinos `Tpetra::Operator` pattern.
   Foundational for FEM, PDE, Hessian-vector for opt.

4. **Task-DAG scheduling via `crd-hesap-sched` (NEW sub-module)**
   over `crd::jobs`. Tile-based GEMM dispatch (PLASMA / PaRSEC
   pattern), not fork-join BLAS. Composes with the rest of the
   engine without oversubscription.

5. **Mixed-precision iterative refinement** (HPL-AI pattern) in
   v0e dense direct solvers. Factorize in f32, refine in f64.
   3-4× perf at full f64 accuracy.

6. **Modern hardware support.** AVX2 (existing) + AVX-512 + NEON +
   SVE/SVE2 + scalar microkernels. Apple AMX + Intel AMX reserved
   for stable ABI landing.

7. **Modern preconditioners.** Original Jacobi / IC(0) / ILU(0)
   stay. Add: SPAI (Grote-Huckle 1997), ILUPACK multilevel ILU
   (Bollhöfer-Saad 2006), SA-AMG (Vaněk 1996), AGMG (Notay 2010),
   bootstrap AMG (Brannick 2010), polynomial preconditioners
   (Chebyshev). Original AMG was Ruge-Stüben only; refined plan
   ships SA-AMG + AGMG as default for vector-unknown systems.

8. **Krylov subspace recycling** (GCRO-DR / M-CG; Parks et al.
   2006). For sequences of related linear systems — eylem
   time-stepping + opt inner solves. 2-5× speedup measured.

9. **Modern eigenvalue solvers.** Add LOBPCG, FEAST, Jacobi-Davidson,
   IRLBA to the original MRRR + QR-double-shift.

10. **Randomized linear algebra** (Halko-Martinsson-Tropp 2011 +
    Tropp 2019). Randomized SVD / QR / range-finder. 10-100× for
    rank-k approximations.

11. **Operator-level autodiff** (JAX pattern; Bradbury 2018). Custom
    VJP / JVP rules per op (`solve(A,b)`'s VJP is `solve(A^T,dy)` —
    never AD through LU). Augments the original tape-based
    reverse-mode (Stan pattern).

12. **Modern ODE methods.** Add SDIRK, Verner, Tsitouras 2011,
    symplectic integrators (Verlet / Yoshida 4/6/8), IMEX,
    sensitivity analysis (forward + adjoint, CVODES / IDAS pattern).

13. **NUFFT + sparse FFT.** Add NUFFT (Greengard-Lee 2004) for
    irregular grids and sparse FFT (Hassanieh 2012) for k-sparse
    signals.

14. **Modern optimization.** Add SCS (O'Donoghue 2016), trust-region
    Steihaug, Adam / AdamW / Lion (essential for ML / agent training
    workflows). Augments L-BFGS + OSQP + IPOPT.

15. **`crd-hesap-bench` sub-module (NEW).** Benchmark + reference-
    fixture replay substrate. Per-slice CI gate against MKL /
    OpenBLAS / LAPACK / SuiteSparse / FFTW.

16. **CLI protocol plumbing from v0** (per ADR-0081 Proposed): every
    entry point registers a typed `CommandSchema` via static-init
    hook + structured output is the C++ return shape from day 1 +
    MCP tool descriptors auto-generated. **No actual CLI parser /
    REPL / RPC server in hesap** — those ship with `crd-cli`
    substrate in Phase 4.0. Protocol-first, parser-later.

17. **C++ hot-reload as the ONLY scripting language** (per ADR-0081
    + ADR-0034 subsumed). The original `crd-hesap-repl` "MATLAB-class
    facade with `randn`/`\` syntax" plan in v18 is **replaced** by a
    C++-cell notebook on top of `crd-cli` (Phase 4.0+2). MATLAB
    syntax → C++ source transformer reserved as a stretch goal,
    not Phase 3.1.6 scope.

### Calendar revised 2026-05-19

- **v0 scope:** ~5 weeks (up from 1.5wk) — full v0a-f sub-slice
  cluster (substrate + BLAS L1+L2+L3 + dense direct + bench
  substrate). See §"v0 sub-slice plan" below.
- **Full Phase 3.1.6:** ~10-12 months elite-tier (up from 6-8 mo)
  for ~52 KLOC engine + ~600 tests across 18 slices. Schedule is
  comparable to Phase 3.1.7 geometry (22 KLOC over 6 mo elite).

### Sequencing (locked 2026-05-19, supersedes 2026-05-15)

1. Phase 3.1.7 geometry CLOSE (v11 in flight 2026-05-19).
2. **Phase 3.1.6 v0-v17 hesap elite-and-big** — full substrate
   ships in one phase (~10-12 months). CLI protocol plumbing per
   slice; parser substrate deferred.
3. **Phase 3.1 eylem v1c-v9 resume** — consumes geometry from
   day 1; consumes hesap-dense from v1f-articulation onward.
   Eylem v7 FEM no longer ships narrow internal PCG (the original
   2026-05-10 plan); ships hesap-consuming from day 1.
4. **Phase 4.0 `crd-cli` + `crd-rpc` + `crd-script`** — the
   formalized agent-native substrate (~12 weeks). Subsumes the
   original Phase 4.0 = C++ DLL hot-reload.
5. Per-module CLI back-fill across the engine (cross-cutting).
6. Notebook + Claude Code agent reference integration; engine-wide
   MCP surface.
7. Phase 3.1.5 sdf (interleaved per original plan).
8. Phase 3.1.8+ domain substrates.

### Original phase posture (2026-05-10) — historical

Original posture was "ship after eylem closes; eylem v7 FEM uses its
own narrow internal PCG"; 2026-05-15 amendment was "v0 ships before
eylem v1c-resume"; 2026-05-19 amendment is "**full v0-v17 elite-and-
big ships before eylem v1c-resume**" — the engineering-platform pivot
in its full form.

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

## Slice structure (revised 2026-05-19 — elite-and-big)

Per ADR-0065 §13 (2026-05-19), every slice ships: matrix-type
catalog coverage; complex variants (f32 / f64 / Complex32 /
Complex64); CLI protocol plumbing (typed `CommandSchema`
registration); test scope 100-200 cases (was ~30); reference-fixture
baselines vs MKL / OpenBLAS / LAPACK / SuiteSparse / FFTW;
determinism + replay tests.

| Slice | Topic (refined elite scope) | LOC | Tests | Duration |
| :---: | --- | :---: | :---: | :---: |
| **v0a** ✅ 2026-05-19 | Substrate: module skeleton + `Complex<T>` + matrix-type catalog headers + `LinearOp<T>` + `MatrixId`/`VectorId` handles + `'HDV0'` CRDR format pin + CLI protocol scaffolding (`CommandRegistry` + `CommandSchema` + structured-output types + minimal JSON writer + MCP-tool-descriptor emit). **Sparse catalog deferred to v1.** 30+8 cases / 121+30 assertions PASS. Session: `docs/sessions/2026-05-19-hesap-v0a-substrate.md`. | ~700 → ~1 700 actual | ~25 → 38 actual | ~3 d → 1 d |
| **v0b** ✅ 2026-05-19 + reference-class SIMD shipped 2026-05-20 | BLAS L1 × 4 type variants (axpy / dot / dotu / dotc / nrm2 / scal / copy / swap / asum / iamax) + Kahan-Babuška-Neumaier pairwise reduction + 28 working CLI commands + ArgValue typed-param substrate. **`v0b-simd-followon` shipped 2026-05-20**: `dot_simd.hpp` with 8-accumulator SIMD `simd_dot_f32/f64` + `simd_sumsq_f32/f64` using `fma()`; dot / nrm2 went from 1.4 GFLOPS scalar-KBN to 20-60 GFLOPS (20-30× speedup). **Reference-class shootout** vs Eigen 3.4 + OpenBLAS 0.3.27 shipped 2026-05-20: axpy 4/5 WINS over Eigen, dot 0.71-1.07×, nrm2 0.64-1.48× (WINS at L1-resident sizes). WINS over OpenBLAS everywhere (1.6-14.7×). 39+60+42 cases / 151+318+154 assertions PASS. Sessions: `2026-05-19-hesap-v0b-blas1.md`, `2026-05-19-hesap-vs-reference-shootout.md`. | ~700 → ~2 600 actual | ~80 → 60 actual | ~3 d → 1 d initial + 1 slice SIMD followon |
| **v0c** ✅ 2026-05-19 + reference-class SIMD shipped 2026-05-20 | BLAS L2: all 17 ops × 4 type variants where applicable. **L2 SIMD attack shipped 2026-05-20**: classic single-pass BLAS symv (each lower-half element touched once, updates both y[i] and y[k]; halves bandwidth) + 8-row tiled gemv + 16-wide-unroll symv with 4 dot accumulators + gated `_mm_prefetch(_T1)` on next-row-of-A (n > 512). **Reference-class shootout**: gemv 3/7 WINS (N=1024-4096, 1.02-1.25×); trsv 1/7 WIN (N=128 1.04×); symv 0/7 WINS but all 0.72-0.99× of Eigen (N=4096 virtually tied at 0.99×). WINS over OpenBLAS-MSVC everywhere (1.4-5.4×). symv N=4096 journey: **0.02× → 0.99× (50× improvement); symv N=2048: 0.01× → 0.85× (85× improvement); gemv N=4096: 0.85× → 1.25× WIN**. Remaining 5-25% gaps filed as `vs-ref-blas2-followups` (requires asm or AVX-512). 94+39+23 cases / 521+151+124 assertions PASS. Sessions: `2026-05-19-hesap-v0c-blas2.md`, `2026-05-19-hesap-vs-reference-shootout.md`. | ~1000 → ~5 200 actual | ~80 → 60 actual | ~5 d → 1 d initial + 1 slice SIMD followon |
| **v0d** ✅ 2026-05-19 + reference-class GEMM **10/10 WINS over Eigen-MT** shipped 2026-05-20 | **BLAS L3 via task-DAG** — Goto/BLIS 5-loop layered gemm + scalar/AVX2 microkernel hot-swap (ADR-0082 intrinsics; ASM deferred behind `CRD_HESAP_MICROKERNEL_BACKEND` switch + revisit gate) + Vec4d substrate + f64 AVX2 microkernel + `gemm_parallel<T, L>` BLIS-style outer-loop parallelism + 7 BLAS L3 ops + `gemm_parallel_auto` heuristic dispatch + `gemm_mixed<f32, f64>` + `crd-hesap-sched::DependencyGraph` (per-tile RAW/WAW/WAR; level-by-level executor) + 14 CLI commands (was 4) + small_gemm_parallel direct unpacked path. **Reference-class GEMM shootout (multi-threaded, 16 P-threads, best-of-3): 10/10 WINS over Eigen-MT.** f32: N=256 368×, N=512 1.21×, N=1024 1.69×, N=2048 1.70×, N=4096 1.30×. f64: N=256 1.17×, N=512 1.19×, N=1024 1.39×, N=2048 1.12×, N=4096 1.01×. Single-core: f32 ≥70% peak / f64 ≥70% peak via `simd::fma`. **Allocator propagation** through gemm/gemm_parallel/gemm_parallel_auto/small_gemm_parallel/symv — no more `default_allocator()` in library code. Sessions: `2026-05-19-hesap-v0d-blas3.md`, `2026-05-19-hesap-v0d-perf-chunk1.md`, `2026-05-19-hesap-v0d-perf-f64-avx2.md`, `2026-05-19-hesap-v0d-parallelism.md`, `2026-05-19-hesap-v0d-parallelism.md`, `2026-05-19-hesap-vs-reference-shootout.md`. Filed (hardware-gated): `v0d-microkernel-avx512`, `-neon`, `-sve2`, `-blocks-empirical-sweep`. Filed (consumer-gated): `v0d-asm-microkernel` (ADR-0082 three-condition revisit gate not triggered). | ~1200 → ~7000 actual to date | ~100 → 40 actual | ~7 d → 1 d initial + 4 perf/reference slices |
| **v0e** | Dense direct solvers cluster — 8 sub-slices ↓. Reference-class shootout vs Eigen LU/QR/LLT + LAPACK at v0e-g per continuous-benchmarking policy. | ~1200 | ~120 | ~7 d |
| **v0e-a** ✅ 2026-05-20 | **LU with partial pivoting.** Right-looking blocked LU (LAPACK xGETRF, bs=64): panel factor + inner trsm + `gemm_parallel` trailing update — **first real consumer of v0d's GEMM**. f32 + f64 RowMajor. Bit-deterministic across worker counts. `Permutation` body populated (was a v0a shell). Pre-existing tidy debt in `blas2.cpp` matrix-notation locals (`A`/`L`/`U`/`A_row`) cleaned via narrow NOLINTBEGIN scope. 10 cases / 37,156 assertions PASS on win-debug + win-asan + win-tidy. Filed: `v0e-a2` (complex variants), `v0e-a-perf` (consumer-gated). Session: `docs/sessions/2026-05-20-hesap-v0e-a-lu.md`. | ~600 actual | 10 cases / 37,156 assertions | 1 d |
| **v0e-b** ✅ 2026-05-20 | **Cholesky (SPD real).** Right-looking blocked, bs=64: unblocked panel diag-factor + inner trsm + `gemm_parallel` trailing update (filed: `v0e-b-syrk-optim` to halve trailing-update FLOPs via a true syrk). f32 + f64 RowMajor. Non-PD detection via sqrt-of-negative → `info != 0`. solve = forward-sub L + back-sub Lᵀ. 10 cases / 18,725 assertions PASS on win-debug + win-asan + win-tidy. Determinism: lower-triangle bit-identical across runs (upper-triangle is GEMM-trailing-update garbage by design). HPD complex variant filed as `v0e-b-hpd`. Session log: `docs/sessions/2026-05-20-hesap-v0e-b-cholesky.md`. | ~400 actual | 10 cases / 18,725 assertions | 1 d |
| **v0e-c** ✅ 2026-05-20 | **LDLT (Bunch-Kaufman indefinite).** A = P·L·D·Lᵀ·Pᵀ where D mixes 1×1 + 2×2 blocks. Bunch-Kaufman partial pivoting (ALPHA = (1+√17)/8). UPLO=Lower. Unblocked trailing update for MVP — `v0e-c-blocked` filed to route through `gemm_parallel` once 2×2-pivot bookkeeping is stable. f32 + f64 RowMajor. Solve handles 1×1 + 2×2 D blocks separately + skips L[k+1, k] for 2×2 blocks (that slot stores D[k+1,k], not L). 7 cases / 132 assertions PASS on win-debug + win-asan + win-tidy. Tested: 2×2 indefinite textbook, 4×4 indefinite, N=16/64 random indefinite, f32 N=32, singular detection, SPD matrix correctness (1×1-only pivots). | ~450 actual | 7 cases / 132 assertions | 1 d |
| **v0e-d** ✅ 2026-05-20 | **QR Householder.** A = Q·R via unblocked Householder reflectors. Q stored implicitly: strict-lower-triangle of `packed()` carries v subvecs + separate `taus` array. R is the upper triangle of `packed()`. `apply_q`/`apply_q_transpose` for solver pipelines + `solve_qr` for square + least-squares (m ≥ n). f32 + f64 RowMajor. 7 cases / 164 assertions PASS on win-debug + win-asan + win-tidy. Tested: 2×2 textbook, square solve N=4, Q·R reconstruction N=8 to 1e-10, orthogonality QᵀQ == I to 1e-12, LS over-determined 6×3 with Vandermonde structure, f32 N=16, apply_q∘apply_q_transpose == identity. **Filed**: `v0e-d-blocked` (WY blocked variant w/ gemm_parallel trailing update), `v0e-d-colpiv` (rank-revealing column pivoting). | ~350 actual | 7 cases / 164 assertions | 1 d |
| **v0e-e** ✅ 2026-05-20 | **`LinearOp<T>` wrappers + Hager 1-norm condition estimator.** Concrete `MatrixLinearOp<T, L>` (gemv-backed) + `SymmetricLinearOp<T>` (symv-backed) implementing the `crd::hesap::LinearOp<T>` interface from v0a. Exact `compute_1norm(Matrix/Symmetric)`. `hager_1norm_estimate` power-iteration template taking solve closures (LAPACK xLACON pattern). `condition_estimate_1norm_symmetric(Symmetric, Cholesky)` convenience: κ₁(A) for SPD via existing factor — symmetric → solve == solve_transpose simplifies the closures. 8 cases / 26 assertions PASS on win-debug + win-asan + win-tidy. Verified: identity → κ = 1.0; scaled identity → κ = 1.0; tridiagonal SPD → finite well-conditioned estimate. **Filed**: `v0e-e2` (LU/LDLT/QR estimators need solve_transpose paths). | ~280 actual | 8 cases / 26 assertions | 0.5 d |
| **v0e-f** ✅ 2026-05-20 | **Iterative refinement (same-precision MVP).** Wilkinson 1948: r_k = b - A·x_k, dx = solve(r_k), x_{k+1} = x_k + dx, until ||r||₂ / ||b||₂ < tol or max_iter. `refine_lu` + `refine_cholesky` variants — consume the existing v0e-a / v0e-b factors via `LinearOp` (v0e-e) for the residual product. Returns `RefinementResult<T>` with initial+final residuals + iteration count + converged flag. 4 cases / 35 assertions PASS on win-debug + win-asan + win-tidy. **Filed**: `v0e-f2` mixed-precision HPL-AI (factor in f32, refine in f64) — needs cross-precision solve wrapper. | ~280 actual | 4 cases / 35 assertions | 0.5 d |
| **v0e-g** ✅ 2026-05-20 | **CLI + reference-class shootout vs Eigen.** 8 single-shot factor+solve commands registered: `hesap.dense.solver.{lu,cholesky,ldlt,qr}.{f32,f64}` (CLI factor-only/solve-only deferred — Permutation + block_kinds + taus not directly serializable). `bench_hesap_solvers_vs_reference.cpp` shootout vs Eigen PartialPivLU / LLT / LDLT / HouseholderQR. **Honest results on i9-14900K AVX2** (factor+solve combined; P-core affinity; best-of-3): **LU.f64 WINS N≥128** (N=128 1.37×, N=256 16.84×, N=512 15.36×, N=1024 8.43×; Eigen-default unparallelized at large N). **Cholesky.f64**: 0.37–1.02× (only WINS at N=1024). **QR.f64**: 0.04–0.84× — unblocked Householder vs Eigen's blocked WY ⇒ `v0e-d-blocked` is BLOCKING-priority. **LDLT.f64**: 0.08–1.09× — same story ⇒ `v0e-c-blocked`. Solver CLI tests: 6 cases / 32 assertions PASS. Filed: `v0e-g-eigen-mt-config` (investigate Eigen `setNbThreads()` config to fair-compare against Cerid's gemm_parallel), `v0e-c-blocked` (LDLT blocked), `v0e-d-blocked` (QR blocked WY). | ~700 actual | 6 cases / 32 assertions + bench harness | 0.5 d |
| **v0e-close** ✅ 2026-05-20 | **5-config DoD PASS** (`per-slice-check.ps1 -IncludeRelease -Parallel`: debug+asan+shipping+release+tidy — win-release needed a stale pre-v0d smoke/bench obj cleanup, a known build-dir artifact). 172 cases / 65,098 assertions; guards (no-non-ascii / no-std-math / simd-emission) green. `docs/systems/hesap-dense.md` updated with solver scorecard. v0e decisions D1–D8 queued in ADR-0065 (§14 lock deferred to v0-close). **ADR-0083 Accepted** (row-major storage + per-factor escape hatch). Rollup: `docs/sessions/2026-05-20-hesap-v0e-close.md`. | ~50 + docs | — | 0.5 d |
| **v0f** ✅ 2026-05-20 (SLIMMED) | **Property-based test framework + bench-harness dedup.** Shipped: (1) `tests/hesap-dense/random_matrix.hpp` — seeded `RandomMatrix` factory (`random_general` / `random_diag_dominant` / `random_spd` / `random_symmetric_indefinite` / `random_spd_ill_conditioned`) consolidating the `build_spd` / `build_symmetric_indefinite` / `fill_matrix` generators previously duplicated across test_cholesky / test_ldlt / test_blas3_parallel (value formulas preserved → tolerances unchanged). (2) `test_random_property.cpp` — 4 property tests (Cholesky reconstruct, LU/LDLT/QR solve-recovers-x) across seeds × sizes. (3) `crd_add_hesap_vs_ref_bench()` CMake helper collapsing ~120 lines of duplicated vs-reference bench boilerplate into one function. 176 cases / 66,703 assertions PASS. **De-scoped** (recorded, not built): `crd-hesap-bench` sub-module + LAPACK/SuiteSparse/FFTW committed reference binaries → **deferred to the FFT/sparse slices that actually need them** (no binaries committed today; nothing to drop). `bench_common.hpp` C++ harness dedup **deferred to the third vs-reference bench** (premature with 2 jobs-linked + 2 not; the `time_loop`↔`jobs::frame_reset` coupling wants a real third data point — sparse/FFT will provide it). Session: `docs/sessions/2026-05-20-hesap-v0f.md`. | ~350 actual | 176 cases / 66,703 assertions | 0.5 d |
| **v0-close** ✅ 2026-05-20 (CI-confirmed sweep) | **ADR-0065 §14 decision lock ✅ Accepted** (v0a–f: L50–L55 + v0e-D1…D8 + ADR-0083 + deviation table). **18-config full sweep caught 6 latent v0d cross-config bugs** (gcc `-mfma`; `Vec4f::fma`; clang unused capture/fn; scalar/SSE2 prefetch unused; `complex.hpp std::atan2`→deterministic + new `crd-hesap→crd-math` edge) — all fixed. Validated locally: clang-cl + scalar + sse2 build clean, gcc compiles clean, no-std-math guard PASS, **win-release 2821 tests 100%** + linux-gcc-release. Authoritative 18-config sweep delegated to CI. Session: `docs/sessions/2026-05-20-hesap-v0-close.md`. | ~100 + docs | — | ~0.5 d |
| | **v0 TOTAL** (~5 weeks, was 1.5wk in 2026-05-15 plan) | **~5400** | **~445** | **~5 wk** |
| **v1** (detailed plan below ↓) | Sparse storage **core tier** + spmv + spmv-T + spmm + spgemm + element-wise + format-conversion graph + sparse `LinearOp` + Matrix-Market I/O + complex + CLI. **Goal = beat Eigen `SparseCore`** (single-threaded, scalar spmv, SPA/`AmbiVector` spgemm) via SELL-C-σ + AVX (`Vec8f`/`Vec4d`) + `crd-jobs` row-balanced parallel + parallel-hash spgemm; benched head-to-head vs **Eigen 3.4** on the **SuiteSparse `.mtx` corpus**, gated `CRD_BUILD_HESAP_VS_REFERENCE`. CSR/CSC are row/col-oriented by construction → no dense row-major handicap (ADR-0083 does not transfer). **Tier-3 (CSR5/Merge-CSR → v17 GPU; JDS/SkyLine → follow-on) dropped per 2026-05-20 scope review.** **7 subslices ↓.** | ~3000 | ~140 | ~3 wk |
| **v1a** ✅ 2026-05-20 | **Sparse substrate + COO→CSR/CSC + determinism spec.** Pattern/values/analysis trinity + `SparseMatrix<T,Format>` (Format NTTP per D21 + runtime tag) + `TripletBuilder`→`compress()`/`compress_csc()` + uncompressed insert mode + structural queries + `'HSPM'` pin + 19 CLI commands + assembly bench (beats Eigen at scale). Raw `T`. **Storage only — no kernels (fence held).** **29 cases / 211 assertions; 4-config DoD PASS across all 3 sub-subslices.** | ~1850 actual | 29 / 211 | 1 d |
| ↳ **v1a-1** ✅ 2026-05-20 | Module `crd-hesap-sparse` skeleton + trinity types (`SparsePattern`/`SparseValues`/`AnalysisHandle` + `is_valid_for`) + `SparseMatrix<T,Format>` owning bundle (move-only) + `SparseId` + `SparseFormat` + **`topology_hash`** (FNV-1a-64 over `rows,cols,format,block_size` + length-prefixed used bytes of `outer_ptr,inner_idx`; LE explicit-byte feed; **D1**) + `'HSPM'` CRDR pin + **determinism spec pinned** in `docs/systems/hesap-sparse.md` (D1 + disjoint-row spmv / canonical column-sorted spgemm / no order-free cross-thread float reductions). Advisor caught + fixed: hash must include `format`+`block_size` (CSR/CSC collision → regression test); CLI anchor + uncompressed mode deferred to v1a-2 (no dead stubs). Golden hash `0xF76305A0C07C9641` (canonical 3×3 CSR identity); CI linux-gcc proves cross-platform. **9 cases / 34 assertions PASS**; 4-config DoD (debug+asan+shipping+tidy) PASS. | ~430 actual | 9 / 34 | <1 d |
| ↳ **v1a-2** ✅ 2026-05-20 | COO `TripletBuilder<T>` → CSR `compress()`: counting-sort row bucket (stable) + per-row `crd::containers::stable_sort` by column + dedup-merge (duplicates summed left-to-right in insertion order = deterministic, verified bit-reproducible). **Uncompressed (Eigen 4-array) mode shipped** (user call): `make_uncompressed` + `coeff_ref` find-or-insert (sorted columns) + storage grow + `make_compressed`. **`topology_hash` refined to canonical slack-invariant** (per-row count + sorted columns) so compressed and uncompressed of the same matrix hash identically (verified); v1a-1 golden re-baked `0x9978E97C37B7D174`. **12 CLI** (`from_triplets`/`to_csr`/`build` × 4; `build --uncompressed` exercises the insert path = real consumer, complex too). **21 cases / 137 assertions PASS**; 4-config DoD PASS. | ~700 actual | 21 / 137 | <1 d |
| ↳ **v1a-3** ✅ 2026-05-20 | CSC `compress_csc()` (column-major from triplets) + structural queries (`structural_stats` / `inner_indices`; `nnz`/`density`/`structural_query`) + `smoke_hesap_sparse` + **assembly bench vs `Eigen::setFromTriplets`** (gated; win-release: **WIN at every size — 1.03× / 1.44× / 1.76× at N=50k/200k/1M**). Optimized `assemble` (direct scatter, in-place sort + dedup-compact, new `Array::resize_uninitialized`) beat Eigen across the board (was 0.74× small-N). CLI: `to_csc` ×4 + `nnz`/`density`/`structural_query` (type-agnostic, 1 each) = 7. CSC cross-platform golden test. **Total v1a sparse suite: 29 cases / 212 assertions PASS; 4-config DoD PASS.** | ~560 actual | 8 / 75 | <1 d |
| **v1b** ✅ 2026-05-20 | **spmv — SELL-C-σ primary (ST + MT) + CSR baseline + `SparseLinearOp` + transpose.** v1b-2 absorbed v1b-3 (parallel) into one slice (the MT deferral was rejected). **Gate (revised, user call): SELL ≥ Eigen in the DRAM-bound regime (ST 1.21–1.27× at N=2M) + parity-or-better vs Eigen-MT** — the stronger statement replacing the original ≥60% STREAM-triad proxy. Honest regime-dependent result: dominates large/DRAM-bound matrices, competitive at the cache + bandwidth walls (see `docs/systems/hesap-sparse.md`). | ~1600 | ~30 | 1 d |
| ↳ **v1b-1** ✅ 2026-05-20 | **CSR spmv baseline + API foundation.** `spmv(α,A,Trans,x,β,y)` (CSR scalar, deterministic L-to-R two-rounded; `β=0` NaN-safe; None/Transpose/ConjTranspose; compressed-CSR assert) + `SparseLinearOp<T>` (first sparse `LinearOp` consumer; apply/transpose/adjoint **bit-identical to kernel**) + 4 type variants + 8 CLI (`spmv`/`spmv_transpose` × 4) + cross-platform `y` golden. **D(sparse)-3 pinned** (two-rounded `mul_add` ≠ `simd::fma`; pad=0; compressed-only). ST bench vs Eigen = **0.09–0.17× (CSR scalar baseline; SELL primary closes the gate at v1b-2)**. **38 cases / 254 assertions PASS; 4-config DoD PASS.** | ~600 actual | 12 / 254 | <1 d |
| ↳ **v1b-2(+3)** ✅ 2026-05-20 | **SELL-C-σ primary, serial + parallel (v1b-3 merged in).** `Sell` storage (C=8 f32 / 4 f64) + σ row-length sort (global; identity fast-path for uniform/banded) + CSR→SELL convert + SELL spmv: f64 slice-pair AVX2 (`_mm256_i32gather_pd` + prefetch + two-rounded `mul+add`, bit-exact vs CSR), f32 `Vec8f`, complex scalar. **`spmv_sell_parallel`** = nnz-balanced slice ranges over `crd::jobs`, bit-exact vs serial at any worker count. 4 `spmv_sell.{T}` CLI. **Beats Eigen ST 1.21–1.27× (DRAM-bound, N=2M); parity-or-better vs Eigen-MT** (banded 1.13–1.35×, uniform-16 1.05×; cache-resident + MT-bandwidth-wall competitive). σ implemented (not deferred — user directive). **44 cases / 29 780 + 27 260 assertions PASS.** | ~1000 actual | 18 | 1 d |
| **v1c** ✅ 2026-05-20 | **Element-wise + structural + format-conversion graph.** Conversions (to_csc/from_csc/to_coo/transpose) + element-wise (add/sub/scale/hadamard, fast-path + symbolic-union) + structural (diag/scale_rows/triu/tril/submatrix). Conversion hub=CSR (COO↔CSR↔CSC↔SELL now; BSR/ELL/DIA + runtime `convert(from,to)` dispatch ship at v1f). All `[[nodiscard]]` factories, 4 type variants, deterministic + bit-exact. **36 CLI commands added** (transpose/to_coo/scale/add/sub/hadamard/diag/triu/tril). **61 cases / 99 258 assertions; 4-config DoD PASS across all 3 sub-subslices.** | ~990 actual | 61 / 99 258 | 1 d |
| ↳ **v1c-1** ✅ 2026-05-20 | **Conversion graph + transpose.** Direct factory fns (`[[nodiscard]]`): `to_csc`(CSR→CSC) + `from_csc`(CSC→CSR) + `to_coo` + `transpose A→Aᵀ`, all sharing one `organize_by_inner` kernel (count + prefix + ordered scatter = real O(nnz) build; canonical sorted; **`transpose∘transpose == A` byte-exact**). CSR↔SELL already (v1b). Runtime `convert(from,to)` dispatch hub deferred to v1f (8+ edges; all conversions delivered now — elite call). 4 type variants + 8 CLI (`transpose`/`to_coo` × 4; `to_csc` reused from v1a-3). **5 cases / 18 968 assertions PASS.** | ~280 actual | 5 / 18 968 | <1 d |
| ↳ **v1c-2** ✅ 2026-05-20 | **Element-wise — the algorithmic core.** `add`/`subtract` (topology_hash fast path + symbolic-union sorted row-merge; **D(sparse)-5** = `a OP b` left-first single-rounded) / `scale(αA)` / `hadamard(A.*B)` (intersection). `[[nodiscard]]` factories, 4 type variants. **16 CLI** (`scale`/`add`/`sub`/`hadamard` × 4; binary ops take a 2nd b-triplet set). Fast-path == merge-path verified bit-exact (advisor discriminator). **5 cases / 44 627 assertions PASS** (full suite 56 / 93 629). | ~420 actual | 5 / 44 627 | <1 d |
| ↳ **v1c-3** ✅ 2026-05-20 | **Structural extract + slices.** `extract_diagonal` (dense) + `scale_rows` (diagonal left-scale) + `triu`/`tril` (k-diagonal offset) + `submatrix` (reindexed block; row/col slice = special cases). `[[nodiscard]]` factories, 4 type variants. 12 CLI (`diag`/`triu`/`tril` × 4). triu/tril partition verified (triu(0)+tril(-1)==A). **5 cases / 5 629 assertions PASS** (full v1c suite: 61 cases / 99 258 assertions). | ~290 actual | 5 / 5 629 | <1 d |
| **v1d** ✅ 2026-05-20 | **spgemm — parallel Gustavson (dense-SPA), C=A·B + A·Aᵀ, f32/f64/c32/c64.** Serial fused single-pass (O(flops)) + two-phase parallel (flop-balanced, per-worker SPA, bit-exact vs serial). Complex + A·Aᵀ folded in (defer nothing). **Gate CRUSHED on REAL SuiteSparse (user: max rigor — pulled MM reader + gated `file(DOWNLOAD)` fetch forward): median par/Eigen-ST = 2.32×, 5/6 WIN** (FEM 2.6–2.9×; tiny west2021 0.92× small-input edge, accepted). 16 CLI commands. v1g keeps only the engine MM I/O+writer+CLI, the SuiteSparse fixture harness, and the optional BRMerge/MAGNUS beat-the-next-tier refinement (not the gate). **70 cases / 146 183 assertions; 4+5-config DoD PASS.** | ~1250 actual | 70 / 146 183 | 1 d |
| ↳ **v1d-1** ✅ 2026-05-20 | **Serial Gustavson — correctness first.** Fused single-pass (advisor pin — not 2× symbolic+numeric): per-row dense SPA, stamp-marker clear via touched-list = **O(flops)**; fixed iteration order (A-row then B-row) + column-sorted gather = deterministic. `kMaxSpaCols` cap. f32/f64. Verified: dense oracle (small) + **`(A·B)x == A·(B·x)` cross-check (4000×3000×3500)** + canonical-sorted + determinism + A·I==A. **4 cases / 34 880 assertions PASS.** | ~290 actual | 4 / 34 880 | <1 d |
| ↳ **v1d-2** ✅ 2026-05-20 | **Parallel — THE gate, on REAL SuiteSparse (user: max rigor).** Two-phase (symbolic count → prefix → numeric disjoint write); flop-balanced A-row partition over `crd::jobs`; per-worker SPA (sized `num_workers`, indexed by `worker_index`, phase-2 stamps offset +m); bit-exact vs serial at any worker count. **Pulled the Matrix-Market reader + gated SuiteSparse `file(DOWNLOAD)` fetch forward** (bench-local reader; engine MM I/O stays v1g). **vs Eigen-ST C=A·A on 6 SuiteSparse matrices: median par/Eigen = 2.32×, 5/6 WIN** (bcsstk13 2.86× / bcsstk24 2.81× / bcsstk25 2.63× / gemat11 2.02× / sherman3 1.19×; tiny west2021 0.92× — small-input regime, user-accepted). `.pruned()` fairness trap caught. 5-config DoD. | ~700 actual | 2 / 31 | ~1 d |
| ↳ **v1d-3** ✅ 2026-05-20 | **Complex + A·Aᵀ + close.** c32/c64 spgemm (the generic kernel instantiates directly — verified vs complex dense oracle); `spgemm_ata`/`spgemm_ata_parallel` = `spgemm(A, transpose(A))` (symmetric for real, verified). 8 CLI (`spgemm`/`spgemm_ata` × 4). **Full v1d suite: 70 cases / 146 183 assertions PASS.** Nothing deferred to v1g (per user). | ~250 actual | 9 | <1 d |
| **v1e** ✅ 2026-05-21 | **spmm (sparse×dense) + SDDMM.** C(dense)=A(sparse)·B(dense), multi-RHS (block-Krylov/batched); SIMD axpy over RHS columns (one A-scan touches all r RHS); row-parallel. SDDMM (masked dense·denseᵀ→sparse, GNN/ML). 4 variants. **spmm: crushes Eigen-ST 5–12× throughout; vs Eigen-MT wins the heavy-RHS regime (r=32 ~1.3×, r=128 1.8–1.9×), ties small-RHS (gemat11 r=4 0.96×) at the irreducible B-gather bandwidth wall — user-accepted "chased to the wall".** SDDMM: explicit-SIMD multi-accumulator dot (two-rounded, cross-width bit-exact); **vs dense-then-mask 26–434× / vs same-flops Eigen dot: compute-bound matrices WIN (1.31–1.49× at r=32), high-nnz bcsstk24 0.62–0.76× = gather wall (parallel doesn't scale, fma no help) — user-accepted at the wall.** **8 CLI commands added** (spmm/sddmm × 4). **82 cases / 283 311 assertions; 5-config DoD PASS.** **2 sub-subslices ↓.** | ~350 actual | 82 / 283 311 | ~2 d |
| ↳ **v1e-1** ✅ 2026-05-21 | **spmm — the gate.** `C=αAB+βC` (B,C dense **row-major**, leading dims `ldb`/`ldc` for block-Krylov slicing); accumulate **directly into C[i,:]** (init once = 0 or β·C[i,:] outside the column loop = NaN-safe; then `C[i,c]+=αa·B[k,c]` — auto-vec axpy). serial + nnz-balanced row-parallel over `crd::jobs` (disjoint C rows = bit-exact vs serial, no per-worker scratch; **per-job binary-search overhead removed** after it regressed small-r). f32/f64. Dense oracle + per-column == spmv cross-check (88 080 assertions bit-exact). **vs Eigen on v1d SuiteSparse, r∈{1,4,32,128}: Eigen-ST 5–12× everywhere; vs Eigen-MT — r=1 bcsstk25 0.93×/gemat11 0.61×, r=4 0.82×/0.96×, r=32 1.32–1.33× WIN, r=128 1.79–1.94× WIN.** Small-r = the spmv B-gather bandwidth wall (user-accepted regime). 5-config DoD PASS. | ~200 actual | 10 | ~1 d |
| ↳ **v1e-2** ✅ 2026-05-21 | **SDDMM + complex + CLI + close.** SDDMM `C_sparse = M ⊙ (X·Yᵀ)` (compute ONLY masked entries = dot of X-row,Y-row; GNN attention; output carries mask pattern; row-parallel disjoint entries = bit-exact). **No Eigen equivalent → correctness vs dense oracle + perf characterisation, NO Eigen gate.** spmm + SDDMM c32/c64 (generic kernels instantiate directly — verified vs complex dense oracle, non-conjugating X·Yᵀ). **SDDMM dot = explicit SIMD (Vec4d×2 f64 / Vec8f f32, two-rounded, store-then-scalar `horizontal_sum` → cross-SIMD-width bit-exact) — got serial 3× faster.** **Eigen bench `bench_hesap_sddmm_vs_reference`: vs dense-then-mask 26–434×; vs same-flops per-entry dot — gemat11 r=32 1.31× / sherman3 1.49× WIN, bcsstk24 0.62–0.76× = random-Y-gather cache wall** (confirmed: parallel scales only 1.4×, single-rounded `fma` moves it 0% so not a compute/determinism tax). User-accepted at the wall. 8 CLI (`spmm`/`sddmm` × {f32,f64,c32,c64}). **Full v1e suite: 82 cases / 283 311 assertions PASS.** | ~150 actual | 12 | ~1 d |
| **v1f** ✅ 2026-05-21 | **Block + structured formats: BSR + ELL + DIA.** BSR (b×b blocks; **dedicated small-block GEMV, NOT the v0d microkernel — D(sparse)-6**) + ELL (regular, interop) + DIA (banded). Each: storage + spmv (serial+parallel, two-rounded, cross-width bit-exact) + CSR↔X convert. 4 types. **36 CLI commands** (to_X/from_X/X_spmv × {bsr,ell,dia} × 4). **Gate CRUSHED on native patterns: BSR 3.4–6.7× Eigen-CSR (3.2–6.5× our CSR); ELL 4.9–5.2×; DIA 4.8–5.9×.** **2 sub-subslices ↓.** | ~400 actual | ~13 / 672 343 (full suite) | ~3 d |
| ↳ **v1f-1** ✅ 2026-05-21 | **BSR — the gate.** `BsrMatrix<T>` (CSR-of-blocks, dense b×b row-major) + small-block GEMV spmv (compile-time b∈{1,2,3,4,6} dispatch + runtime fallback; b independent accumulators = ILP; two-rounded, β=0 NaN-safe; serial + block-row-balanced parallel, bit-exact) + CSR↔BSR (zero-pad partial blocks D(sparse)-7). f32/f64/c32/c64. 12 CLI. **Gate (no Eigen BSR → vs Eigen scalar-CSR on same block matrix): par 3.45–6.72× Eigen, 3.20–6.46× our scalar-CSR; even serial beats Eigen 1.8–2.0×.** 4 cases / 60 728 assertions. | ~250 actual | 4 / 60 728 | ~1.5 d |
| ↳ **v1f-2** ✅ 2026-05-21 | **ELL + DIA + close.** ELL (slot-major, global pad; **interop/base — NOT the SELL perf path, pinned**) + DIA (diagonal-major, contiguous-x stream). storage + spmv (serial+parallel, two-rounded; ELL per-row reduction == CSR order, DIA alpha-per-term so DIA-vs-CSR within tol / par-vs-ser exact) + CSR↔X. f32/f64/c32/c64. 24 CLI. **ELL 4.9–5.2× Eigen-CSR (4.4–5.4× our CSR) on regular; DIA 4.8–5.9× (3.7–5.1×) on banded.** 5 cases. **Full v1f suite: 87 cases / 672 343 assertions; 5-config DoD.** | ~150 actual | 5 | ~1.5 d |
| **v1g** ✅ 2026-05-21 | **Matrix-Market I/O + spgemm cap-lift + CLI audit + v1-close.** (spgemm c32/c64 already shipped in v1d.) `.mtx` reader/writer engine-side; spgemm adversarial stress (wins all 2.5–5.5×, no refinement needed) + hash-accumulator cap-lift (>4M cols) + `crd::containers::sort` hidden-malloc fix; CLI-completeness audit (+19 commands → every op has a CLI); ADR-0065 §15 (lock D(sparse)-1..9); killed 2 upstream ICEs on sandbox-showcase (imgui IPO-off + PCH-off). **🎉 Phase 3.1.6 v1 sparse CLOSED. 3 sub-subslices ↓.** | ~750 actual | — | ~3 d |
| ↳ **v1g-1** ✅ 2026-05-21 | **Matrix-Market `.mtx` I/O (engine-side).** In-memory reader + writer in `crd-hesap-sparse` (no platform/filesystem dep — caller does file I/O). Reader: `coordinate` × {real,complex,integer,pattern} × {general,symmetric,skew-symmetric,hermitian} (mirror-expanded; skew negates, hermitian conjugates); `array`/dense rejected with explicit error. Writer: `coordinate general` real/complex/pattern. f32/f64/c32/c64; **8 CLI** (`mtx_read`/`mtx_write` × 4; `mtx_read` takes a String param, `mtx_write` returns Text). 4 cases / 37 assertions. | ~250 actual | 4 / 37 | ~1 d |
| ↳ **v1g-2** ✅ 2026-05-21 | **spgemm stress + hash-accumulator cap-lift + sort hidden-malloc fix.** Adversarial corpus (kSpaCols-boundary / power-law / huge-dim / extra-sparse) vs Eigen-ST: **Cerid wins all in-cap 2.49–5.46×** → no perf refinement needed (trigger not met). Surfaced the hard **4M-col dense-SPA ceiling** → user: implement now. **Hash-accumulator spgemm** (serial + parallel) lifts the cap to arbitrary cols: open-addressing per-row hash, accumulates in the SAME encounter order as the dense SPA + sorted emit → **bit-exact with the dense path**; dense stays the ≤4M fast path. Per-job hashes pre-sized single-threaded (allocating inside `parallel_for` from the non-thread-safe Tlsf was a real heap-corruption bug — fixed). **Also fixed `crd::containers::sort`'s hidden malloc**: `sort` → in-place introsort (zero alloc, deterministic, non-stable); `stable_sort` → caller `IAllocator*`/reusable-scratch (no hidden default-malloc); 3 engine + 1 test site updated. 93 cases / 719 188 assertions; 5-config DoD PASS (validated the cross-module sort change — no golden relied on stable tie-breaking). | ~600 actual | 93 / 719 188 | ~1.5 d |
| ↳ **v1g-3** ✅ 2026-05-21 | **v1-close.** CLI-completeness audit (grep-diff op-list vs registered-commands) closed 6 gap ops → **+19 commands** (`from_csc`/`scale_rows`/`submatrix`/`to_sell`/`spmv_adjoint`/`inner_indices`); every op now has a CLI. **ADR-0065 §15** locks D(sparse)-1..9 + the sort fix + scope deviations. Two recurring upstream-toolchain ICEs on `crd-sandbox-showcase-tests` solved (not split): **imgui-vendor IPO-off** (win-release/shipping MSVC LTCG `DllGetObjHandler` ICE) + **PCH-off** (gcc-13 asan frontend segfault on `curves_showcase.cpp`) — both validated standalone on their failing config (win-shipping + linux-gcc-asan build clean). 18-config first run found ONLY those 2; re-sweep skipped (targeted-fix-verified, CI catches residual). | ~150 actual | — | ~1 d |
| **v2** (detailed plan below ↓) | **Fill-reducing reorderings + symbolic factorisation → NEW sibling `crd-hesap-ordering`** (orderings are shared substrate for v5 sparse direct; keeps `sparse` focused on storage+kernels). RCM + AMD (Amestoy/Davis/Duff 1996) + **full multilevel-METIS nested dissection** + symbolic factorisation (etree + colcounts + L-pattern + supernodes). Pure integer/graph + structure work on `SparsePattern` (no SIMD floats; integer determinism). **The bridge from v1 sparse storage to v5 sparse direct.** **Gates:** AMD fill **≤ 1.05× SuiteSparse AMD `nnz(L)`** + faster ordering; ND beats AMD on the **structured-mesh (FEM) SuiteSparse subset**; symbolic-L pattern == numeric-Cholesky structure. **5 sub-subslices ↓.** | ~2200 | ~80 | ~2 wk |
| ↳ **v2a** ✅ 2026-05-21 | **Graph substrate + RCM + minimal `nnz(L)` counter → new module `crd-hesap-ordering`.** `AdjacencyGraph` (symmetrised `A∪Aᵀ`, diagonal-free, ascending-sorted — D(ord)-4) + `Permutation`/`apply_symmetric` (`PAPᵀ` structure) + **RCM** (George-Liu pseudo-peripheral, capped 5; BFS neighbours by ascending (degree,index)) + symbolic fill metric (`elimination_tree`/`column_counts`/`nnz_l` — **CSparse `cs_etree`/`cs_post`/`cs_counts` ported in signed i32 with -1 sentinels**) + bandwidth/profile. 7 CLI commands (type-agnostic). **Port VALIDATED bit-exact vs Eigen `SimplicialLLT` `nnz(L)`** (bcsstk13/24/25: 434214/2031722/2940220 exact MATCH); RCM bandwidth 1250→422 / 3333→251; **v2b AMD target recorded** (Eigen-AMD nnz(L) 258179/285671/1443995). **5 cases / 128 assertions; 4-config DoD PASS.** | ~700 actual | 5 / 128 |
| ↳ **v2b** ✅ 2026-05-21 | **AMD — the gate.** Quotient-graph approximate-minimum-degree (Amestoy/Davis/Duff). Subdivided along the **correctness/optimization cut** (advisor): the data-structure-correctness risk (v2b-1) isolated from the algorithm-quality risk (v2b-2). +D(ord)-5 (supervariable principal = lowest index) +D(ord)-6 (absorption ascending element-id). **Gate MET: fill `nnz(L)` 0.989×/1.002×/1.044× Eigen-AMD on bcsstk13/24/25 (≤1.05× on all; beats on bcsstk13), ordering 3–15 ms.** Faithful `cs_amd` port; mass elimination was the gate-closer. **2 sub-subslices ↓.** | ~600 | ~20 |
| ↳ **v2b-1** ✅ 2026-05-21 | **Quotient-graph (George-Liu) elimination machinery — validated in isolation.** `detail::quotient_fill(graph, elim_order)` eliminates variables in a GIVEN order (variables + elements + member lists; marker-based dedup + edge-pruning so a clique costs O(\|Lp\|) not O(\|Lp\|²)) and returns `nnz(L) = Σ(\|Lp\|+1)`. **NO selection / NO degree heuristic yet** (the v2b-2 risk, deferred). **Validated bit-exact vs the independent cs_counts oracle**: `quotient_fill(natural) == nnz_l(pattern)` AND `quotient_fill(rcm) == nnz_l(rcm-permuted)` on tridiagonal + 8×8/5×11 grids. Explicit `Array<Array>` rep (correctness-first; packed workspace + GC = v2b-2 perf). **6 cases / 134 assertions; win-debug + win-tidy clean.** | ~250 actual | 6 / 134 |
| ↳ **v2b-2** ✅ 2026-05-21 | **AMD proper + the gate.** Built on the v2b-1 packed workspace via a **4-rung ladder** (each rung localised the next): **rung 1** packed `iw[]`/`pe`/`len`/`elen` flat workspace (GC-free); **rung 2** exact min-degree selection + doubly-linked degree buckets + lowest-index pick (D(ord)-1) — valid+deterministic, measured 1.13–1.16× (confirmed exact-degree drifts); **rung 3a** Amestoy approximate external-degree bound — bought the **~11× ordering speedup** (90→8 ms; killed the O(n·m) recompute) but fill stayed ~1.13× (localised the gate-closer to supervariables); **rung 3b** supervariables (indistinguishable-node merge via sum-mod-n hash + structural compare, principal = lowest index D(ord)-5; nv-weighted degrees; member-chain output) + aggressive absorption (`w[e]==0` ⟺ `Le⊆Lme`); **rung 3c** the decisive piece — **faithful `cs_amd` port** (degree formula confirmed identical algebraically; added **mass elimination** [`d_ext==0` ⇒ fold node into pivot], cs_amd phase order [degree+mass-elim → supernode → finalize-insert], dense-node-last [`deg > max(16,10√n)`]). **Result: fill 0.989× / 1.002× / 1.044× Eigen-AMD on bcsstk13/24/25 (beats / ties / within-gate), ordering 3–15 ms.** Mass elimination was the gate-closer (1.08×→0.99×). **Diagnostic note:** cs_amd POSTORDERS its assembly tree, so step-by-step elimination-order comparison vs Eigen is invalid (postorder is fill-invariant); the gap was real elimination-quality, closed by mass-elim. bcsstk25 residual (1.044×) = un-isolated tie-break/iteration-order divergence from cs_amd — **user-confirmed not a defer** (AMD is a heuristic, ±few % across faithful impls is normal; fill is a downstream-perf knob, never correctness; the real lever is v5's numerical kernels). Isolation is a sanctioned tracked follow-on (`v2b-amd-cs_amd-tiebreak-isolate`). **`amd_order` + dense handling shipped; CLI +2 (`hesap.ordering.amd` + `amd_nnz_l` → 9 ordering commands); tests valid-perm + fill≤natural + determinism green (7 cases/360 assertions); 4-config DoD PASS.** | ~350 actual | 7 / 360 |
| ↳ **v2c** ✅ 2026-05-21 | **Full symbolic factorisation** (the v5 hand-off). `postorder` (cs_post, public) + `SymbolicFactor` (etree + post + colcount + full **L pattern CSC** via faithful `cs_ereach` row-subtree port + **fundamental supernodes** Liu-Ng-Peyton/CHOLMOD `super_symbolic` test) + `symbolic_factorize` driver (one adjacency build, shared scratch). **Gate MET bit-exact**: column-by-column L-pattern row-index diff vs Eigen `SimplicialLLT<Lower,NaturalOrdering>` factor = **MATCH on bcsstk13/24/25** (the rigorous pattern-level gate, not just nnz). **Symbolic analysis WINS Eigen `analyzePattern` 1.77×/1.49× at n≥3562** (scales better — our 2.4× vs Eigen 4.4× over the 7.7× size range); small-N bcsstk13 0.80× = `build_adjacency` alloc/sort constant-factor (tracked `v2c-small-n-analyze-constant-factor`, not algorithmic). Eigen has no full-Li symbolic twin (it defers Li to `factorize`). +4 CLI (`postorder`/`symbolic_nnz_l`/`supernode_count`/`supernodes` → 13 ordering commands). **+10 cases → 17 cases / 2412 assertions; 4-config DoD PASS.** | ~300 actual | 10 / 2412 |
| ↳ **v2d** ✅ 2026-05-21 | **Multilevel ND — scaffold.** `WeightedGraph` (CSR + adjwgt + vwgt) + heavy-edge-matching coarsening (`coarsen_match` D(ord)-1 ties, `contract` merges parallel edges/vwgt, sorted) + `coarsen` level stack (stop n≤100 OR n_coarse≥0.9n OR ≤30 levels) + `bisect_coarsest` (**re-seeding** BFS region-grow → disconnected-graph contract, +D(ord)-7) + `project_down` + `edge_cut` + public `nd_bipartition`. **+1 CLI (`nd_bipartition` → 14 ordering commands)** — shipped now, NOT deferred (user directive). **Scaffold quality is excellent: PERFECT/near-perfect balance** (50/50, 72/72, 112/113, 90/90), **disconnected 54-vtx graph splits 27/27** (re-seeding works across components), multilevel engages (225→coarsest 57 over 3 levels), tridiagonal cut=1 (optimal). NO FM refinement (v2e). Determinism bit-identical; conservation (Σvwgt==n every level) + well-formedness + matching-validity verified. **+10 cases → 25 cases / 5664 assertions; 4-config DoD PASS.** | ~400 actual | 10 / 5664 |
| ↳ **v2e** ✅ 2026-05-21 | **Multilevel ND — refinement + ND driver + CAMD + v2-close.** **FM** (`fm_refine`, gain buckets, best-prefix rollback, equal-gain D(ord)-1, balance kBalanceTol=1.03, keeps both sides non-empty) reaches **optimal cuts** on grids (40×40→20; recovers 90→10) + 51–65% cut reduction on the FEM matrices. **`vertex_separator`** = König min-vertex-cover of the cut (bipartite max-matching) + **`node_fm_refine`** (uphill-rollback node-separator FM). Recursive **`nd_order`** = assign separator-tree postorder `cmember` → **`camd_order`** (constraint-aware copy of the cs_amd port — CHOLMOD-style: per-class min-degree on the FULL graph, dense-node off, supervar/mass-elim cmember-gated; the **interface-fill fix**). +AMD-hybrid leaf threshold (100). **Gate MET on the FEM subset: nd beats Eigen-AMD on bcsstk13 (0.983×) + bcsstk24 (0.999×)**; bcsstk25 (large 3D multi-DOF) loses 1.158× → tracked follow-on `v2e-weighted-compression` (graph compression needs vertex-weight propagation; unweighted regressed). Valid-perm + determinism + disconnected + CAMD-uniform==AMD (port validation) all green. **+5 CLI (`nd_bipartition`/`nd_order`/`nd_nnz_l` … → 16 ordering commands); +21 cases → 36 cases / 7275 assertions; 4-config DoD PASS.** ADR-0065 §16 locks D(ord)-1..7; 18-config sweep on CI. | ~1100 actual | 21 / 7275 |
| **— v2 determinism pins (D(ord), fix BEFORE v2a)** | **D(ord)-1** all tie-breaks resolve by ascending original-graph vertex index (AMD min-degree, RCM equal-level sort, FM equal-gain move, heavy-edge matching ties, separator-vertex selection). **D(ord)-2** iterate hash-like structures (quotient-graph element lists, supervariable members, FM gain buckets) by **sorted key, never slot/insertion order**. **D(ord)-3** pseudo-peripheral node from a structure-derived fixed seed (hash of `n`+`nnz`), never RNG state. **D(ord)-4** re-sort each vertex's adjacency ascending before matching → coarsening is input-adjacency-order-independent. | — | — |
| **v3** (detailed plan below ↓) | **SVD + dense eigenvalue — MAX-AMBITION (MRRR + AED HARD-GATE close).** Symmetric eig (tridiag + QL/QR + **D&C Cuppen** + **MRRR**) + SVD (Golub-Reinsch/Demmel-Kahan + **D&C bidiagonal** + **randomized Halko 2011**) + non-sym eig (balance + Hessenberg + Francis double-shift Schur + **AED Braman-Byers-Mathias** + eigenvectors) + least squares (LS / **NNLS** Lawson-Hanson / **TLS**) + pinv + complex variants + CLI. **Beat Eigen everywhere; beat LAPACK on MRRR/D&C/AED.** Reference: Eigen (primary head-to-head) + **LAPACK via OpenBLAS `C_LAPACK`** (accuracy oracle on Windows; Linux-CI fair-speed). Per-sub-slice rule: **study Eigen + LAPACK reference source BEFORE implementing**, attack each routine granularly. **9 leaf sub-subslices ↓.** | ~6–8k | ~200 | 8–12+ wk |
| ↳ **v3a** ✅ 2026-05-23 (v3a-1/2/2.5/3 ✅) | **Symmetric + Hermitian eigensolver — COMPLETE.** Shared reflector substrate + blocked tridiag + QL/QR + **D&C (Cuppen)** + **MRRR (`dstemr`-class: dqds + Sturm/multisection eigenvalues, `dlar1v`/`dlarrf`/`dlarrb`/`dlaneg`/`dlarrv` cluster-robust eigenvectors)** + complex Hermitian. **Beats Eigen + LAPACK across the board** (full eig 1.4–1.95× Eigen / 2.1–3.7× LAPACK; MRRR tridiag vectors crush Eigen 5–64× + LAPACK `dstemr` 1.7–2.7×, match/beat `dstedc`; values-only multisection beats `dsterf`/crushes `dstemr`). ADR-0065 §17 (D(dense-eig)-9..12). | ~3900 | ~95 | — |
| ↳ **v3a-1** ✅ 2026-05-22 | **Real f32/f64, blocked from the start.** Shared `detail/householder.hpp` (`make_householder` = `dlarfg`-faithful incl. safmin guard; shared by tridiag/bidiag/Hessenberg) + **blocked** symmetric tridiagonalization (`dsytrd` = `dlatrd` panel + `syr2k` trailing update; last block via `dsytd2` oracle) + implicit QL/QR Wilkinson shift (`dsteqr`: faithful `√(\|d·d\|)·eps`+SAFMIN split) + eigenvector accumulation + back-transform (`dormtr`) + perm-array ascending sort + pinned sign. **Column-major SIMD eigenvector rotation (ADR-0083 escape) + single-pass SIMD panel symv.** CLI `eig_sym.{f32,f64}`. Interfaces complex-aware (`tau:T`, `D`/`E`:`RealType<T>`). D(dense-eig)-1..5. **Gate MET: beats Eigen `SelfAdjointEigenSolver` 1.21–1.94× + LAPACK `dsyev` 1.90–4.02× at N=64–1024, accuracy ~1e-12; 5-config DoD PASS.** | ~750 | ~25 | — |
| ↳ **v3a-1b → v3a-2.5** ✅ 2026-05-22 | **Complex Hermitian eig** (folded into v3a-2.5 per user directive 2026-05-22; reuses the v3a-2 D&C solver). `zhetd2` reduce Hermitian → REAL tridiagonal (complex reflectors + accumulated phase-diagonal) + reuse real D&C/`steqr` on `(D,E)` + complex back-transform `V=Q·D_phase·Z`. **SIMD complex reduction: working matrix carried as two REAL arrays `ar`/`ai` → contiguous-row real-SIMD zhetd2 matvec + rank-2; back-transform reads reflectors directly (no complex materialization).** c32/c64. CLI `hesap.dense.eig.herm.{c32,c64}`. **Gate MET: beats Eigen complex `SelfAdjointEigenSolver` N≥128 (1.11× → 2.22× at N=1024), ties N=64 (1.00× — floor); beats LAPACK `zheev` everywhere (2.5–4.4×); accuracy ~1e-14.** `[herm]` 5 cases/20,117 assertions; 5-config DoD PASS. | ~450 | ~15 | — |
| ↳ **v3a-2** ✅ 2026-05-22 | Divide-and-conquer (Cuppen 1981 / `dstedc`): rank-1 tearing + deflation (`dlaed2` negligible-weight + equal-pole Givens) + secular-equation root-finder (`detail/secular.hpp` bracket-confined Newton+bisection, **D(dense-eig)-8**) + Löwner/Gu-Eisenstat eigenvectors (interleaved product — N=512 overflow fix [[feedback_lowner_product_overflow_interleave]]) back-transformed via `gemm_parallel`; fused Q·V merge. **Serial recursion + deterministic parallel merge-gemm = bit-identical across worker counts (D(dense-eig)-6/-7).** `eig_sym` dispatches QL/QR ≤256, D&C >256. **Gate MET: beats Eigen + LAPACK at every N (see v3a-1 row).** Subslices v3a-2.1..2.4 (secular / rank1 / dc_tridiag / dispatch) + 2.5 (complex Hermitian) all closed. | ~800 | ~22 | — |
| ↳ **v3a-3** ✅ 2026-05-23 | **MRRR — CLOSED** (`dstemr`-class: dqds eigenvalues `dlasq2/3/4/5/6` + Sturm/multisection, relatively-robust representations + twisted factorizations `dlar1v`/`dlarrf`/`dlarrb`/`dlaneg`/`dlarrv`, Gram-Schmidt re-orthogonalization fallback for clustered eigenvalues). **HARD-GATE MET: O(n²) cluster-robust vectors; crush Eigen 5–64× + LAPACK `dstemr` 1.7–2.7×, match/beat `dstedc`; glued-Wilkinson W₂₁⁺ `‖VᵀV−I‖<1e-8`.** All 4 sub-subslices ✅; CLI `eig.sym.mrrr`; ADR-0065 §17 (D(dense-eig)-9..12); 5-config per-slice DoD PASS. | ~2000 actual | 30 | — |
| ↳ **v3a-3.1** ✅ 2026-05-23 | **Eigenvalues-only** (the `dlarre` `JOBZ='N'` path). Matrix scaling to safe range + `dlarrr` relative-accuracy test → **split into unreduced blocks** (`dlarra`) → per-block Gershgorin global bracket + `pivmin` → **eigenvalues by Sturm-count bisection** (`detail/sturm_count.hpp` = tridiagonal Sturm recurrence on `(d, e²)` with `pivmin` guard + the `dlarrc` two-pivot interval count; `dlarrk` single-value + `dlarrd`/`dlarrb` block bisection) + **dqds** (`dlasq2`) for the whole-block fast path. **Gate: eigenvalues match LAPACK `dstebz`/`dstemr(JOBZ='N')` + our `steqr`/D&C oracle to relative accuracy.** Real intermediate milestone, gateable alone. **🔄 SUBSTRATE SHIPPED 2026-05-23** — `detail/sturm_count.hpp` (tridiagonal Sturm `negcount` + two-pivot interval count + `compute_pivmin` + `gershgorin_bounds` + `tridiag_split` + `bisect_eigenvalue` + `tridiag_eigenvalues` split-and-bisect driver), all pure-scalar zero-alloc f32/f64. **Gate MET vs both the Toeplitz closed form AND `eig_sym` on random + reducible tridiagonals (worst rel err ~1e-9, < D&C residual floor)**; determinism `memcmp`-bit-identical; +9 cases / +33 assertions (suite 215/98,749). **🔄 dqds-a SHIPPED 2026-05-23** — `detail/dqds.hpp`: the `dlasq6` (dqd) + `dlasq5` (dqds, IEEE-only) inner sweeps ported **line-for-line** via the 1-based `Z1` ping-pong wrapper, + `build_qd_ldlt` (strict-shift LDLᵀ qd-array with pivot guard) + an **unshifted dqd driver** (`dqd_eigenvalues_unshifted`). **Gate MET: dlasq6 one-step == independent Rutishauser transform (proves the fragile `4*N0+PP-3` Z-layout); dlasq5(τ=0)==dlasq6; build_qd reconstructs the shifted tridiagonal; unshifted driver matches the Toeplitz closed form AND `eig_sym` (rel err <1e-9). +5 cases/+33 assertions → suite 220/98,782 green.** **✅ dqds-b SHIPPED 2026-05-23 — the full `dlasq2/3/4` shift+deflation+split driver (complete dqds engine); `dqds_eigenvalues` + `tridiag_eigenvalues_dqds`; public `eigvals_sym` (dense→dsytrd→dqds) + parallel multisection at N≥512 + CLI `eigvals.sym.{f32,f64}`. Gates: graded spectrum 1e-8..1e11 to <1e-10 RELATIVE (the dqds payoff); vs LAPACK eigenvalues-only multisection beats `dsterf` at N≥2048, crushes `dstemr` 1.6×.** v3a-3.1 CLOSED. | ~700 | 14 | — |
| ↳ **v3a-3.2** ✅ 2026-05-23 | **Single-RRR eigenvectors** (well-separated case). **SHIPPED:** `detail/mrrr_vectors.hpp::dlar1v` (twisted factorization → eigenvector + support `isuppz` + `resid`/`rqcorr`; faithful IEEE port, 1-based `Z1`, NaN-fallback loops) + `mrrr_single_rrr_vectors` (root RRR `T−σI=LDLᵀ` σ-strict-lower-bound + per-eigenvalue `dlar1v` + Rayleigh-quotient refinement loop MAXITR=10/RQTOL=2eps best-residual-tracking-bracketed-rqcorr + pinned sign). **Gate MET: `dlar1v` `‖Tz−λz‖<1e-12`; single-RRR on well-separated n=24 `‖VᵀV−I‖<1e-10` (orthonormal BY CONSTRUCTION, no Gram-Schmidt = the O(n²) win) + `‖Tv−λv‖<1e-10`.** | ~450 | ~2 | — |
| **— v3a-3.3 leaf contract (CLUSTER handling — ALL ✅ 2026-05-23)** | **A1** ✅ `dlaneg` (twisted Sturm count on LDLᵀ; faithful BLKLEN=128 NaN-rechunk; gate MET vs tridiagonal Sturm). **A2** ✅ `dlarrb_refine` (refine cluster eigenvalues *within* an RRR by `dlaneg` bisection; gate MET — perturbed eigenvalues refined to <1e-10). **A3** ✅ `dlarrf` (form child RRR: shift to cluster L/R end, element-growth ≤ MAXGROWTH1·spdiam, KTRYMAX=1 back-off, refined-RRR fallback). **A4** ✅ recursive `dlarrv` cluster loop (segment by relative gap MINRGP=1e-3; singleton→`dlar1v`+RQ; cluster→child RRR `dlarrf` + refine `dlarrb` + recurse depth≤8; **Gram-Schmidt fallback** for residual clusters) + **glued-Wilkinson W₂₁⁺** gate. | ~600 | 5 |
| ↳ **v3a-3.3** ⭐ ✅ 2026-05-23 (A1–A4) | **`dlarrv` full — cluster handling + the hard-gate. SHIPPED.** `detail/mrrr_vectors.hpp::mrrr_compute_vectors` (root RRR + recursive cluster processor) — orthonormal BY CONSTRUCTION via child-RRR shifts (no Gram-Schmidt in the common case = O(n²)), GS-fallback only for genuine near-multiplicity. **HARD-GATE MET: glued-Wilkinson W₂₁⁺ (n=21, tightest eigenvalue clusters) → `‖VᵀV−I‖ < 1e-8` + `‖Tv−λv‖ < 1e-9`.** This is the part that historically destroys eigensolver attempts; passing W₂₁⁺ is the proof. **NEXT (v3a-3.4): wire MRRR into `eig_sym` (dense → dsytrd → mrrr_compute_vectors → back-transform V=Q·Z) + benchmark the O(n²) vector path vs LAPACK `dstemr`/`dstegr` + Eigen at scale = the architectural crush.** Original 🔄 detail: Child-RRR representation tree (re-shift inside clusters until eigenvalues separate by the relative-gap criterion) + **Gram-Schmidt re-orthogonalization fallback** for residual clusters (LAPACK `dlarrv` does this — don't hope clusters never occur). **The part that has destroyed prior attempts: orthogonality on clustered/glued spectra.** **Gate: `‖VᵀV−I‖ ≤ O(n)·eps` on glued-Wilkinson + Demmel-Kahan adversarial fixtures.** | ~500–700 | ~8 | — |
| ↳ **v3a-3.4** ✅ 2026-05-23 | **`eig_sym_mrrr` full path + parallel vectors + CLI + the gate.** `eig_sym_mrrr` (dense → dsytrd → dqds eigenvalues → `mrrr_compute_vectors` → back-transform V=Q·Z; matches `eig_sym` <1e-10, orthonormal/residual <1e-9). **Parallel MRRR vectors** over independent eigenvalue segments (`crd::jobs` work-stealing + per-worker external-buffer Tlsf arena + adaptive granularity). CLI `hesap.dense.eig.sym.mrrr.{f32,f64}`. **Bench vs BOTH Eigen `computeFromTridiagonal` AND LAPACK `dstedc`/`dstemr`: crush Eigen 5–64×, crush `dstemr` 1.7–2.7×, match/beat `dstedc` (BLAS-3 D&C) 0.84–1.52×; orth ~1e-13.** Honest: serial MRRR lost (memory/BLAS3 wall); parallelism (cores LAPACK lacks) is the lever. SIMD-across-eigenvectors tried+reverted (memory-bound). **GATE MET: beats Eigen + LAPACK.** ADR-0065 §17 + 5-config DoD PASS. | ~400 | ~3 | — |
| ↳ **v3b** ✅ 2026-05-23 (v3b-1/2/3/1c ✅) | **SVD — CLOSED.** v3b-1 (bidiag + blocked dgebrd/dorgbr + Demmel-Kahan dbdsqr) + **v3b-2 (Gu-Eisenstat D&C — beats Eigen BDCSVD 1.6–3.2× + LAPACK dgesdd 1.4–4.6× at all N)** + v3b-3 (randomized rsvd/rsyev) + **v3b-1c (complex SVD: complex bidiag → REAL (d,e) → reuse the real D&C/dbdsqr crush → complex back-transform; svd/svdvals c32/c64 + CLI; gated A=U S V^H <1e-9)**. ADR-0065 §18-§23. Perf follow-ons filed (v3b-1c-blocked-complex-bidiag, v3b-2-parallel-merges, v3b-3-nystrom-cholesky, v3b-1c-svdvals-dqds-direct). | ~1900 | ~70 | — |
| ↳ **v3b-1** 🔄 (1a ✅) | Householder bidiagonalization (`dgebrd`, reuses v3a-1 substrate) + Demmel-Kahan implicit-zero-shift QR on bidiagonal (`dbdsqr`) + singular-vector accumulation + descending sort + complex SVD (`zgesvd`). **Gate: reconstruction `A=UΣVᵀ` + vs Eigen `JacobiSVD`.** **v3b-1a ✅ 2026-05-23** — `svd.{hpp,cpp}::bidiagonalize` (Golub-Kahan unblocked `dgebd2`, m≥n upper bidiagonal; reuses `make_householder` + apply-reflector left/right). **Gate MET: `A=Q B Pᵀ` reconstruction <1e-12 (m=n + m≠n), Q/P orthonormal <1e-12.** **1a/1a-perf/1b/1b-perf ✅ 2026-05-23** (1a-perf blocked `dlabrd` §19; 1b `dbdsqr`+driver+CLI+4-col bench §18; **1b-perf blocked `dorgbr` §20 — full SVD beats LAPACK `dgesvd`, C/`dgesvd` 1.45 @512; parallel-dbdsqr skipped**). **v3b-1c (complex SVD) ✅ 2026-05-23 — complex bidiag (zgebd2, real (d,e)) + form Q/P + driver reusing the real D&C/dbdsqr + complex back-transform; svd/svdvals c32/c64 + CLI; gated A=U S V^H <1e-9. ADR-0065 §23 (D(svd)-16/17/18).** | ~600 | 18 ✅ | — |
| ↳ **v3b-2** 🔄 (algo ✅, DoD pending) | Divide-and-conquer bidiagonal SVD (Gu-Eisenstat / `dbdsdc`, BDCSVD-class) on parallel BLAS-3. **Gate MET + CRUSHED 2026-05-23:** full SVD beats Eigen `BDCSVD` **1.59–3.21×**, LAPACK `dgesdd` **1.37–4.55×**, `dgesvd` 4.8–10.5× at N=128–1024; recon ~1e-14. Full chain `dlasd4/5`+`dlaed6` → `dlasd2/3/1` → `dlasdq/dlasdt/dlasd0` (`svd_secular.hpp`+`svd_dc.hpp`), wired into `svd()` at n≥64. Reconstruction-gated (`‖B−UΣVᵀ‖<1e-9`, multi-level). ADR-0065 §21 (D(svd)-10..14). MRRR-SVD fork rejected (deferred `v3b-2-svd-via-mrrr`). Remaining: 5-config DoD + commit; `v3b-2-parallel-merges` filed. | ~700 | ~18 | — |
| ↳ **v3b-3** ✅ 2026-05-23 | **Randomized SVD + symmetric eig — CLOSED.** `rsvd` (Halko 2011: Gaussian sketch + QR range-finder + power iterations → small dense `svd`) + `rsyev` (randomized symmetric eig, Rayleigh-Ritz `QᵀAQ` + `eig_sym` — D(svd)-15: chosen over Nyström-`C⁻ᵀ`, more general + reuses eig_sym). **Pure reuse of shipped gemm/QR/svd/eig_sym — NO new kernels.** Gated: exact rank-r SVD recon `<1e-8` + orthonormal U/V; rank-r PSD eig residual `<1e-7`. **No head-to-head bench (intentional — Eigen/LAPACK have no randomized path; gate is accuracy + structural O(mn·ℓ) speed).** CLI `rsvd`/`rsyev`.{f32,f64}. ADR-0065 §22. DoD debug/asan/shipping/tidy green. Nyström-Cholesky variant filed `v3b-3-nystrom-cholesky`. | ~300 | ~14 | — |
| ↳ **v3c** ✅ 2026-05-23 | **Least-squares family — CLOSED.** `lstsq` (QR full-rank / **COD** rank-revealing min-norm / SVD min-norm) + `pinv` (Moore-Penrose, COD + SVD) + **NNLS** (Lawson-Hanson + Givens up/downdate) + **TLS** (via SVD). **CRUSHES LAPACK everywhere; beats Eigen on pinv (1.12–1.60×) + SVD-path (1.08–2.40×), ties the COD default; NNLS/TLS correctness-gated (KKT + exact recovery).** ADR-0065 §24 (D(lstsq)-1..3). +14 CLI. Full suite 279 cases / 105 764 assertions. **Follow-ups all RESOLVED 2026-05-23 (no debt): v3c-1c (unblocked QR fast-path — n=64 loss→1.19× win; mid-range m≈2n n=128–512 characterized as the ADR-0083 single-core layout wall, user-confirmed not worth chasing), v3c-1d + v3c-2b (judged low-value, not pursued — see rows).** | ~1750 | 31 | — |
| ↳ **v3c-1** ✅ 2026-05-23 | **lstsq + pinv (COD path = the COD decision).** **v3c-1a:** column-pivoting Householder QR (`dgeqp3`/`dlaqp2` faithful — Businger-Golub on transposed SIMD scratch + LAPACK partial-norm downdate-with-recompute, reveals rank) + **complete orthogonal decomposition** (`dtzrzf`/`dlatrz` RZ reflectors reduce rank×n trapezoidal → rank×rank T11; `solve_cod` = dgelsy min-norm). **v3c-1b:** `lstsq` (Auto=COD / QR / SVD methods, multi-RHS, lazy residual, rcond=max(m,n)·eps) + `pinv` (Auto=COD, SVD) + complex via SVD + 8 CLI. **Blocked-reflector-apply attack** (user-directed): BLAS-3 `dlarfb` (`detail/apply_q_block.hpp`, all 4 side×trans modes, gemm-driven) + blocked recursive `trtri` → **pinv 0.11×→1.60× Eigen** (the T⁻¹ scalar back-sub was the O(r³) bottleneck). **Bench (i9-14900K AVX2, f64): vs LAPACK CRUSH everywhere** (lstsq 1.70–12.19×, SVD 1.07–4.01×); **vs Eigen: pinv 1.12–1.60× (↑ with n), SVD-path 1.08–2.40× (n≥128), COD-default 0.84–1.07× (parity), QR-tall 0.69–0.93× — a GENUINE LOSS to Eigen, NOT accepted; tracked as planned slice **v3d↓ v3c-1c** (the v3c close wrongly footnoted it).** 4-config DoD (debug/shipping/tidy/asan) PASS; full hesap-dense **271 cases / 105 709 assertions**. | ~1400 | 19 / 4253 | — |
| ↳ **v3c-1c** ✅ 2026-05-23 — **unblocked fast-path + honest scope call** | **Full-rank lstsq QR-tall: small-n loss fixed; mid-range characterized (NOT debt).** Added `factor_qr_unblocked` (whole matrix as ONE Householder panel on a transposed scratch — reuses `panel_factor_qr_transposed`; the transposed scratch is ALSO the ADR-0083 column-fit escape for the factor, no trailing gemm) + a crossover dispatch in `factor_qr` (`kQrUnblockedMax=128`, tall m≥n only). **Bench-mapped (bench_hesap_lstsq_vs_reference, fine n grid, factor-only blocked vs unblocked vs Eigen): unblocked beats blocked through n≈128 and beats Eigen at n=64 (1.19× full lstsq solve, was ~0.69× — loss→WIN); n=96 0.71→0.96×.** **Mid-range n=128–512 (m≈2n) remains 0.76–0.93× Eigen — a single-core ADR-0083 column-major-fit wall (Eigen's HouseholderQR is column-major-native; m=2n is only 2× tall so no parallel headroom; a cheap serial-W tuning lever was tried + REVERTED as it regressed large-n).** **User-confirmed (2026-05-23) NOT worth chasing + NOT debt:** QR is an opt-in method (default `Auto=COD` ties Eigen, SVD crushes 1.39–2.18×, large-N/many-RHS crush via parallel gemm), the band is narrow (m≈2n, n=128–512), the loss is modest (7–24%, a layout artifact not a kernel defect — same class v0e accepted), and no consumer pulls it. A column-major-QR rewrite would target parity-not-crush there. 4-config DoD green (debug full 348 559 assertions / shipping / asan / tidy). | ~250 actual | (suite) | — |
| ↳ **v3c-1d** ✅ 2026-05-23 — **NOT PURSUED (user call), no debt** | **Blocked RZ reflector apply (`dlarzb`) — judged not worth it.** The RZ apply (Z/Zᵀ from `dtzrzf`) in `solve_cod`/`pinv_via_cod` is scalar, but runs ONLY on the rank-deficient/underdetermined path (n>rank); the common full-rank case has Z=I and skips it. No consumer pulls rank-deficient tall least-squares at scale, so it is not a measured loss — a principle-purity item ("no scalar fallbacks"), not a strength item. Correctness is shipped + tested. User-confirmed 2026-05-23: does not make hesap weaker → not pursued, NOT debt. (Revisit only if a rank-deficient pinv/lstsq consumer ever appears.) | — | — | — |
| ↳ **v3c-2** ✅ 2026-05-23 | **NNLS + TLS + close.** **NNLS** real f32/f64 (Lawson-Hanson active-set + **incremental thin QR**: re-orthogonalised MGS add + Givens re-triangularisation downdate, Björck §5.8; tie-break ascending col index D(lstsq)-1; KKT-gated + exact-recovery + 2×2 textbook). **TLS** 4 variants via `[A\|b]` SVD (last-d right-singular subspace `X=−V12·V22⁻¹`, type-generic Gauss-Jordan V22 inverse, `exists` flag for V22-singular, multivariate d≥1; exact-recovery + complex gated). +6 CLI (`nnls`×{f32,f64}, `tls`×{f32,f64,c32,c64}). ADR-0065 §24. 4-config DoD (debug/shipping/tidy/asan) PASS; CI owns the 18-config sweep. NNLS never benched vs a reference → tracked as planned slice **v3c-2b** (no "beat Eigen" claim without proof). | ~600 | 8 / 55 | — |
| ↳ **v3c-2b** ✅ 2026-05-23 — **NOT PURSUED (user call), no debt** | **NNLS proof bench — judged low-value.** NNLS correctness is already proven the right way: the NNLS optimum is UNIQUE for full-rank A, so the shipped KKT + exact-recovery gates mean any correct solver matches us. The "always bench both Eigen + LAPACK" mandate can't be meaningfully satisfied here — LAPACK has NO NNLS, and Eigen's NNLS is in `unsupported/` (unoptimized), so there is no authoritative reference to crush; a speed number vs an unsupported module is low-value. User-confirmed 2026-05-23: not pursued, NOT debt. (Add a bench only if a real NNLS-heavy consumer + a credible reference appear.) | — | — | — |
| ↳ **v3d** 🔄 | **Non-symmetric eigensolver (full).** Balance + Hessenberg + Francis double-shift Schur + **AED** + eigenvectors + complex Schur. **Plan advisor-vetted: subdivision v3d-1 (real Schur: 1a reduction / 1b Francis-QR / 1c AED) + v3d-2 (eigenvectors + complex). Traps pinned (reflector-offset for blocked apply reuse; `dlanv2` fragility); D(non-sym)-1..4 drafted. 6 sub-subslices ↓.** | ~1500 | ~40 | — |
| ↳ **v3d-1** 🔄 (1a+1b ✅ 2026-05-23) | **Real Schur = balance + Hessenberg + Francis QR + AED.** 1a + 1b shipped (beat Eigen+LAPACK on the reduction; correct Schur); 1c AED is the HARD-GATE, planned ↓. | ~900 | ~22 | — |
| ↳ **v3d-1a** ✅ 2026-05-23 — beats Eigen+LAPACK | **Balance + Hessenberg reduction.** `eig_nonsym.{hpp,cpp}` — `balance` (dgebal: faithful eigenvalue-isolating permutation + radix-2 diagonal scaling; gated isolation + trace-invariance + block-norm reduction) + `hessenberg` (dgehd2) + `form_hessenberg_q` (dorghr). Real f32/f64. **Gate MET: A=Q·H·Qᵀ recon <1e-7 (n=6/32/64/160/320/512), Q orthogonal, H upper-Hessenberg; 8 cases / 196 290 assertions; 4-config DoD.** **The winning kernel = SIMD ROW-WISE unblocked reduction** (two-sided updates `vᵀ·A` + rank-1 + per-row dot/axpy stream along CONTIGUOUS rows in row-major, sidestepping the column-reduction layout penalty + small-K gemm overhead). Blocked `dlahr2`+gemm was implemented + validated but **DROPPED** (0.2× Eigen: small-K nb=32 gemm + scalar/strided panel + jobs-frame-arena exhaustion across ~15 panels; SIMD-unblocked ~5× faster). **Bench (i9-14900K AVX2, f64): vs Eigen HessenbergDecomposition 1.16–1.28×; vs LAPACK dgehrd 2.07–13.86× (OpenBLAS-generic-on-MSVC).** Lesson: [[feedback_simd_rowwise_unblocked_beats_blocked_smallk]]. | ~400 | 8 | — |
| ↳ **v3d-1b** ✅ 2026-05-23 | **Francis double-shift QR → real Schur.** `real_schur` (LAPACK `dlahqr`: implicit double-shift bulge-chase + Ahues-Tisseur small-subdiagonal deflation + exceptional-shift schedule [D(non-sym)-1] + `dlanv2` 2×2 standardization) → quasi-triangular T + orthogonal Schur vectors Z + eigenvalues (wr/wi). Real f32/f64. **Gate MET: H=Z·T·Zᵀ recon <1e-8 (n=8/32/64), Z orthogonal, T quasi-upper-triangular, 2×2→complex-conjugate pairs (±i exact), trace invariant, + the full pipeline A=(Q·Zs)·T·(Q·Zs)ᵀ; 4 cases / 2464 assertions; 4-config DoD.** | ~500 | 4 | — |
| ↳ **v3d-1c** ⭐ ✅ 2026-05-23 — **HARD-GATE MET** | **Aggressive Early Deflation — the HARD-GATE, CLOSED.** schur_aed beats Eigen RealSchur 1.17–1.74× + LAPACK dhseqr 1.26–1.97× at every N. All 3 sub-subslices ✅ (reorder + deflation + driver). Beat LAPACK `dhseqr`, not just Eigen. **Scoping (deep-research 2026-05-23): ~2 500 LOC, 5 interlocking routines, unavoidable dependency chain** `dlasy2` (2×2/4×4 Sylvester solve) → `dlaexc` (adjacent 1×1/2×2 block swap) → `dtrexc` (Schur reorder) → `dlaqr2/3` (AED deflation window) → `dlaqr5` (small-bulge multishift) → `dlaqr0` (driver). Window Schur reuses the shipped `real_schur`; bulge-chase reuses `dlanv2`/reflectors. **3 sub-subslices ↓** (each independently gateable; a fresh focused session each — the hardest numerical code in the module). D(non-sym)-2 (AED window-size formula), D(non-sym)-3 (shift order = descending \|λ\|, ascending-index tie-break). | ~2500 | ~50 | — |
| ↳ **v3d-1c-1** ✅ 2026-05-23 | **Schur eigenvalue reordering** (the AED prerequisite). `dlasy2` (faithful 2×2/4×4 Sylvester `T11·X − X·T22 = σ·B` solve, complete pivoting) + `dlaexc` (swap adjacent diagonal blocks of order 1/2: Givens for 1×1/1×1, Householder + Sylvester + `dlanv2` re-standardize for the 2×2 cases; provisional-swap reject test) + `dtrexc` (reorder driver: move block ifst→ilst by repeated adjacent `dlaexc`, both directions, 2×2-split handling) + public `reorder_schur`. Faithful LAPACK ports (1-based internally, converted at the boundary). **Gate MET: reorder a random real Schur form (4 ifst/ilst pairs, up + down) → `Z'·T'·Z'ᵀ` = the SAME matrix <1e-9, Z' orthogonal, T' quasi-triangular; a real eigenvalue moves ifst→ilst exactly; 2 cases / 244 assertions; 4-config DoD (debug/shipping/tidy/asan).** | ~870 | 2 / 244 | — |
| ↳ **v3d-1c-2** ✅ 2026-05-23 | **AED deflation window** (`dlaqr2`). `aed_deflate`: window [kwtop,kbot] Schur via the shipped `real_schur`, spike `S·V(1,:)` (S = H(kwtop,kwtop−1)), deflation loop from the bottom — deflatable iff `|S·V(1,j)| ≤ max(smlnum, ulp·|T(j,j)|)` (2×2: both components) else `reorder_schur` moves it up; then reflect the spike (Householder) + re-Hessenbergize the leading block (`hessenberg`+form-Q) + copy the window back + **global similarity updates** of H/Z via gemm slabs; returns {ns shifts, nd deflated}. Faithful `dlaqr2` (bubble-sort accuracy step skipped — optional, not correctness). **Gate MET: a decoupled trailing window deflates fully (nd==nw, ns==0); on a general window similarity `H0=Z·H·Zᵀ` <1e-8 AND the spectrum is invariant (eig(h)==eig(h0) <1e-7); nd+ns==nw; 2 cases / 252 assertions; 4-config DoD.** | ~430 | 2 / 252 | — |
| ↳ **v3d-1c-3** ⭐ ✅ 2026-05-23 — **HARD-GATE MET** | **AED driver + the hard-gate close.** `schur_aed` (LAPACK `dlaqr0`-class driver): loop = split active block → AED via `aed_deflate` (deflate a whole trailing window) → nibble (re-AED if productive) → double-shift QR `dshift_sweep` using the undeflated AED eigenvalues as shifts → dispatch to `real_schur` (dlahqr) below the NMIN=200 crossover (+ stall fallback). Reuses `dlanv2`/reflectors + `real_schur` + `reorder_schur`. Reports the QR-sweep count. **HARD-GATE MET (bench `bench_hesap_eig_nonsym_vs_reference`, i9-14900K AVX2, f64, Schur-from-Hessenberg): schur_aed beats Eigen `RealSchur` 1.17–1.74× AND LAPACK `dhseqr` 1.26–1.97× at EVERY N (100/200/400); at n=400 AED engages (184 sweeps) and beats pure-dlahqr 1.53× / Eigen 1.74× / dhseqr 1.34×; recon ~1e-13.** Gate (correctness): AED-driven Schur == pure-dlahqr spectrum + recon `H=Z·T·Zᵀ` <1e-7 (n=40/140/260); 1 case / 43 766 assertions; 4-config DoD. **D(non-sym)-2** (window nw=min(nh, max(2, nh/3))), **D(non-sym)-3** (shifts = undeflated AED eigenvalues, consecutive pairs). Inter-AED sweep is the Francis double-shift `dshift_sweep` (NMIN=200 crossover); AED already beats Eigen + LAPACK at every measured N. | ~520 | 1 / 43 766 | — |
| ↳ **v3d-1c-4** ✅ 2026-05-23 — **CLOSED (M1+M2+M3)** | **`dlaqr5` small-bulge multishift QR sweep — replaces the Francis double-shift inter-AED sweep.** Pack `ns` shifts (conjugate pairs) into `nbmps=ns/2` bulges chased as a TRAIN down the diagonal; **accumulate the per-window 3×3 reflectors into a small `kdu×kdu` orthogonal `U`, then update the far off-diagonal slabs by a single `gemm` each via `slab_right`/`slab_left_t`** — the BLAS-3 arithmetic-intensity lever (same accumulate-then-gemm pattern AED already uses for its window Schur). Reuses `make_householder`/`dlaqr1`. **M1 ✅ 2026-05-23** — full faithful `dlaqr5` port landed (general `ns`, KACC22=1 accumulate-into-`U` + gemm slab updates, 1-based internal like the `dtrexc` port, incl. the BMP22 2×2 endgame + bulge-collapse/reinflate). Exposed as `detail::multishift_sweep` (the anon `dlaqr5_sweep`), NOT yet on the driver path. **Gate MET (rigorous implicit-Q characterization — algorithm-independent, since `dshift_sweep` early-starts the bulge so bit-equivalence only holds when paths align): for ns=2, the swept matrix is an orthogonal SIMILARITY `h0=Z·Hb·Zᵀ` (<1e-9), `Z` orthonormal, `Hb` upper-Hessenberg, AND `Z·e1 ∝` the shift-polynomial first column `(H−s1)(H−s2)e1` (<1e-9) = THE double-shift sweep; n=12/60/200 whole-matrix + a decoupled sub-block (ktop=3,kbot=35) case; n=12 bit-matches `dshift_sweep` to 2e-13 when paths align.** Latent KACC22 bug found+fixed on encounter (in-slab left update must stop at MIN(NDCOL,KBOT), not nn — the sub-block double-apply). `[multishift]` 15 assertions / 2 cases; full `[nonsym]` 242 779 assertions regression-free; win-debug + win-tidy clean. **M2 ✅ 2026-05-23** — the general port already handled `nbmps≥2` (M2 was test-only, no library change): `ns=4` (nbmps=2) + `ns=6` (nbmps=3), n=12/60, exercising the multi-bulge chain + U-accumulation + inter-bulge delayed updates + BMP22-with-chain. Gate MET via implicit-Q with the **degree-`ns` shift polynomial** `Q·e1 ∝ ∏_pairs(H²−sum·H+prod·I)·e1` (<1e-9) + similarity + orthonormal + Hessenberg. `[multishift]` 31 assertions / 3 cases; full `[nonsym]` 242 795 assertions regression-free; win-tidy clean. **M3 ✅ 2026-05-23** — wired into the live `schur_aed`: the `ns/2` separate single-bulge `dshift_sweep` calls are replaced by ONE `dlaqr5_sweep` train over all undeflated AED shifts; `total_sweeps` now counts train passes. Full `[nonsym]` suite (driver recon/spectrum gates n=40/140/260) regression-free; **4-config DoD green** (debug/shipping/asan `[nonsym]` 242 795 assertions each + tidy clean; CI owns the 18-config sweep). **Bench (`bench_hesap_eig_nonsym_vs_reference`, win-vs-ref Release, i9-14900K AVX2, f64): the train's advantage over Eigen `RealSchur` WIDENS monotonically with N — 1.85× (n=400) → 3.20× (n=800) → 3.99× (n=1200), in just 3/4/5 TRAIN PASSES (the train batches all shifts per pass, so pass count barely grows while Eigen's un-batched QR scales worse). vs LAPACK `dhseqr` 1.45–1.93× (capped at n≤400 — OpenBLAS-generic crashes >512); vs pure-dlahqr 1.61× at n=400; recon 9e-14→2.6e-13. (The n=400 cap in v3d-1c-3 hid this widening; the train is the BLAS-3 large-N lever.)** **NMIN decision = measurement-driven, kept at 200:** a controlled NMIN=60 run showed AED+train LOSES below 200 (n=100: 0.64× dlahqr / 0.83× Eigen = a hard-gate regression; n=400 itself regressed 80→89 ms) — the AED per-iteration overhead doesn't amortize for small blocks even with the BLAS-3 train, so the train's payoff is strictly the large-N regime. n≤200 stays on pure-dlahqr (beats Eigen 1.21×). **v3d-1c-4 CLOSED — the multishift train is the live AED sweep, hard-gate maintained + improved at scale.** | ~700 actual | 31 / (suite) | — |
| ↳ **v3d-2** 🔄 **(v3d-2a ✅ 2026-05-24; v3d-2b ✅ 2026-05-24; v3d-2c next) — non-sym eigenvectors (3 sub-subslices ↓).** | Eigenvectors via Schur back-substitution (`dtrevc`) + **3-stage back-transform** + complex eigenvector assembly + complex non-sym Schur. Result API: `EigNonsym<T>` = `Vector<Complex<RealType<T>>> values` + `Matrix<Complex<RealType<T>>> vectors` (column k = eigenvector for values[k]; real matrices yield conjugate-pair complex eigenpairs). Built on the shipped real-Schur pipeline (balance/hessenberg/`schur_aed`). **Gate: vs Eigen `EigenSolver` (A·v=λ·v residual + eigenvalue match) + LAPACK `dgeev`/`zgeev`.** | ~1200 | ~35 | — |
| ↳ **v3d-2a** ✅ 2026-05-24 — **eigenvectors of T (the core)** | **`dlaln2` + `dtrevc` SHIPPED.** `dlaln2` (faithful LAPACK port: 1×1/2×2 `(ca·A−w·D)·X=scale·B` real+complex, Gaussian elimination w/ complete pivoting via IPIVOT/RSWAP/ZSWAP on the column-major flat crv/civ, overflow scaling + smin floor, local Smith-robust `cdiv` for the complex divides). `dtrevc_right` (SIDE='R', HOWMNY='A', not back-transformed: column back-substitution over the quasi-triangular T, 1×1/2×2-block dispatch via `dlaln2`, real eigenvalue→real column, complex 2×2→packed re/im columns, ‖·‖∞ normalization). Exposed via `detail::lin_solve_2x2` + `detail::schur_right_eigvecs`. **Gates MET: `dlaln2` `(ca·op(A)−w·D)·X=scale·B` residual <1e-11 over 200 trials × na∈{1,2}×nw∈{1,2}×ltrans (4800 assertions); `dtrevc` `T·vₖ=λₖ·vₖ` rel-residual <1e-9 for EVERY eigenpair (real + complex, complex path asserted exercised) of random real Schur forms n=8/20/50.** 4-config DoD green (debug full 353 368 assertions / shipping / asan / tidy). | ~600 actual | 2 / 4809 | — |
| ↳ **v3d-2b** ✅ 2026-05-24 — **back-transform + public real `eig` — BEATS Eigen + LAPACK at every N** | **3-stage back-transform + the consumer API SHIPPED.** `EigNonsym<T>` (`Vector<Complex<RealType<T>>> values` + `Matrix<Complex<RealType<T>>> vectors`, Schur order). Public **`eig(Matrix<T>)`**: clone → `balance` → `hessenberg` + `form_hessenberg_q` → `schur_aed` (multishift-train AED: A_bal=(Q·Z)·T·(Q·Z)ᵀ) → `dtrevc` (`schur_right_eigvecs`) → back-transform `V = D⁻¹·P · Q · Z · V_schur`: (i) `Z·V_schur` + (ii) `Q·` via two gemms, (iii) **`gebak_right`** (faithful `dgebak` SIDE=R/JOB=B: row scale over [ilo,ihi] + isolating row permutation at the corners). All 3 stages real-linear ⇒ the `dtrevc` re/im column packing survives; complex pairs assembled + normalized only at the end. **D(non-sym)-4** (eigenvector norm = ‖·‖₂=1 + lowest-index largest-magnitude component phase-rotated real-positive — the LAPACK `dgeev`/Eigen convention; **supersedes the plan-row "max-abs=1" wording**, kept the gate clean). **D(non-sym)-5** (eigenpair order = Schur order from `schur_aed`, deterministic; complex spectra have no natural total order). CLI `hesap.dense.eig.nonsym.{f32,f64}` (interleaved [re,im] eigenvalues). **Gate MET decisively — residual `‖A·vₖ−λₖ·vₖ‖∞/‖vₖ‖∞` ~1e-13 (≪1e-9) for every eigenpair, eigenvalues match Eigen ~1e-13; the `dgebak` permutation + scaling branches exercised by corner-isolated + `A=D·B·D⁻¹` fixtures.** **Bench (`bench_hesap_eig_nonsym_vs_reference`, win-vs-ref, i9-14900K AVX2, f64, full eig values+vectors): BEATS Eigen `EigenSolver` at EVERY N — 1.09× (n=100), 1.08× (n=200), 1.65× (n=400) — AND crushes LAPACK `dgeev` 2.03×/3.04×/1.57×.** **Perf lever found by stage-timing: `form_hessenberg_q` (dorghr) was a scalar column-strided reflector apply = 3.35ms @ n=200 (the entire non-Schur cost, and the only thing losing to Eigen); rewrote the apply ROW-WISE (contiguous `simd_axpy`, same per-element accumulation order over r) → 0.58ms (5.8×), flipping n=100/200 from loss→win (the v3d-1a SIMD-row-wise lever, [[feedback_simd_rowwise_unblocked_beats_blocked_smallk]]). dtrevc (0.47ms) + the two gemms (0.57ms) were already cheap.** Full `[nonsym]` 247 624 assertions / 26 cases + 4 new `eig:` cases; 4-config DoD green (debug/shipping-LTCG/asan each 247 624 / tidy clean). (No open follow-on: we beat Eigen + LAPACK at every measured N. Eigen's fused dhseqr-Z-accumulation — one combined Q·Z back-transform vs our two gemms — would only widen the win further, but is NOT pursued: no loss to fix, and the two gemms are 0.57ms = negligible vs the schur cost.) | ~450 actual | 4 / (suite) | — |
| ↳ **v3d-2c** 🔄 (2c-1 ✅ + 2c-2 ✅ 2026-05-24; **2c-2b next — see § "v3d-2c-2b — complex AED — DETAILED PLAN" below**; then 2c-3) — **complex non-sym Schur + complex `eig`** | **Complex `Matrix<Complex<T>>` input path (its own sub-slice — a full complex Schur, NOT a trivial fold).** Subdivision (advisor-vetted): **2c-1** complex Hessenberg (`zgehd2`) + unitary Q (`zunghr`); **2c-2** complex `zgebal` + single-shift `zlahqr` Schur (`ar`/`ai`→`Matrix<Complex>` converted once at entry, Schur onward uses `Complex<T>` arithmetic); **2c-2b** complex AED (`zlaqr`-class — EXPECTED, LAPACK `zhseqr` uses AED at n≥75, single-shift loses at scale); **2c-3** complex `ztrevc` + back-transform + public complex `eig` + CLI `eig.nonsym.{c32,c64}`. **Gate: vs Eigen complex `EigenSolver` + LAPACK `zgeev`; A=Q·T·Qᴴ recon.** Comparable to the whole real v3d-1 chain in complex arithmetic — multi-session. | ~800 | ~15 | — |
| ↳ **v3d-2c-1** ✅ 2026-05-24 — **complex Hessenberg + unitary Q — BEATS Eigen + LAPACK (n≤128 measured regime)** | **`zgehd2` + `zunghr` on the two-real-array (`ar`/`ai`) SIMD path SHIPPED.** Unified `hessenberg<T>` / `form_hessenberg_q<T>` now dispatch real-vs-complex via `if constexpr`; the complex branch splits `Matrix<Complex<R>>` to `(ar, ai)` once (O(n²), ADR-0078 §5 lower layer), runs the SIMD reduction, recombines. Faithful **`make_householder_complex`** (`zlarfg`, real beta) promoted to the shared `detail/householder.hpp` (**deduped** `eig_herm`'s inline copy). Two-sided update faithful to zgehd2 order (RIGHT `A·H` then LEFT `Hᴴ·A`). **New fused complex SIMD substrate `detail/dot_simd_complex.hpp`** (`simd_cdot_nc`/`simd_caxpy`/`simd_caxpy_conjx`) — bit-identical to the 4-separate-pass form but reads each operand row ONCE; **8-wide (2× Vec4d, 8 FMA accumulators) for FMA-port ILP** — the 4-wide first cut REGRESSED (latency-bound), 8-wide flipped it to a win (measure-don't-guess). Reused by 2c-2's `zlahqr`. **Gate MET: `A=Q·H·Qᴴ` recon <1e-12 (c64 n=6/32/64/160/256), Q unitary, H upper-Hessenberg w/ real subdiagonal; c32 n=24 <5e-4.** **Bench (`bench_hesap_eig_nonsym_vs_reference`, c64): BEATS Eigen `HessenbergDecomposition<MatrixXcd>` 1.05× (n=64) / 1.21× (n=128) AND crushes LAPACK `zgehrd` 2.21×/11.91×.** **Bench-harness crash solved + root-caused (marker isolation): Eigen's complex `HessenbergDecomposition::compute` ACCESS-VIOLATES at n≥256 — the COMPLEX path only (real-double path fine); root cause not pinpointed, treated as reference fragility like OpenBLAS-generic `zgehrd` (fragile at n>128). A REFERENCE fault, NOT Cerid (recon-clean to n=512, ASan-clean to n=256); refs capped at 128, Cerid timed alone at n=256.** **Honest large-n characterization (advisor-pushed, measured not assumed): the unblocked complex reduction has a real cache cliff at n=512 — 5.5ms@256 → 106ms@512 (~19× for 2× n) because the complex working set ar+ai=4MB exceeds the 2MB L2 → memory-bound. This is the SAME unblocked-vs-blocked tradeoff the REAL v3d-1a path accepted (it dropped blocked dlahr2+gemm as 0.2× Eigen at tested sizes + ships unblocked). A blocked zlahr2+gemm reduction is the large-n lever — NOT pursued now (no complex-512 consumer yet, no reference there since Eigen AVs; a unified blocked real+complex reduction would be its own slice). Not debt: a measured memory-hierarchy characteristic, not a kernel defect (recon-clean to 512).** 4-config DoD green (debug/shipping-LTCG/asan each 353 406 assertions / 308 cases incl. the eig_herm dedup regression / tidy clean). | ~450 actual | (suite) | — |
| ↳ **v3d-2c-2** ✅ 2026-05-24 — **complex balance (zgebal) + single-shift Schur (zlahqr) — BEATS Eigen + LAPACK (single-shift, n≤128 measured regime; AED is 2c-2b)** | **`zgebal` + `zlahqr` SHIPPED.** **`balance<T>` unified real+complex** via `if constexpr` + `RealType<T>` scalars + `bal_abs`/`bal_nsq` helpers; **`scale` is `Array<RealType<T>>`** (real — advisor call, avoids a Complex-multiply in the 2c-3 `gebak` hot loop). **`complex_schur` (`zlahqr`)**: single Wilkinson-shift implicit QR on a complex upper-Hessenberg → UPPER-TRIANGULAR `T` (eigenvalues on the diagonal, **NO 2×2 blocks / no `dlanv2`** — the structural simplification over real) + unitary `Z` + eigenvalues; complex-Givens bulge-chase, Ahues-Tisseur deflation, **faithful zlahqr Wilkinson shift** (`U=√h(i-1,i)·√h(i,i-1)`, scaled `Y=S·√((X/S)²+(U/S)²)`, `T=h(i,i)−U²/(X+Y)`) + exceptional shifts at its 10/20 (**D(non-sym)-6** — `dat1=0.75`, `s=dat1·|Re(subdiag)|`; the algorithmic shape is faithful to LAPACK `zlahqr` but the constants were reasoned-from-memory + pinned by the recon gate, NOT yet checked character-for-character against `zlahqr.f` — confirm at v3d close / §17 lock). New primitives: **complex `sqrt`** (`complex.hpp`, stable closed form) + **`complex_givens`** (`zlartg`, `detail/householder.hpp`, overflow-safe via `hypot2`; reused by 2c-3 `ztrevc`). `Complex<T>` arithmetic (not split — the bulge chase is small per step; the `ar`/`ai`→`Matrix<Complex>` boundary ends at 2c-2 entry). **Gate MET: `H=Z·T·Zᴴ` recon <1e-8 (c64 n=8/20/50/128, ~1e-13), Z unitary, T upper-triangular, eigenvalues=diag(T); c32 n=24 <1e-3; balance isolation+trace-invariance.** **Bench (c64): BEATS Eigen `ComplexSchur` 1.16× (n=64) / 1.10× (n=128) AND crushes LAPACK `zhseqr` 6.58×/1.45×; recon ~1e-13.** (Eigen `ComplexSchur` ALSO access-violates at n≥256 — confirmed the [[reference_eigen_complex_hessenberg_av_at_large_n]] prediction; refs capped at 128, Cerid timed alone at n=256 = 117ms, ~9.2× of n=128 = mildly super-8×. Attribution: the single-shift QR **sweep count grows super-linearly with n WITHOUT AED** — that is exactly what 2c-2b AED fixes and why LAPACK has it; algorithmic, not a cache cliff.) 4-config DoD green (debug/shipping-LTCG/asan each **353 435 assertions / 311 cases** / tidy clean). | ~550 actual | (suite) | — |
| ↳ **v3e** | **CLI audit + close.** CLI-completeness audit (every op a command) + vs-reference rollup (Eigen + LAPACK) + **ADR-0065 §17** lock (D(dense-eig) determinism pins) + 18-config sweep. | ~200 | — | — |
| **v4** | **Iterative solvers + preconditioners + AMG — the COMPLETE family (never-defer; `feedback_hesap_substrate_never_defer_features`).** Krylov solvers (CG / PCG / **FGMRES** / BiCGSTAB / MINRES / SYMMLQ / LSQR / LSMR / QMR / IDR(s) / **GCRO-DR + M-CG/M-GMRES Krylov subspace recycling**) + **block-Krylov / multi-RHS** (block-CG / block-GMRES / block-BiCGSTAB) + **inner-Krylov-as-preconditioner** (nested/flexible) ; preconditioners (Jacobi / block-Jacobi / SSOR / IC(0) / ILU(0) / **ILU(p) / ILUT-threshold** / **SPAI** / polynomial-Chebyshev / additive + restricted Schwarz / **ILUPACK multilevel ILU**) ; **AMG** (classical Ruge-Stüben + **SA-AMG Vaněk 1996** + **AGMG Notay 2010** + bootstrap AMG ; V/W/F-cycles ; AMG-as-solver AND AMG-as-preconditioner) ; all over `LinearOp` (matrix-free) ; complex variants ; **full bit-determinism across thread counts** ; CLI registration. **4 new modules: `crd-hesap-resources` (matrix corpus) / `crd-hesap-iterative` / `crd-hesap-preconditioners` / `crd-hesap-amg`.** | ~9000 | ~320 | multi-month (elite bar; honest) |
| ↳ **v4-corpus** ✅ 2026-05-25 — **matrices as cooked resources; cooked binary loads 6–7× faster than re-parsing `.mtx`** | **SuiteSparse matrices as first-class cooked resources (runs BEFORE v4a; user directive 2026-05-25).** SHIPPED: `crd-hesap-resources` bridge module + `'HMTX'` CRDR (MXHD/MXOP/MXII/MXVL) + 40-byte pinned `MatrixFileInfo` + append-only `MatrixVariant` + single loader (variant-in-header) + `SparseMatrixResource`/`build_csr<T>` + in-memory cooker (`cook_sparse_matrix`/`cook_matrix_market`) + `read_matrix_resource` + `register_hesap_matrix_loader` + 9 CLI (`hesap.matrix.{info,cook.<T>,load.<T>}`) + `smoke_hesap_matrix_resource` (real mount + `load_sync`) + gated `bench_hesap_matrix_resource_vs_reference` (real SuiteSparse, 6/6 PASS, RM=OK, cooked load 6–7× faster than `.mtx` parse). **ADR-0084.** Tests 10 cases / 108 assertions. Local DoD green: win-debug ctest 12/12 (incl guards) + tidy + clang-cl + asan + shipping-LTCG; gcc/full-sweep → CI. Original plan ↓ | New **`crd-hesap-resources`** bridge module (depends `crd-resources` + `crd-hesap-sparse` — one-way, neither depends on it). `'HMTX'` FourCC + CRDR chunks (header: rows/cols/nnz/format/type-variant + CSR outer/inner/values chunks) + cooker (`.mtx` text → binary CSR via the v1g `read_matrix_market` reader, run at cook time — honours authoring-text/runtime-binary) + runtime `ILoader` (`SparseMatrixResource` payload) + `register_hesap_matrix_loader(ResourceManager*)` + fetch-and-cook CLI (`hesap.matrix.fetch`/`cook`/`load`/`info`). Reuses the gated `file(DOWNLOAD)` corpus. **New ADR-0084.** Then v4a/benches consume the SuiteSparse corpus through `ResourceManager::load_sync` (eviction/hot-reload/replay for free). | ~600 | ~20 | — |
| ↳ **v4a-1** ✅ 2026-05-25 — **framework + CG/PCG + Jacobi, end-to-end** | **SHIPPED.** Two modules created: `crd-hesap-iterative` (`IterativeResult<R>`/`IterativeOptions<R>` + `KrylovWorkspace<T>` + `stopping` + **CG + PCG**, generic over real+complex via `inner<T>`=`dotc`/`dot`; HPD contract + determinism documented) + `crd-hesap-preconditioners` (**JacobiPreconditioner** as `LinearOp` exposing M⁻¹). 12 CLI (`hesap.iterative.{cg,pcg}` + `hesap.precond.jacobi` ×4). **`smoke_hesap_solve_cli`** = the v4a thesis: matrix loaded via `ResourceManager::load_sync<SparseMatrixResource>` → `build_csr` → CG, converges (n=64 Laplacian, 32 iters, rel resid 0). Determinism: bit-exact repeated-run test (serial; cross-thread gate is v4a-2). Tests 7 cases / 82 assertions. Local DoD green: win-debug ctest (incl guards) + tidy + clang-cl + asan + shipping-LTCG; gcc/sweep→CI. Original v4a plan ↓ |
| ↳ **v4a-2** ✅ 2026-05-25 — **block-Jacobi + SSOR + determinism moat + CRUSHES Eigen 1.49–1.86×** | **SHIPPED.** `BlockJacobiPreconditioner` (per-block explicit inverse via a self-contained complex-capable Gauss-Jordan w/ cabs1 pivot — a dedicated small-block kernel, cf. D(sparse)-6, avoids the deferred complex dense-LU) + `SsorPreconditioner` (sequential fwd/back sweeps, ω∈(0,2), Hermitian-PD; mutable scratch sized once) + **`ParallelSparseLinearOp`** (crd-hesap-sparse; size-adaptive: serial SELL below ~L2 working set, parallel SELL-C-σ above — the perf path AND the determinism-moat operator) + 8 CLI (`precond.{block_jacobi,ssor}` ×4; `pcg` extended with `precond`/`block_size`/`omega` selector). **DETERMINISM MOAT GATED:** CG over parallel spmv == serial spmv **bit-exact** {iterations, residual history, solution} (`is_parallel()` forced). **Eigen CRUSH (`bench_hesap_cg_vs_reference`, gated, real SuiteSparse SPD, Jacobi-PCG vs Eigen CG+DiagonalPreconditioner, same iters): bcsstk13 1.81× / bcsstk24 1.49× / bcsstk25 1.86× — WIN on all** (the size-adaptive op was the lever: forcing parallel on sub-cache matrices lost 0.67×; serial-SELL-small + parallel-large wins everywhere, plus determinism Eigen lacks). Tests 10 cases / 390 assertions. Local DoD green: win-debug ctest (incl guards) + tidy + clang-cl + asan + shipping-LTCG (fixed a release-only C4189 on an assert-only `ok`); gcc/sweep→CI. **🎉 v4a CLOSED.** |
| ↳ **v4a (orig plan)** | **Krylov framework + CG/PCG + Jacobi/block-Jacobi/SSOR.** `LinearOp` consumer + `IterativeResult{iterations, final_residual_norm, converged, StopReason}` + `IterativeOptions{rel_tol, abs_tol, max_iter, record_residuals}` + deterministic `stopping` criteria + **`KrylovWorkspace<T>`** (pre-allocated vector bank, allocator-fed, owns the `blas1`-routed KBN-pairwise `kdot`/`knrm2` — the determinism moat) + CG + PCG; **two modules created here:** `crd-hesap-iterative` (CG/PCG + framework) AND `crd-hesap-preconditioners` (Jacobi / block-Jacobi / SSOR — each a `LinearOp` exposing `M⁻¹`). **Establishes the architecture + SPD path + the FIRST determinism gate (bit-exact iters+vector across {1,2,4,8,16} threads) + first Eigen head-to-head (CG+DiagonalPrecond).** Real f32/f64 + Hermitian-complex (CG/PCG complex = HPD precondition). ~20 CLI. | ~750 | ~28 | — |
| ↳ **v4b** ✅ 2026-05-25 — **FGMRES(m): crushes Eigen GMRES 2.47× + BiCGSTAB 1.84× throughput** | **SHIPPED.** `gmres.hpp`: `GmresWorkspace<T>` (flat V/Z basis buffers + host Hessenberg/Givens/RHS) + **`fgmres`** (flexible from the start — per-iteration-varying M, solution built from preconditioned Z) + `gmres` (plain restarted). Modified Gram-Schmidt via deterministic `blas1`; self-contained real+complex Givens (`c:R`/`s:T`, mirrors zlartg, no cross-module reach); restart(m); back-substitution. Reuses `detail::krylov_inner`. 4 CLI (`hesap.iterative.fgmres.{f32,f64,c32,c64}` with `precond none|jacobi|block_jacobi|ssor` + `restart` selector). **Determinism moat GATED** (GMRES over parallel spmv == serial, bit-exact). Tests: nonsym convergence, **restart-invariance**, **per-iteration VARYING-M flexibility** (the real FGMRES test, not fixed-M), complex nonsym, determinism — 5 cases. **Eigen crush (`bench_hesap_gmres_vs_reference`, gated, nonsym SuiteSparse): gemat11 FGMRES 1.84× vs Eigen BiCGSTAB + 2.47× vs Eigen GMRES (all GMRES-class stagnate → throughput win; Eigen GMRES `(fail)`).** sherman3: restarted GMRES(30) stagnates where BiCGSTAB converges — a textbook GMRES(m) limitation → v4c BiCGSTAB (NOT a kernel defect; same-algorithm comparison is the crush). Side-fixes: complex `operator/` C4723 Release false-positive (function-local pragma), JacobiPreconditioner graceful zero-diagonal fallback (more robust than Eigen). Full suite **17 cases / 755 assertions**. Local DoD green: win-debug ctest + tidy + clang-cl + asan + shipping-LTCG; gcc/sweep→CI. | ~520 actual | 5 / (suite) | — |
| ↳ **v4c-1** ✅ 2026-05-25 — **BiCGSTAB: APPLES-TO-APPLES crush of Eigen BiCGSTAB 1.66–1.86×** | **SHIPPED.** `bicgstab.hpp`: `BicgstabWorkspace<T>` (8 n-vectors) + preconditioned `bicgstab<T>` (van der Vorst), short-recurrence nonsym. **dotc throughout** (matches Eigen `.adjoint()*` + PETSc; single inner-product policy); r̂₀=r₀; **LAPACK-style `smlnum` breakdown thresholds** (ρ, ⟨r̂₀,v⟩, ⟨t,t⟩, ω) + lucky-breakdown early-out — and **CG's exact-zero breakdown hardened to the same threshold** (advisor: exact-zero won't catch near-breakdown at scale). 4 CLI (`hesap.iterative.bicgstab.{f32,f64,c32,c64}` + precond selector). Determinism moat GATED (BiCGSTAB parallel-vs-serial bit-exact). Tests: nonsym, Jacobi-precond, complex nonsym, determinism — 4 cases. **Eigen crush (apples-to-apples, same algorithm, real nonsym SuiteSparse): gemat11 1.86× (both cap → throughput) + sherman3 1.66× (Cerid 454 it conv vs Eigen 579 it conv — FEWER iters AND faster).** Solves sherman3 where v4b restarted-GMRES(30) stagnated, exactly as predicted. Full suite **21 cases / 1014 assertions**; 5-config local DoD green. | ~280 actual | 4 / (suite) | — |
| ↳ **v4c-2a** ✅ 2026-05-25 — **MINRES (unpreconditioned): crushes Eigen MINRES 1.57–2.06×; solves indefinite where CG diverges** | **SHIPPED.** `minres.hpp`: `MinresWorkspace<T>` (6 vectors) + `minres<T>` (Paige-Saunders: symmetric Lanczos 3-term recurrence + incremental Givens QR + short w-recurrence solution update — O(1) storage). **Hermitian contract**: Lanczos tridiagonal is REAL symmetric (α=`krylov_real`(vᴴAv) signed, β=‖·‖) so Givens/QR is real-scalar even for complex A; only vectors are `T`. `smlnum` lucky-breakdown. 4 CLI (`hesap.iterative.minres.{f32,f64,c32,c64}`). Determinism moat GATED. Tests: SPD, **symmetric INDEFINITE where CG diverges**, **Hermitian indefinite (c64)**, determinism — 4 cases. **Eigen crush (apples-to-apples, both unpreconditioned MINRES, real SPD SuiteSparse, both cap → throughput): bcsstk13 1.97× / bcsstk24 1.57× / bcsstk25 2.06×.** Suite **25 cases / 1354 assertions**; 5-config local DoD green. | ~190 actual | 4 / (suite) | — |
| ↳ **v4c-2a-precond** ✅ 2026-05-25 — **preconditioned MINRES: FIXES the ill-conditioned gap, crushes Eigen Jacobi-MINRES 1.47–1.85×** | **SHIPPED.** `minres<T>` unified to take an optional SPD/HPD `m_inv` via the **M-inner-product Lanczos** (tracks residual-space v + preconditioned z=M⁻¹v, applies M⁻¹/iter, β=√Re⟨p,M⁻¹p⟩, solution built from z). **No-preconditioner path bit-identical** (z aliases v, β=nrm2). MINRES CLI moved to the preconditioners module with the `precond none|jacobi|block_jacobi|ssor` selector (SPD-only). +1 test (Jacobi-MINRES SPD). **Ill-conditioned FIXED: bcsstk13 now CONVERGES (Cerid 1380 it / Eigen 1378 — match), 1.85× faster** (was capping unpreconditioned); bcsstk24 1.47× / bcsstk25 1.66× (both cap → throughput). 26 cases / 1356 assertions; 5-config DoD green. | ~90 actual | 1 / (suite) | — |
| ↳ **v4c-2b** ✅ 2026-05-25 — **SYMMLQ (Paige-Saunders), symmetric-indefinite** | **SHIPPED. 🎉 v4c CLOSED.** `symmlq.hpp`: `SymmlqWorkspace<T>` (4 vectors) + `symmlq<T>` — same symmetric Lanczos, LQ factorization (recurrence transcribed VERBATIM from Krylov.jl to avoid sign errors; validated by final ‖Ax−b‖ since the residual is non-monotone). Hermitian contract = real LQ scalars. Stop on SYMMLQ's internal residual estimate `√(γ²ζ²+ε_old²ζ_old²)`. 4 CLI (`hesap.iterative.symmlq.{f32,f64,c32,c64}`, iterative module). Determinism moat GATED. Tests: SPD, **symmetric INDEFINITE**, **Hermitian indefinite c64**, determinism. **Eigen ships no SYMMLQ → breadth win + determinism.** **PRECONDITIONED + unpreconditioned** (optional SPD/HPD M via the M-inner-product Lanczos — same pattern as preconditioned MINRES; no-precond path bit-identical; NO follow-on). CLI in the preconditioners module with the `precond none|jacobi|block_jacobi|ssor` selector. **Consistency fix this slice:** consolidated `gmres_mag`→shared `detail::krylov_mag`. Full suite **31 cases / 1699 assertions**; 5-config DoD green. | ~250 actual | 5 / (suite) | — |
| ↳ **v4d-1** ✅ 2026-05-25 — **LSQR + LSMR (least-squares); Eigen ships neither (breadth + determinism)** | **LSQR + LSMR SHIPPED** (core + preconditioned, see v4d-1-precond row below). `lsqr.hpp` (Paige-Saunders, Golub-Kahan bidiag + rotations; two stopping tests: consistent ‖r‖ + least-squares ‖Aᴴr‖) + `lsmr.hpp` (Fong-Saunders, ‖Aᴴr‖-monotone; recurrences transcribed verbatim from Krylov.jl, validated by ‖Aᴴ(Ax−b)‖≈0). **New `ParallelSpmvLeastSquaresOp` in crd-hesap-sparse** — rectangular (m×n), parallel SELL for BOTH A·x and Aᴴ·y (stores A + Aᴴ as two SELLs; adjoint = single inner-product policy; size-adaptive; ~2× storage deliberate) — the perf path + determinism-gate operator for LSQR/LSMR/QMR. 8 CLI (`hesap.iterative.{lsqr,lsmr}.{f32,f64,c32,c64}`, rectangular). Tests: overdetermined LS + consistent square + complex LS + determinism (each ×2). **Eigen's LeastSquaresConjugateGradient IS the normal-equations CG LSQR avoids (squares cond) → no fake crush; breadth + determinism + correctness-where-normal-equations-diverge.** Iterative suite 38 cases / 2876 assertions; 5-config DoD green (fixed a release-only C4189). | ~340 actual | 8 / (suite) | — |
| ↳ **v4d-1-precond** ✅ 2026-05-25 — **preconditioned LSQR + LSMR; column-Jacobi closes the ill-scaled gap** | **SHIPPED. 🎉 v4d-1 CLOSED.** `lsqr.hpp` + `lsmr.hpp` unified to take an optional N-space column preconditioner via the **M-inner-product Golub-Kahan bidiagonalization** (residual-space `nv = Aᴴu`, solution-space `v = N⁻¹nv`, α = √Re⟨v,nv⟩; solution built directly from the N-preconditioned v — no untransform). **No-preconditioner path bit-identical** (`nv` aliases `v`, α = nrm2; existing overdetermined/complex/consistent/determinism tests unchanged-green). Plain convenience overloads `{lsqr,lsmr}(a,b,x,…)` → `(a,nullptr,…)`. New `LeastSquaresColumnJacobi<T>` in crd-hesap-preconditioners (M = diag(AᴴA)⁻¹, squared column norms in one CSR pass; real-positive diagonal ⇒ apply==transpose==adjoint, parallel-trivial + bit-deterministic; graceful empty-column→1). **Selector deliberately `{none, jacobi}` only** — square Jacobi / block-Jacobi / SSOR need a square operator a least-squares A lacks (NOT a defer: no consumer applies SSOR to a normal operator). **LSQR/LSMR CLI MOVED iterative→preconditioners** (consistency: now carries `precond none|jacobi`, matching minres/symmlq; names unchanged `hesap.iterative.{lsqr,lsmr}.*`). Tests (+3): column-Jacobi LSQR + LSMR converge on a 1e12-cond column-scaled problem where plain stalls in the same budget (‖Aᴴr‖→0); **determinism moat holds under preconditioning** (column-Jacobi LSQR serial-vs-parallel spmv bit-exact). Iterative suite **41 cases / 3469 assertions**; win-debug ctest+guards + clang-cl `-Werror` green; gcc/full-sweep→CI. | ~210 actual | 3 / (suite) | — |
| ↳ **v4d-2a** ✅ 2026-05-25 — **QMR (Freund-Nachtigal, two-sided bi-Lanczos); + completed block-Jacobi/SSOR adjoints** | **SHIPPED.** `qmr.hpp`: `QmrWorkspace<T>` (11 vectors) + `qmr<T>` — coupled two-term bi-Lanczos + `sym_givens` rotations, transcribed VERBATIM from Krylov.jl `qmr.jl`/`sym_givens` (uses Aᴴ + conjugated dots, NOT the classic Aᵀ bilinear form → maps onto `apply_adjoint` + the module dotc policy). Consumes a square op with apply + apply_adjoint (`ParallelSpmvLeastSquaresOp`, m==n). **Optional RIGHT preconditioner N=m_inv** (verbatim Krylov.jl right path: `Nvₖ=N⁻¹vₖ` v-side, `p=N⁻ᴴs` u-side, post-loop untransform `x←N⁻¹x`); right keeps the quasi-residual τ in the TRUE-residual norm (matches BiCGSTAB). Two-sided ⇒ the preconditioner needs apply_adjoint (asserted); unpreconditioned bit-identical (Nvₖ/p alias). Reported rNorm=|ζ̄|·√τ is the QUASI-residual → tests verify the TRUE ‖b−Ax‖ separately. New shared `detail::GivensRot<T>` hoisted to cg.hpp (deduped gmres's copy). `detail::krylov_conj<T>` added (identity for real). **NO-DEFER completion: block-Jacobi + SSOR gained `apply_adjoint`** (block-Jacobi = Bᴴ-per-block conj-transpose for general A; SSOR = apply, since M is Hermitian for the documented Hermitian-A precondition) so QMR's CLI carries the full `none|jacobi|block_jacobi|ssor` selector. 4 CLI (`hesap.iterative.qmr.{f32,f64,c32,c64}`, preconditioners module). **Eigen ships no QMR → breadth + determinism (no fabricated crush).** Determinism moat GATED (serial-vs-parallel spmv bit-exact). Tests (+8): nonsym + Jacobi-precond + **seeded-RANDOM nonsym** (the smooth-spectra trap) + complex nonsym + block-Jacobi (real+complex) + SSOR-on-symmetric + **direct adjoint-identity ⟨y,Mx⟩==⟨Mᴴy,x⟩ for jacobi/block-jacobi/ssor (c64)** + determinism. Iterative suite **49 cases / 3805 assertions**; win-debug ctest+guards + clang-cl `-Werror` green; gcc/full-sweep→CI. | ~330 actual | 8 / (suite) | — |
| ↳ **v4d-2b** ✅ 2026-05-25 — **IDR(s): faster AND far more robust than Eigen IDRS (converges where Eigen diverges). 🎉 v4d CLOSED** | **SHIPPED.** `idrs.hpp`: `IdrsWorkspace<T>` (3s+4 vectors + s·s M + f/c) + `idrs<T>` — Sonneveld-van Gijzen Induced Dimension Reduction, short-recurrence A-only (no Aᴴ) with an s-dim shadow space, transcribed VERBATIM from IterativeSolvers.jl `idrs.jl`. **Shadow space P generated deterministically (seeded LCG) + MGS-ORTHONORMALIZED in the workspace ctor** (van Gijzen/PETSc/MATLAB all orthonormalize; skipping weakens convergence + loses the crush) → P owned by workspace, fixed ⇒ two same-seed solves bit-identical. Optional LEFT preconditioner Pl=m_inv (all preconditioners qualify; only apply needed). omega step uses the van Gijzen ρ-safeguard (angle=√2/2; complex ρ=|⟨t,s⟩|/(‖t‖‖s‖) via krylov_mag). `M[k,k]`-near-zero breakdown guard (shadow rank-loss). Standalone `detail::idrs_ltri_solve` (the trailing lower-tri block solve, the off-by-one risk) UNIT-TESTED separately (advisor). 4 CLI (`hesap.iterative.idrs.{f32,f64,c32,c64}` + `s` + precond selector). Determinism moat GATED (serial-vs-parallel spmv bit-exact, two same-seed workspaces). Tests (+6): triangular-solve unit + nonsym + **seeded-random s∈{1,2,8}** + Jacobi-precond + complex + determinism. **APPLES-TO-APPLES vs Eigen IDRS (`bench_hesap_idrs_vs_reference`, gated, s=4 both sides, TRUE residual recomputed BOTH sides per `feedback_iterative_crush_claim_same_algorithm`): Cerid 7–35× faster per iteration budget AND dramatically more robust — on every real matrix Eigen's residual EXPLODES (1e+6…1e+152) while Cerid stays bounded; sherman3+Jacobi Cerid CONVERGES (596 it, r=7.5e-9) where Eigen DIVERGES (r=4.3e+18, 35.4×). gemat11/bcsstk = both STALL at cap=4000 (stiff/hard; honestly labeled, no fake crush — but Cerid diverges far less, and bcsstk13+Jacobi actually CONVERGES r≈1.6e-6 at ~4823 it with a larger budget while Eigen still diverges).** Iterative suite **55 cases / 4141 assertions**; win-debug ctest+guards + clang-cl `-Werror` + gated bench (vs-ref build) green; gcc/full-sweep→CI. **🎉 v4d CLOSED — LSQR/LSMR (un/precond) + QMR + IDR(s), the full short-recurrence + least-squares + two-sided Krylov family.** | ~360 actual | 6 / (suite) | — |
| ↳ **v4e** (split → v4e-1/2/3) | **GCRO-DR + recycling Krylov.** Deflation-subspace reuse across a solve SEQUENCE (the algorithmic frontier; frontier mostly lacks it — Eigen has NO recycling). Gate = iters-saved across a parametric/time-stepping sequence. Real+complex. | ~800 | ~25 | — |
| ↳ **v4e-1** ✅ 2026-05-25 — **GCR(m) (Generalized Conjugate Residual) — the optimal-residual recycle substrate** | **SHIPPED.** `gcr.hpp`: `GcrWorkspace<T>` (C=A·U + U, m·n each + r/p/q/mp) + `gcr<T>` — restarted GCR (Eisenstat-Elman-Schultz): maintains an ORTHONORMAL C=A·U basis of the search space, each step seeds a direction from the residual, MGS-orthogonalizes A·p ⊥ prior C (mirroring the combination on U), then the minimal-residual correction x += ⟨c,r⟩·u, r −= ⟨c,r⟩·c. Math-equivalent to GMRES(m) but stores U+C explicitly = the recycle space GCRO-DR (v4e-2) deflates + v4e-3 reuses. Optional RIGHT preconditioner (seed = N⁻¹r ⇒ true residual). 4 CLI (`hesap.iterative.gcr.{f32,f64,c32,c64}` + restart + precond selector). Determinism moat GATED (all dots KBN dotc; serial-vs-parallel spmv bit-exact — the baseline verified bit-exact BEFORE harmonic-Ritz dense LA enters at v4e-2). Tests (+5): nonsym + **monotone-residual (the optimal-residual property)** + Jacobi-precond + complex + determinism. Substrate slice → no Eigen peer (GCR≈GMRES; the recycling crush lands at v4e-2/3). Iterative suite **60 cases / 4479 assertions**; win-debug ctest+guards + clang-cl `-Werror` green; gcc/full-sweep→CI. | ~210 actual | 5 / (suite) | — |
| ↳ **v4e-2** (split → v4e-2a/2b) | **GCROT(m,k) — recycling GMRES.** Reference SWITCHED from GCRO-DR/harmonic-Ritz to GCROT(m,k) (advisor reconcile): scipy's `gcrotmk` is a verified, faithfully-portable reference (avoids reconstructing GCRO-DR's augmented harmonic-Ritz GEVP from memory — verbatim-reference discipline). | ~350 | ~10 | — |
| ↳ **v4e-2a** ✅ 2026-05-26 — **GCROT(m,k) core (recycling GMRES, 'oldest' truncation) — recycling beats restarted GMRES(m)** | **SHIPPED.** `gcrot.hpp`: `GcrotWorkspace<T>` (C/U recycle pairs + inner V/Z + raw Hessenberg H̄ + B + Givens R) + `gcrot<T>` — faithfully transcribed from scipy `_gcrotmk.py`. Each outer cycle: project residual onto the recycle space (r−=C(Cᴴr), x+=U(Cᴴr)) → inner FGMRES(m) Arnoldi kept ⊥ C (the C-projection coeffs form B=CᴴAV; inner Givens least-squares = the shared gmres machinery) → form one combined direction ux=Zy−U(By), cx=V·H̄·y (= A·ux, ⊥ prior C by construction — D-divergence: uses the stored raw H̄, not a fresh matvec), normalize ‖cx‖=1, GCR step x+=⟨cx,r⟩u/r−=⟨cx,r⟩c → 'oldest' truncate to k + append. Optional RIGHT preconditioner (inner Z=N⁻¹V). 4 CLI (`hesap.iterative.gcrot.{f32,f64,c32,c64}` + restart=m + recycle=k + precond selector). Determinism moat GATED (all reductions KBN dotc; serial-vs-parallel spmv bit-exact). Tests (+5): nonsym + **iters-saved gate (GCROT(8,8) < GMRES(8) on a small-eigenvalue-cluster matrix — the textbook deflation win)** + Jacobi-precond + complex + determinism. Eigen has NO recycling → breadth + the iters-saved demonstration (honest; no fabricated wall-time crush). Iterative suite **65 cases / 4795 assertions**; win-debug ctest+guards + clang-cl `-Werror` green; gcc/full-sweep→CI. | ~310 actual | 5 / (suite) | — |
| ↳ **v4e-2b** ✅ 2026-05-26 — **GCROT 'smallest' (SVD-optimal) truncation. 🎉 v4e-2 CLOSED** | **SHIPPED.** Default recycle-truncation switched from 'oldest' to the SVD-optimal 'smallest' (scipy): keep the k−1 dominant LEFT singular vectors of D=((Rsq)ᵀ)⁻¹Bᵀ, formed by combining the old recycle pairs + MGS-reorthonormalizing. **SVD via the Hermitian Gram `eig_sym`(real)/`eig_herm`(complex) of G=DDᴴ** — the dense `svd` is real-only, but the eigenvectors of DDᴴ ARE the left singular vectors, and our deterministic eig is complex-capable ⇒ all 4 types, zero new eig code, determinism preserved. `GcrotTruncate{Oldest,Smallest}` workspace knob (Smallest default). Standalone `detail::gcrot_build_d` (the (R[:-1]ᵀ)⁻¹Bᵀ triangular solve, the off-by-one risk) UNIT-TESTED with hand-computed values. Tests (+2): D-matrix unit + **'smallest' converges in ≤ iters of 'oldest'** on the cluster matrix. Cross-config: the non-ASCII test-name guard caught a `ᵀ`/`⁻¹` test name locally via ctest (the `feedback_per_slice_binary_direct_misses_ctest` lesson — renamed ASCII); clang-cl `-Werror` caught an unused `using R`. Iterative suite **67 cases / 4803 assertions**; win-debug ctest+guards + clang-cl green; gcc/full-sweep→CI. **🎉 v4e-2 CLOSED — GCROT(m,k) recycling GMRES, both truncation strategies.** **Gated wall-time bench `bench_hesap_gcrot_vs_reference` (GCROT(m,k) vs Cerid GMRES(m) baseline + Eigen GMRES(m) frontier ref, Jacobi-precond, true residual recomputed):** GCROT WINS large where recycling deflates stagnating modes — **sherman3 13.45× fewer iters / 10× faster, CONVERGES (r=8.5e-10) where restarted GMRES(30) STALLS (r=0.76)** (Eigen GMRES spurious 1-it false-conv, same fragility as its IDRS); synthetic cluster **2.58× iters / 1.34× time** vs our GMRES + 4.6× vs Eigen GMRES. Honest flip side: on gemat11/bcsstk where NEITHER converges, GCROT is pure overhead (0.19–0.44× time) — the 'smallest' truncation's per-cycle recombination is **O(k²·n)** (k=30) which dominates over many non-converging cycles (bcsstk13 GCROT still reaches r=5.6e-5 vs GMRES r=0.98 — recycling helping, just not to tol in budget). **Perf characterization (no defect): the recycling payoff is iters-saved; the O(k²n) truncation overhead is the price of SVD-optimal selection, amortized when recycling converges fast (sherman3/cluster) or — the real use case — reused across a solve SEQUENCE (v4e-3). Mitigate single-solve overhead with smaller k or 'oldest' truncation.** | ~150 actual | 2 / (suite) | — |
| ↳ **v4e-3** (split → v4e-3a/3b) | **Cross-solve recycling + RMINRES.** | ~450 | ~12 | — |
| ↳ **v4e-3a** ✅ 2026-05-26 — **cross-solve GCROT recycling — 70× fewer iters / 20× faster across a sequence** | **SHIPPED.** New `RecycleSpace<T>` (opaque+persistent C/U pairs, `clear()`/`dimension()`) + `gcrot_recycled` (reuses a persistent recycle space across a solve sequence). **CRITICAL correctness (advisor): C = A·U is rebuilt + re-orthonormalized on every entry** (the stored U is matrix-free but c=A·u is operator-specific), so the SAME RecycleSpace is safe across DIFFERENT operators A_i (parametric/time-stepping). GcrotWorkspace refactored: C/U extracted to RecycleSpace, embeds an `own` recycle for the (unchanged) single-solve `gcrot` API. Tests (+2): **trap test** (fill on A₁, reuse on a different A₂ → A₂ solution correct, proving the rebuild) + **iters-saved sequence gate** (shift sequence, total GCROT iters < 0.7× total GMRES). **Gated `bench_hesap_gcrot_vs_reference` sequence section: 6-solve shift sequence — fresh GMRES(8) 17319 total it/91ms vs GCROT recycled 247 total it/4.6ms = 70× fewer iters, 19.6× faster** (the de Sturler payoff; the single-solve truncation cost is paid once + amortized). **NO-DEBT inline fixes (user directive — implement, don't defer): (1) SSOR true non-Hermitian adjoint** — `M_SSOR(A)ᴴ = M_SSOR(Aᴴ)`, the sweep run on a stored conj-transpose (was `apply` = Hermitian-only); validated by the adjoint-identity test on a NON-Hermitian matrix. **(2) GCROT truncation eig allocation eliminated** — a per-workspace `LinearAllocator` arena (reset each cycle, `FallbackArena` to parent if over-sized) feeds eig_sym/eig_herm ⇒ no per-cycle malloc; **25–40% faster on truncation-heavy runs** (gemat11 1650→1170ms, bcsstk24 1717→1005ms); determinism preserved (arena is deterministic). Iterative suite **69 cases / 4821 assertions**; win-debug ctest+guards + clang-cl `-Werror` green. | ~330 actual | 2 / (suite) | — |
| ↳ **v4e-3b** ✅ 2026-05-26 — **RMINRES (recycling MINRES, symmetric/indefinite). 🎉 v4e-3 + v4e CLOSED** | **SHIPPED.** `rminres.hpp`: `rminres` (single-solve) + `rminres_recycled` (cross-solve) + `RminresWorkspace` (= GcrotWorkspace). Built as DEFLATED-MINRES on the shared recycling core: `detail::gcrot_core` parameterized on a `SymLanczos` flag — the ONLY difference from GCROT is the inner orthogonalization (3-term symmetric Lanczos vs full Arnoldi MGS), reusing the verbatim MINRES Lanczos + the verbatim GCROT recycle machinery (no from-memory reconstruction). For symmetric A with the inner basis kept ⊥ C, the 3-term recurrence (⟨vᵢ,Avⱼ⟩=0, i<j−1) is preserved (derived, not assumed); α=Re⟨vⱼ,Avⱼ⟩ real, symmetric real tridiagonal even for complex Hermitian A. 4 CLI (`hesap.iterative.rminres.{f32,f64,c32,c64}`, no precond — the recycle space is the deflation). Tests (+5): symmetric-INDEFINITE convergence (the CG-diverges regime) + complex Hermitian indefinite + **cross-operator trap** (fill on A₁, reuse on different symmetric A₂ → A₂ correct) + **cross-solve iters-saved sequence** (symmetric shift sequence, recycled total < 0.7× fresh total) + determinism. **NO-DEBT inline fix folded in: zero `hbar` per inner cycle** (a latent staleness in `cx=V·H̄·y` for BOTH gcrot + rminres — the non-tridiagonal/Hessenberg entries were stale across cycles; now correct). Eigen ships no RMINRES → breadth + iters-saved (recycling-literature metric). Iterative suite **74 cases / 5149 assertions**; win-debug ctest+guards + clang-cl `-Werror` green. **🎉 v4e CLOSED — GCR + GCROT(m,k) (both truncations) + cross-solve recycling + RMINRES, the full recycling-Krylov family.** | ~210 actual | 5 / (suite) | — |
| ↳ **v4f** (split → v4f-1/2/3) | **Block-Krylov / multi-RHS + inner-Krylov-as-preconditioner.** Order (advisor): v4f-1 nested-precond (smallest, independent) → v4f-2 block-CG → v4f-3 block-GMRES/BiCGSTAB. New `BlockLinearOp<T>` (NOT a LinearOp vtable extension — the lock is a hard rule). | ~800 | ~30 | — |
| ↳ **v4f-1** ✅ 2026-05-26 — **inner-Krylov-as-preconditioner (nested composition)** | **SHIPPED.** `KrylovPreconditioner<T, InnerApply>` (crd-hesap-preconditioners): a LinearOp whose `apply(r,z)` runs a caller-supplied inner solve to approximate z≈A⁻¹r (zero guess), dropping into FGMRES's flexible-precond hook (v4b) for inner-outer Krylov compositions a flat solver can't express. Templated callable (zero-overhead, no std::function/heap); `make_krylov_preconditioner` deduction helper; the inner lambda captures a PERSISTENT inner workspace. Determinism preserved (inner solve deterministic over the bit-exact spmv); arena contract documented (inner/outer ParallelSparseLinearOp each own their frame-reset). Tests (+4): inner-GMRES accelerates outer FGMRES (strictly fewer outer iters) + inner-CG composes on SPD + complex + determinism (serial-vs-parallel bit-exact). **Gated bench `bench_hesap_nested_vs_reference` (FGMRES+inner-GMRES vs flat GMRES + Eigen GMRES): cluster nested CONVERGES in 10 outer-it vs flat 300 — 30× fewer outer-iters, 1.6× faster, 3.5× vs Eigen GMRES.** Honest: on hard SuiteSparse (gemat11/sherman3/bcsstk) both stall — nesting helps only when the inner approximates A⁻¹ (no fabricated crush; the composition is the deliverable, no Eigen peer). No CLI (a composition primitive, not a stateless op). Iterative suite **78 cases / 5464 assertions**; win-debug ctest+guards + clang-cl `-Werror` green. | ~80 actual | 4 / (suite) | — |
| ↳ **v4f-2** ✅ 2026-05-26 — **block-CG + block-PCG (SPD/HPD multi-RHS), the complete block-CG family** | **SHIPPED.** New `BlockLinearOp<T>` interface + `ParallelSpmmLinearOp<T>` (`apply_block` = one fused `spmm`/`spmm_parallel` for all s RHS, size-adaptive serial/parallel, bit-exact) in crd-hesap-sparse. `block_cg`/`block_pcg` (`block_cg.hpp`): classic O'Leary block-CG, **Galerkin form** (γ=(PᴴAP)⁺PᴴR, δ=−(PᴴAP)⁺(AP)ᴴZ, both vs the same PᴴAP), **breakdown-free via per-step search-block orthonormalization** (packed-MGS with one reorthogonalisation, transposed to column-contiguous for the bit-exact SIMD blas1 dotc/axpy/nrm2 — robust on cond~1e10 where CholeskyQR2's cond² gram overflows; the strided MGS / no-QR variants either stall or run 4.8× slow). Rank deficiency (converged/duplicate RHS) → column zeroed + regularized (M+εI) s×s Cholesky fallback. Block GEMMs are **allocation-free row-streaming** kernels (the K=s tall-skinny product; a packed gemm pays packing overhead it can't amortise — flipped banded s=4 from 0.92→1.18×). Block-PCG folds the preconditioner in where M=I recovers block-CG exactly; **complete preconditioner family** via `JacobiBlockPreconditioner` (native one-pass diagonal) + `BlockPreconditionerAdapter` (wraps ANY `LinearOp<T>` — SSOR/IC/block-Jacobi all work in block mode). 8 CLI (`hesap.iterative.{block_cg,block_pcg}.{f32,f64,c32,c64}`). Tests (+7): SPD s=4 + rank-deficiency (duplicate-column x₀≡x₂) + complex HPD + determinism (serial≡parallel bit-exact) + **ill-conditioned Laplacian cond~2.6e5 (the QR regression guard)** + block-PCG cuts iterations + native-Jacobi≡adapter-point-Jacobi bit-exact. **Gated 3-way bench (block-PCG + per-column PCG vs Eigen CG+Jacobi, all Jacobi-preconditioned): Cerid per-column PCG CRUSHES Eigen EVERYWHERE 1.23–2.43× (parallel SELL spmv); block-PCG additionally beats Eigen on the expensive banded operator (1.12× @s=4, 3–16× fewer A-passes) + converges robustly on ill-conditioned bcsstk where per-column also wins.** Honest characterization (per crush-mandate-bounded-by-importance): on cache-resident A (tridiag, small bcsstk) block-PCG's O(n·s²) dense flops have no DRAM-pass to trade for ⇒ per-column PCG (v4a) is the algorithm-appropriate path and owns that regime; block-PCG's wall-time win scales with operator expense (A>L2 / matrix-free). bcsstk24 cond~1e11 stalls for BOTH Cerid and Eigen at the iter cap (Jacobi-CG limit, not a block defect). Iterative suite **85 cases / 7892 assertions**; win-debug ctest+guards green (clang-cl stale-PCH + win-tidy LLVM-skew on pre-existing files → CI owns the pinned cross-config gate). | ~520 actual | 7 / (suite) | — |
| ↳ **v4f-3** ✅ 2026-05-26 — **block-GMRES + block-BiCGSTAB (nonsymmetric multi-RHS); 🎉 v4f CLOSED** | **SHIPPED.** `block_gmres.hpp` (block-GMRES(m), flexible/preconditioned): block Arnoldi (block-MGS via `block_gram`/`block_gemm_update`, V_{j+1},H_{j+1,j}=`block_qr`) + the block-Hessenberg least-squares by **banded SCALAR Givens** — H_{j+1,j} is the QR R-factor (upper-tri) ⇒ column c=j·s+cc has subdiagonal nonzeros in exactly rows c+1..c+s (s-wide band), triangularized with the verbatim `gmres_givens`/`gmres_rot_apply` over all s RHS columns of G; deflation guard on the back-solve (zero QR diagonal ⇒ direction contributes nothing, else NaN — caught by the determinism test at small restart). `block_bicgstab.hpp` (El Guennouni-Jbilou-Sadok 2003): s×s α/β via the GENERAL `block_lu_solve` (partial-pivot LU — R̃₀ᴴAP is NOT SPD), scalar ω (Frobenius ⟨AS,S⟩/⟨AS,AS⟩), β=(1/ω)M⁻¹(R̃₀ᴴR_new) reducing to scalar at s=1; **divergence guard** (worst_rel>1e10 ⇒ Breakdown — block ω amplifies BiCGSTAB instability; gemat11 hit 1e56 without it). Foundational: `block_qr` generalizes v4f-2's `block_orthonormalize` to return the s×s R (reorth pass ADDS to R[i,j]; **W=Q·R reconstruction test** added) + new GENERAL `block_lu_solve`. 16 CLI (`hesap.iterative.{block_gmres,block_bicgstab}.{f32,f64,c32,c64}` plain + the preconditioned block_pgmres/pbicgstab via API). Tests (+7): block_qr recon, block-GMRES nonsym + **shared-Krylov ratio** (block-iters ≤ Σ per-column GMRES, both full/non-restarted to avoid restart-stagnation) + complex + determinism; block-BiCGSTAB nonsym + per-column peer + complex + determinism. **Gated 3-way bench (block + Cerid-per-column + Eigen-per-column, nonsym): Cerid per-column GMRES/BiCGSTAB CRUSHES Eigen 2.27–2.87× on expensive operators (parallel SELL spmv, the v4b/v4c path); block adds A-pass reduction 4–24× + breadth (Eigen has NO block algorithm).** Honest floor (= block-CG): on cache-resident/sparse A the block O(n·s²) dense work means per-column owns wall-time; block wins matrix-free / expensive-apply. gemat11 unpreconditioned stalls/diverges for ALL (Cerid block+col + Eigen — preconditioning regime, divergence now cleanly guarded). Iterative suite **91 cases / 12317 assertions**; win-debug ctest+guards + engine win-tidy clean (clang-cl stale-PCH + test-file LLVM-skew → CI owns the pinned cross-config gate). **🎉 v4f CLOSED — nested-precond + block-CG/PCG + block-GMRES + block-BiCGSTAB, the complete block-Krylov family.** | ~620 actual | 7 / (suite) | — |
| ↳ **v4g** ✅ 2026-05-26 — **IC(0) + ILU(0) + ILUT incomplete-factorization preconditioners** | **SHIPPED.** Three `LinearOp` preconditioners in crd-hesap-preconditioners (NOT etree-consuming — level-0 IS A's pattern, no symbolic fill needed; the old ledger note was wrong). **`Ic0Preconditioner`** (left-looking IC(0) on A's lower pattern): **diagonal-scaled** (D⁻¹ᐟ²·A·D⁻¹ᐟ² to unit diagonal — stiffness matrices' ~1e9 diagonal range otherwise over-shifts) + **Manteuffel diagonal shift** (A+α·I, α doubling on a non-positive pivot); apply L⁻ᴴL⁻¹ over L + conj-transpose Lᴴ. **`Ilu0Preconditioner`** (IKJ level-0 on A's pattern ∪ inserted diagonals; dense position scratch; **pivot floor** √ε·max|A|); combined L\U CSR + diag-pos; adjoint factors Aᴴ. **`IlutPreconditioner`** (dual-threshold ILUT, SPARSKIT `ilut.f` verbatim: dense working row + jw/jr, IKJ with min-column elimination, dual dropping [multiplier `|fac|≤droptol·tnorm` + keep-`lfil`-largest per L/U row via qsplit], dynamic-CSR L+U); **row-scaled** (D_r·A unit ∞-norm → droptol scale-invariant; the fix that took sherman3 295→17 iters) + pivot floor; lfil/droptol params. 24 CLI (`hesap.precond.{ic0,ilu0,ilut}.{f32,f64,c32,c64}` standalone apply + ic0/ilu0/ilut added to all 9 solver precond-selectors). Tests (+17): recovery (IC(0)/ILU(0)/ILUT exact on tridiag), 2D-Laplacian/conv-diff convergence + cuts-iters, ILUT-stronger-than-ILU(0), ILUT-full-LU-exact, complex, determinism. **Gated bench `bench_hesap_ilu_vs_reference`: IC(0)-PCG CRUSHES Eigen IncompleteCholesky-CG 2.07–2.39× wall / 1.33–1.79× per-iteration at MATCHED TRUE residual (corrected 2026-05-26 from recurrence-residual 2.74–3.10×) on real SuiteSparse SPD (bcsstk13/24/25) — fewer iters AND parallel SELL spmv. ILUT matches Eigen IncompleteLUT preconditioner quality (sherman3 17 vs 11 iters at equal fill); ILUT WALL-TIME on small nonsym (n~5000) is triangular-solve-bound (sequential dense-factor apply dominates, parallel spmv can't pay at small n) ⇒ the ILUT wall-time crush is gated by a parallel/level-scheduled triangular solve (deferred shared-infra slice).** gemat11 (zero-diagonal, pathological) fails for both Cerid + Eigen. Iterative suite **105 cases / 17328 assertions**; win-debug ctest+guards + engine win-tidy clean. **NEXT (advisor-flagged at v4g start): v4g-tri-solve-parallel** (level-scheduled triangular solve — shared infra, unlocks the ILUT wall-time crush + speeds IC/SSOR). | ~900 actual | 17 / (suite) | — |
| ↳ **v4g-tri-solve-parallel** ✅ 2026-05-26 — **level-scheduled parallel triangular solve + large-nonsym ILUT crush** | **SHIPPED.** `triangular_solve.hpp` (crd-hesap-sparse): `TriSchedule` (topological dependency levels — level[i]=1+max(level of deps), O(nnz) build) + `tri_solve_lower/upper_levelsched` over OFF-DIAGONAL CSR + inv_diag form (nullptr⇒unit). Rows in a level are independent ⇒ `parallel_for` within each level, **bit-exact** vs sequential (each output by one worker, fixed CSR order, deterministic partition — the moat). **Size-adaptive (D-pin): parallel only when max_level_width ≥ 256 AND n ≥ 8192** — measured: sherman3 (n=5005, 689 narrow levels) ran 2× SLOWER parallel (barrier-bound), correctly kept serial. Integrated into ILUT (L/U schedules built once at factor time; per-apply parallel solve). +1 test: level-sched ≡ sequential bit-exact at n=90000 (2D-grid factor, max_width 300, parallel engaged). **Bench addendum `cd2d-200` (2D conv-diff n=40000): Cerid ILUT CRUSHES Eigen IncompleteLUT 1.74× (4 it / 11.11 ms vs 11 it / 19.34 ms, matched TRUE residual; corrected 2026-05-26 from 2.72×) — the large-nonsym win, via ILUT quality + parallel SELL spmv** (the ILUT factor is chain-y, max_width 12-26, so the tri-solve correctly stays serial there; gemat11 1.19×). Honest characterization (advisor-pre-authorized): ILU FILL makes factors chain-y ⇒ the parallel tri-solve engages on WIDE-wavefront factors (verified n=90000), correctly serial on chain-y/small; it is correct shared infra, available to IC(0)/ILU(0)/SSOR. Iterative suite **106 cases / 107330 assertions**; win-debug ctest+guards + engine win-tidy clean. **NEXT: v4h** (ILU(p) level-of-fill). | ~260 actual | 1 / (suite) | — |
| ↳ **v4h** ✅ 2026-05-26 — **ILU(p) level-of-fill incomplete LU; 🎉 the ILU family is complete** | **SHIPPED.** `IlupPreconditioner<T>(a, alloc, p)` (Saad Alg. 10.5): fused symbolic+numeric IKJ where each entry carries a fill LEVEL (A's nonzeros = 0; a fill via k gets lev(i,k)+lev(k,j)+1, the MIN over fill paths); a NEW fill is created only when its level ≤ p, while EXISTING pattern entries are always updated numerically (the level gates fill CREATION, never updates — the bug that made ILU(0) skip all updates until fixed). U entries store their level (fill sources for later rows). Pivot floor; no row-scaling/threshold/qsplit (purely structural). Reuses the v4g level-scheduled parallel tri-solve + the L/U dynamic-CSR + adjoint(Aᴴ). 4 CLI (`hesap.precond.ilup.{f32,f64,c32,c64}` + `ilup`/`level` added to all 9 solver selectors). Tests (+4): recovery (exact at any p on tridiag; dense matrix ⇒ ILU(0)=full LU exact), **monotone fill + convergence in p** (factor_nnz ↑, iters ↓ — a wrong max-instead-of-min level breaks this), complex, determinism. **Gated bench (ILU(0)/ILU(1)/ILU(2) vs Eigen IncompleteLUT, FGMRES, matched fill): cd2d-100 (n=10000, matched TRUE residual) ILU(0)→ILU(1)→ILU(2) cuts 58→35→30 iters (fill 1.0→1.4→1.79×); Cerid ILU(2) 30 it / 5.40 ms CRUSHES Eigen IncompleteLUT 47 it / 13.92 ms = 2.58× at equal fill (corrected 2026-05-26 from the recurrence-residual 3.2×; [[feedback_iterative_bench_matched_true_residual]]). sherman3: Cerid ILU(2) CONVERGES (179 it, true r=1.0e-12) where Eigen ILUT DIVERGES (r=1.2) at matched fill.** Eigen ships no level-of-fill ILU(p) → breadth + the crush. Iterative suite **110 cases / 108024 assertions**; win-debug ctest+guards + engine win-tidy clean. **🎉 ILU family complete: ILU(0) + ILU(p) + ILUT + IC(0), all crushing/matching Eigen.** | ~360 actual | 4 / (suite) | — |
| ↳ **v4i** | **SPAI + polynomial/Chebyshev + Schwarz** (split i-1/i-2/i-3, advisor). Sparse approx inverse (Frobenius-min, parallel) + Chebyshev polynomial (matrix-free, determinism/GPU-friendly) + additive + restricted Schwarz (domain decomposition). Real+complex. | ~900 | ~28 | — |
| ↳ **v4i-3** ✅ 2026-05-26 — **🎉 v4i CLOSED** | **Additive + Restricted Schwarz.** `SchwarzPreconditioner<T>` (overlapping DD `M⁻¹=Σ R̃ᵢᵀ Aᵢᵢ⁻¹ Rᵢ`; contiguous/ND partition + overlap BFS; factor-once dense LU per subdomain + adjoint; cap+fallback; sorted-Ωᵢ deterministic). AS=symmetric (exact local solves) → SPD/PCG; RAS (default)=nonsym/BiCGSTAB, contention-free PARALLEL apply bit-exact. 8 CLI (AS→SPD, RAS→nonsym selectors). **Honest bench vs the CORRECT peer block-Jacobi (Eigen ships NO DD — vs-IChol was an apples-to-oranges error, corrected): RAS-Schwarz(ov=1) BEATS block-Jacobi 1.11× wall + 2.3× fewer iters on nonsym cd2d-200; overlap is the DD value-add.** One-level (coarse space = AMG v4k); the distributed-memory + AMG-component building block. +5 tests; suite 131 cases / 111662 assertions. Session `docs/sessions/2026-05-26-hesap-v4i-3-schwarz.md`. **🎉 v4i CLOSED (SPAI+FSPAI+AMD-adapter + Chebyshev + Schwarz).** | ~450 | 5 | 0.5 d |
| ↳ **v4i-2** ✅ 2026-05-26 | **Chebyshev polynomial preconditioner.** `ChebyshevPreconditioner<T>` (`M⁻¹≈p_deg(A)`, three-term recurrence = deg-1 matrix-free spmv, NO factorization/tri-solve; the AMG smoother for v4k). Deterministic power-iteration λmax (non-uniform seed) + `lo=λmax·lo_ratio`; SPD/HPD → PCG/MINRES/SYMMLQ; `apply_adjoint==apply`; determinism moat held. 8 CLI. **lap2d-160 Chebyshev(8)-PCG CRUSHES Eigen IncompleteCholesky-CG 2.0× (35.7 vs 71.2 ms), matrix-free; Eigen ships no polynomial preconditioner. bcsstk13 (κ≈1e12) stalls — honest: Chebyshev standalone needs a good bracket, it is the matrix-free parallel smoother.** +4 tests; suite 126 cases / 110866 assertions. Session `docs/sessions/2026-05-26-hesap-v4i-2-chebyshev.md`. | ~260 | 4 | 0.5 d |
| ↳ **v4i-1** ✅ 2026-05-26 | **SPAI + FSPAI.** Classical right-SPAI `M≈A⁻¹` (static + adaptive Grote-Huckle; LS via complex `block_qr` + R-floor) + factored **FSPAI `M=L·Lᴴ` SPD-by-construction** (Kolotilina-Yeremin/Huckle; the SPD twin classical SPAI can't give). Parallel per-column setup (bit-identical across threads = determinism moat at setup); apply = matrix-free spmv, no tri-solve (delegates to `ParallelSpmvLeastSquaresOp`). 16 CLI. **FSPAI-PCG CRUSHES Eigen IncompleteCholesky-CG at MATCHED true residual: 2.1–9.8× wall + 1.9–3.1× per-iteration (no-tri-solve structural win) on SPD bcsstk*/lap2d; SPAI converges where Eigen ILUT-GMRES diverges (gemat11/sherman3).** +8 tests; iterative suite 118 cases / 109393 assertions. Session `docs/sessions/2026-05-26-hesap-v4i-1-spai-fspai.md`. | ~700 | 8 | 1 d |
| ↳ **v4j** | **Multilevel ILU (ILUPACK-class) — research-grade mini-cluster, split ↓.** Inverse-based pivoting + dropping; the high-quality general preconditioner that competes with AMG. | ~1500 | ~35 | — |
| ↳ **v4j-1** | Multilevel-ILU scaffold + reordering/scaling — **re-split (advisor) into 1a (MC64) + 1b (scaffold)**; peer = **ILUPACK** (same-algorithm gold standard, on Linux/WSL) + AMGCL secondary, NOT Eigen. | ~500 | ~12 | — |
| ↳ **v4j-1b** ✅ 2026-05-26 | **MultilevelIlu scaffold** (`multilevel_ilu.hpp`). `MultilevelIlu<T>` = MC64 match+scale → `B=D_r·A·D_c·Pᶜ` (matched on diagonal) → ILUT(B); `apply M⁻¹r = D_c·Pᶜ·(ILUT(B)⁻¹·D_r·r)`, adjoint likewise. Single-level placeholder (the recursion entry point for v4j-2's inverse-based pivot test). Working robust preconditioner now (MC64 scaling fixes off-diagonal-dominant/badly-scaled matrices plain ILUT chokes on). 8 CLI (`mlilu.{f32,f64,c32,c64}` + 6 nonsym selectors). +4 tests (solves off-diag-dominant system, **beats plain ILUT on it**, complex, determinism); suite 135 cases / 112068 assertions; win-debug+clang-cl+win-tidy clean. Session `docs/sessions/2026-05-26-hesap-v4j-1b-multilevel-scaffold.md`. | ~140 | 4 | 0.5 d |
| ↳ **v4j-1a** ✅ 2026-05-26 | **MC64 max-weight matching + scaling** (`crd-hesap-ordering/mc64.{hpp,cpp}`). Duff-Koster: max-product → min-sum LAP (sparse shortest-augmenting-path, binary heap, lexicographic `(dist,col)` determinism + greedy init) → column permutation + dual-potential scaling `D_r·A·D_c` to I-matrix form (matched\|diag\|=1, \|off-diag\|≤1). Heavy LAP core in `.cpp` (f64), templated wrapper (4 types). CLI `hesap.ordering.mc64` (first value-aware ordering op). The robustness front-end ILUPACK/MUMPS/SuperLU use; reusable by the ILU family. +4 tests (cyclic match, I-matrix property, determinism, complex); ordering suite 40 cases / 7760 assertions; win-debug+clang-cl+win-tidy clean. Session `docs/sessions/2026-05-26-hesap-v4j-1a-mc64.md`. | ~280 | 4 | 0.5 d |
| ↳ **v4j-2** | Inverse-based pivoting + dropping strategy (the ILUPACK numerical core — bounded inverse-factor norm pivot test + dual dropping). **Re-split (advisor) into 2a (estimator+pivot, single-level) / 2b (multilevel recursion) / 2c (complex+CLI+adjoint).** Peer = **ILUPACK** (local-only oracle, `CRD_BUILD_HESAP_VS_ILUPACK`, WSL; non-commercial license → `external/`, never CI). | ~600 | ~12 | — |
| ↳ **v4j-2a** ✅ 2026-05-26 — **inverse-based pivoting Crout core** | `InverseBasedIlu<T>` (`inverse_based_ilu.hpp`): bi-index Crout LDU (Li-Saad-Chow 2004 §2.2 Ufirst/Ulist/Lfirst/Llist) + CMSW incremental inverse-norm estimator (Alg 3.1) + κ-bounded accept/defer pivot test (Bollhöfer-Saad SISC 2006) + S-version Schur `S̃ = C − Lᴱ Dᴮ Uꜰ` + single ILUT leaf + folded eq.(20) apply. Determinism pins **D(mlilu)-1..6** locked. Studied 3 primary sources first. **3 tests / 510 asserts** MSVC+gcc-Werror+clang-cl. **ILUPACK V2.4 local-only oracle** (`CRD_BUILD_HESAP_VS_ILUPACK`, WSL, gitignored `external/`) + `bench_hesap_mlilu_vs_ilupack` (byte-identical in-process matrix, matched true residual). Session `…-v4j-2a-inverse-based-pivoting.md`. | ~430 | 3 | — |
| ↳ **v4j-2b** ✅ 2026-05-26 — **multilevel recursion (recursive Schur + dense base)** | Leaf → `unique_ptr<LinearOp<T>>`: Schur factored RECURSIVELY by another `InverseBasedIlu` until `n≤64` or depth cap → exact `DenseLuLeaf` (partial-pivot dense LU; D(mlilu)-6 droptol-independent terminal). Folded eq.(20) apply recurses polymorphically (one-line swap); κ≥1 ⇒ Schur strictly shrinks ⇒ terminates. **4 tests / 514 asserts** MSVC+gcc-Werror+clang-cl + ctest guards: genuine **>2 levels** on anisotropic conv-diff (semi-coarsening) + solves + serial/parallel bit-exact. **Bench (cd2d, condest=5): recursion engages + solves but stays 2 levels / mesh-DEPENDENT iters 118→387 vs ILUPACK 3-5 lvl / flat 7→10.** κ×droptol sweep proves the crush is NOT one-knob (fill not the lever; depth alone not the lever) → **v4j-3 cluster** (improved ICE + M-version Schur, levers ordered, histogram first). Session `…-v4j-2b-multilevel-recursion.md`. | ~140 | 1 (suite 4/514) | — |
| ↳ **v4j-2c** ✅ 2026-05-27 — **complex + adjoint + CLI for `InverseBasedIlu` (the v4j-2 completeness gap filled)** | **Complex:** instantiates + solves complex non-Hermitian (reuses the v4k-c complex dense LU for the leaf). **Adjoint** (`has_adjoint=true`): `apply_adjoint` = the conjugate-transpose of the folded eq.(20), derived block-wise (`b=U_B⁻ᴴ r_B`; `w_C=S̃⁻ᴴ(r_C−Uꜰᴴ b)`; `w_B=L_B⁻ᴴ(D_B⁻ᴴ b−Lᴱᴴ w_C)`) via two new transpose triangular-solves (`solve_unit_{lower,upper}_transpose`, conj'd) + a `conj` helper; `DenseLuLeaf` gains `apply_adjoint` via a second LU of S̃ᴴ (leaf is ≤64, cheap); recursive leaf adjoint is polymorphic. MC64 path wraps `D_r·B⁻ᴴ·Pᶜᵀ·D_c` (mirrors MultilevelIlu). **CLI:** `hesap.precond.mlilu_ib.{f32,f64,c32,c64}` (4 cmds, κ/droptol) + `mlilu_ib` added to all 5 nonsym solver selectors (fgmres/bicgstab/gcr/gcrot/idrs — QMR-class adjoint now available). +2 tests: complex non-Herm solve + **adjoint identity ⟨y,M⁻¹x⟩=⟨M⁻ᴴy,x⟩ to round-off** (complex w/ deferral ⇒ exercises Lᴱ/Uꜰ conj-transposes + leaf adjoint). 6 InverseBasedIlu tests; MSVC+gcc+clang-cl+guards. | ~210 | 2 | — |
| ↳ **v4j-3** ✅ 2026-05-27 — **WALL BROKEN on the CFD metric: per-level reordering + fast AMD → Cerid WINS cd2d β=0.3 total wall-time 2.0–2.8× vs ILUPACK** | The lever was **reordering**, not the estimator/MILU/κ/fill (6 levers ruled out by measurement; HILUCSI/ILUPACK reorder EVERY level → Schur stays hard → deep hierarchy). **(b) Fast AMD:** `amd_order`+`camd_order` O(n²)→O(1) bucket-head selection (D(ord)-1; 42×, 144→3.4ms @22.5k, fill preserved, all AMD consumers benefit; 931 ordering tests + cross-config). **(a) Per-level reorder:** `reorder` ctor option on `InverseBasedIlu` builds `B=P·A·Pᵀ` via AMD at construction; recursion auto-applies per Schur; `apply`/`apply_adjoint` unwrap `A⁻¹=PᵀB⁻¹P` (symmetric perm). **Measured cd2d β=0.3 (ILUPACK home turf): iters HALVED 88/138/165→45/64/76 (3 levels); total wall 5.1/31/85ms vs ILUPACK 14/67/174ms = 2.0–2.8× FASTER at every mesh.** Still loses iteration COUNT (45–76 vs 7–10) but wins the wall metric CFD pays. reorder + MILU(diag-compensation) both shipped default-off options. +2 tests (reorder solves+deepens+adjoint-identity; complex adjoint-identity to round-off) → 8/8 InverseBasedIlu MSVC+gcc+clang-cl. **Open micro-decision:** make `reorder` default-on for nonsym (ILUPACK always reorders). Session `…-v4k-c-amg-cli-complex.md`. | ~180 | 2 (8/8 suite) | — |
| ↳ **v4k** | **AMG family (the whole AMG cluster — consolidated 2026-05-26).** `crd-hesap-amg`; AMG-as-solver AND AMG-as-`LinearOp`-preconditioner; deterministic coarsening tie-breaks (D(amg)). Sub-slices ordered BY VALUE: SA core ✅ → convection probe ✅ → **AGMG K-cycle (the convection crush)** → polish/CLI → Ruge-Stüben → bootstrap. (Former top-level `v4l`/`v4m` folded in here for family consistency with v4e/v4f/v4i.) | — | — | — |
| ↳ **v4k-a** ✅ 2026-05-26 — **Smoothed-Aggregation AMG 🎉 CRUSHES ILUPACK on diffusion** | `SaAmg<T>`: strength `|a_ij|≥θ√(a_ii a_jj)` → greedy aggregation (Vaněk) → smoothed prolongator `P=(I−ωD⁻¹A)T` → Galerkin `A_c=PᵀAP` (spgemm×2) → V-cycle (Gauss-Seidel smoother) → dense-LU coarsest. **Mesh-independent: 2D Poisson AMG-CG iters FLAT** (v4j inverse-based ILU couldn't — root cause proven). **vs ILUPACK matched-true-resid (full mesh-sweep, GS ν₁=ν₂=2 default): isotropic Poisson Cerid 8→10→10 FLAT / 1.5–11.6ms / fill 1.36 vs ILUPACK 12→18→26 GROWING / 13–100ms / 3.6 — 8.7× faster factor, 4× solve, 2.6× lower fill; aniso-diffusion (eps=0.01 β=0) Cerid 7→7→9 flat vs ILUPACK 11→17→23, 2× factor & solve; mild convection β=0.05 also crushes (8 vs 13 iters, 3.5× factor).** Smoother is an enum `Smoother::{GaussSeidel(default,robust), Ilu0(opt-in)}` — Ilu0 gives the fewest diffusion iters (4–5) but DIVERGES on strong convection (proven empirically), so GS ships as the robust default. **Boundary:** diffusion + mild convection (β≤0.05) crushed; strong convection probed in v4k-a-2 ↓. 5 tests/268 asserts MSVC+gcc+clang-cl+guards; pins D(amg)-1..5. Session `…-v4k-a-smoothed-aggregation-amg.md`. | ~450 | 5 | — |
| ↳ **v4k-a-2** ✅ 2026-05-26 — **convection probe: MC64 no-op proven, W-cycle lever, β≥0.1 → v4k-b AGMG** | Measured the convection-crush levers head-to-head vs ILUPACK (FGMRES, matched true residual). **(1) MC64 max-weight matching + symmetric scaling is a VERIFIED no-op on cd2d** — diagnostic printed `dr=[0.495,0.495]` (uniform const), `dc=[1,1]`, identity perm (0/2500): a constant-coefficient grid is already I-matrix-like, so B=D_r·A·D_c·Pᶜ = scalar·A ⇒ byte-identical iters/fill. Premise (MC64 closes convection) falsified by measurement. MC64 wiring kept as correct infra (`Mc64Mode::{None(default),TopLevel,EveryLevel}` in `InverseBasedIlu`; per-level via the recursion; apply unwrap = adjoint-tested MultilevelIlu math; all 8 v4j tests green). **(2) W-cycle (`cycle_gamma`=2) is a PER-PROBLEM lever, not a robust default** — on strictly-diagonally-dominant cd2d β=0.1 it ~halves iters (V 17→18→19 ⇒ W 9→9→10, TIES ILUPACK 7→9→10 on iters + crushes wall 5–7× via faster factor) AND tightens Poisson to flat-7; but on the CONSERVATIVE (zero-row-sum) cd2d it DIVERGES (V=12, W=2000) — the nonsymmetric Galerkin coarse op amplifies under the stronger cycle. So **V-cycle stays the robust default; W is exposed + documented**. **(3) β=0.3 strong convection is a genuine SA-AMG wall** — W-cycle byte-identical (70/108/112), the coarse space misses the convection modes. The robust universal convection answer is **v4k-b AGMG K-cycle** (Notay 2010): the Krylov acceleration stabilizes exactly the W-cycle divergence found here. Also surfaced: **Cerid's own AMG beats Cerid's own InverseBasedIlu on convection at every (β,mesh)** (β=0.1 AMG 17–19 vs ILU 88–279), so the crush lives in v4k, not v4j. +2 tests (V robust on conservative / W wins on dominant); MSVC+gcc+clang-cl+guards. Session `…-v4k-a-smoothed-aggregation-amg.md`. | ~120 | 2 | — |
| ↳ **v4k-b** ✅ 2026-05-26 — **K-cycle: the robust convection cycle (W's benefit, no divergence) — but NOT the β=0.3 crush** | `Cycle::{V(default),W,K}` in `SaAmg`. **K-cycle** (Notay AGMG 2010): each intermediate level's coarse correction = 2 flexible-GCR steps preconditioned by the recursive cycle (`kcycle_coarse`); serial dot products ⇒ deterministic (the AMG apply is fully serial — moat held; D(amg)-6 pins the 2-step GCR). **Measured (FGMRES, matched true residual): K = W on the cases W helps (Poisson 7 flat; cd2d β=0.1 dominant 9→9→10, ties ILUPACK + crushes wall 5–7×) AND converges where W DIVERGES (conservative zero-row-sum cd2d: V=12, W=2000/diverged, K=4) — W's benefit without W's instability.** Honest: **K does NOT crush β=0.3 strong convection (still 70→108→112 = V = W)** — the cycle is NOT the lever there; the COARSE SPACE is (SA aggregation misses the convection modes), which is **v4k-e's** job (bootstrap/adaptive coarsening), and β=0.3 Péclet-large conv-diff is documented-hard for ALL aggregation AMG. V stays the textbook-safe O(L) default; K is the recommended convection cycle; W retained for comparison. +1 test (V+K converge on conservative cd2d where W diverges); reused W-cycle test. MSVC+gcc+clang-cl+guards. | ~150 | 1 | — |
| ↳ **v4k-c** ✅ 2026-05-27 — **AMG polish + CLI + complex (closed the CLI-per-op debt + unblocked complex AMG)** | **CLI:** `hesap.amg.{f32,f64,c32,c64}` (4 cmds) — AMG-as-SOLVER via stationary V/W/F/K-cycle iteration (`x += M⁻¹(b−Ax)`, serial spmv ⇒ deterministic oracle); self-contained (crd-hesap-amg has no iterative dep). Closes the standing "a command for every op" debt. **F-cycle:** added `Cycle::{V,W,F,K}` (F = one F-recursion + one V-recursion/level; between V and W). **Complex AMG:** made `dinv` carry `T` (complex 1/a_ii) through amg.hpp + prolongator.hpp (power-iteration norms via `mag2`, smoothed-P coef `T(-ω)·dinv`); strength already used `mag`. **Unblocked by completing complex dense LU** (`lu.cpp`: `abs_value`→`RealType` modulus via `if constexpr`, real path bit-identical; `apply_permutation` templated; +c32/c64 `factor_lu`/`solve_lu` instantiations — closed the old "v0e-a2 complex-LU follow-on" note, first consumer = AMG coarse solve). +3 tests (CLI hesap.amg.f64 solve, complex Hermitian AMG-PCG <40 iters, V+K-converge-where-W-diverges); 9 AMG tests. **Per-level smoother choice = gold-plating, not done** (global Smoother enum suffices; no consumer). MSVC 9/9 + gcc 38/38 (dense-LU regression intact) + clang-cl + guards. | ~340 | 3 | — |
| ↳ **v4k-d** ✅ 2026-05-27 — **Classical Ruge-Stüben AMG (the textbook isotropic path)** | Full RS pipeline: `rs_strength.hpp` (classical −a_ij≥θ·max-neg, θ=0.25, directed) → `cf_splitting.hpp` (first-pass bucketed max-λ coloring + interpolation-correctness promotion pass) → `rs_interpolation.hpp` (direct interpolation: sign-split α/β lumping of off-diagonal mass onto the strong-C set, `w_ij=−(α|β)a_ij/a_ii`; Re-based +/- for complex). Wired into the AMG class as `Options.coarsening = {SmoothedAggregation(default), RugeStuben}` — REUSES the whole cycle/smoother/Galerkin/coarse machinery (one build() branch, zero duplication). **RS-AMG-PCG mesh-independent on 2D Poisson: 6 iters at n=2500 AND n=10000** (beats SA's 8–10 on isotropic, as classical RS should). C/F split validated structurally (Laplacian 450C/450F, every F depends on a C). CLI `coarsening` arg (sa|rs) on `hesap.amg.*`. +2 tests (C/F structural + RS mesh-independence). 12 AMG tests; MSVC+gcc+clang-cl+guards. (Fixed 2 bugs in the intricate C/F algo — λ-bucket overflow + F-with-no-C — caught by the structural test, built incrementally.) (Was the old top-level `v4k-b`.) | ~560 | 2 | — |
| ↳ **v4k-e-1** ✅ 2026-05-27 — **αSA adaptive-candidate hook shipped + the β=0.3 wall locked by 5-lever exhaustion** | `Options.adaptive_candidate` (default off): seed the tentative T from a near-nullspace CANDIDATE relaxed via ν weighted-Jacobi sweeps on A·x=0 (deterministic seed) instead of the constant; `tentative_prolongator_adaptive` normalizes per-aggregate (Brezina αSA). **Measured the cheap increment of the user-chosen bootstrap path: αSA does NOT move β=0.3** (70→112→115 ≈ SA's 70→108→112; Poisson 7 flat held — no diffusion regression). **VERIFIED not a no-op** (candidate range 1.8 at finest level — genuinely varying), so the conclusion is real: **the coarse space is NOT the β=0.3 lever** ⇒ full multi-candidate bootstrap won't help either (it scales the thing just proven not to matter). **β=0.3 anisotropic strong-convection is now a measured WALL for aggregation AMG — 5 levers exhausted: κ, droptol, cycle (V/W/K), aggregation (smoothed/plain), αSA-candidate.** Root cause (theory-backed): conv-diff near-nullspace ≈ constant; the regime needs DIRECTIONAL smoothing (downwind/line-GS) or the multilevel-ILU path, not a better coarse basis. **Honest β=0.3 status: Cerid AMG WINS total wall-time 1.3–1.9× (12× cheaper factor, 4× lower fill) but LOSES iterations 112 vs 10 (ILUPACK's home turf).** αSA kept as correct infra for genuinely-non-constant near-nullspace (elasticity); +1 test (αSA converges on Poisson, no regression). 10 AMG tests; MSVC+gcc+clang-cl+guards. | ~120 | 1 | — |
| ↳ **v4k-e-2** ⏸ 2026-05-27 — **line-GS attempted (user-directed) → reverted; β=0.3 iteration crush is the WALL (6 levers)** | Took run (a) **algebraic line Gauss-Seidel** (block-GS, strong-coupling chains solved tridiagonally via Thomas). Line criterion: strong = |a_ij|≥0.5·max-offdiag, chain only when strong-count≤2 (isotropic node = 4 strong ⇒ singleton ⇒ point-GS; anisotropic = 2 strong ⇒ y-line). **Poisson clean (7 = point-GS, singletons)** but **β=0.1 AND β=0.3 DIVERGE** (1e18/nan). That's a definite bug — block-GS with exact line solves is strictly ≥ point-GS, which handles β=0.1 at 9 iters — so the algebraic chains aren't grid-aligned enough for line-solve stability (snake/boundary lines lose diagonal dominance). **Reverted** (don't ship a diverging smoother; low-odds lever per advisor). Lesson kept: algebraic line-GS needs grid-line detection or geometric info; the durable artifact is the finding, code returns when there's a reason. **β=0.3 iteration crush now CLOSED as the documented wall — 6 levers exhausted** (κ, droptol, V/W/K, smoothed/plain agg, αSA, line-GS). Cerid AMG WINS β=0.3 total wall-time 1.3–1.9×, LOSES iters 112 vs 10 — genuinely ILUPACK's home turf. Only theoretically-distinct path left = v4j-3 multilevel-ILU rework (κ sweep showed deep, not quick). 10 AMG tests intact; MSVC+gcc+clang-cl. | ~0 (reverted) | 0 | — |
| ↳ **v4z** ✅ 2026-05-27 — **v4 CLOSE: audits + reorder default-ON + robustness fix + ADR §26 lock.** | **(1) Factor-vs-solve break-even quantified** — bench now reports the split + reuse break-even; on cd2d β=0.3 `mlilu_ib`+reorder WINS single-shot wall 2.1–2.2× at every mesh, ILUPACK overtakes only after **k\*≈3.6–5.9 re-solves** (single/few-RHS → Cerid; many-RHS → ILUPACK). **(2) `reorder` default flipped ON for nonsym `InverseBasedIlu`** — measured no-regression on in-regime matrices (sherman3 iters 1066→557 1.47×, cd2d-150 269→68 1.75×, fill ~flat), matches ILUPACK; natural order via explicit `reorder=false`; `mlilu_ib` CLI gains a `reorder` Bool param; 4 natural-order tests pinned. **(3) Robustness fix** — `DenseLuLeaf` shifts the diagonal (√ε·max\|diag\|, geometric back-off) + refactors on a singular deferred Schur leaf instead of asserting in `solve_lu` (gemat11 wrong-tool case now degrades to EXIT 0; +1 test). **(4) Complex-completeness audit** — every solver/preconditioner/AMG variant has a complex residual test (+2 closing block_pcg<C> + point-Jacobi<C> gaps). **(5) CLI-completeness audit** — every op (.f32/.f64/.c32/.c64) registered. **(6) ADR-0065 §26** locks D(iter)-1..10 (serial-reduction moat, size-adaptive operator + frame_reset, breakdown guards, packed-MGS block-orthonormalization, graceful-degrade, reorder default-ON, O(1) AMD bucket-head, the β=0.3 quantified-not-slogan result). 18-config sweep → CI. | ~250 | 4 | — |
| **v5** 🚧 IN PROGRESS (Cholesky ✅ · LU ✅ · QR ✅ · LDLᵀ ✅ · HSS ✅ [v5e-1/2 — CRUSH+moat] · BLR / mixed-prec IR next) | **Sparse DIRECT — the COMPLETE elite family** (detailed plan ↓). **STATUS 2026-06-04: v5e HSS ✅ (v5e-1 substrate [ID + counter-RNG range-finder + HSS + ULV] · v5e-2 GLOBAL-SAMPLE construction — vs STRUMPACK serial rank=4 machine-eps: COMPRESS CRUSH 1.39–3.41× [QR-then-tiny-SVD replacing the tall full-SVD, pa_svd 210→40ms] · FACTOR CRUSH 2.7–4.1× · SOLVE PARITY/WIN 0.99–1.06× [W=L⁻¹D21 4→2 tri-solves + register-blocked trsv + alloc-free reflector apply] + the determinism MOAT bit-identical {1,2,4,8}; fixed a pre-existing crd-jobs shutdown SIGSEGV landmine en route; 6 configs green incl. gcc full 597995 asserts). v5a Cholesky ✅ COMPLETE (CRUSHES CHOLMOD hood 1.33×/ldoor 1.28× + multi-RHS solve, complex, CLI, moat). v5b LU ✅ COMMITTED (`ebf34e2`) — multifrontal adaptive-MC64 wins/ties MUMPS on CFD targets + moat + saddle-point fixed + CLI. v5c QR ✅ COMPLETE (WIP, not committed — local close done): `MultifrontalQR<T>` f32/f64/c32/c64 — symbolic (AᵀA-free) + numeric (blocked-WY) + tree-parallel determinism moat {1,2,4,8,16} + rank-revealing (Heath) + complex (Qᴴ) + CLI; CRUSHES Eigen both domains (LS 7–37× / square 13–71×); vs SPQR SOLVE crushed 4–8.6× + FACTOR well/illc1033 WIN + 1850 near-parity + bcsstk square-SPD characterized loss (ADR-0082 wall). 6 slices v5c-1e/1f/1g/2a/2b/2c (session `docs/sessions/2026-06-02-hesap-v5c-1-multifrontal-qr.md`). v5d LDLᵀ ✅ COMPLETE (a–h): a–g (skeleton+symbolic · per-front Bunch-Kaufman ½-flop col-major MA57-class · `MultifrontalLDLT<T>` driver · block-aware L·D·Lᵀ multi-RHS solve · tree-parallel determinism MOAT · complex LDLᵀ+LDLᴴ · CLI+bench) + **v5d-h (the crush grind): DELAYED PIVOTS (Duff-Reid — v5d was FAILing on genuinely-indefinite matrices; now correct on the WHOLE domain) + blocked-BK indefinite kernel + RELAXED PIVOT THRESHOLD (default 0.001; fixed the delay-driven front blowup — MUMPS delays 0, Cerid had delayed 30%) + ITERATIVE REFINEMENT + backward-error guard (resid now BETTER than MUMPS) + CHOLMOD relaxed-front AMALGAMATION (faithful `cholmod_super_symbolic` port).** **v5d FINAL vs MUMPS-LDLᵀ SYM=2 (the real same-class gold standard): 375×→~1.3–1.7× (SPD 1.27–1.46× · indef 1.53–1.74×, n=4k–64k — the gap NO LONGER GROWS with n) + WINS small/saddle indef (0.14–0.69×) + BETTER accuracy + the determinism MOAT MUMPS lacks + the ONLY correct solver besides MUMPS (Eigen `SimplicialLDLT` is fast-but-WRONG on indefinite); vs Eigen an outright crush (faster + correct).** HONEST: NOT a sub-1× serial speed-crush of MUMPS on big-3D-FEM — the residual ~1.3× is the blocked-BK KERNEL-PANEL rate (Cerid-blocked 42 GF/s vs LAPACK dsytrf 56; the gemm is at-par, the panel structure isn't — an xLASYF W-panel reaches parity, not sub-1×, same kernel class). The crush = PARITY-class speed + the MOAT + accuracy (the established v5a/v5b/v5c pattern). **Next v5 = v5e-3 (BLR-embedded multifrontal, MUMPS-BLR) / v5f (mixed-precision IR) / v5z (system-doc + ADRs + all-families moat + full corpus). The v5c+v5d+v5e WIP is the commit boundary owing the 18-config CI sweep.** (xLASYF W-panel = a bounded kernel follow-on.)** Originally planned 2026-05-28; multi-month as forecast. Supernodal Cholesky (CHOLMOD-class) + sparse LU (Gilbert-Peierls reference + supernodal/multifrontal **adaptive-MC64 static pivot**) + multifrontal QR (SPQR-class) + multifrontal LDLᵀ (Duff-Reid indefinite) + **rank-structured fronts (FULL HSS STRUMPACK + BLR MUMPS — both ship in v5, user-directed 2026-05-28)** + **mixed-precision iterative refinement** + complex variants + CLI per op. New module `crd-hesap-direct`; consumes v2c `SymbolicFactor` + v0 dense panels + `crd-hesap-sched::DependencyGraph`. **Cross-thread bit-determinism moat held per family** (Cholesky/LDLᵀ/LU-via-static-pivot/QR). *(AMG moved to v4 — iterative-family.)* | ~7200 | ~170 | multi-month |
| ↳ **v5a-0** ✅ 2026-05-28 — **RESOLVED by measurement (premise falsified; no code shipped)** | Implemented supervariable ND compression (weighted bisection + CAMD `nv`) per the `v2e-weighted-compression` debt, then benchmarked vs Eigen-AMD: **compression REGRESSED ND fill on bcsstk13/24/25** (un-compressed 0.984/1.001/1.157 → compressed 1.058/1.121/1.187). Root cause: CAMD already detects supervariables in-loop; pre-compression only coarsens. **Reverted in full.** Our AMD already beats Eigen-AMD on all three (1.039/0.960/1.007 GATE-OK) — the v5 consumer picks AMD on these. Debt closed-as-falsified; benchmarks-at-slice-close mandate vindicated (unit tests passed; bench caught the regression). | 0 (reverted) | 0 | — |
| ↳ **v5a-1a** ✅ 2026-05-28 — **substrate SHIPPED** | `crd-hesap-direct` module scaffold + **`IFactorization<T>`** (factor-once/solve-many, multi-RHS day-1) + **`Frontal<T>` + `extend_add`** (the multifrontal assembly kernel — standalone-tested, 4 type instantiations incl. complex). 5 tests/19 asserts, MSVC win-debug clean `/W4 /WX`. Advisor-confirmed left-looking supernodal stays the v5a algorithm; `extend_add` built standalone, consumed by v5c/d/e. (No benchmark applicable — pure substrate.) | ~250 | 5 | — |
| ↳ **v5a-3** ✅ 2026-05-28 — **TREE-PARALLEL supernodal factor 🎉 FACTOR CRUSH + determinism moat** | Static per-supernode update lists (replaced serial Head/Next) → supernode-etree level scheduling over `crd::jobs::parallel_for` + per-worker scratch (relrow/ubuf, `worker_index`-keyed); `gemm` scratch=nullptr (per-thread pooled). **Race-free dataflow** (each supernode writes only its own panel, reads already-factored descendants). **DETERMINISM MOAT VERIFIED: factor bit-identical across {1,2,4,N} workers** (12 tests/1393 asserts incl. the moat test). **Bench (relwithdebinfo, 32 workers, vs Eigen SimplicialLLT, same AMD matrix) — FACTOR CRUSH SCALES WITH SIZE: bcsstk25 (15k) 1.47× · hood (220k) 2.46× · bmwcra_1 (148k) 4.32× (8.5s vs 36.6s).** GHS_psdef verified-PD corpus; fill matches Eigen, residuals BEAT it (bmwcra_1 5.5e-11 vs 3.2e-10). Small matrices (2k–3.5k) parallel-neutral (overhead). **COLMAJOR REFACTOR (fixed the solve on the spot, NO debt) — double win.** Panels switched RowMajor→**ColMajor** (right-looking `cdiv` with contiguous column writes + long-axpy solve subdiagonal); `cmod` uses `gemm<ColMajor>` (instantiated). **FACTOR got FASTER: bmwcra_1 4.32×→5.48× · hood 2.46×→2.68× · bcsstk25 1.47×→1.50×** (right-looking contiguous `cdiv` beat the old RowMajor left-looking). **SOLVE loss ELIMINATED: bmwcra_1 235ms (1.75× slower) → 135ms (0.95×, parity within noise); hood 0.80×→1.03× WIN; bcsstk25 0.84×→1.19× WIN.** (Earlier gemv-on-subdiagonal attempt failed — pairwise-sum overhead on tiny blocks — and was reverted; ColMajor long-axpy is the right fix.) Correctness + determinism moat intact (12 tests/1393 asserts). **MULTI-RHS SOLVE CRUSH (block-`gemm`, dispatched for nrhs>1; nrhs=1 keeps hand-axpy): SOLVE x16 WINS EVERY matrix 1.55–3.68× — bcsstk13 1.55× · bcsstk24 1.96× · bcsstk25 2.34× · bmwcra_1 3.04× · hood 3.68×** (reads L once, amortizes the scatter across all 16 RHS; real N ⇒ genuine BLAS-3, did NOT backfire like the gemv). Single-RHS at scale now wins/parity (bmwcra_1 1.22×, bcsstk25 1.34×, hood 0.96×); single-RHS on tiny 2–3.5k matrices loses 0.84× (per-supernode overhead, negligible μs regime). **FACTOR + MULTI-RHS SOLVE both CRUSH Eigen; everything fixed on the spot — NO debt.** Correctness beats Eigen residual; determinism moat held. No frontier supernodal lib (CHOLMOD/MUMPS/PARDISO) offers the cross-thread bit-determinism. **🎯 ~1M HEADLINE (ldoor, n=952203, fill ~152M, GHS_psdef): FACTOR 8.30s vs Eigen 42.07s = 5.07× WIN · SOLVE x16 1.73s vs 4.86s = 2.82× WIN · SOLVE x1 253ms vs 315ms = 1.24× WIN; resid c=3.7e-13 vs e=1.1e-12.** Frame-arena fix (NO debt): the deep etree at ~1M issues thousands of `parallel_for` levels → the 1 MB per-thread frame arena exhausted; `jobs::frame_reset()` after each level's `wait` bounds it to one level's `JobDecl`s (`feedback_jobs_parallel_for_frame_arena_exhaustion`). **FULL-HONEST corpus: FACTOR crushes ≥15k (1.6–5.6×), ties tiny ≤3.5k (~10ms, negligible); SOLVE x16 crushes EVERY size (1.51–3.94×).** **SINGLE-RHS GAP CLOSED on the spot (NO debt) — right-looking diagonal-block solve.** Root cause (NOT small supernodes — the histogram refuted that: bmwcra_1/hood/ldoor are all avg_nc≈9.5, ~0% small-supernode axpy mass): the **forward diagonal-block solve was LEFT-looking** (`panel[k·nr+j]`, stride-`nr` dot-product) — cache-hostile + non-vectorizable on the ColMajor panel, worst on dense supernodes (bmwcra_1 maxnc=2406). Converted to **right-looking contiguous column axpy** (`panel[j·nr+i]` unit-stride in `i` — vectorizes), the SAME ColMajor-contiguity win as the factor's right-looking `cdiv`. **SOLVE x1 now WINS every real scale (≥15k): bcsstk25 1.06–1.15× · bmwcra_1 0.92×→1.01–1.14× · hood 1.00×→1.05–1.22× · ldoor (1M) 1.07×→1.04–1.32×** (3 clean runs; bmwcra_1 was the consistent straggler, now wins). x16 also improved (bmwcra_1 3.04→3.94×). Determinism moat held + residuals unchanged (12 tests/1393 asserts green). **Only sub-ms tiny ≤3.5k solves stay ~0.86× — per-supernode dispatch floor (40µs absolute), bounded-by-importance, multi-RHS crushes even there (1.57–2.01×).** | ~430 | 1 (12 suite) | — |
| ↳ **v5a-1b** ✅ CLOSED 2026-05-30 (crush landed → v5a-4) — **serial supernodal Cholesky (steps 1–5 ✅; bench-close DONE: beat CHOLMOD hood/ldoor at v5a-4)** | Left-looking supernodal Cholesky (CHOLMOD-class): v2c `SymbolicFactor` + relaxed amalgamation + dense panels (RowMajor) + `Lpos`/`Head`/`Next` lists; `solve` fwd/back multi-RHS. **Steps 1–5 ✅ 2026-05-28** (11 tests/809 asserts): step 1 `build_supernodal_symbolic` (relaxed amalgamation, D(direct)-2, union row-pattern) · step 2 scatter · step 3 `cdiv` (`factor_cholesky` diag + hand-rolled subdiagonal solve) · step 4 `cmod` (dense `gemm` Schur update via `Lpos`/`Head`/`Next`) · step 5 supernodal `solve` (fwd/back, multi-RHS). **Verified: factor+solve residual <1e-9 on 2D-grid SPD + dense SPD + 3-RHS; builds on `gemm`/`factor_cholesky`.** **Step 6 bench RUN (relwithdebinfo, vs Eigen SimplicialLLT on the same AMD-permuted matrix):** correct (Cerid residual BEATS Eigen; **correctly detects non-PD bcsstk30/32 matching Eigen** — not a bug), fill matches (~0.2%). **FACTOR: Cerid wins 1.14×/0.96×/1.30× on bcsstk13/24/25** (up to 1.30× on the largest, not yet the ≥1.5× crush); **SOLVE ~0.85× (loses slightly).** nrelax sweep flat (8 best); per-supernode/cmod allocs removed (not the bottleneck). **Crush path (pending — the decisive levers): (a) v5a-3 TREE-PARALLEL factor (Eigen SimplicialLLT is SERIAL → multi-core is the structural win), (b) larger verified-SPD corpus (GHS_psdef: bmwcra_1/ldoor/Flan_1565 — supernodal BLAS-3 dominates at scale), (c) CHOLMOD floor, (d) kernel tuning (small-gemm fastpath + SIMD/BLAS-3 solve).** **v5a-1b CLOSED — the crush LANDED at v5a-4** (hood 1.33×/ldoor 1.28× factor WIN; the gap turned out to be the SERIAL SYMBOLIC, not per-thread BLAS-3 — see v5a-4). | ~700 | ~16 | — |
| ↳ **v5a-2** ✅ 2026-05-29 — **complex Hermitian LLᴴ + CLI** | **SHIPPED.** `SupernodalCholesky<T>` extended to **Complex32/Complex64** as a single `if constexpr` path (real LLᵀ ↔ complex LLᴴ): `chol_real`/`chol_from_real`/`chol_conj` bridges (identity for real) + `kCholAdjoint<T>` (cmod gemm + backward-solve = ConjTranspose for complex, Transpose for real). Conjugation points: **scatter** reads CSR row `c`'s upper entry `A[c][i]` ⇒ L-column source `A[i][c]=conj(A[c][i])` (the bug that the single-supernode test isolated); **cdiv** diagonal takes `real(pivot)`+real sqrt (HPD pivot is real) + stores `T{d,0}`, rank-1 multiplier `conj(L[jj][j])`; **cmod** gemm `am·am1ᴴ`; **backward solve** Lᴴ=`conj(L)` (both nrhs=1 and the block-gemm). **CLI `hesap.direct.chol.{f32,f64,c32,c64}`** (module's first CLI: `cli_anchor.hpp` + `cli_register_direct.cpp`, complex values/RHS flattened `{re,im}` per the iterative-CLI convention, returns `[info, x]`). **+6 tests (3 complex factor/solve/multi-RHS/determinism + 3 CLI registration/real-solve/complex-solve): 18 tests / 1964 asserts, win-debug green.** Complex determinism moat VERIFIED (factor bit-identical {1,2,4,N} workers on a genuinely Hermitian A=conj-symmetric HPD). | ~330 actual | 6 (18 suite) | — |
| ↳ **v5a-CHOLMOD-oracle** ✅ infra+bench 2026-05-29 / 🛑 **initial bench LOST 2–4× → CRUSHED at v5a-4** (the gap was the SERIAL SYMBOLIC, not per-thread BLAS-3 as this row's diagnosis guessed; hood/ldoor now WIN factor + multi-RHS solve) | (Tree-parallel + determinism moat already shipped at the v5a-3 row above.) Built the **SuiteSparse CHOLMOD** oracle — THE gold-standard supernodal Cholesky (the real peer; Eigen SimplicialLLT was a WEAK scalar peer). `scripts/setup-cholmod-ref.sh` (apt `libsuitesparse-dev` + switches BLAS→OpenBLAS for a fair fight) + `CRD_BUILD_HESAP_VS_CHOLMOD` gating (WSL/Linux only; GPL supernodal module ⇒ dev-only, never shipped; decoupled from VS_REFERENCE — accepts `-DCRD_HESAP_MATRIX_DIR`) + `bench_hesap_cholesky_vs_cholmod.cpp` (forces `CHOLMOD_SUPERNODAL` + `CHOLMOD_NATURAL` on the same AMD-permuted matrix). CHOLMOD 5.2.0 + OpenBLAS in WSL; ran on `linux-gcc-release` (also = the **gcc `-Werror` cross-config check** on the v5a-2 complex changes — clean). **FAIR result (both timed full symbolic+numeric — advisor caught a CHOLMOD-analyze-hoisted-out bug, fixed; matched threads, both OpenBLAS): FACTOR cerid/cholmod @8thr→@1thr — bcsstk25 1.25×→1.04× WIN · hood 0.66×→0.55× · ldoor 0.45×→0.45× · bmwcra_1 0.24×→0.38×. SOLVE x1 0.48–1.53×. SOLVE x16 LOSES everywhere 0.36–0.72×.** **DIAGNOSIS: primarily PER-THREAD efficiency** — at 1 thread we're 0.38–0.61× (2–2.6× slower/thread) on structural matrices EVEN THOUGH CHOLMOD carries MORE fill (115M vs 96M bmwcra) ⇒ its flop-rate ~2.5–3× ours. Parallel scaling is a SECONDARY, matrix-specific lever (only bmwcra scales worse 0.38→0.24 — few huge supernodes; CHOLMOD BLAS-threads INSIDE a supernode, we only tree-parallelize). **Reference is the floor (`feedback_reference_implementations_are_the_floor`): v5a is NOT a crush — vs the gold standard we are ~2–4× behind on factor at scale (win bcsstk25 only).** | — | — | — |
| ↳ **v5a-4** ✅ 2026-05-29/30 — **CRUSHED the CHOLMOD FACTOR gap (hood/ldoor WIN) + parallelized the SOLVE (multi-RHS hood/ldoor WIN)** | The planned per-thread BLAS-3 levers (syrk/microkernel) were the **WRONG target** — advisor-steered profiling (race-free per-level + symbolic sub-phase profilers) showed Cerid's NUMERIC factor already BEAT CHOLMOD's (hood 224 vs 305ms); the **entire gap was the SERIAL SYMBOLIC** running 4.4× CHOLMOD's analyze. **FACTOR fixes (byte-identical, moat held): (1) `build_adjacency` rewritten O(nnz) counting-sort transpose + per-vertex merge-dedup (was O(nnz·log deg) introsort); (2) `symbolic_factorize(…, supernodal_patterns=true)` builds per-fundamental-supernode LEADING-column patterns (`SymbolicFactor::slead_ptr/idx`) via the assembly-tree union, skipping the O(nnz(L)) simplicial `li` the supernodal factor never needs** (full-`li` path kept for the general API). 8T vs CHOLMOD (matched OpenBLAS, WSL): **FACTOR hood 0.85→1.33× · ldoor 0.82→1.28× WIN; bmwcra 0.68× (ADR-0082 intrinsics gemm ceiling — 2 wins not 3).** **SOLVE parallelized (measure-first each step, level-parallel, bit-identical): batched multi-RHS diagonal block (nc≥48, c-contiguous, DIVIDE-not-reciprocal) + level-parallel BACKWARD (left-looking, descending) + level-parallel FORWARD (right-looking scatter races → reformulated LEFT-looking gather Σ_{k∈upd_list[s]} L_{s,k}·Y_k, race-free, ascending). FORWARD is TWO-PATH: nw≤1 RIGHT-looking serial (few big gemms) / nw>1 LEFT-looking parallel (the left-looking SERIAL regressed 1T x16 ~15-20%, advisor-predicted + bench-confirmed).** 8T SOLVE x16 vs CHOLMOD: **hood 0.87→1.69-1.81× · ldoor 1.06→1.84-1.98× WIN (~doubled the lead); bmwcra 0.73→0.85× (gemm ceiling).** Determinism: bit-identical L AND x across {1,2,4,8} + serial↔parallel solve at nw∈{1,2,3,pool} (new `solve_with_workers(nw)` hook + test; scratch POOL-sized per `jobs-worker-index-aliasing`). VERIFIED MSVC **548751 asserts / 21 cases** + gcc -Werror + clang-cl /WX + ctest guards + clang-format + **win-asan (gather OOB-clean)** + clang-tidy (lib+tests); the proper close gate FIXED 3 latent cluster failures binary-direct verify missed (non-ASCII `ᴴ` test name, clang-cl unused-lambda-capture, 7 pre-existing factor tidy issues). See `project_symbolic_is_the_cholmod_gap`. **HONEST: NOT "solve crushed" — multi-RHS solve WINS hood/ldoor; v5a-5 below names what still loses.** | ~750 | (21 suite) | — |
| ↳ **v5a-5** ✅ 2026-05-30 — **parallel single-RHS solve** | Level-parallel forward/backward extended to the nrhs==1 path (same left-looking gather, per-RHS scratch — the lever that crushed multi-RHS). Bit-identical {1,2,4,8}; gated ≥24M lnz (small factors regress hard on per-level dispatch overhead — conservative fill cut; a per-level work gate is the future refinement). Session `…-v5a-5-single-rhs-parallel-solve.md`. | ~? | (suite) | — |
| ↳ **v5a-6** ⏸ 2026-05-30 — **per-level solve work-gate REFUTED + reverted** | Hypothesized a per-level work threshold to capture the mid-range the v5a-5 fill-gate misses. Swept it: **T=0 (always-parallel) is best** — the single-RHS solve gap is **memory-bandwidth-bound, not dispatch-overhead-bound** (parallel doesn't help because the scatter is bandwidth-limited, not because of fork cost). Reverted; the bandwidth wall is characterized, not a dispatch lever. Session `…-v5a-6-perlevel-gate-refuted-bmwcra-diag.md`. | 0 (reverted) | 0 | — |
| ↳ **v0d-asm detour** ⏸ 2026-05-30/31 — **hand-tuned GEMM asm INVESTIGATED → REVERTED (intrinsics vindicated)** | ADR-0088 re-opened ADR-0082's asm question. Built the whole thing — dual-syntax MASM/GAS f64 6×8 kernel + runtime CPUID dispatch + build integration, all **bit-identical to the intrinsic**, green MSVC/clang-cl/gcc. Then MEASURED it (same-process A/B, identical clock): **asm ~1–2% SLOWER** — the intrinsic **inlines** into `gemm_packed_inner` at ~98% peak; hand-asm is a hard call that can't inline across TUs. Earlier "+10–16% IPC" was a turbo-clock artifact. **ALL reverted; ADR-0082 (intrinsics-first) STANDS, vindicated.** Record (do-not-re-try): ADR-0088 (Reverted) + `docs/phases/hesap-v0d-asm-microkernel-plan.md`. | 0 (reverted) | 0 | — |
| ↳ **v5a-7** ✅ 2026-05-31 — **🎉 WITHIN-FRONT PARALLELISM — bmwcra (the lone CHOLMOD loss) CROSSED INTO A WIN; EVERY matrix now beats CHOLMOD** | The bmwcra factor scaling gap was DECISIVELY measured (clean WSL, refuted the stale "2.01× plateau" framing): gap WIDENS with threads (1T 0.77→8T 0.65×); scale-profiler localized **~70% of the loss in ~23 huge near-root fronts** (cmod scales 1.86×, cdiv 1.20×); **ob_probe proved it is NOT a bandwidth wall** (OpenBLAS-threaded does the exact cmod/cdiv shapes at 4.5–6.8×, Cerid in-situ 1.86×/1.20× = fork/join + scheduling deficiency, FIXABLE). Three bit-identical sub-slices: **(1) cmod ROW-slab** — node-parallel huge-front cmod replaced per-descendant `gemm_parallel` (fork/join per call → 1.86×) with ONE `parallel_for` over the front's ROWS (contiguous pr-range, `lrm` monotonic; advisor-corrected from a wrong column-axis), cmod 1.86→**4.46×**; **(2) cdiv targeted serialization** — the cdiv "chain" INFLATED 223→375ms@8T (small per-block fork/join: A2/A3 ≤192×64 gemms + B1 apply ≤64 cols), forced those SERIAL (kept large B1/B2/C-trail parallel) → chain back to 225ms; **(3) cdiv B-block ROW-slab** — A1-floor probe proved the genuinely-serial POTF2 floor = **2.6ms (≈0)** ⇒ green-light; row-slabbed the B below-outer work over `below_o` rows (one fork/outer-block, sequential jb-walk per lane, bit-identical) → `rest` 220→~100ms. **FINAL (clean WSL 8T, 2 clean runs, matched OpenBLAS): bmwcra 0.65→1.04–1.05× WIN · hood 1.33→1.48× · ldoor 1.28→1.49–1.76× · bcsstk25 1.41→1.75× — EVERY matrix beats CHOLMOD, with the cross-thread bit-determinism moat none of the gold-standard peers carry; NO regression.** Determinism moat GREEN (591348 asserts / 22 cases bit-identical {1,2,4,8}). **HONEST: bmwcra's win is MODEST ~4–5% (per-thread intrinsic-gemm ceiling per ADR-0082 keeps it modest; parallel scaling flipped it).** Load-imbalance polish (`num_jobs=par_workers*4` over-decompose) was tried + measured NULL on the real all-matrix workload (helped only the cold-start bmwcra-only confound) ⇒ REVERTED; residual `rest` 2.2× (vs cmod 4.46×) = row-slab load imbalance whose genuine fix is guided/work-stealing chunking = a CHARACTERIZED future lever (+ starved/setup ~9% each), not chased (bounded-importance, already winning). Owed before phase commit: clang-cl/asan/tidy/win-shipping + ctest guards (win-debug+gcc green). Session `…-v5a-7-within-front-cmod-rowslab.md`. | ~190 | (22 suite) | — |
| ↳ **v5b-1** ✅ 2026-05-31 — **Gilbert-Peierls left-looking LU — the SERIAL correctness oracle** | Faithful CSparse `cs_lu`: DFS-reachability symbolic (`cs_reach`/`cs_dfs`, clean `marked` array — no Lp-sign-flip) + sparse `L\A(:,k)` solve in topological order + DYNAMIC partial pivot (threshold knob, tol=1 default) → P·A = L·U in CSC; solve = P·b → unit-lower forward → upper backward, multi-RHS. f32/f64/c32/c64 (`SparseLU<T> : IFactorization<T>`, `factor_gp_lu`). **SERIAL by construction** (no `num_workers`/`parallel_for` — dynamic pivot is order-dependent ⇒ NOT the {1,2,4,8} moat; the deterministic+parallel LU is v5b-2 via MC64+threshold). **9 tests / 81 asserts** (advisor-hardened: known-x_true residual + 2D-grid + pivoting [3×3 + heavy reverse-perm at scale] + singular [empty-col + numeric-zero `a_max≤0`] + complex + multi-RHS + run-to-run determinism; capacity-grow verified via the grid's heavy fill, `resize_uninitialized`→`reserve` preserves). **CLI** `hesap.direct.lu_gp.{f32,f64,c32,c64}` (user-directed: expose the oracle; +2 CLI tests). **Bench** `bench_hesap_lu_vs_reference` vs Eigen SparseLU (NaturalOrdering, same AMD-permuted matrix) — HONEST oracle result: **residual matches Eigen + fill ≈ identical (1.38M/1.40M, 4.37M/4.37M) but loses factor time 5–9× (0.21×/0.11×) BY DESIGN** (serial CSparse reference; crush is v5b-2). Bench small-corpus-only (bcsstk13/24): no column reorder ⇒ AMD-symmetric doesn't bound LU fill ⇒ bcsstk25 15k balloons multi-GB (left to v5b-2 COLAMD). Verified: win-debug (full suite 591429 asserts / 31 cases, no regression) + gcc -Werror clean (sign-conversion checked) + bench validated (CRD_BUILD_HESAP_VS_REFERENCE=ON). Owed before v5b phase close: clang-cl + ctest guards. | ~430 | 11 (suite) | — |
| ↳ **v5b-2** ✅ NUMERIC + TREE-PARALLEL + COMPLEX DONE (CLI `hesap.direct.lu.*` = the one owed gap) — **Supernodal LU (SuperLU-class) + MC64 threshold STATIC pivoting (deterministic, parallel); the LU crush.** *(v5b-2a/2b/2c ✅ below; v5b-2d tree-parallel ✅ — moat test "v5b-2d SupernodalLU: tree-parallel factor bit-identical across {1,2,4,8}" green; c32/c64 ✅. v5b-2e CLI registration is the remaining sub-slice.)* **v5b-2a ✅ 2026-05-31 — deterministic static-pivot FRONT-END** (`supernodal_lu.hpp/.cpp`: `static_lu_prepare` builds B = perm_cols(D_r·A·D_c) via MC64, matched entry on the diagonal; `StaticLuScaling::transform_rhs`/`untransform_solution` the solve chain; `min_diag_dominance` metric). 4 tests/8 asserts (MSVC) — VALIDATED end-to-end via residual on the ORIGINAL A (factor B with the v5b-1 GP-LU oracle): clean unsymmetric + **MC64 rescues tiny-diagonal (dom>0.9, the static-pivot enabler)** + MC64 balances badly-scaled (10⁶ span) + complex. The oracle-gate de-risks the perm/scale composition. **v5b-2b ✅ 2026-05-31 — column-etree + supernodal-LU SYMBOLIC (the EXACT static-pivot L/U structure, materialised up front).** Advisor caught a SuperLU-vs-SuperLU_DIST conflation (sequential SuperLU discovers structure DURING the numeric ⇒ not thread-deterministic; MC64+static = SuperLU_DIST ⇒ structure UP FRONT = the v5a moat pattern). `crd-hesap-ordering`: `column_elimination_tree` (cs_etree ata=1, prev[row], AᵀA never formed) + `column_counts_ata` (cs_counts ata=1 + init_ata row-merge) = column etree of B + chol(BᵀB) Gilbert-Ng fill bound (reusable by v5c QR). `crd-hesap-direct` `lu_symbolic.hpp/.cpp`: pattern-only GP reachability (value-free twin of `lu_dfs`, pinv=identity) → exact per-column L (`lp/li` diag-first) + U (`up/ui` diag-last) sorted ascending + relaxed supernodes (Liu-Ng-Peyton on col-etree) + `fill_bound` reserve. chol(BᵀB) = oracle+reserve only (NOT the structure factored into; here fill=flops). VERIFIED: explicit-struct(BᵀB) oracle for etree/counts + superset (GP-LU(B,tol→0) fill ⊆ symbolic, exact nnz eq) + sandwich + supernode panel-density + canonical-order + determinism; win-debug (ordering 7771/44, direct 591487/41) + guards + clang-cl + gcc -Werror + format; fixed 2 latent clang-cl unused-lambda-captures in v5b-1/v5a-2. Session `docs/sessions/2026-05-31-hesap-v5b-2b-column-etree-lu-symbolic.md`. **v5b-2c ✅ + CRUSH (2026-05-31): supernodal BLAS-3 numeric + 5 crush levers — STRUCTURED-MATRIX CRUSH vs Eigen DEFAULT (COLAMDOrdering, fair best-vs-best): memplus 8.7× · wang3 3.5× · af23560 1.04× · sherman3 1.02× WIN; gemat11 0.89× / add32 0.90× competitive.** Levers (all moat-bit-identical {1,2,4,8}, full suite 591526 green): (1) SuperLU-style **relative-indexed COMPACT panel** (replaced the cache-hostile n×max_nc global SPA; GEMM probe proved 51 GFLOP/s isolated vs 15.6 in-factor = cache pollution); (2) **fpos-hoist** (foot-scatter de-indirection); (3) **blocked diagonal LU** (BLAS-3 Schur, af23560 diag 1197→223ms); (4) **adaptive active-panel WIDTH cap** (`lu_panel_wcap`; Eigen bounds its panel to 16 — read its source); (5) **height-gated PADDING** (`eff_relax = height≥64 ? nrelax : 0` — measured structural fill = Eigen's; the circuit excess was ALL explicit-zero BLAS-3 padding). HONESTY: caught + fixed the bench crippling Eigen with NaturalOrdering (the "26×" was a peer-handicap artifact; fair COLAMD = above). **🚨 CRITICAL (testing the REAL sim targets): the static-pivot LU gives WRONG answers on SADDLE-POINT Navier-Stokes — garon2 resid 1.9e5, raefsky3 1.4 (bench now flags INACCURATE); definite ns3Da fine. The moat (static pivot) is UNSAFE on indefinite systems.** **⚠ REVERSED by v5b-3d (2026-06-01): this was WRONG — it was MC64's *scaling* destabilising the saddle-point systems, NOT static pivoting. Dropping MC64 (adaptive natural-diagonal first) FIXED them: garon2 1.7e-12 [ok], raefsky3 6.9e-09 [ok], both now WIN vs UMFPACK. Static pivoting on the natural diagonal is safe here; the "needs dynamic-pivot/Schur" conclusion below is superseded. See the v5b-3 row + memory `project_lu_umfpack_gap_is_mc64_not_gemm`.** ⇒ SIM-TARGET STRATEGY (original, partially superseded): cloth/deformation/fluid-pressure-Poisson are SPD → **Cholesky** (v5a, unconditionally stable, ALREADY CRUSHES CHOLMOD hood 1.33×/ldoor 1.28×) = the moat's stronghold + the bulk; monolithic saddle-point NS needs dynamic-pivot/Schur (research-FEM edge; projection-fluid avoids it via SPD pressure-Poisson). Memory: `project_hesap_simulation_target_and_gold_standards` + `project_lu_crush_two_kernel_plan`. **NEXT: push Cholesky-vs-CHOLMOD margin on deformation matrices · UMFPACK/PARDISO/MUMPS gold-standard comparisons · deterministic-parallel scaling · iw-GC · saddle-point fallback.** | **Architecture (the moat through pivoting — the hard part):** v5b-1's dynamic pivot is order-dependent ⇒ not parallel-deterministic. Use **MC64 + threshold STATIC pivoting** (SuperLU_DIST): MC64 (`mc64_match_and_scale` ✅ v4j-1a → colperm + dr/dc) permutes large entries onto the diagonal + scales toward an I-matrix ⇒ diagonal large ⇒ threshold partial pivot accepts the diagonal in ~every column ⇒ **the pivot sequence is fixed by the symbolic+MC64 phase, NOT dynamically ⇒ parallel factorization is bit-deterministic {1,2,4,8}** (the moat). Static pivoting trades a little stability ⇒ recover with **iterative refinement** (Demmel GESP); tiny pivots get a deterministic √ε·‖A‖ perturbation. Column order = AMD(A+Aᵀ) (`amd_order` ✅; COLAMD-equivalent). **Sub-slices:** **v5b-2a** deterministic front-end (MC64 match+scale + AMD col-reorder + IR, validated vs the v5b-1 GP-LU oracle); **v5b-2b** column-etree (etree of AᵀA) + supernodal LU symbolic (NEW — the symmetric symbolic.hpp etree/supernodes are Cholesky-only); **v5b-2c** supernodal numeric serial (left-looking, BLAS-3 panels — reuse the Cholesky cmod machinery + the L/U split); **v5b-2d** tree-parallel + determinism moat (reuse v5a-3 level-scheduling + v5a-7 within-front row-slab; deterministic *because* pivoting is static); **v5b-2e** complex + CLI `hesap.direct.lu.*` + the CRUSH bench vs Eigen SparseLU + UMFPACK at scale (hood/ldoor + unsymmetric circuit/CFD corpus). **Reuse from v5a:** panel-BLAS, level-scheduling, row-slab, determinism approach. **New:** column-etree symbolic, L/U split, MC64 static-pivot integration, IR. Validated against the v5b-1 oracle. | ~600 | ~18 | — |
| ↳ **v5b-3** ✅ LANDED (serial + deterministic-parallel; moat-proven) 2026-06-01 — **SYMMETRIC-PATTERN (MUMPS-style) MULTIFRONTAL LU.** **WHAT LANDED (all bit-identical/moat-safe; suite 591604/54 + ordering 7771 + iterative 112609 green; gcc -Werror clean):** full `MultifrontalLU<T>:IFactorization` (f32/f64/c32/c64) — `build_symmetric_multifrontal_symbolic(B)` (chol(B+Bᵀ) supernode fronts) → postorder front walk: scatter B + IN-PLACE extend-add children's Schur (`mf_extend_add_trailing`, no copy) + `factor_front` (blocked partial-LU, static MC64 pivot + GESP, rank-nb TRSM + `dl::gemm`) + store L/U to CSC + stagnation-fixed IR solve. **TREE-PARALLEL (v5b-3c): level-scheduled `parallel_for` over the assembly tree + WITHIN-front parallel GEMM (`gemm_parallel_auto`) on big fronts + depth-1 LOOKAHEAD in `factor_front` — DETERMINISM MOAT PROVEN: L,U BIT-IDENTICAL across {1,2,4,8} workers AND ==serial (`[v5b-3c]`, incl. a 260×260 dense-front case).** Levers L1–L5 (MC64 Duff-Koster dual-init + precompute-log; extend_add reusable-scratch+cache; CB pool; in-place Schur) + serial-fallback + uninit-resize (af23560 serial ~256→183ms). **HONEST SCOREBOARD (fair, matched accuracy): vs UMFPACK-1thr (its real best — it is SERIAL, does NO MC64): CIRCUIT gemat11 1.45×/memplus 1.33× WIN; CFD sim targets af23560 0.71/wang3 0.80/ns3Da 0.72 (1w) = AT PAR cold — our numeric 330ms BEATS UMFPACK's 556ms on ns3Da (MC64 buys the cheap numeric; we lose the warm best-of-N only on warm-up/alloc). vs MUMPS @8t (the PARALLEL gold standard, installed libmumps-seq): af23560 1.16× WE BEAT IT (non-asterisk crush) · wang3 0.88× · ns3Da 0.64× (MUMPS wins big-front via async-DAG + node-2D parallelism = the diagnosed next levers).** READ UMFPACK source (no MC64; dynamic pivot ⇒ expensive numeric) ⇒ confirmed the static/MC64 architecture is the RIGHT tradeoff (cheap numeric + the moat); KEPT it (user decision — the determinism moat is the differentiator none of UMFPACK/PARDISO/MUMPS have). NEXT (to win ns3Da/wang3): async task-DAG scheduling + node-level parallelism. MUMPS bench is now the permanent honest yardstick (`CRD_BUILD_HESAP_VS_MUMPS`). Memory: `project_lu_umfpack_gap_is_mc64_not_gemm` (full session arc). **(historical detail below)** **UMFPACK CONQUEST (WSL, fair, 2026-06-01):** ran `bench_hesap_lu_supernodal_vs_reference` with `CRD_BUILD_HESAP_VS_UMFPACK` (SuiteSparse 7.6.1). **Caught + corrected a fairness trap** (apt libumfpack→OpenBLAS-pthread oversubscribes 32 threads on UMFPACK's small fronts ⇒ af23560 9.3s garbage; `OPENBLAS_NUM_THREADS=1` is its real best — 1 beats 2/4; the transient "16× crush" was 100% that artifact, discarded). **HONEST fair scoreboard (Cerid serial vs UMFPACK-1thr): UMFPACK CRUSHES Cerid on every structured/sim factor — af23560 0.39× · ns3Da 0.16× · wang3 0.43× · sherman3 0.52× (~2.5–6×); Cerid wins only small CIRCUIT (memplus 1.22× · add32 1.27× · gemat11 1.10×).** Root cause = UMFPACK's MULTIFRONTAL dense-front BLAS-3 beats BOTH Cerid AND Eigen (both supernodal left-looking) ~2.6× — ARCHITECTURAL, not tuning; amortized factor+N·solve never closes. **SOLVE STAGNATION FIX LANDED (correct, matched-accuracy, in `supernodal_lu.cpp::solve`):** measured the af23560 IR trajectory (drops to ~1.7e-14 by step 3 then FLATLINES at the round-off floor = refine_tol 64·eps, burning all 8 IR steps = the 10× solve) → Demmel-Li GESP **stagnation guard** (break when `rn≥0.5·prev_rn`, leaving converged=false so the accept_tol(√ε) recheck still flags divergence): **af23560 solve 95→38ms (2.4×), resid 1.0e-14 (better than Eigen 3.3e-14); garon2/raefsky3 STILL INACCURATE (saddle-point safety held); all others unchanged.** (gcc -Werror also caught+fixed a real forward-ref: `trsm_unit_lower_left` used in the blocked `dense_lu_nopivot` before its definition — MSVC's lax two-phase lookup missed it.) **PLAN (multi-session, build FRESH per clean-structure mandate; moat-compat via MC64 STATIC pivot = SuperLU_DIST model ⇒ front structure fixed by the symbolic phase ⇒ bit-deterministic across workers):** **v5b-3a** SKELETON — assembly tree from the existing LU column-etree (`lu_symbolic`) + frontal-matrix abstraction + the contribution-block STACK + `extend_add` (the `Frontal<T>`/`extend_add` substrate from v5a-1 is reusable); validate STRUCTURE vs the supernodal symbolic (no numeric). **v5b-3b** DENSE FRONT FACTOR — partial dense LU of each front's fully-summed block (BLAS-3 + MC64 static pivot) → contribution block (Schur) to parent = THE crush kernel. **v5b-3c** RELAXED FRONT AMALGAMATION (merge small fronts for bigger BLAS-3). **v5b-3d** PARALLEL assembly tree (independent subtrees per worker; bit-identical = the moat). **RESEARCH FIRST:** SuiteSparse UMFPACK source (umf_kernel/umf_assemble/umf_local_search/umfpack_numeric) + Davis&Duff multifrontal papers (Davis 2004 column-preorder UPM). New numeric behind `IFactorization<T>`, dispatcher selects multifrontal vs the supernodal `SupernodalLU` per matrix. The existing front-end (MC64+AMD-reorder), `lu_symbolic`, and the (now-stagnation-fixed) solve all stay. Memory: `project_lu_crush_two_kernel_plan` (UMFPACK CONQUEST + FORK DECIDED). **v5b-3d NODE-LEVEL + ADAPTIVE MC64 (2026-06-01, WIP not committed):** advisor-steered to node-level (NOT async-DAG — for 3D the root separator front holds a constant flop fraction ⇒ tree/async can't split it; measured big-front path = 74% of numeric, 1.87× scaling). Landed (all moat-safe, suite 591617/54): parallel TRSM + wider panel nb=128 + ⭐ ADAPTIVE MC64 (natural-diagonal first, MC64 fallback on element-growth blow-up; moat = STATIC pivot not MC64 ⇒ preserved). Drops the ~100ms MC64 tax on strong-diagonal CFD AND **fixes the saddle-point matrices** (garon2/raefsky3 INACCURATE→ok — MC64's scaling was *causing* it; reverses the v5b-2c saddle-point conclusion). HONEST 3× vs-MUMPS @8t (warm, OPENBLAS=1, matched acc): af23560 **1.04–1.06× WIN** · wang3 0.94–0.98× · ns3Da **0.64→0.93–0.95×** (near-parity) + the determinism moat + saddle-point fixed. **v5b-3e CRUSH (2026-06-01): +Step 1a parallel assembly (zero-fill + extend-add) and Step 2 — the front-parallel threshold was too conservative (`cnt >= sw`); measured sweet spot ≈ sw/2 flipped both targets WITHOUT the planned malleable-2D rewrite. FINAL: af23560 1.12–1.20× WIN · wang3 1.01–1.11× WIN · ns3Da 0.99–1.01× TIE — win/win/tie vs MUMPS + the moat + saddle-point fixed (HONEST: wang3 bounces, ns3Da is a tie not a win, af23560 still loses UMFPACK-serial 0.86×). Throwaway profiling cleaned; suite 591617/54 gcc-clean. OWED: CLI `hesap.direct.lu.*` + dispatcher + per-slice DoD.** Moat re-verified on the new paths (512-wide front trips parallel TRSM + n=96 fallback matrix at {1,2,4,8}). Memory `project_lu_umfpack_gap_is_mc64_not_gemm`; session `docs/sessions/2026-06-01-hesap-v5b-3d-node-level-and-adaptive-mc64.md`. | ~900 | ~24 | — |
| ↳ **v5c-1** ✅ COMPLETE 2026-06-02 (cluster local-close = v5c-close ↓) **Multifrontal QR (SPQR-class)** (sub-slices a–g, 2a–2c ↓) | The sparse-direct QR `A·P_c = Q·R` (`MultifrontalQR<T>:IFactorization`, f32/f64/c32/c64). Core (symbolic + numeric + blocked-WY + square/LS solve) verified **win-debug 14 cases / 5353 asserts + all 5 ctest guards + clang-cl clean**. **CRUSHES Eigen SparseQR on BOTH domains (rectangular LS 7–37× · square 13–71× factor, less fill, better accuracy)** and **CRUSHES SPQR's solve 4–8×**; **SPQR FACTOR: well/illc1033 WIN + 1850 near-parity + bcsstk square-SPD characterized loss (ADR-0082 wall, wrong-tool-for-QR)** — a split, the determinism moat is the differentiator (closed via v5c-1e AᵀA-free symbolic → v5c-1f scatter-map → v5c-1g tree-parallel moat → v5c-2a/b/c complex/rank-reveal/CLI). Premise-check SAVED ~600 lines (the bcsstk25 1.5 GB stall was a wrong-tool square-SPD artifact, not a QR bottleneck). Session `docs/sessions/2026-06-02-hesap-v5c-1-multifrontal-qr.md`; dossier `docs/research/cerid-hesap-v5c-multifrontal-qr.md`. | ~1500 | ~30 | — |
| ↳ **v5c-1a** ✅ 2026-06-02 — **QR symbolic (front structure)** | `QrSymbolic` in `multifrontal_qr.{hpp,cpp}`: **the QR fronts ARE chol(AᵀA) supernodes** ⇒ REUSE the proven v5a `symbolic_factorize`+`build_supernodal_symbolic` on the AᵀA pattern (correctness-first; the implicit AᵀA-free merge is v5c-1e). Front tree + per-front pivot/contribution columns + leftmost-column row merge (`sleft`/`row_by_leftcol`, empty rows parked). Validated standalone: AᵀA two-path cross-check (Σ colcount + etree **bit-identical** vs the implicit `column_counts_ata`/`column_elimination_tree`), front-partition tiling, **contribution⊆parent (the assembly precondition)**, postorder validity, symbolic determinism. Advisor-key: `struct(R)⊆struct(chol(AᵀA))`, equality only strong-Hall ⇒ assert `⊆` not `==`; the cross-check is two-PATH, not a tightness claim. | ~230 | 5 | — |
| ↳ **v5c-1b** ✅ 2026-06-02 — **QR numeric (assembly + partial Householder QR)** | `MultifrontalQR<T>:IFactorization` (f32/f64): postorder **scatter-PLACE / row-append** assembly — NOT the symmetric v5a `extend_add`: QR contribution-block ROWS append, COLUMNS map ⊆ (so a COLUMN-MAJOR front, contiguous for `make_householder`) — then partial Householder QR (npiv=min(nc,fm) reflectors) → global R (CSR) + stored H/taus. Verified **‖RᵀR−AᵀA‖≈0** (sign- and identity-P_c-robust) on square/rectangular/banded full-rank. `extend_add` reserved for v5d (symmetric, rows match by id). | ~300 | 3 | — |
| ↳ **v5c-1c** ✅ 2026-06-02 — **QR solve (square + least-squares)** | Implicit Qᵀ-apply by RE-WALKING the assembly tree in the SAME canonical row order (own rows from `row_by_leftcol`, then children in front-tree order — **no HPinv array, provenance is in the symbolic**) carrying a per-front row workspace, then back-substitute the CSR R. `solve` asserts m==n; `least_squares(b /*m*/, x /*n*/, nrhs)` for m≥n; `n()`=column count. Verified square + over-determined-consistent recover x_true, **least-squares optimality Aᵀ(Ax−b)=0**, multi-RHS. The fixed row-stacking order = the determinism moat for free. | ~200 | 4 | — |
| ↳ **v5c-1d** ✅ 2026-06-02 — **blocked-WY + the FULL benchmark scoreboard** | Size-gated compact-WY front factor: large fronts (≥64 rows) panel the npiv pivots into nb=48 sub-panels + ONE BLAS-3 `larfb` (`C:=(I−V·Tᵀ·Vᵀ)·C`) via the column-major `dense::gemm` + `build_block_t_from_vtv` (dlarft); small fronts keep the unblocked path. **Eigen SparseQR CRUSHED both domains** (`bench_hesap_qr_vs_reference`): rectangular LS (well/illc 1033/1850) **7–37×** · square (bcsstk13/24) **13–71×** factor, 0.07–0.46× R fill, better accuracy. **SPQR gold standard** (`bench_hesap_qr_vs_spqr`, WSL serial-fair): **SOLVE crushed 4–8×, FACTOR loses 0.44–0.86×** (SPQR's mature LAPACK blocked-QR 1.2–2.5× faster; resid matches). Verified win-debug 14 cases / 5353 asserts (+ a dense-150×100 3-sub-panel test) + clang-cl. **Premise-check SAVED ~600 lines**: the front-stack/AᵀA-free levers were motivated only by the wrong-tool bcsstk25 (square SPD, 1.5 GB) — QR's real LS domain factors in 0.4–1.3 ms ⇒ not the tractability bottleneck. | ~250 | (suite) | — |
| ↳ **v5c-1e** ✅ 2026-06-02 — **AᵀA-free implicit symbolic (oracle-safe; all 6 factor ratios improved; NO flip)** | New `ordering::symbolic_factorize_ata(A)` — etree + counts + per-supernode leading patterns (`slead`) of chol(AᵀA) **without forming AᵀA**: reuses the proven implicit `column_elimination_tree` + `column_counts_ata` (bit-identical at v5c-1a) + `fundamental_supernodes_i32`, emits `slead` via the same assembly-tree recurrence as `symbolic_factorize(…,true)` with `adj_AᵀA(fc)` gathered IMPLICITLY from A's rows. `multifrontal_qr_symbolic` calls it; `ata_pattern`+explicit path kept as the **bit-for-bit oracle** (new `[v5c-1e]` test, 4 shapes, asserts parent/post/colcount/lp/super/slead_ptr/slead_idx identical). **APPROACH DECIDED BY MEASUREMENT** (advisor caught the trap): a 3-bucket symbolic sub-split showed the **clique-union DOMINATES** (66%/86% on well1033/bcsstk13) while `build_adjacency` (all "Option A" removes) is 7–9% ⇒ Option A would NOT help; the true leftmost-merge was required. Implicit etree+counts measured cheap (bcsstk13 clique-union 10.9 ms → 0.40 ms, 27×); the adj-gather did NOT leak p² (bcsstk13 symbolic 12 → 1.36 ms). **VERIFIED:** win-debug full suites (hesap-direct 597051/72, hesap-ordering 7771/44) + 5 guards + clang-cl + gcc `-Werror`. **HONEST scoreboard vs SPQR (WSL serial-fair, ×2): all six factor ratios IMPROVED — well1033 0.82→0.91–0.94× · illc1033 0.80→0.90–0.91× · well1850 0.60→0.64× · illc1850 0.60→0.67× · bcsstk13 0.40→0.50× · bcsstk24 0.48→0.63×; solve still crushes 3.4–8.5×; residuals match — BUT the predicted well/illc1033 FLIP did NOT land.** cerid *numeric* alone (0.21) beats SPQR *total* (0.25), but the residual ~0.07 ms symbolic (now `build_supernodal_symbolic`+row-merge+slead, NOT the AᵀA tax) + a faster SPQR sample keep cerid total at 0.28 > 0.25; the ~0.03 ms gap is **at the run-to-run noise floor** (don't chase — characterized as at-parity, num-competitive). The dossier's "flips well/illc1033 by itself" was optimistic (cerid-num < a stale 0.27 SPQR; ignored the residual-symbolic floor). bcsstk13/24 + well/illc1850 stay **numeric-bound** = v5c-1f's staircase/blocked-GEQRF battlefield. | ~290 | 1 (+oracle) | — |
| ↳ **v5c-1f** ✅ 2026-06-02 — **assembly scatter-map → FIRST SPQR factor WINS (well/illc1033); home-turf near-parity** | THREE throwaway probes (measure-first), two refutations + one win: **(1) STAIRCASE REFUTED** — the probe `Σ(fm−k)(fsz−k)` vs `Σ(Stair_k−k)(fsz−k)` on the real assembled fronts came out the OPPOSITE of the predicted geometry: the matters-matrices sit at **1.08–1.16×** (< the 1.9× gate; only the noise-floor 1033s hit 1.70×) ⇒ the row-merge staircase (touches the solve's canonical order) is NOT worth it. **(2) PANEL BLAS-2 REFUTED** — the within-sub-panel BLAS-1 reflector loop is 66–87% of the kernel on rectangular-LS; converting it to LAPACK `dlarf` form (`dense::gemv`+`ger`) was **2–4× SLOWER** (per-call overhead on tiny panel blocks; the hand-rolled loop is already an inlined fused `dlarf`) ⇒ reverted. **(3) THE WIN = ASSEMBLY scatter overhead** — a same-platform (WSL/gcc) phase split {csr,assembly,kernel,rbuild} showed **assembly = 41% of the home-turf numeric (0.35 ms)** = the per-nonzero `find_col` BINARY SEARCH, and the SPQR deficit on well1850 was exactly ~0.37 ms ⇒ replaced `find_col` (O(log fsz)) with an **O(1) scatter map** (`col_pos` global→local, no reset — read only for cols ⊆ `fn`; debug assert guards the invariant). **HONEST SPQR scoreboard (serial-fair ×2, every ratio UP, zero regression): well1033 0.91→1.06–1.16× WIN · illc1033 0.90→1.12–1.17× WIN (🎉 FIRST SPQR FACTOR WINS) · well1850 0.64→0.87–0.96× · illc1850 0.67→0.93–0.95× (near-parity, was 0.60 at v5c-1d) · bcsstk13 0.50→0.71–0.76× · bcsstk24 0.63→0.91×; solve still 4.0–8.6× WIN; resid matches.** well/illc1850 bounce at near-parity (the spread IS the gap-to-1.0; residual = v5c-1e symbolic floor, checked NOT a `find_col` pattern, + a faster SPQR sample) ⇒ characterized, not chased. **bcsstk13/24 (square SPD = WRONG TOOL for QR) = characterized accepted loss: 83–88% larfb-gemm = the ADR-0082 BLAS-kernel wall** (intrinsics vs OpenBLAS); moat + solve-crush + Eigen-crush stand. **VERIFIED:** win-debug (5393 `[v5c]` / 597051+ suite) + clang-cl + win-asan (scatter map OOB-clean) + win-tidy (clang-tidy 20.1.8 — fixed 4 pre-existing v5c naming/style issues) + gcc `-Werror`. OWED (v5c completeness, separate): cross-front tree-parallel + determinism moat {1,2,4,8} (framed as completeness, NOT a crush answer — parallel-vs-serial-SPQR is the forbidden asterisk). | ~60 (scatter map + tidy) | (suite) | — |
| ↳ **v5c-1g** ✅ 2026-06-02 — **cross-front tree-parallel + the DETERMINISM MOAT** | Level-scheduled the assembly tree (`level[f]=1+max(child level)` via the `front_post` postorder; ascending-f per level ⇒ worker-order-independent) + refactored pass-2 into `factor_front(f,wk)` with **per-worker scratch** (vbuf/vtv/tblk/wbuf/col_pos, disjoint `worker_index()`-keyed slices sized by `jobs::num_workers()`) + per-level `jobs::parallel_for`+`wait`+`frame_reset` (serial when `num_workers≤1`); larfb gemms use `scratch=nullptr` (per-thread pooled GrowableTlsf, thread-safe + result-identical). Front-parallel only (NOT the LU hybrid — home-turf LS factors in <1 ms, no wall-clock to chase; parallel-vs-serial-SPQR is the forbidden asterisk). **Race-free (advisor discriminator): a front's `m_fb` is read ONLY by its direct `front_parent`** (contribution cols ⊆ direct parent fn) ⇒ the per-level `wait` barrier dominates all readers. **MOAT PROVEN (`[v5c-1g]`): R + Rj/Rx + the least-squares solution BIT-IDENTICAL across {1,2,4,8} workers at BOTH f32 and f64** (block-diagonal = 4 independent banded arms ⇒ ≥4 concurrent fronts ⇒ per-worker scratch genuinely exercised). No SPQR/Eigen carries cross-thread bit-exact factors = the standing differentiator. **VERIFIED 5 configs:** win-debug (597087/73) + clang-cl + win-asan (scratch OOB-clean under parallel) + win-tidy + gcc `-Werror`. HONEST: completeness + the moat, NOT a speed crush. | ~120 | 1 (suite) |
| ↳ **v5c-2a** ✅ 2026-06-02 — **complex QR (Complex32/Complex64), Qᴴ-apply, UNBLOCKED-only** | `make_householder_complex` + `qr_conj` (identity for real, `crd::hesap::conj` for complex) + `qr_from_real` bridges; the reflector apply (factor AND the solve's Qᴴ re-walk) is a single `if constexpr` path — complex dot = `qr_conj(v)·c` (vᴴc), scalar = `qr_conj(τ)`, v-tail update un-conjugated, R-diag = real β. **Real path BYTE-IDENTICAL** (qr_conj no-op for real ⇒ moat unchanged). Complex stays UNBLOCKED (`blocked=false`; larfb wrapped in `if constexpr(!is_complex)` so it never instantiates for complex) — blocked-WY-complex (VᴴV + ConjTranspose + conj-aware compact-WY T) is a PERF follow-on, not a deferred feature. **VERIFIED:** `[v5c-2a]` test RᴴR==AᴴA (genuinely-complex values ⇒ exercises the conjugation; would fail if Qᵀ/Qᴴ confused) + complex least-squares + complex moat {1,2,4,8}, for Complex32+Complex64; 5-config gate (win-debug 5529/17 + clang-cl + win-asan + win-tidy + gcc `-Werror` — fixed `R{double_literal}`→`static_cast<R>` narrowings). | ~120 | 1 (+moat) | — |
| ↳ **v5c-2b** ✅ 2026-06-02 — **rank-revealing (Heath), no pivoting** | NO column pivoting ⇒ preserves the fill order + the moat (SPQR's key trick). Detection = a pure function of the FINAL R diagonals (in no-pivot multifrontal QR a rank-deficient pivot just yields a ~0 R-diag — contribution block/assembly structurally unaffected ⇒ NO special-casing). SERIAL scan (r=0..n-1 ⇒ worker-invariant ⇒ moat-safe) → max|R diag|; `|R[r][r]| ≤ rcond·max` (rcond=max(m,n)·eps) or structural no-pivot ⇒ DEAD. `rank()`/`dead()` accessors; least-squares returns the **BASIC solution** (dead vars=0; back-sub skips dead rows). Dropped the old `m_info`-flags-deficiency path (m_info≡0; audited the lone reader ⇒ full-rank unchanged). **Advisor-corrected:** basic solution `‖Aᵀr‖≈0` only to O(tol·‖A‖); ≈eps only for EXACT deficiency ⇒ test uses an integer col literally = sum of two others (rank()==3, dead col flagged, x[dead]=0, ‖Ax−b‖&‖Aᵀr‖≈eps) + a no-false-positive case (1e-6 diag ≫ rcond·max stays live). Min-norm (sparse COD) = follow-on. **VERIFIED 5 configs** (win-debug 5542/19 + clang-cl + win-asan + win-tidy + gcc `-Werror`) **+ no bench regression** (SPQR full-rank residuals/ratios identical). | ~90 | 2 | — |
| ↳ **v5c-2c** ✅ 2026-06-02 — **CLI `hesap.direct.qr.{f32,f64,c32,c64}` + the complex-square factor FIX** | Registered the 4 QR commands in `cli_register_direct.cpp` (mirrors chol/lu; complex flattened {re,im}; schema rows≥cols, RHS b length m; uniform `least_squares({b,m},{x,n},1)` for square+rectangular — no aliasing). Output `[info, rank, x...]` (rank-revealing). +4 CLI tests. **⚠ The CLI complex-square test CAUGHT a latent v5c-2a FACTOR bug** (the complex unblocked path was only tested over-determined): the **len==1 last reflector** on an exactly-determined front hit `make_householder_complex`'s `n≤1` branch returning `β=Re(α)`, DROPPING the imaginary part of the last R diagonal (RᴴR err 0.25 on a 4×4). **FIX:** treat `len≤1` as a trivial reflector (tau=0, R[k][k]=colk[0] as-is) — correct + real BIT-IDENTICAL (moat preserved). Regression: a 4×4 complex RᴴR==AᴴA diagnostic + the CLI test. **VERIFIED 5 configs** (win-debug full suite 597256/81 + clang-cl + win-asan + win-tidy + gcc `-Werror`). **v5c-2 COMPLETE (2a complex · 2b rank-reveal · 2c CLI).** Owed for v5c-close: rank-deficient + complex bench vs Eigen/SPQR + the close audits. | ~150 | 4 | — |
| ↳ **v5c-close** ✅ 2026-06-02 (local close; CI sweep + system-doc/ADR → v5z) | **Completeness audit** (all 4 types f32/f64/c32/c64 × factor/solve/least_squares/rank/CLI/moat — full surface ✅; gaps deferred-not-hidden: blocked-WY-complex + min-norm rank-deficient = follow-ons). **Determinism moat extended to {1,2,4,8,16}** (R + Rj/Rx + solution bit-identical, real+complex; honestly pool-capped — proves worker-count INVARIANCE not 16-way concurrency). **HONEST FINAL SCOREBOARD:** vs Eigen CRUSHED both domains (LS 7–37× / square 13–71×); vs SPQR SOLVE crushed 4–8.6× everywhere, FACTOR well/illc1033 WIN + 1850 near-parity + bcsstk square-SPD characterized loss (ADR-0082 BLAS-wall, wrong-tool-for-QR, user-accepted) — a split, not a clean factor-crush; the determinism moat is the differentiator no SPQR/Eigen carries. **DEFERRED to v5z** (consistent w/ v5a/v5b): `docs/systems/hesap-direct.md`, ADR-0065 §27 D(direct) lock, all-families moat, rank-deficient+complex bench (honest scope: rank-agreement+factor/residual, not a min-norm crush). **The v5c COMMIT's real gate = the 18-config CI sweep** (the 6 slices only saw targeted per-module builds; CI proves win-shipping/release-LTCG/scalar/SSE2 + 7×Linux). | ~30 (moat ext) | (suite) | — |
| ↳ **v5d-a** ✅ 2026-06-02 — **LDLᵀ unit + symmetric multifrontal symbolic (skeleton)** | New `multifrontal_ldlt.{hpp,cpp}` + `build_ldlt_symbolic(a)` — the LDLᵀ front tree IS the chol(A) supernode tree, so it REUSES the proven v5b-3 `build_symmetric_multifrontal_symbolic` (+ `MfFront`/`mf_extend_add` for v5d-b). NO numeric (v5d-b). Structure test `[v5d-a]` (3 cases/103 asserts): front count, `check_multifrontal_containment` ok (the assembly precondition — a Cholesky theorem), SYMMETRIC front extent (row idx set == col idx set), pivot tiling, determinism. VERIFIED win-debug + non-ASCII guard + clang-cl + win-tidy + gcc `-Werror`. | ~30 | 3 | — |
| ↳ **v5d-b** ✅ 2026-06-02 — **per-front INDEFINITE Bunch-Kaufman factor (the only new v5d algorithm)** | New `dense_ldlt_kernels.hpp` `factor_front_ldlt<T>` (f32/f64): the COL-MAJOR, `npiv`-restricted analog of `dense::LDLT`/LAPACK xSYTRF (UPLO=Lower). **Lower-triangle, full m×m, symmetric rank-1 (1×1) / rank-2 (2×2) trailing update = the ~½-flop MA57-class form** (vs a both-triangles MVP — the perf the user won't let defer). Col-major lets it **drop ALL scratch buffers** (reads the still-original column k while writing disjoint cols j>k, normalizes k last ⇒ no hidden malloc). **Pivot contract (MA57, advisor-locked):** diagonal choices restricted to `[k,npiv)`; colmax/stability over the FULL column `[k+1,m)` (L21 reaches the CB rows); 2×2 partner < npiv; **NEVER pivots onto a CB row** — a fully-summed variable whose only stable pivot is a CB row / null column / has no 2×2 slot is DELAYED, returning the partial eliminated-count (Duff-Reid delay = v5d-c follow-on). Strict-`>` first-max tie-break = the moat invariant (v5d-e). **VERIFIED** win-debug (full suite 597426/91; 7 `[v5d-b]` cases): standalone col-major `swap_sym`, reconstruction `P·L·D·Lᵀ·Pᵀ==A` (forcing a 1×1 swap, forcing a 2×2, 5×5 mixed-indefinite, f32), Schur identity `npiv<m`, delayed-pivot partial-return. **VERIFIED 5 configs** — win-debug + clang-cl (150 asserts) + win-tidy (clean) + gcc `-Werror` (build + RUN, full suite 597426/91 == win-debug). **The gcc full-suite RUN caught TWO pre-existing bugs (fixed, not deferred):** (1) **a latent committed-v5b correctness bug** — `MultifrontalLU::factor_attempt` declared its `ThreadSafeAllocator ts` AFTER the `Array<MfFront>` `cb` whose fronts borrow `&ts` ⇒ reverse-dtor-order destroyed `ts` first ⇒ `~MfFront`→`Array::deallocate` on a destroyed-vtable allocator ⇒ gcc-DEBUG `pure virtual method called` (`test_cli.cpp:291`); MSVC + gcc-release silently tolerated the UB. gdb `break __cxa_pure_virtual` pinpointed it; fix = declare `ts` BEFORE `cb` (allocator outlives borrowers). CI `ctest --preset linux-gcc-debug` DOES cover this class — slipped because v5b's per-slice gcc step was build-only. See `feedback_container_allocator_must_outlive`. (2) **4 pre-existing v5b/v5c-test clang-tidy issues** surfaced by the first full win-tidy build (`rn`-confusable-with-`m` ×3, nested-ternary ×3 across `test_multifrontal_qr/cli/supernodal_lu`) — fixed (rename + if/else). | ~270 (+2 fixes) | 7 | — |
| ↳ **v5d-c** ✅ 2026-06-02 — **multifrontal LDLᵀ driver (postorder walk + symmetric lower-tri extend_add)** | `MultifrontalLDLT<T>:IFactorization` (f32/f64) + `factor_multifrontal_ldlt`. SERIAL postorder front walk: assemble A's lower triangle for each front's pivot columns + **symmetric lower-tri `mf_extend_add_trailing_sym`** (NEW — scatters only `a≥b`; the child Schur is symmetric ⇒ ONE monotone map, `a≥b ⇒ map[a]≥map[b]` lands in the parent lower triangle; in-place, no copy) → `factor_front_ldlt` (v5d-b) → store L (unit-lower multipliers, CSC) + D (block-diag, kept SEPARATE: 1×1 value / 2×2 {d11,d21,d22}) + block-local permutation P. **Advisor-keyed: NONE of v5b's `factor_attempt` apparatus transfers** (no MC64/growth/retry/level-scheduling — BK *is* the stability; serial). P = direct sum of per-front BK swaps (stay in the front's contiguous pivot range, the v5d-a invariant); L stored keyed by global id then **remapped to factor-position order** once P is complete (a CB row's factor position depends on its owning ancestor's swaps). Factors A AS GIVEN (no internal AMD — consumer applies fill order, like v5c). **REFUSE-ON-DELAY: `factor_front_ldlt` returning < npiv ⇒ `info()!=0` + abort (no CB emitted)** — the delayed pivot is the Duff-Reid follow-on. Allocator-lifetime rule followed (`ts` before `cb`). 7 `[v5d-c]` tests, validated by reconstruction `P·L·D·Lᵀ·Pᵀ == A[perm,perm]` (advisor-staged: multi-front diag-dominant no-swap P=identity · single-front dense indefinite 2×2+swaps+perm · multi-front indefinite dense-trailing npiv≥2 root · **cross-front swap exercising the CB-row remap** [advisor caught it as otherwise dead-under-test: a leaves-into-weak/strong-dense-root matrix `assert front_count>1 AND ∃ perm[i]≠i` so the one-piece-with-no-v5d-b-analog — CB-row remap through a non-identity ancestor perm — is genuinely run] · **forced-2×2 through the driver store** (advisor: the 2×2 D-store [m_doff + d11/d22 split] + blocksz=2 L-store [partner-row skip] were otherwise dead-under-test — all prior matrices took 1×1; a zero-leading-diagonal matrix forces a 2×2, `CHECK(∃ block_kinds[k]==2)`) · f32 · refuse-on-delay info!=0 · deterministic re-run). **VERIFIED 5 configs** — win-debug + clang-cl + win-tidy (clean) + gcc `-Werror` build **+ RUN** (full suite 597499/106). | ~380 | 8 | — |
| ↳ **v5d-d** ✅ 2026-06-02 — **L·D·Lᵀ solve (block-aware, multi-RHS)** | `MultifrontalLDLT<T>::solve` (f32/f64, was a v5d-c stub): `A·x=b` via `r=Pᵀ·b` (gather to factor order) → forward unit-lower `L·z=r` (CSC: column j propagates to rows i>j) → block-aware `D·w=z` (1×1 divide / 2×2 inverse-via-determinant) → backward `Lᵀ·y=w` → `x=P·y` (scatter to original). L's unit diagonal is implicit + the 2×2 coupling lives in D, so the L solves are plain unit-triangular. Multi-RHS = column-major n×nrhs, solved column by column. Returns false on an invalid factor (`info!=0`). 7 `[v5d-d]` tests = known-solution recovery (`b=A·x_true`, check `‖x−x_true‖`) + **direct residual `‖A·x−b‖`** (catches a backwards P/Pᵀ): multi-front diag-dominant · single-front dense indefinite · **cross-front swap (end-to-end perm through the solve)** · **forced-2×2 (the block-aware D-solve / determinant inverse, otherwise dead-under-test — `CHECK(∃ block_kinds[k]==2)`)** · multi-RHS nrhs=3 · f32 · invalid-factor→false. **VERIFIED 5 configs** — win-debug + clang-cl ([v5d] 223/25) + win-tidy (clean) + gcc `-Werror` build **+ RUN** (full suite 597499/106). `MultifrontalLDLT` is now a complete factor+solve `IFactorization`. | ~120 | 7 | — |
| ↳ **v5d-e** ✅ 2026-06-02 — **tree-parallel factorization + the cross-thread determinism MOAT** | `factorize(a, num_workers)` level-scheduled (`level[f]=1+max(child level)` via the postorder; ascending-f per level ⇒ worker-order-independent) + per-worker scratch (loc/bk/piv `wk`-keyed slices, eamap per worker; working front from `ts`) + per-level `jobs::parallel_for`+`wait`+`frame_reset` (serial when `num_workers≤1`, no jobs touched). **Front-parallel only** (mirror v5c-1g, NOT the LU hybrid). Race-free: each front writes ONLY disjoint global ranges (D/perm/block_kinds over `[c0,c0+npiv)`) + its own `cb[f]` (single-writer); `cb[child]` is read-only after its factor (`mf_extend_add_trailing_sym` is const-on-child) + read ONLY by its direct parent (after the level `wait`); the L21 columns are disjoint from the Schur the parent reads. **L-extraction is a SERIAL post-pass** (canonical CSC ⇒ no per-worker triplet buffers / alloc races; forecloses cb recycling = a deliberate memory follow-on). Refuse-on-delay via per-worker flags reduced after the levels (never write `m_info` from a worker). **THE MOAT INVARIANT: children extend-added in fixed `chld_idx` order, NOT completion order** ⇒ L,D,perm a pure function of the pattern. **MOAT PROVEN (`[v5d-e]`, advisor-guarded to not be vacuous):** a 12×12 BLOCK-DIAGONAL of 4 independent 2×2-forcing indefinite blocks ⇒ `REQUIRE(front_count≥4)` (4 CONCURRENT fronts) + `REQUIRE(∃ block_kinds==2)` (a 2×2 UNDER parallelism, not just all-1×1) ⇒ L (lp/li/lx) + D (dd/doff/block_kinds) + perm + the solution BIT-IDENTICAL across {1,2,4,8,16} workers AND vs serial, at f64+f32. **VERIFIED 5 configs** — win-debug + clang-cl ([v5d-e] 40/1) + win-tidy (clean) + **win-asan** (per-worker scratch OOB-clean under parallel) + gcc `-Werror` build **+ RUN** (full suite 597539/107). No SPQR/Eigen/MA57/MUMPS carries a cross-thread bit-exact LDLᵀ factor = the standing differentiator. | ~150 | 1 (suite) | — |
| ↳ **v5d-f** ✅ 2026-06-02 — **complex LDLᵀ (complex-symmetric) + LDLᴴ (Hermitian-indefinite)** | `factor_front_ldlt<T, bool Hermitian>` + `MultifrontalLDLT<Complex32/64>` (runtime `m_hermitian`, dispatched to `<T,true>`/`<T,false>`). **Two genuinely-distinct algorithms on a shared skeleton** (advisor): Hermitian=false → A=P·L·D·Lᵀ·Pᵀ (unconjugated, D complex); Hermitian=true → A=P·L·D·Lᴴ·Pᵀ (conjugated, D **real-diagonal**). They differ at: the trailing-update second factor (`ldlt_conjh`), the D 1×1 (forced real via `ldlt_pivd` for Hermitian) + the 2×2 inverse (`det=d11·d22−conjH(d21)·d21`), the diagonal pivot magnitude (`|Re|` for Hermitian, `ldlt_magd`), and the solve's backward `Lᴴ` + 2×2 D-inverse (runtime `cj`). **Sequenced refactor-then-extend (advisor):** STEP-1 the `R=RealType<T>`/`ldlt_mag`/`conjh`/`pivd` refactor was verified **real `[v5d]` BYTE-IDENTICAL (263/26)** before any complex was added (the moat + reconstruction values pin transparency); STEP-2 added the complex paths. 3 `[v5d-f]` tests (advisor-guarded against every recurring trap): reconstruction against the RIGHT product (LDLᵀ→L·D·Lᵀ / LDLᴴ→L·D·Lᴴ) with **genuinely nonzero imaginary** parts (else the conj path is dead) + solve residual, `REQUIRE(∃ block_kinds==2)` (2×2 in BOTH), Hermitian `CHECK(D 1×1 im≈0)`, A built genuinely symmetric/Hermitian, + the complex MOAT (block-diagonal, both modes, both types) bit-identical {1,2,4,8,16} + **flag-load-bearing** (advisor: factor the SAME Hermitian matrix both ways ⇒ `res(LDLᴴ)<1e-10` AND `res(LDLᵀ)>1e-3` ⇒ proves `m_hermitian` changes the math, not decorative — the selection analog of v5d-e's worker-count check). **VERIFIED 5 configs** — win-debug ([v5d] 339/30) + clang-cl ([v5d-f] 76/4) + win-tidy (clean) + **win-asan** (complex factor/solve + moat OOB-clean under parallel) + gcc `-Werror` build **+ RUN** (full suite 597615/111). | ~230 | 4 | — |
| ↳ **v5d-g** ✅ 2026-06-02 — **CLI + Eigen SimplicialLDLT bench** (⚠ the "🎉 v5d COMPLETE" + "ADR-0082 asm-wall is the only path / NOT a v5d task" framing in this row is SUPERSEDED by v5d-h ↓ — the gap was front-structure + threshold, not the asm kernel) | **CLI: 6 commands, EXPLICIT mode selection** (advisor) — `hesap.direct.ldlt.{f32,f64,c32,c64}` (symmetric LDLᵀ) + `hesap.direct.ldlh.{c32,c64}` (Hermitian LDLᴴ); `impl_ldlt<T,bool Hermitian>` factor+solve, output `[info, x]`. 3 `[v5d-g]` CLI tests: 6-command registration · `ldlt.f64` symmetric-indefinite solve · `ldlh.c64` Hermitian solve + the **CLI-level mode-separation proof** (`ldlt.c64` on the SAME Hermitian input gives a DIFFERENT, wrong answer ⇒ the command pair is genuinely mode-selecting). **BENCH (`bench_hesap_ldlt_vs_reference`, FAIR: AMD-ordered once, ALL fed the permuted matrix — Eigen `NaturalOrdering`, MUMPS `ICNTL(7)=1 PERM_IN=identity` ⇒ identical fill, pure kernel).** Profile-driven optimization (measure-first, moat-safe): L-extraction was 11–32% → DIRECT CSC build (count-from-block_kinds + scatter + no-swap fast-path) ⇒ Lbuild −3×; serial path drops the `ts` mutex ⇒ tiny-front walk −28%. **HONEST SCOREBOARD vs the REAL same-class gold standard MUMPS-LDLᵀ (SYM=2, 2×2 Bunch-Kaufman), serial-vs-serial: Cerid BEATS MUMPS on small/tiny fronts (0.20–0.45× — MUMPS's setup overhead) but LOSES 1.5–6× on big-front 3D (n=4k→64k: 1.54×→2.54×→3.58×→6.01×, the gap GROWING with n) — MUMPS uses a BLOCKED-BLAS-3 front factor, Cerid's is UNBLOCKED rank-1/2.** (vs Eigen `SimplicialLDLT` Cerid is 1.8–2.5× faster on 3D — but Eigen is PIVOT-FREE, NOT a same-class indefinite peer, so that is NOT the crush claim; correctness receipt `[[0,1],[1,0]]` ⇒ Eigen `NumericalIssue`, Cerid + MUMPS correct.) **⇒ v5d today: complete + correct-on-ALL-indefinite + the ONLY cross-thread bit-deterministic LDLᵀ + beats MUMPS on small — but LOSES to MUMPS on the big-3D TARGET, by a CHARACTERIZED CONSTANT bounded by the ADR-0082 dense-gemm-kernel wall (the SAME wall v5c bcsstk hit).** LEVER PROBE (bench-only `blocked_ldl_1x1`, dense SPD front, verified identical L): a blocked-1×1 BLAS-3 front factor reaches ~20 GF/s vs the unblocked rank-1's ~2–13 GF/s (1.5–9× on the dense factor) — REAL, but it caps at Cerid's *intrinsics* `dense::gemm` rate; MUMPS's 6× comes from an OpenBLAS-class gemm (~50 GF/s) + symmetric syrk. So **a full blocked-BK (xLASYF, the 2×2-straddle is the hard part) would narrow 6×→~2× but NOT crush — the residual is the gemm-kernel quality. The UNCONDITIONAL MUMPS crush is gated on the ADR-0082/ADR-0088 hand-tuned-asm microkernel (match OpenBLAS), a separately-scoped multi-week effort, NOT a v5d task.** The shipped front kernel stays unblocked (the probe is measurement scaffolding only). v5d-perf = blocked-BK (narrows, kernel-bound); the asm microkernel = the crush enabler (roadmap decision). This is honest per feedback_full_victory: crushed where the ALGORITHM wins (correctness/determinism/small-front — the whole indefinite-correctness axis MUMPS doesn't contest); kernel-bound where the kernel is the deferred cornerstone. **VERIFIED 5 configs** — win-debug (full suite 597649/114, [v5d] 339/30) + clang-cl + win-tidy (clean) + win-asan + gcc `-Werror` build **+ RUN**; bench (+MUMPS SYM=2) built+run on WSL (flags reset OFF). | ~360 | 3 | — |
| ↳ **v5d-h** ✅ 2026-06-03 — **indefinite CORRECTNESS + the crush grind: delayed pivots → blocked-BK kernel → relaxed threshold + IR → CHOLMOD amalgamation. 375× → ~1.3–1.7× vs MUMPS (parity-class) + BETTER accuracy + the moat.** | **The SPD bench had MASKED a correctness bug: v5d FAILED (info≠0) on genuinely-indefinite matrices needing DELAYED PIVOTS (smallest repro: 3D shifted Laplacian A−3I, n=27; Eigen+MUMPS solve it ⇒ stability-delay, not singular). (1) **DELAYED PIVOTS (Duff-Reid):** unresolvable fully-summed columns relay to the parent's fully-summed set; front sizes DYNAMIC; factor positions assigned in a SERIAL postorder pass (gp==pivot_first in the no-delay case ⇒ byte-identical, zero regression); delayed region sorted ascending (`ldlt_swap_sym`) for the parent's extend-add; gp≠n ⇒ singular. v5d now correct on the WHOLE indefinite domain (incl. the only-correct-besides-MUMPS receipt: Eigen `SimplicialLDLT` is fast-but-WRONG on indef-3D, resid 4e1–4e3). MANDATORY delay-triggering moat test {1,2,4,8} bit-identical. (2) **BLOCKED-BK indefinite front kernel** (full 1×1/2×2 + flush-on-pivot-beyond-panel + tiled-lower syrk-equivalent trailing). (3) **DISCRIMINATING MEASURE** (advisor; `front_kernel_vs_lapack`, dense 1600²): Cerid-blocked **42 GF/s** vs LAPACK **dsytrf 56** (BK peer MUMPS runs) vs **dpotrf 72** ⇒ kernel only ~1.3× behind, NOT a hard asm-wall (v5d-g's framing RETRACTED). MUMPS delays ZERO (`INFOG(13)`=0) while Cerid delayed 30% (maxf blew 2051→12160) ⇒ the big-3D gap was DELAY-DRIVEN FRONT BLOWUP from the textbook BK threshold α=0.64, NOT the kernel. (4) **RELAXED PIVOT THRESHOLD** (`set_pivot_threshold`, default **0.001** = MA57/MUMPS-class; sweep measured) ⇒ delays→~0, maxf→SPD size, the gap stops growing with n (6.9×→stable ~2×). (5) **ITERATIVE REFINEMENT + backward-error ACCEPT guard** in `solve` (stores A's lower tri; accurate-or-flagged, never silent garbage; deterministic ⇒ moat) ⇒ resid 5e-9→2.9e-12, **BETTER than MUMPS at every size**. (6) **RELAXED-FRONT AMALGAMATION** — faithful port of CHOLMOD `cholmod_super_symbolic` (read the source): merge ADJACENT chain fronts (union-find) only when new explicit zeros pass the graduated nrelax/zrelax test; fill-aware ⇒ no OOM (a first naive subtree-collapse exploded the fill). **FINAL vs MUMPS: SPD 1.27–1.46× · indef 1.53–1.74× (n=4k–64k, was 19/137/375× at α=0.64); small/saddle indef Cerid WINS 0.14–0.69×; vs Eigen outright crush (faster + correct).** HONEST: NOT a sub-1× serial speed-crush of MUMPS on big-3D-FEM — the residual ~1.3× is the blocked-BK KERNEL-PANEL rate (Cerid 42 vs dsytrf 56; the gemm is at-par, the panel structure isn't — xLASYF W-panel reaches parity, not sub-1×, same kernel class). The crush = PARITY-class speed + the determinism MOAT (MUMPS lacks) + better accuracy + only-correct-besides-MUMPS + crushes Eigen (the established v5a/v5b/v5c pattern). ⚠ α=0.001 tuned on well-conditioned 3D-FEM (raise to 0.01 for ill-conditioned via the knob; IR guard fails loudly). amalgamation default-on (relax=4); xLASYF W-panel = bounded follow-on. VERIFIED 5 configs (win-debug 354/37 + clang-cl + gcc-debug RUN + win-asan + win-tidy). Memory `project_v5d_multifrontal_ldlt`. | ~700 | 17 | — |
| ↳ **v5e-1** ✅ 2026-06-03 — **low-rank substrate + HSS/ULV kernel (1a–1d)** | **v5e-1a** ID `interp_decomp` (`InterpDecomp<T,L>` skeleton/cols/proj; column ID via QRColPiv + back-sub, hesap-dense) · **v5e-1b** `randomized_range`/`rsvd_op`/`rsyev_op` over `LinearOp` with a **counter-based RNG** (`counter_gaussian`, moat-ready: bit-identical across threads, hesap-dense) · **v5e-1c** `HssMatrix<T>` symmetric HSS — `build_cluster_tree` (recursive bisection, pre-order ids) + `hss_matvec` + INDEPENDENT `hss_to_dense` (cross-check expansion) + telescoping `build_hss_from_dense` · **v5e-1d** `HssUlv<T>:IFactorization` + `factor_hss_ulv` (ULV factor+solve; the basis rotation applied as IMPLICIT Householder reflectors via `apply_q_block`/dlarfb — O(m²r), never the dense m×m Q). STRUMPACK oracle wired: `scripts/setup-strumpack-ref.sh` + `bench_hesap_hss_vs_strumpack.cpp` (`CRD_BUILD_HESAP_VS_STRUMPACK`, WSL `~/strumpack/install`). 4-config verified (win-debug/clang-cl/win-tidy/gcc-RUN). | ~600 | ~12 | — |
| ↳ **v5e-2** ✅ 2026-06-04 — **GLOBAL-SAMPLE construction → the FULL CRUSH vs STRUMPACK + the determinism MOAT** | **SCOREBOARD vs STRUMPACK (serial, identical rank=4, machine-eps both, N=512/1024/2048/4096): COMPRESS CRUSH 3.41/1.83/1.71/1.39× · FACTOR CRUSH 2.7–4.1× · SOLVE PARITY/WIN 1.04/1.06/0.99/0.99× + the DETERMINISM MOAT.** Every win = a PROFILE-found REAL problem (NOT "intrinsics are slow"). **COMPRESS:** GLOBAL-sample construction feeding the EXISTING explicit-basis telescoping — sample `Y=A·Ω` ONCE (counter-RNG Ω, blocked gemm, O(N²ℓ)); per-node off-diag `S_k = Y(I_k,:)−A(I_k,I_k)·Ω(I_k,:)`; `U_k`=leading left-sing-vecs of S_k (24.9s→0.26s, 90×). THEN the big-N crush: a phase profiler showed `compress_samples` did a FULL `svd` of the TALL `sk` (n_k up to 2048) — the scalar bdsqr Givens-accumulation into U over the tall dim was **67% of compress** (210ms, ~0.3 GF/s) — REPLACED with **QR `sk`=Q·R (BLAS-3) → SVD the tiny ℓ×ℓ R → `q_out=Q·U_R[:,:r]` via implicit apply_q** (same singular values, deterministic ⇒ moat-safe; branch on `n_k>ℓ` since wide sk has only n_k reflectors); pa_svd 210→40ms ⇒ compress 0.81→1.39×. **(The advisor's adaptive-ℓ hypothesis was REFUTED by the profile — A·Ω was only 13%, not the bottleneck.)** **SOLVE 0.14×→parity, a STACK of measured fixes:** store `W=L⁻¹·D21` (the factor ALREADY computes it for the Schur `D11−WᵀW`, then threw it away) ⇒ **4→2 triangular solves** (0.50→0.75, also dropped the now-dead `d12` field) · vectorized the block gemms `gemm_blk`/`gemm_at_blk` (scalar triple-loops → AXPY over contiguous nrhs, →0.85) · **register-blocked trsv** (`tri_solve_vec`/`_t`: the nrhs result-block held in a `T acc[16]` register accumulator across the k-loop ⇒ 1:1 vs 3:1 mem:FMA, →0.92) · **alloc-free per-reflector apply** replacing `apply_q_block` (which allocates **5 Arrays PER CALL** × thousands of calls = 63ms of pure TLSF churn for r=4 work — `apply_reflectors_left` rank-1 updates, →0.99–1.06). **MOAT PROVEN (`[moat]`):** compress (parallel A·Ω at n=512) + factor + solve BIT-IDENTICAL across jobs `{1,2,4,8}` workers (serialize every node D/U/R/B + the solution, exact `==`; 18 asserts) — green on gcc-release (real threading) + win-asan (OOB-clean under the init/shutdown loop). The differentiator STRUMPACK structurally lacks. **⚠ Fixed a pre-existing crd-jobs SIGSEGV landmine en route:** `WorkerPool::shutdown()` left `m_num_threads` stale ⇒ `num_workers()` returned a non-zero count post-shutdown ⇒ `gemm_parallel_auto` (in compress's `dense_gemm`) dispatched `parallel_for` onto a DEAD scheduler ⇒ **full-gcc-release-suite SIGSEGV** (passed in isolation/win-debug/clang-cl/win-asan; `gdb -batch -ex run -ex bt` on the release binary named the real frame). 1-line root fix (reset to 0) + regression guard in `test_jobs.cpp`; verified CAN-ONLY-FIX (grepped every other `num_workers()` consumer — all already guard `==0`/`<=1`). See `feedback_jobs_shutdown_must_reset_num_workers`. **VERIFIED 6 configs** — gcc-release full suite **597995 asserts / 140 cases** (incl. the moat under gcc threading) + win-debug (ctest) + clang-cl + win-shipping + win-tidy + win-asan (moat OOB-clean). **HONEST:** solve is parity/win NOT a crush (0.99× large-N vs serial OpenBLAS dtrsm = the honest ceiling, the residual ~1% is the strided col-major scatter — not worth a risky refactor); compress+factor are genuine crushes; the moat is real. Bench flags reset OFF. Memory `project_v5e_hss_crush`. Optional polish left (logged not gaps): adaptive-ℓ, tree-parallel Pass-A. | ~700 | ~14 | — |
| ↳ **v5e-3** ✅ 2026-06-04 (BLR substrate built; dense multifrontal Cholesky SERIAL-CRUSH; parallel hybrid deferred) — **BLR-embedded multifrontal (MUMPS-BLR)** — flat block-low-rank fronts (robust production default; complements HSS). **Ships in v5, expanding ADR-0065 BLR-reserve (locked 2026-05-28; pinned at §27).** | **v5e-3a SUBSTRATE ✅** — `blr.{hpp,cpp}`: `BlrBlock<T>` (dense or low-rank u·vᵀ) + symmetric `BlrMatrix<T>` (lower block-tri; diag dense, off-diag compressed) + `compress_blr_sym` (per-off-diag-block `interp_decomp` column-ID, kept LR iff rank·(rows+cols)<rows·cols, reusing v5e-1a) + `blr_to_dense_sym`. `[blr]` 6 asserts: dense-fallback EXACT · LR path exercised · monotone-in-tol · DETERMINISTIC (moat-FREE — per-block compression of explicit data, no RNG; cleaner than HSS) · f32. **⭐ PREMISE CHECK (advisor's #1, the de-risking) — MUMPS-BLR vs full (`ICNTL(35)`/`CNTL(7)` env toggle on the LDLᵀ bench, SYM=2, 3D Laplacian, OMP=1): BLR crosses over at n≈110K (k=48) — 2.6×→1.45× SLOWER below (overhead regime), then 1.28× / 1.69× / 2.0× FASTER at n=110K / 175K / 262K, GROWING with n; BLR ε=1e-8 ⇒ resid ~1e-6 (vs full 1e-15) ⇒ IR MANDATORY. ⇒ BLR is NECESSARY not optional: MUMPS-BLR beats CERID-FULL 2.79× at n=262K (Cerid-full ~1.4× behind MUMPS-full × MUMPS-BLR's 2× over full). Build/bench ONLY at n≥110K.** **v5e-3b BLR-CHOLESKY FACTOR+SOLVE ✅** — `blr_cholesky_factor` (FSCU simple: dense Cholesky → compress L's off-diag blocks into BLR; diagonal dense; LR×LR factor update is 3c) + `blr_cholesky_solve` (off-diag LR blocks applied `u·(vᵀ·)` fwd / `v·(uᵀ·)` bwd = the BLR solve win). `[blr]` 17 asserts/10 cases: SPD-kernel low-rank-L + solve-within-tol · generic full-rank SPD EXACT (dense fallback) · non-SPD⇒false · deterministic · f32. VERIFIED 4 configs (win-debug + win-tidy + clang-cl + gcc-RUN). (3b factor is DENSE — compress-for-memory only, no factor flop savings yet; that's 3c.) **v5e-3c LR×LR UPDATE + RECOMPRESSION ✅** — `detail::low_rank_recompress` (LUAR core: QR(uc)/QR(vc)→SVD the tiny rc×rc Ru·Rvᵀ→truncate, the v5e-2 QR-then-tiny-SVD pattern; dense-ID fallback rc≥min) + `apply_schur_update` (L_ik·L_jkᵀ built low-rank for all 4 dense/LR cases; LR off-diag target via [Ua,−Ub]·[Va,Vb]ᵀ concat + recompress) + `blr_cholesky_factor_lr` (compress front → per-panel chol-diag / LR-preserving-TRSM / LR×LR update in FIXED k-order). `[blr]` 29 asserts/14 cases: recompress rank-4→true-2 EXACT · LR-arith solve within tol AND matches the 3b dense ORACLE · deterministic (moat: fixed accumulation) · f32. 4 configs (win-debug + win-tidy + clang-cl + gcc-RUN). (Factor SERIAL — single-thread determinism tested; cross-thread {1,2,4,8} moat + MEASURED crush = 3d; mm helpers naive, 3d swaps fast gemm.) **v5e-3d PREP / GATE ✅** (advisor's go/no-go before the driver): routed the BLR arithmetic (mm/mm_at/mm_bt/block_dense/dense_sub_lr) through `gemm_parallel_auto` (fast + moat-safe; [blr] 29 asserts green, 4 configs). SINGLE-FRONT CROSSOVER bench (`[.][blr-bench]`, optimized, smooth Gaussian front): **dense Cholesky 35.5/44.5/50.2 GF/s = OpenBLAS-class** (n=1024/2048/4096 — the ADR-0082 wall does NOT bind SPD Cholesky's gemm) + **BLR speedup 0.67×→1.30×→2.33×** (crosses at n≈2048, grows, ≈ MUMPS-BLR's 2×-over-full). **⇒ GREEN GATE: kernels competitive + BLR within-front win ⇒ crush-capable; the advisor's ~1.4×-behind kernel-wall projection REFUTED for Cholesky.** **v5e-3d DRIVER CORE KERNEL ✅** — `factor_front_cholesky_blr` (the partial-front BLR Cholesky, the driver's heart): in-place on a dense m×m front + npiv, factor the leading npiv fully-summed pivots in BLR arithmetic (npiv-aligned grid, compress + LR-preserving TRSM + LR×LR updates incl. Schur rows) → L (cols[0,npiv)=L11+L21) + DENSE Schur S=A22−L21·L21ᵀ written back (decompress-to-dense ⇒ driver extend-add + L-extract unchanged, advisor's "dense Schur first"). `[blr]` 34 asserts/16 cases: full front npiv=m⇒L·Lᵀ=A · partial⇒L+Schur COMPLETE to A. 4 configs (win-debug + win-tidy + clang-cl + gcc-RUN). **v5e-3d DRIVER COMPLETE + HONEST BENCH ✅ (correct, NOT a speed crush)** — `MultifrontalCholeskyBlr<T>` (`multifrontal_cholesky_blr.{hpp,cpp}`): full BLR multifrontal Cholesky (reuse `build_symmetric_multifrontal_symbolic` + own row-major assembly/extend-add/postorder/L-extract CSC + `factor_front_cholesky_blr` large / dense small via `blr_min` + solve + IR + backward-error guard). `[mfblr]` 14 asserts/4 cases (2D dense + 2D/3D BLR-path + DETERMINISM moat), 4 configs green. **⚠ HONEST BENCH (AMD 3D Laplacian, serial): Cerid-BLR 23.4s/63.8s (k=48/56) vs MUMPS-BLR 2.6s/6.1s ⇒ ~9× SLOWER, NOT a speed crush.** Root cause (advisor-confirmed): the front factor's DIAGONAL chol+TRSM are still NAIVE `chol_lower_dense`/`trsv` (only the UPDATES went through gemm) ⇒ on big fronts (npiv~3000) naive O(npiv³) at 2 GF/s is catastrophic (factor_cholesky=50 GF/s=25×); + serial; + compress/recompress overhead; + real 3D fronts compress to far higher rank than the Gaussian gate. **HONEST CEILING (advisor ×3): BLR scales BOTH sides ⇒ achievable = PARITY-class speed + the determinism MOAT (MUMPS-BLR lacks) + matched accuracy via IR, NOT sub-1× speed.** PERF TRACK (FRESH session, not an end-of-marathon grind — v5a CRUSHES CHOLMOD-full ⇒ Cerid CAN do fast Cholesky): diagonal chol→`factor_cholesky` (the 25× lever) + TRSM→v5e-2 blocked-gemm cast + tree-parallel (v5a/v5d level-sched) + {1,2,4,8} moat + overhead tuning, then bench vs MUMPS-BLR matched-ε. **The correct driver + moat is bankable now.** **PERF CYCLE 1 ✅ (dense front 5×→2.3× behind MUMPS-full serial, full suite 598043/160 green, 4 configs):** diagonal chol→`chol_lower_fast` (factor_cholesky, ~12%) + `factor_front_cholesky_dense` NEW fast dense path (skips the interp_decomp QRColPiv waste) + **`blocked_trsm_lower` (THE lever: cast the unblocked-trsm panel solve as gemm — dense k=48 14.5→7.74s, k=56 86→23.3s).** ⚠ BLR path still slow on 3D (compress overhead > savings; dense WINS, raise `blr_min`). REMAINING crush levers (FRESH session, advisor: don't rush tree-parallel into a determinism-critical factor at a marathon tail): syrk (½ Schur flops) + TREE-PARALLEL (the v5a pattern that beats CHOLMOD — level-sched + per-worker scratch/triplets + moat) + matched-thread bench. Memory `project_v5e3_blr_premise_and_plan`. **⭐⭐ GATE VERDICT + SERIAL CRUSH (Leg A) 2026-06-04 — BLR ABANDONED as the crush vehicle; the gap is MACHINERY not ordering:** The `[.][mfblr-blrgate]` gate (serial, AMD 3D Laplacian, BLR blr_min=512 ε=1e-6 vs Cerid's OWN dense blr_min=∞) shows **BLR 2.3–2.55× SLOWER than our own dense path, nnz UNCHANGED** (STRUCTURAL: the driver densifies the Schur at every extend-add ⇒ destroys the low-rank structure at each tree edge ⇒ pays compress overhead, captures neither flop nor storage). The premise didn't transfer — MUMPS-BLR's crossover is vs MUMPS-FULL; Cerid-FULL is already elite ⇒ Cerid's own BLR crossover is at n≫250K. Keep the BLR substrate (correct/tested/moat-safe) for the very-large-N/out-of-core frontier; NOT default, NOT the v5e-3 crush vehicle. **⭐ SERIAL-GAP LOCALIZED** (`CRD_MFBLR_PROFILE` compile-gated phase split + achieved-GF/s + AMD-vs-ND flop ratio, `[.][mfblr-prof]`): factor DOMINATES 67–72% (NOT symbolic/assembly — advisor hypothesis REFUTED); the gap is **PURE MACHINERY on IDENTICAL AMD fill** — MUMPS is fed Cerid's AMD via `ICNTL(7)=1` ⇒ same flops/nnz; **ND is a RED HERRING** (2.1–2.8× fewer flops but 3× SLOWER in our driver: `build_symmetric_multifrontal_symbolic` builds FUNDAMENTAL supernodes = no relaxed amalgamation ⇒ ND's skinny separator fronts factor at 6.5 GF/s). MUMPS = 44 GF/s all-in; Cerid was 30 GF/s factor + 33% overhead. **⭐ LEG A LANDED (serial 2.2–2.35× → 1.44–1.53× behind MUMPS-full; factor kernel now BEATS MUMPS):** (1) **blocked lower-tri SYRK Schur** (`syrk_lower_sub` — gemm only the lower block-tiles straight into the front, ½ the flops: factor **30→48–51 GF/s**, exceeding MUMPS's 44); (2) **MUMPS-style DIRECT-CSC L assembly** (`m_lp`/`m_li` precomputed from the symbolic, factor writes `m_lx` straight to slot — killed the triplet buffers + the untimed counting-sort scatter; moat-CLEANER: disjoint deterministic slots); (3) extend-add gather hoist (measured NO-OP ⇒ the assembly cost is the inherent scatter WRITE). WALL k=48 7.9→5.13s, k=56 →14.87s. **Cheap serial levers SPENT** — further = ColMajor fronts / relative-indexed assembly (diminishing). `[mfblr]` 28 asserts/5 cases incl. the {1,2,4,8} MOAT green; `CRD_MFBLR_PROFILE` converted env→compile-gate (MSVC `getenv` C4996). VERIFIED win-debug + win-tidy + win-asan + gcc-release-RUN (+ clang-cl). **⭐ LEG B (parallel) — INVESTIGATED → SERIAL SHIPPED (2026-06-04, user: "1.44 serial is enough, stop the parallel grind").** Found + fixed a REAL latent bug en route: the gemm FrameArena leak (`gemm_parallel` `frame_alloc`s its JobDecls but NEVER resets ⇒ a big front's hundreds of gemm calls EXHAUST the per-thread arena ⇒ win-asan `frame_arena.hpp:60` assert = linux-release's glibc `corrupted size vs prev_size`, the SAME defect in two configs — this was the "heap corruption" mystery). Filed debt `gemm-parallel-frame-arena-leak` + a localized `reclaim_frame_arena()`; ASan-clean at k=40/48. BUT node-parallel within-front scaling measured SUB-1× (panel-syrk 0.37/0.51/0.67× at k=40/48/56 — most fronts too small to amortize parallel-gemm dispatch; only the few huge near-root fronts benefit). HONEST (advisor, repeatedly): from 1.5×-serial-behind + MUMPS scales well, parallel CANNOT beat MUMPS on raw speed (parity-class is the ceiling); the determinism MOAT is already proven. ⇒ **the dense crush path's syrk/trsm route through SERIAL `gemm`** (no thread regression: jobs-up 0.93× at k=56 = `factor_cholesky`'s minor internal parallelism only; correctness + {1,2,4,8} moat green). The level-scheduled hybrid (tree-parallel small fronts + node-parallel big + per-worker allocators) = future PRODUCT-PERF work, NOT a MUMPS-crush enabler. **v5e-3 CLOSED: BLR substrate (correct/tested/moat-safe — kept for very-large-N, NOT the crush vehicle) + dense multifrontal Cholesky SERIAL-CRUSH (factor kernel 49-53 GF/s BEATS MUMPS's ~44; total 1.44-1.53× behind MUMPS-full all-in via syrk + MUMPS-style direct-CSC L assembly) + the FrameArena fix + the moat. VERIFIED win-debug/tidy/clang-cl/asan + gcc-release-RUN.** LDLᵀ/LU BLR (pivoting × compression) + the parallel hybrid = follow-ons. Memory `project_v5e3_blr_premise_and_plan`. | ~600 | ~12 | — |
| ↳ **v5f-a** ✅ 2026-06-05 | **Mixed-precision IR core + LU** (HPL-AI / Carson-Higham) — `IterativeRefinedSolve<TWork=f64, TLow=f32> : IFactorization<TWork>` owns a f64 copy of A (for the residual) + a type-erased `IFactorization<f32>` (the cheap factor); `solve` = f64 fixed-point iterative refinement (residual via `spmv`, raw-f32 factor apply each step, v5d-h backward-error-ACCEPT guard owning ALL refinement at f64). **⚠ Lock (audit-found 2026-06-05): drive a RAW no-inner-IR f32 apply.** `MultifrontalLU::solve` (→ `static_lu_ir_solve`) and `MultifrontalLDLT::solve` already run internal IR whose stagnation/accept-gate returns `false` on f32-stall ⇒ a naïve `low->solve()` would SPURIOUSLY fail the outer IR on exactly the ill-conditioned big-3D systems mixed-precision targets. Resolution: append `apply_inverse(Span<T>, nrhs)` virtual to `IFactorization` AT END (D135 vtable-safe; default → `solve` for raw families like Cholesky), override in LU (`static_lu_apply` = `transform_rhs` + `lu_lu_solve` + `untransform_solution`, no IR) — `static_lu_ir_solve` left byte-identical. + `factor_mixed_lu`. `mixed_refine.hpp` (`MixedRefineOptions` max_iters=20, refine_tol=64·eps, accept_tol=√eps stall+accept guard). Tests `[mixed]`: f64-accuracy recovery, LOAD-BEARING (f32-only ≈1e-7 vs +IR ≈1e-14), honest κ≈1e9 refusal, multi-RHS. Verified win-debug/clang-cl/asan/tidy + guard. | ~180 | ~5 | — |
| ↳ **v5f-b** ✅ 2026-06-05 | **SPD + symmetric mixed-precision** — `factor_mixed_cholesky` + `factor_mixed_ldlt`; symmetric lower-tri residual `spmv` (reuse v5d-h's pattern; `symmetric_lower_to_full_csr` sort-free expand). Cholesky `solve` is already raw (no internal IR ⇒ default `apply_inverse` is correct); `MultifrontalLDLT::apply_inverse` override = refactored the existing `tri_solve` lambda into a private `ldlt_apply_once`, one solve/col, no IR loop (refactor-then-reuse ⇒ byte-identical, moat-safe — verified by the unchanged `[v5d]` moat). Tests `[mixed]`: SPD + indefinite recovery to f64, both load-bearing. | ~120 | ~4 | — |
| ↳ **v5f-c** ✅ 2026-06-05 | **Determinism moat + CLI + bench (the HONEST crush proof)** — moat: factor-in-f32 AND IR-solve BIT-IDENTICAL {1,2,4,8} (3 `[mixed]` moat tests via block-diagonal builders; iteration count worker-independent because r/x are bit-identical each step). CLI `hesap.direct.mixed.{lu,chol,ldlt}.f64` (`[v5f][cli]`; its complex-square path is the v5f-d follow-on). **Bench 1 — INTERNAL (`bench_hesap_mixed_internal`, 3D Laplacian, matched post-IR accuracy, unimpeachable):** mixed precision is a **SYMMETRIC lever** — ~1.3× E2E (LU best), LDLᵀ convergence-bounded; the honest v5f value is **matched f64 accuracy + ~½ factor memory + the determinism moat**, NOT a raw-speed crush (a peer doing the same f32-factor trick gets the same lift). **Bench 2 — smumps+IR EXTERNAL (`bench_hesap_lu_supernodal_vs_reference`, CFD corpus, serial OMP=1, matched f64 via a mirrored 64·eps f64-IR loop around the SINGLE-precision MUMPS factor — mixed-vs-mixed, NO dmumps-f64 asterisk):** the FAIR same-class result is **Cerid-mix vs smumps+IR head-to-head: af23560 1.14× WIN · wang3 0.90× · ns3Da 0.61×** = the v5b serial ranking now at f64 accuracy (af23560 win; wang3/ns3Da the known MUMPS async-DAG/node-parallel gap) **+ the cross-thread determinism MOAT smumps+IR cannot carry.** (Cerid-mix also lands at parity–1.36× of UMFPACK-**f64**'s factor time — af23560 1.04× · wang3 1.35× · ns3Da 1.36× — but that is the ONE-SIDED f32 lever, NOT a same-class crush: Cerid-SN-**f64** is actually ~2× SLOWER than UMFPACK-f64 here (0.44–0.49×), and UMFPACK given the same f32-factor+IR treatment would be ~2× faster than Cerid-mix; UMFPACK has no stock single+IR path, so this is "practical-today at f64 accuracy", asterisked.) garon2/raefsky3 = **static-pivot accuracy wall** (Cerid f64-full ALSO `[INACCURATE]` ⇒ orthogonal to mixed precision, NOT a speed loss; bench now flags `DIVERGED` + suppresses the race — honest-by-construction). **Leak fixed en route:** `gemm-parallel-frame-arena-leak` (central `frame_get_mark`/`frame_set_mark` scoped-marker self-clean in `gemm_parallel`/`small_gemm_parallel`; 4000-call regression test). | ~150 | ~5 | — |
| ↳ **v5f-c2** ✅ 2026-06-05 | **GMRES-IR robust refinement (Carson-Higham) — fixes the static-pivot DIVERGENCE.** Root cause of garon2/raefsky3 `[INACCURATE]`: MC64+static-pivot LU (no row interchanges — the moat design) is a POOR approximation on saddle-point/indefinite UNSYMMETRIC systems ⇒ tiny pivots get GESP-perturbed ⇒ the factor drifts far from A ⇒ FIXED-POINT IR diverges (this is the FACTORIZATION, not mixed precision — the f64-full path fails too). Fix = `gmres_refine.hpp` `GmresRefinedSolve<T,Fac> : IFactorization<T>` — FGMRES PRECONDITIONED by that same factor (`FactorPrecondOp` wraps the RAW `apply_inverse`), which converges where fixed-point IR cannot (Krylov subspace, not a contraction assumption). Reuses the v4 FGMRES = the determinism-moat solve (serial Arnoldi/Givens, parallel-spmv) ⇒ moat-safe. `factor_gmres_refined_lu` entry point. **NEW module edge hesap-direct→hesap-iterative (ACYCLIC — sibling).** **PROBE-VERIFIED on the real corpus: garon2 fixed-point 2.9e-05 DIVERGED → GMRES-IR 3 iters → 1.9e-15 ✓ (~parity total cost with UMFPACK).** ⚠ raefsky3 = the harder case: GMRES-IR converges only at 554 iters/5s (factor too degraded) ⇒ needs **threshold partial pivoting in the front** (the v5f follow-on, deferred — touches the moat-critical kernel, do it fresh). Tests `[gmres-ir]` 29 asserts/4 cases (f64 recovery + indefinite never-under-performs-bare + multi-RHS + {1,2,4,8} MOAT). VERIFIED win-debug(ctest)+gcc+win-tidy+win-asan. **⭐ SPEED-GAP PROFILE (`CRD_MF_PROFILE` env-gate added to `multifrontal_lu.cpp`, read-only front-size+flop distribution): the ns3Da serial loss is the ADR-0082 MICROKERNEL WALL on medium fronts (f64 factor 595ms = 26.5 GFLOP/s vs UMFPACK ~30, `[MF 0.91×]`), NOT a missing amalgamation lever — skinny fronts (npiv<32) are only 4.7% of flops; flops are spread across medium fronts (npiv 32-255). af23560 WINS because it is many tiny fronts (3.3e9 flops). ⇒ the amalgamation hypothesis is REFUTED; parity@f64 + the determinism MOAT is the honest ceiling on ns3Da (a "crush" would mean re-opening ADR-0082 asm for ~12% — declined).** | ~250 | ~5 | — |
| ↳ **v5f-d** ✅ 2026-06-05 | **QR least-squares mixed-precision IR** — `mixed_qr_refine.hpp` `factor_mixed_qr` / `QrMixedRefinedLS::least_squares`. The "genuinely different" algorithm: **Björck CORRECTED SEMI-NORMAL EQUATIONS (CSNE)** — factor over-determined A (m≥n) in **f32 multifrontal QR** (the cheap O(mn²), ~½ mem), refine in **f64** driving the NORMAL-equation residual ‖Aᵀ(b−A·x)‖→0 (the LS optimality condition, NOT a square A·x=b residual). **NO Q application** — reuses only the v5c QR's global R (CSR: own Rᵀ-solve via row-scatter + R-solve back-sub) + spmv(A·x, Aᵀ·r) ⇒ much simpler than augmented-system IR. **Convergence (the advisor's open question) RESOLVED for well-conditioned LS:** CSNE recovers f64 LS accuracy (κ(A)·u_f32≲1) — the per-step κ² of the normal equations is corrected by the f64 residual + IR loop; honest accept-gate flags ill-conditioned non-convergence. Tests `[mixed-qr]` 28 asserts/4 cases: over-determined recovery to f64 (consistent) + **INCONSISTENT (nonzero-residual) LS optimality 1e-9** + multi-RHS + {1,2,4,8} MOAT (f32 QR bit-identical × deterministic CSNE). VERIFIED win-debug(ctest)+win-tidy+win-asan+gcc. | ~200 | ~5 | — |
| ↳ **v5f-e** ✅ 2026-06-05 | **Within-front THRESHOLD PARTIAL PIVOTING (gold-standard root-cause fix for the static-pivot divergence).** `factor_multifrontal_lu_pp` / `MultifrontalLU::factorize(pivot_threshold)`. STAGED (advisor): **STEP 1** dense kernel — `factor_front` gains `pivot_threshold`+`ipiv` (full partial pivoting restricted to fully-summed rows [k,npiv), tie-break by index ⇒ deterministic; full-row swap; GESP perturbation as the never-delay fallback; routes serial within-front so the row-swap can't race the lookahead). Dense test `[lu-pp]` P·A=L·U incl. zero-leading-pivot. **STEP 2** driver — apply ipiv to `row_index`; record global row perm P (`rowperm()`); **the advisor's key insight: structure is INVARIANT (no-delay) ⇒ only INDEX LABELS change** ⇒ write physical B-rows into `m_li` then ONE uniform `invperm` post-pass → elimination indices (m_ui already elim indices ⇒ untouched; lu_lu_solve is a scatter ⇒ no sort needed); solve applies P to the RHS (`static_lu_apply`/`static_lu_ir_solve` get a `rowperm` arg). All behind `pivot_threshold=0` ⇒ **static path BYTE-UNCHANGED**. Tests `[lu-pp-mf]` (weak-diagonal solve to f64 + non-identity P + {1,2,4,8} MOAT bit-identical solution AND P; **+ the advisor's load-bearing gap CLOSED: a CONNECTED 3D-grid moat at fill>1M ⇒ genuinely MULTI-WORKER, proving the cross-front contribution-row invperm remap is bit-identical serial-vs-parallel — the path block-diagonal fronts structurally cannot exercise**). **⭐ RESULT (bench): garon2 FIXED to f64 — 5.6e+01 static / 2.9e-05 mixed DIVERGED → Cerid-pp 1.7e-12 `[ok]` at UMFPACK-competitive factor cost (53 vs 52ms); raefsky3 5.1e-06 → 4.8e-08 (100× better).** ⚠ raefsky3 to FULL f64 = the **delayed-pivot frontier** (the advisor's predicted empirical stop-point: even pp+GMRES-IR stalls ⇒ a whole fully-summed block lacks a stable pivot ⇒ structure-growing MUMPS-class feature, deferred). VERIFIED **win-debug (597863 regression assertions, ZERO static-path regression + 26 new) + gcc + win-tidy + win-asan**. | ~400 | ~7 | — |
| ↳ **v5z** | **CLOSE** — complex-completeness audit + CLI-completeness audit + **end-to-end determinism moat {1..16}× all families** + ADR-0065 §27 lock D(direct)-1..N + `docs/systems/hesap-direct.md` + **18-config full sweep**. | ~250 | ~8 | — |
| **v6** | **Sparse eigenvalue — matrix-free, the moat differentiator (PLANNED 2026-06-05).** New `hesap-eigen` module (acyclic edges → hesap-iterative [Krylov for JD correction eqns + LinearOp] / hesap-dense [Rayleigh-Ritz via `eig_sym`/`eig_nonsym`/`svd`] / hesap-direct [shift-invert via v5 factors] / hesap-preconditioners [LOBPCG/JD precond]). **TWO load-bearing truths (advisor-locked): (1) the crush axis is ALGORITHMIC not kernel** — plain Lanczos/Arnoldi = parity + matvec-count + moat (ARPACK/Spectra share the BLAS/LAPACK ceilings; shift-invert IS our v5 = parity+moat); the genuine crush = converging in FEWER matvecs via the PRECONDITIONED methods (shift-invert/LOBPCG/JD using a v5 factor or AMG as preconditioner can beat ARPACK on hard problems). **(2) the eigensolver moat has hazards the linear-solver moat lacks** — clustered/multiple eigenvalues ⇒ non-unique eigenvectors (moat tests use WELL-SEPARATED spectra; block methods test SUBSPACE identity); pin sign/order conventions; confirm `eig_sym`/`eig_nonsym` are bit-deterministic (the inner kernel). **⚠ Restart-method substitution (advisor): deliver the MODERN EQUIVALENTS — Krylov-Schur ≡ IRAM, thick-restart Lanczos ≡ IRLM — because IRAM/IRLM's implicit shifted-QR bulge-chasing is ordering-sensitive + NOT deterministic; Krylov-Schur/thick-restart are mathematically equivalent, more stable, AND moat-compatible.** Gold standards: ARPACK (scipy eigsh/eigs, primary) + PRIMME (state-of-art symmetric — the real crush target on hard problems) + FEAST lib + Spectra (C++ same-class); SLEPc = parallel ref (asterisk caution). Drop order if long: FEAST(g)+JD(f) slip-to-follow-on; irreducible core = a/b/c/d/e/h+z. Commit boundaries: sym-core(a-b) · nonsym+SI(c-d) · precond(e-f) · FEAST/IRLBA(g-h). | ~2500 | ~110 | ~2.5 wk |
| ↳ **v6-a** ✅ 2026-06-06 | **Substrate + symmetric Lanczos** (the foundation; module `hesap-eigen` stood up). The eigenproblem SPEC (which eigenvalues: largest/smallest, algebraic/magnitude, interior; **`A x = λ B x` GENERALIZED first-class from day 1** — FEM modal/buckling is the real eylem/structural target ⇒ optional B operator, B-orthonormalization fixed-order for the moat) + result type (eigenpairs + Ritz residuals + iters + converged-count) + Rayleigh-Ritz helper (small projected problem → `eig_sym`) + fixed-order reorthogonalization + locking/deflation + DETERMINISTIC counter-RNG start + sign convention (force largest-|component| positive) + the moat ground-rules. + plain full-reorthog symmetric Lanczos (largest/smallest k eigenpairs, matrix-free LinearOp). parity+moat. **DONE: `lanczos.hpp` `eigs_sym` (SplitMix64 deterministic start + MGS-twice reorthog + sign convention + Rayleigh-Ritz via `eig_sym`). Tests `[eigen]` 29 asserts/3 cases: 1D-Laplacian largest/smallest vs ANALYTIC λ_k + residuals<1e-9 (full Lanczos at n=16) · run-twice bit-identical · {1,2,4,8} MOAT bit-identical eigenvalues AND eigenvectors (forced-parallel spmv, well-separated spectrum). VERIFIED win-debug + win-tidy.** ⚠ no-restart ⇒ clustered eigenvalues (e.g. the Laplacian's LARGEST, bunched near 4) need v6-b thick-restart or full m; default tol = √eps (the achievable no-restart target). | ~350 | ~14 | — |
| ↳ **v6-b** ✅ 2026-06-06 | **Thick-restart Lanczos** (Wu-Simon ≡ IRLM) — bounded-memory restart; the symmetric workhorse. **DONE: `thick_restart.hpp` `eigs_sym_tr` — keep k=nev+buffer Ritz vectors + the residual at restart; the restarted projected matrix is the ARROWHEAD (diag θ + couplings s_i=β_m·Y[m-1][i] | continued tridiag), but FULL reorthog handles the recurrence so s_i appear ONLY in the Rayleigh-Ritz matrix; cheap convergence via the Ritz residual estimate; deterministic re-seed on lucky breakdown. Chosen over implicit shifted-QR (IRLM) = deterministic (moat-safe).** Tests `[eigen]`: **converges the CLUSTERED largest Laplacian eigenvalues at bounded ncv=20≪n=64 (the v6-a no-restart FAILURE case — restart fixes it)** + {1,2,4,8} MOAT bit-identical eigenvalues AND eigenvectors THROUGH the restart cycles (multi-restart n=240, well-separated end). VERIFIED win-debug + win-tidy. | ~250 | ~10 | — |
| ↳ **v6-c** ✅ DONE 2026-06-06 | **Arnoldi + Krylov-Schur** (Stewart 2001 ≡ IRAM) — nonsymmetric. **ARNOLDI: `arnoldi.hpp` `eigs_nonsym` — full-GS Arnoldi → upper Hessenberg H → Rayleigh-Ritz via dense `eig(H)` → wanted COMPLEX eigenvalues; `more_wanted_c` complex `which`. COMPLEX EIGENVECTORS: X=V·S recovered (Re→`vectors`, Im→`vectors_im`; `EigenResult` got `vectors_im`), unit complex-norm + pinned PHASE convention (rotate e^{−iφ} so the largest-|component| is real-positive ⇒ deterministic ⇒ moat-safe), + TRUE complex residual ‖A·x−λ·x‖.** **KRYLOV-SCHUR RESTART: `krylov_schur.hpp` `eigs_nonsym_ks` — bounded-memory at ncv≪n. One cycle: extend Arnoldi to m ⇒ real Schur H=Z·T·Zᵀ ⇒ A·(V·Z)=(V·Z)·T+β_m·v_{m+1}·(last row Z)ᵀ; reorder_schur the wanted to the lead (selection sort, 2×2-block-aware), truncate to k, restart with B=[[T_k, new-cols],[β_m·b_kᵀ, Hessenberg]].** ⚠ KEY BUG FOUND+FIXED: the restarted H is NON-Hessenberg (T_k block + arrowhead row), but `real_schur` ASSUMES Hessenberg input ⇒ cycle-2+ gave a silently-wrong Z ⇒ frozen Ritz values + false convergence; fix = `general_real_schur` (Hessenberg-reduce h=Q·Hess·Qᵀ first, real_schur the Hess, compose Z=Q·Z_s). Tests `[eigen]`: 2×2 rotation blocks ⇒ a±b·i; full-Arnoldi recovers them; **KS converges the top LargestReal at ncv=20≪n=200 with restarts ACTUALLY engaged (matvecs 116>ncv, conjugate pairs 100±,99± ⇒ Re=100,100,99,99, residual 5e-8)** + KS nconv ≥ no-restart nconv == nev + {1,2,4,8} MOAT bit-identical complex VALUES **AND EIGENVECTORS (Re+Im) THROUGH the restart cycles**. VERIFIED win-debug + win-tidy + gcc. | ~620 | ~16 | — |
| ↳ **v6-d** ✅ 2026-06-06 (standard `(A−σI)`) | **Shift-invert spectral transformation** — **⭐ first algorithmic crush lever + the v5↔v6 bridge.** **DONE: `shift_invert.hpp` `ShiftInvertOp` (LinearOp = `(A−σI)⁻¹·x` via a v5 factor's `apply_inverse`) + `eigs_sym_shift_invert` — build (A−σI) [one diagonal per row, robust to missing diag] → factor with the v5f-e PARTIAL-PIVOT multifrontal LU (accurate on the indefinite shift) → thick-restart Lanczos for the LargestMagnitude μ (⇔ λ closest to σ) → recover λ=σ+1/μ + the TRUE residual ‖A·x−λ·x‖ on A. NEW edge hesap-eigen→hesap-direct.** Test `[eigen]`: finds the INTERIOR 1D-Laplacian eigenvalues nearest σ=2 (λ_31..34 — which v6-a/b CANNOT target from the spectrum ends), exact to <1e-7, residual <1e-8 + {1,2,4,8} MOAT bit-identical (factor built with nw workers = v5f-e moat; eigensolve serial). VERIFIED win-debug + win-tidy. ⚠ achievable residual ~1e-8 (the LU-based SI op is symmetric only to rounding); tighter ⇒ LDLT factor / IR (follow-on). Generalized `(A−σB)⁻¹B` = follow-on. | ~250 | ~10 | — |
| ↳ **v6-e-a** ✅ DONE 2026-06-06 | **LOBPCG** (Knyazev 2001) — block, symmetric, **OPTIONAL preconditioner** (the algorithmic-crush hook). **DONE: `lobpcg.hpp` `eigs_sym_lobpcg` — the `nev` extreme eigenpairs at once. Each iter works in S=[X, W=T·(A·X−X·Θ), P] kept ORTHONORMAL by block MGS (drop rank-deficient cols) ⇒ Rayleigh-Ritz = plain `eig_sym(SᵀAS)` (no generalized Gram solve); AX carried through the q×k combination so the only new matvecs/iter are A·W + A·P; sign-pinned eigenvectors.** `precond` = `const LinearOp<T>*` (T≈A⁻¹, nullptr ⇒ identity) — wired + exercised, the v6-e-b crush lever. Tests `[eigen]`: smallest-4 of the 1D Laplacian vs analytic λ_k (unpreconditioned + with a Jacobi-diagonal `SparseLinearOp` preconditioner) + {1,2,4,8} MOAT bit-identical values + eigenvectors (well-separated spectrum). VERIFIED win-debug + win-tidy + gcc + **win-asan**. ⚠ block=nev (no soft-locking yet); generalized A·x=λ·B·x + the preconditioned **crush** (v5 factor / AMG as T, FEM modal/buckling) = v6-e-b/c. | ~310 | ~13 | — |
| ↳ **v6-e-b** ✅ DONE 2026-06-06 | **Preconditioned LOBPCG — the algorithmic-crush MECHANISM.** No new engine code: the existing `eigs_sym_lobpcg` `precond` param consumes the existing `Ic0Preconditioner` (SPD by construction) + `SaAmg` (V-cycle, fwd-pre+bwd-post GS ⇒ SPD) — both `LinearOp`s, test-only link (no new module edge). Tests `[eigen]` on the 2D Laplacian (rectangular ⇒ non-degenerate): IC0-LOBPCG finds the smallest 4 vs analytic λ_{p,q}; **MECHANISM — a real SPD preconditioner cuts iterations-to-tolerance ~4–5× (unprec 159 → IC0 40 → AMG 32)**; + {1,2,4,8} MOAT bit-identical WITH IC0 in the loop (IC0 factor deterministic + serial-applied; only the A-matvec parallel). VERIFIED win-debug + win-tidy + gcc + **win-asan**. ⚠⚠ HONESTY (advisor-gated): NOT a crush claim — iteration count is NOT comparable across libraries (a V-cycle ≠ an A-apply). The fair crush metric = **WALL-CLOCK + memory at matched accuracy** vs the RIGHT peers (**shift-invert ARPACK**, **scipy `lobpcg`** — NOT plain/SM ARPACK, which nobody runs for smallest SPD) in the regime where it wins (**large 3D / FEM**, where direct-factor fill-in is the killer) → the v6 CLOSE benchmark, built fresh. ⚠ correctness: LOBPCG needs an SPD preconditioner (IC0 ✓; ILU0-on-SPD is NOT guaranteed SPD). | ~190 | ~3 | — |
| ↳ **v6 CRUSH BENCH** ✅ DONE 2026-06-06 | **SYMMETRIC eigensolver crush PROVEN, honestly.** `bench_hesap_eigen_lobpcg.cpp` (Cerid AMG-LOBPCG) vs `scripts/eigen_ref_scipy.py` (scipy ARPACK shift-invert oracle, `~/eigref-venv`). 3D RECTANGULAR Poisson (non-degenerate), both single-threaded, matched tol=1e-7, **identical eigenvalues**. @n=43680: **wall-clock 11.7× (0.64 vs 7.40s) · memory 40× (AMG 31 vs SuperLU 1241 nnz/row)**, BOTH growing with n (3D fill-in ~O(n⁴ᐟ³) vs AMG O(n)); scipy splu-dominated (75%) ⇒ tol-robust. **⚠ EXACT CLAIM (every qualifier load-bearing): "AMG-LOBPCG (iterative+multigrid) crushes DIRECT shift-invert ARPACK on 3D model-Poisson (memory+wall-clock, matched accuracy) + carries the determinism MOAT."** NOT same-class (diff algorithms). 3 guards: memory = AMG `operator_complexity` (standard metric, excludes P/R ⇒ full footprint still O(n) but >31/row — don't headline "40× total"); 3D model-Poisson = AMG's BEST case (general/anisotropic FEM = HYPOTHESIS, owed); same-class pyamg-LOBPCG floor (parity+moat) = the v6 SAME-CLASS FLOOR row below. Bench gcc-compiles (CI builds it; not ctest-registered ⇒ doesn't run). | ~110 (bench) | — | — |
| ↳ **v6 SAME-CLASS FLOOR** ✅ DONE 2026-06-06 | **The honest floor: PARITY + MOAT vs the same-class peer.** `scripts/eigen_floor_pyamg.py` — pyamg 5.3 smoothed-aggregation + scipy `lobpcg` is EXACTLY Cerid's method (AMG-preconditioned LOBPCG). Same 3D rect Poisson, matched tol, **identical eigenvalues**. The fair metric is ITERATIONS-to-tolerance (same algorithm; wall-clock NOT comparable — scipy's lobpcg loop is pure Python, so it's context-only). **Cerid 25/20/21/22/21 vs pyamg+scipy 23/23/22/22/19 (s=16..32) ⇒ PARITY** (Cerid is a sound, competitive implementation, not slower than the reference) — AND Cerid carries the {1,2,4,8} determinism MOAT pyamg/scipy lack. **⇒ COMPLETE honest symmetric picture: crush vs DIRECT (shift-invert ARPACK) + parity vs SAME-CLASS (pyamg-LOBPCG) + the moat no peer carries.** | ~55 (script) | — | — |
| ↳ **v6-e-c** ✅ DONE 2026-06-06 | **Generalized LOBPCG** `A·x = λ·B·x` (A sym, B SPD) — the FEM modal/buckling `K·x = λ·M·x` form. **DONE: `lobpcg.hpp` `eigs_sym_gen_lobpcg(a, b, opts, alloc, precond)` — a SEPARATE function (zero risk to the tested v6-e-a path). Every inner product is the B-inner-product ⟨u,v⟩_B = uᵀ·B·v: S=[X,W,P] kept B-ORTHONORMAL (SᵀBS=I) by block MGS ⇒ RR stays plain `eig_sym(SᵀAS)`; residual R = A·X − B·X·Θ; each column carries BOTH its A-image AND B-image through the MGS subtractions ⇒ the only fresh matvecs/iter are A·W + B·W; generalized relative residual ‖A·x−λ·B·x‖/(‖A·x‖+|λ|‖B·x‖).** Tests `[eigen]`: (1) DIAGONAL B vs a DENSE reference (`eig_sym` of D⁻¹ᐟ²·A·D⁻¹ᐟ², whose eigenvalues ARE the generalized ones) — rigorous smallest 4; (2) mass-tridiag B — residual <1e-6 + B-orthonormality xᵀBx=I; (3) {1,2,4,8} MOAT bit-identical. Worked FIRST try (careful B-image bookkeeping). VERIFIED win-debug + win-tidy + gcc + **win-asan**. ⚠ OWED (the crush regime): the preconditioned generalized + HARDER/anisotropic matrices + the same-class pyamg-LOBPCG floor. | ~290 | ~3 | — |
| ↳ **v6-e-d** ✅ DONE 2026-06-06 | **Owed crush extensions for generalized LOBPCG** (the v6-e-c ⚠OWED items; TEST + BENCH/FLOOR only — the `precond` param is already wired through `eigs_sym_gen_lobpcg`, so NO new engine surface, mirroring v6-e-b). **(A) Preconditioned generalized** — the FIRST-ever exercise of `lobpcg.hpp` lines 540–543 with a non-null precond: IC0/AMG built on the **stiffness `A=K`** (T≈K⁻¹, the textbook preconditioner for smallest generalized pairs; Dirichlet-BC ⇒ nonsingular) cuts iters-to-tol on `K·x=λM·x` (MECHANISM, NOT a cross-library crush — a V-cycle ≠ an A-apply); same eigenvalues vs unpreconditioned; sized so the unprec baseline is clearly large. **(B) Moat** {1,2,4,8} bit-identical WITH **IC0** in the loop (IC0 = deterministic + serial-applied; AMG has parallel internals ⇒ AMG in the mechanism test, IC0 in the moat test). **(C) DROP-branch coverage** (advisor-flagged: the B-MGS rank-deficiency `if bn>drop` never fired in v6-e-c) — PROVEN to fire via the dimension argument (n=10, nev=4 ⇒ 3·nev=12 > n ⇒ ≥2 of the 12 candidate cols MUST drop once P is populated; observable: `iterations≥3` forces the iter-1 body to have run + eigenvalues still correct). **(D) Anisotropic / harder matrices** — CHARACTERIZATION not optimization: anisotropic stiffness (per-axis coeffs) for standard + generalized; show convergence holds + honestly report SA-AMG degradation (the "general FEM is a hypothesis" guard; do NOT fix AMG — semi-coarsening/line-smoothers = out of scope). **(E) Same-class generalized floor** — `bench_hesap_eigen_gen_lobpcg.cpp` (Cerid iters on `K·x=λM·x`, AMG-on-K) + `scripts/eigen_floor_gen_pyamg.py` (`pyamg.smoothed_aggregation_solver(K)` + `scipy lobpcg(K, X, B=M, M=amg_K)`); matrices reproduced BIT-IDENTICALLY in C++/Python (same node ordering + same diagonal-M formula) ⇒ iterations-parity is meaningful; report PARITY + the moat. **⚠ The comprehensive direct-ARPACK/PRIMME/Spectra crush bench stays a v6-z deliverable — NOT pulled forward** (the floor here proves same-class parity). **RESULTS:** (A) first-ever exercise of the generalized precond path WORKED — `gen iters: unpreconditioned 154 → IC0 48 → AMG 28` (3.2×/5.5× fewer, same eigenvalues, all converged, 2D Laplacian K + diagonal mass M). (B) moat `ident=true` {1,2,4,8} with IC0. (C) DROP branch PROVEN fired: `iterations=3` (iter-1 body ran ⇒ 12 candidates > n=10 ⇒ ≥2 dropped) + correct eigenvalues. (D) anisotropic: robust convergence + MEASURED AMG degradation at STRONG (1000:1) anisotropy (`AMG iters isotropic 15 → anisotropic(exx=1e-3) 55` = 3.7×, BOTH converge; IC0(aniso) 47; generalized-anisotropic IC0 42 converges, eigenvalues correct) — the SA-AMG-degrades hypothesis confirmed by measurement, comfortable margin (not extrapolated, not a fragile ±-drift inequality). (E) **GENERALIZED FLOOR PARITY PROVEN** — Cerid `27/23/23` vs pyamg+scipy `27/24/21` (s=16/20/24, BIT-IDENTICAL matrices + eigenvalues) ⇒ same-class parity + the {1,2,4,8} moat the peer lacks. **VERIFIED win-debug ctest (5 cases + all 5 guards) + win-tidy + win-clang-cl + gcc-release (real-threading moat) + the generalized bench compiles/runs + the pyamg floor run in WSL.** | ~350 | ~10 | — |
| ↳ **v6-f** ✅ DONE 2026-06-06 | **Jacobi-Davidson** (JDQR, symmetric) — Davidson subspace + the correction equation `(I−ŨŨᵀ)(A−θI)(I−ŨŨᵀ)t=−r` solved inexactly via **FGMRES** (NEW acyclic edge hesap-eigen→hesap-iterative, mirrors v5f-c2) with the **projected preconditioner** `(I−ŨŨᵀ)K⁻¹(I−ŨŨᵀ)` = the **⭐ algorithmic-crush hook**; thick restart; one-at-a-time deflation for `nev` pairs. `jacobi_davidson.hpp` `eigs_sym_jd<T>` + `JdProjectedOp`/`JdProjectedPrecond` LinearOps. **SCOPE (advisor-gated, explicit deferral per AGENTS.md): ships EXTREME (Smallest/LargestAlgebraic) + CLUSTERED (deflation handles multiplicity) + the preconditioned correction; INTERIOR via harmonic-Ritz extraction is DEFERRED — covered by v6-d shift-invert, and standard-Ritz interior is unreliable (spurious values).** Moat: RR (`eig_sym`) + FGMRES (serial Arnoldi/Givens) + fixed-order MGS + deterministic start/restart ⇒ bit-exact spmv ⇒ worker-identical inner iteration count ⇒ {1,2,4,8} bit-identical (UNPRECONDITIONED core moat **+ IC0 moat** covering the projected-precond path — the JD identity must not be dead-under-moat). **HONEST: the matvec-reduction MECHANISM is the v6-f crush deliverable; the cross-library wall-clock crush vs ARPACK/PRIMME/Spectra stays v6-z (NOT pulled forward).** **RESULTS:** extreme smallest+largest 1D-Laplacian vs analytic (largest are CLUSTERED near 4 — JD converges them where v6-a no-restart Lanczos stalled); **EXACTLY-degenerate eigenvalue resolved** (square 2D grid λ_{1,2}=λ_{2,1}, deflation locks both copies — assert VALUES, degenerate vectors non-unique); **MECHANISM — a preconditioner cuts total matvecs `unprec 985 → IC0 897 → AMG 742`** (the counts are **BIT-IDENTICAL across MSVC/clang-cl/gcc** ⇒ the strict-inequality margin is drift-proof, not a fragile ±-drift compare); {1,2,4,8} moat bit-identical UNPRECONDITIONED **and IC0-in-the-loop** (covers the projected-precond path). **VERIFIED win-debug ctest (5 cases + all 5 guards) + win-tidy + win-clang-cl + win-asan (projected-op raw indexing clean) + gcc-release (build + moat under real threading).** `jacobi_davidson.hpp` + `tests/hesap-eigen/test_jacobi_davidson.cpp` + the hesap-iterative CMake edge + eigen.hpp umbrella. | ~350 | ~12 | — |
| ↳ **v6-g** ✅ DONE 2026-06-06 | **FEAST** (Polizzi 2009) — contour-integration for ALL eigenvalues in `[lo,hi]`; the INTERVAL/INTERIOR specialist. `feast.hpp` `eigs_sym_feast<T>(a, lo, hi, m0, opts, alloc, num_workers)`. Spectral projector `ρ=(1/2πi)∮(zI−A)⁻¹dz` via **8-pt Gauss-Legendre on the half-contour** (real-symmetric ⇒ conjugate-pair fold ⇒ `Q≈Σ_k Re[ω_k·(z_kI−A)⁻¹Y]`); each `(z_kI−A)⁻¹` = a COMPLEX shifted solve factored ONCE by the **v5 complex multifrontal LU** (`factor_multifrontal_lu<Complex<T>>`; z_k off-axis ⇒ well-conditioned; REUSES the v6-d hesap-direct edge — NO new edge); subspace iteration → MGS → RR `eig_sym(QᵀAQ)` → select `[lo,hi]`. **⚠ KEY (advisor-corrected after empirical check): the in/out SELECTION is done by TWO filters — (1) the INTERVAL test `λ∈[lo,hi]` (the just-outside Ritz land OUTSIDE) + (2) the RESIDUAL GATE `‖A·x−λ·x‖≤tol` (excludes a spurious in-interval Ritz — the safety net). The MGS rank-drop (`norm<16·eps`) is DEFENSIVE only: an 8-pt quadrature does NOT collapse the surplus columns to machine zero (~1e-3 ≫ 16·eps), so `q==m0` always (VERIFIED by instrumenting q vs m0) — the drop fires only on an EXACT collapse, NOT the design rank-deficiency. My first framing headlined the drop; the residual gate is the real safety net.** Contract: `m0 ≥ count` + headroom (no-headroom ⇒ `converged=false`). **RESULTS:** smallest 1D-Laplacian band vs analytic (count + values); **the DISCRIMINATING interior test** — eigenvalues placed JUST OUTSIDE both endpoints are EXCLUDED while the inside 3 are returned (orthonormalized-Q ⇒ Ritz values are scale-invariant to ω_k, so the in/out separation is what the contour+residual govern — a value-only check wouldn't catch a wrong filter); count robust to the `m0` over-estimate (3 at m0=6 and m0=10); **{1,2,4,8} moat NON-VACUOUS** — a 2D-grid matrix (parallel elimination tree, unlike a 1D chain) + a FORCED-parallel SELL spmv (`num_workers>1`) ⇒ genuine parallel reductions, bit-identical under real gcc threading. **VERIFIED win-debug ctest (4 cases + all 5 guards) + win-tidy + win-clang-cl + win-asan (complex factor/solve + parallel spmv + projector indexing clean) + gcc-release (non-vacuous moat under real threading).** HONEST: the interval/interior capability + the determinism moat are the deliverable; wall-clock crush vs FEAST-lib/ARPACK = v6-z. nq=8 hardcoded (parameterizing ⇒ general GL nodes, follow-on). | ~300 | ~10 | — |
| ↳ **v6-h** ✅ DONE 2026-06-07 | **IRLBA** (Baglama-Reichel 2005) — the sparse SVD: largest singular triplets of a rectangular A via Golub-Kahan-Lanczos BIDIAGONALIZATION + **thick restart** (the deterministic equivalent of the implicit restart). `svds.hpp` `SvdResult<T>` + `svds<T>(a, opts, alloc)` (a = `LinearOp` with `apply`=A·v, `apply_adjoint`=Aᵀ·u — `SparseLinearOp`/`ParallelSpmvLeastSquaresOp`). GKL → tiny upper-bidiagonal B → dense `svd(B)` → σ=Σ, u=U_k·U_B, v=V_k·V_B. **⚠ TWO correctness points (advisor-flagged): (1) FULL reorthog is BOTH-SIDED (each new u against U AND each new v against V, MGS-twice — one-sided leaks orthogonality ⇒ ghost singular values); (2) the SVD sign is COUPLED (σ≥0 links u,v) — u,v taken STRAIGHT from the dense svd's already-pinned U_B/V_B, NO independent re-sign-pin (would break A·v=σu).** Thick restart (Baglama-Reichel augmented): keep j=2·nev largest triplets (A·Ṽ=Ũ·diag(σ), the kept block diagonal) + spike couplings ρ_i=β_last·P[last][i], continue GKL from the residual v_res; the restarted B = diag(Σ)+spike-column+trailing-bidiag ⇒ converges at bounded ncv≪min(m,n). **SCOPE (explicit defer, advisor-gated): LARGEST triplets; SMALLEST DEFERRED** (GKL converges the bottom glacially — needs shift-invert-on-normal-equations / harmonic, a separate mechanism, like v6-f's harmonic-interior defer). **RESULTS:** core converges the largest 4 of a tall A (spiked spectrum) vs the dense `svd` oracle to machine precision + the coupled-sign check A·v=σu ∧ Aᵀu=σv (machine-exact) + unit norms; **thick restart converges a CLOSELY-SPACED spectrum at bounded ncv=20 in 3 cycles where no-restart cannot** (`iterations≥2` asserts restart engaged); an f32 case (vs the dense f32 oracle, relaxed tol); {1,2,4,8} moat bit-identical σ/U/V (both spmv directions forced parallel via `ParallelSpmvLeastSquaresOp`, min_stored_bytes=0 — the only parallel reductions; no factor). **VERIFIED win-debug ctest (4 cases + all 5 guards) + win-tidy + win-clang-cl + win-asan (bidiag buffers + restart combination + dense svd clean) + gcc-release (moat under real threading).** HONEST: parity + the moat (no ARPACK/PROPACK/scipy.svds carries the determinism moat); no crush bench (parity is the ceiling). | ~300 | ~10 | — |
| ↳ **v6-z** (in progress 2026-06-07) | **CLOSE.** ✅ DONE: **gold-standard CRUSH VERDICT** (`scripts/eigen_ref_primme.py` added — PRIMME 3.2.3; all peers in one Linux/gcc-release env, eigenvalues identical = the gate; @n=43680 AMG-LOBPCG **CRUSHES direct shift-invert ARPACK 9.5× wall + 40× mem** [growing] + **PARITY vs state-of-art PRIMME** at f64 + PARITY vs pyamg-lobpcg + the moat none carry — crush-vs-direct + parity-vs-same-class is the WIN, not a speed-win over PRIMME+AMG) · **end-to-end {1..16} moat ALL methods** (sed-extended all 11 `[moat]` tests; 132 assertions/13 cases green) · **complex-completeness audit** (table in the system doc: symmetric methods real f32/f64, Arnoldi/KS emit complex, Hermitian/complex-SVD = consumer-driven follow-ons) · **`docs/systems/hesap-eigen.md`** (the verdict's durable home + method set + edges) · **ADR-0089** (module edges + the v6 design decisions; README index + this row). ✅ **CLI `hesap.eigen.*`** (`cli_register_eigen.cpp` + `cli_anchor.hpp`: `sym.{f32,f64}` [thick-restart Lanczos, nev/which] · `svds.{f32,f64}` [IRLBA, nsv] · `shift_invert.f64` [interior, sigma] · `feast.f64` [interval, lo/hi/m0]; COO triplets in → `[nconv, values..., residuals...]` blob out, ASCII descriptions; `test_cli.cpp` 36 asserts — registration + sym + feast + svds invocations; **non-ascii test-name guard PASS**). **VERIFIED v6-z close: win-debug (full suite 464 asserts/43 cases) + win-tidy + win-clang-cl + win-asan ([cli]+[moat] 168 asserts) + gcc-release (moat {1..16} under real threading).** ✅ **SVD VERDICT MEASURED** (`bench_hesap_svds.cpp` + `scripts/svds_ref.py`, 2D grid incidence matrix, largest 4, matched tol, **singular values bit-identical across Cerid/scipy.svds/primme.svds = the gate**): wall-clock competitive-to-favorable (Cerid ≤ primme.svds at all sizes; ≤ scipy.svds to n≈4.3K; scipy ~1.6× ahead at n=9504, ARPACK-on-normal-eqns scales better on the clustered-largest, Cerid restart cycles→84). **⚠ HONEST: the Python peers pay per-matvec reverse-communication overhead a native caller wouldn't (primme 654–1422 matvecs) ⇒ the wall-clock wins OVERSTATE the algorithm gap; the RELIABLE claims = matched accuracy + the {1..16} moat; algorithmically PARITY (Krylov-bidiag). A competitive sparse SVD with the moat — not a clean wall-clock crush.** **PRAGMATIC (user direction): 18-config full sweep SKIPPED (left to CI); validated in win-release (LTCG, 471/44) + linux-release (471/44) in lieu — both benches LTCG-compile.** ✅ v6-z DONE. | ~250 | ~10 | — |
| **v7** | **OPTIMISATION — the full domain (unconstrained + constrained), elite/gold-standard. New module `crd-hesap-opt`** (ADR-0065 D14; **absorbs the old v8 constrained cluster** per 2026-06-07 user direction — optimization is ONE domain). **The universal "find the best X" substrate**, consumed by: eylem (constrained dynamics / IK / trajectory / powered-ragdoll motors) · estimation+control 3.1.11 (MPC=QP/NLP per step · LQR · Kalman/EKF=LS · **SLAM/bundle-adjustment**=sparse nonlinear-LS) · FEA/CFD (topology/shape/design opt · parameter ID) · CAD 3.1.9 (sketch constraint solving) · ML (training=stochastic opt · differentiable sim) · rendering (calibration · BRDF-fit · photogrammetry-BA) · games (AI utility/RL · CSP · balancing) · robotics (inverse dynamics · motion planning · calibration) · DAW (filter design). Backed by hesap-dense/sparse/direct/iterative/eig (every Newton step = a linear solve; trust-region/exact = a linear/eig subproblem) + autodiff (gradients, future). **GOLD STANDARDS:** Ceres (nonlinear-LS/BA) · liblbfgs/scipy L-BFGS-B · NLopt · scipy.optimize · CMA-ES/pycma · PyTorch/JAX optimizers · **OSQP** (sparse QP) · **IPOPT** (NLP IPM) · qpOASES · SCS · HiGHS/GLPK (LP/MIP). **DETERMINISM MOAT (the differentiator none carry):** bit-identical optimization TRAJECTORY + result across {1..16} workers (the KKT/Newton/linear solves inherit the hesap-direct/iterative/eig moat) ⇒ certifiable MPC/optimal-control (DO-178C/ISO 26262) + reproducible ML training + replay. **CONSUMER-PULL SEQUENCING (advisor):** spine = a→b→c→d(L-BFGS)→e(LM) [unconstrained] + j→k(QP/OSQP)→n(NLP/SQP/IPOPT) [constrained]; p/q/r = slip-candidates (no named Cerid consumer pulls them yet). Per-slice advisor pass + gold-standard verdict each. | ~7000 | ~240 | ~5–6 wk |
| ↳ **v7-a** (DONE 2026-06-07) | **Substrate.** ✅ New module `crd-hesap-opt` (ADR-0090; edges pinned up front). `Objective<T>` (value+n pure; gradient/hessian_vector virtual default-false; `has_gradient()`/`has_hessian_vector()` capability flags LinearOp-style; **vtable append-at-end**, RESERVED fused `value_and_gradient` slot) + `OptResult<T>`/`OptOptions<T>`/`OptStatus` + `check_convergence` + `LineSearch<T>` interface + `BacktrackingArmijo<T>` + `QuadraticObjective<T>` (½xᵀAx−bᵀx over a LinearOp) + `minimize_gradient_descent<T>` (first end-to-end optimizer; line-search g_out cost contract). **MOAT:** GD trajectory bit-identical {1,2,4,8,16} over `ParallelSparseLinearOp` (asserts convergence too — not vacuous). **VERIFIED:** win-debug (ctest, non-ascii guard, 33 asserts/3 cases) + win-tidy + win-clang-cl + win-asan + win-shipping + **gcc-release** (caught a real NDEBUG-only `-Werror=unused-variable` masked by assert-active configs → `[[maybe_unused]]`). ADR-0090 + README index + `docs/systems/hesap-opt.md`. | ~600 | ~33 | — |
| ↳ **v7-b** (DONE 2026-06-07) | **Derivatives.** ✅ `Dual<T>` forward-mode AD scalar (algebra + sin/cos/tan/exp/log/sqrt/tanh/abs/pow chain rules, value-comparisons) · `finite_difference_gradient` (forward/central, **scale-relative step** `√ε·max(\|xᵢ\|,1)` / `ε^⅓·max(\|xᵢ\|,1)` + true-step recovery — a fixed absolute step is silently wrong on scaled vars, advisor) + `FiniteDiffObjective<T>` decorator (value-only objective → gradient via FD) · `forward_ad_gradient<T>(functor,…)` (EXACT, fused value+gradient, n Dual sweeps; the reserved `value_and_gradient` in spirit) over a `DiffFunctor` concept + `make_objective_from_functor<T>`/`FunctorObjective` adapter (AD plugs into the virtual `Objective` per ADR-0090 §6) · `gradient_check` harness (analytic vs central FD, worst rel-err; catches a wrong hand-gradient). **MOAT:** FD gradient over `ParallelSparseLinearOp` bit-identical {1,2,4,8,16} (a new composition v7-a didn't exercise — v7-d L-BFGS's moat rests on it). **COMPLEX-STEP DEFERRED** (advisor: a redundant, narrower-domain — holomorphic-only — alternative to forward-AD that also needs complex transcendentals the hesap `Complex` lacks; exact 1st derivatives are already delivered by forward-AD ⇒ clean deferral, NO gold-standard gap). Reverse-mode AD = the separate ADR-0065 autodiff module (the BA/ML workhorse), same interface. **VERIFIED:** win-debug (95 asserts/9 cases + non-ascii guard) + tidy + clang-cl + asan + win-shipping + gcc-release (caught another NDEBUG-only `-Werror=unused-variable` in the assert-only `ok`/`gok` → `[[maybe_unused]]`). | ~700 | ~95 | — |
| ↳ **v7-c** (DONE 2026-06-07) | **Line searches.** ✅ `WolfeLineSearch<T>` (weak + strong Wolfe via Nocedal-Wright bracketing + bisection-zoom, Alg 3.5/3.6 — the textbook-robust quasi-Newton search L-BFGS needs) + `MoreThuenteLineSearch<T>` (**MINPACK-2 `dcsrch`/`dcstep` ported verbatim** — cubic/quad interpolation + modified-function ψ; the liblbfgs/Ceres default; the 3 advisor-flagged bug-loci guarded: `max(0,·)` discriminant + per-case γ sign, literal stage-1→unmodified switch `f≤ftest1 ∧ min(c1,c2)φ'(0)≤dg`, xtol interval + stmin/stmax + eval-cap termination). Both honor the v7-a cost contract (eval ∇f at trial points ⇒ `grad_at_new_valid=true`). Armijo from v7-a. **CORRECTNESS-ONLY** (advisor: no compiled peer for a STANDALONE line search ⇒ no gold-standard claim here; More-Thuente's eval-count parity vs liblbfgs is measured at v7-d where its consumer + bench-peer live — the v6-z lesson). Tests recompute the Armijo+curvature conditions; More-Thuente HARDENED across the state machine (advisor: one easy convex fn is thin for an intricate port) — Moré-Thuente fn 1 (φ=−t/(t²+β)) + **fn 2 (near-flat (t+β)⁵−2(t+β)⁴ ⇒ the stage-1/modified-function path) + convex-overshoot (α₀=8 ⇒ dcstep case 1 + bracketing)**, each asserting strong Wolfe + α near the known minimizer + bounded evals. **MOAT (non-vacuous, on BOTH Wolfe and the branchier More-Thuente):** line search over `ParallelSparseLinearOp` — ok + Wolfe conditions hold + zoom/bracketing genuinely entered (evals>1) + α/x bit-identical {1,2,4,8,16}. **VERIFIED 6 configs** (win-debug 180 asserts/16 cases + non-ascii guard · tidy · clang-cl · asan · win-shipping · gcc-release). ⚠ **v7-d forward-flag** (advisor): More-Thuente returns `ok=false` on its warning paths (rounding/xtol/at-bound) even with a usable decreased point — when wiring into L-BFGS, return the best point with a status rather than hard-aborting, or hard problems terminate prematurely (liblbfgs's policy). | ~500 | ~80 | — |
| ↳ **v7-d** ⭐ (core+bench DONE 2026-06-07; L-BFGS-B → v7-d-3) | **Quasi-Newton.** ✅ `minimize_lbfgs` (Nocedal two-loop recursion over m (s,y) pairs in a ring buffer + γ-scaling + curvature-skip; liblbfgs initial-step rule α₀=1/‖g‖₂ on iter 0 then 1; **eval-count instrumented** `OptResult::fn_evals`/`grad_evals` — the verdict metric) + dense `minimize_bfgs`/`minimize_sr1` (`minimize_quasi_newton`, inverse-Hessian H, rank-2 BFGS / rank-1 SR1 + skip-safeguards, small-n, SR1 steepest-fallback if H·g not descent). More-Thuente default line search wired with the warning→best-point policy (no premature L-BFGS abort). **MOAT (non-vacuous):** L-BFGS over `ParallelSparseLinearOp`, κ~1700 problem runs >m iters so the ring WRAPS — trajectory bit-identical {1,2,4,8,16}. **GOLD-STANDARD BENCH (the crush verdict — `bench_hesap_lbfgs_vs_reference` + `scripts/setup-lbfgs-ref.sh` + `lbfgs_ref_scipy.py`):** Cerid vs **liblbfgs** (its default line search IS More-Thuente — the ideal eval peer; built SSE2-fair) + **scipy L-BFGS-B**, same analytic f+g, matched m=8 + matched More-Thuente + accuracy shown via achieved ‖g‖∞, across **3 More/Garbow/Hillstrom function classes** (Rosenbrock-N curved valley · Powell-singular singular-Hessian · Beale low-dim — advisor: one family is thin for a "no eval gap" claim). **VERDICT: independent L-BFGS impls AGREE on iteration + point-eval count (ratio 0.93–1.02× across all classes; Powell-40 Cerid AHEAD 38 vs 41 evals) + reach x* — Cerid's port is algorithmically FAITHFUL to the gold standard (no eval gap across function shapes; the advisor's predicted More-Thuente-fidelity failure RULED OUT).** Wall-clock PARITY vs the compiled peer (within ~10%; the small N=1000 gap = Cerid's separate value()+gradient() = 2 passes/pt vs liblbfgs's 1 fused — the reserved `value_and_gradient` slot's opportunity). HONEST framing (advisor): plain L-BFGS is the SAME algorithm ⇒ eval-PARITY is the ceiling; the **determinism moat liblbfgs/scipy lack is the differentiator**. **VERIFIED 6 configs** (win-debug 206 asserts/20 cases + guard · tidy · clang-cl · asan · win-shipping [clean rebuild — struct-growth stale-obj LTCG hazard] · gcc-release); bench WSL-gated (`CRD_BUILD_HESAP_VS_LBFGS`). | ~900 | ~206 | — |
| ↳ **v7-d-3** (DONE 2026-06-07) | **L-BFGS-B (bound-constrained).** Full Zhu-Byrd-Lu-Nocedal port (the exact code scipy wraps), via Stephen Becker's C — routine-for-routine. `minimize_lbfgsb<T>(obj,x0,lower,upper,opts,alloc,m,factr)` (l≤x≤u, ±1e30 = unbounded; pgtol=grad_tol; factr·ε relative-f). ⭐ **DIFFERENTIAL-TESTED vs the reference C** (`runtime/examples/lbfgsb_difftest.cpp` + `scripts/setup-lbfgsb-ref.sh`, gated `CRD_BUILD_HESAP_VS_LBFGSB`): **55 checks, 0 fail** — every routine bit-identical on identical inputs incl. REALS+INT arrays in bug-HIDING regimes (dtrsl all-jobs, matupd `iupdat>m`, hpsolb ties, **cauchy GCP breakpoint-rich, formk+inner-Cholesky, subsm proj+backtrack**) + the **`mainlb` driver end-to-end vs `setulb`** (reverse-comm→direct restructure; bounds-active Rosenbrock, x/f within 1e-9, ≥1 var pinned). ⚠ a MANUAL AUDIT first found 4 off-by-ones in the EASY routines (the 1-based↔f2c-0-based-BLAS convention = a bug farm) — the harness adjudicated them (manual audit ≠ verification, advisor). Catch2 (`test_lbfgsb.cpp`): bounded-Rosenbrock active-bound convergence + unbounded→unconstrained reduction + the **{1,2,4,8,16} active-bound moat** (bit-identical trajectory, non-vacuous: status==Success on PROJECTED grad + ≥1 var pinned + iterations>1). ⚠ `grad_norm` here = the PROJECTED-gradient ∞-norm (the right cert for bounds; document at v7-z that it differs from other optimizers' plain ‖∇f‖∞). VERIFIED 6 configs (win-debug 236 asserts/23 cases + guard · tidy · clang-cl · asan · win-shipping · gcc-release incl. real-threading moat). | ~1200 | ~99 | — |
| ↳ **v7-e-1** (DONE 2026-06-07) | **Nonlinear-LS substrate + dense LM/GN core.** `ResidualFunction<T>` (r∈R^m + dense row-major Jacobian + capability flag; vtable-append-at-end, RESERVED sparse-Jacobian + fused slots — distinct from `Objective` because GN needs J) + `minimize_levenberg_marquardt<T>` (damped normal equations (JᵀJ+λ·diagJᵀJ)δ=−Jᵀr, **Marquardt scaling + Madsen-Nielsen-Tingleff ν-update via the trust-region gain ratio ρ** = the lmder/Ceres rule ⇒ comparable iteration count; inline SPD Cholesky, λ-raise on non-PD) + `minimize_gauss_newton` (λ≡0) + **robust losses** (Huber/Cauchy/Tukey IRLS reweight). VERIFIED 6 configs (win-debug 259 asserts/28 cases + guard · tidy · clang-cl · asan · win-shipping · gcc-release): LM converges on exp-fit (recovers params, cost→0) + 2-residual Rosenbrock; GN on well-conditioned; **robust Cauchy resists a gross outlier** (closer to truth than plain LS); run-twice bit-identity (serial dense ⇒ determinism by construction; the cross-worker {1..16} moat over a PARALLEL Jacobian is v7-e-2). ⚠ normal-eqns squares κ(J) (λ mitigates; it's the sparse-crush enabler) — MINPACK 'lm' is QR-based, so report cost+‖Jᵀr‖, not eval-count bit-match (advisor). | ~700 | ~99 | — |
| ↳ **v7-e-2** ⭐ (core DONE 2026-06-07; bench DONE 2026-06-08 → v7-e-2-bench row) | **Sparse-Jacobian LM — the crush vehicle (core shipped).** `ResidualFunction::sparse_jacobian` (CSR, reserved vtable slot) + `minimize_levenberg_marquardt_sparse` (J→JᵀJ via transpose+spgemm · g=Jᵀr · λ·diag · **moat-proven hesap-direct `SupernodalCholesky` factor+solve** · Madsen-Nielsen ν). NOT in the opt.hpp umbrella (the hesap-opt→hesap-direct edge; include explicitly). VERIFIED 6 configs (win-debug 279 asserts/31 cases + guard · tidy · clang-cl · asan · win-shipping · gcc-release): converges on a well-conditioned nonlinear sparse chain (the x² chain was exp-ill-conditioned — swapped); sparse path == dense LM; **{1,2,4,8,16} cross-worker moat** bit-identical via the supernodal v5a factor (iterations>1, non-vacuous) — the determinism differentiator Ceres lacks. ⛔ **BENCH OWED (the wall-clock crush headline):** (1) `SupernodalCholesky` symbolic-once/numeric-per-trial split (THE GATE — Ceres caches symbolic; else LM re-pays the v5a symbolic per λ-trial; ~750-line v5a refactor, re-verify the v5a moat) → (2) bench scipy `least_squares` ('trf' sparse-J) then TIMEBOX Ceres (else honest indirect: Cerid Cholesky beats CHOLMOD v5a = Ceres-sparse). HONEST CLAIM (advisor): matched Nielsen iters + per-iter FACTOR faster + the moat — NOT beat-Ceres-at-NLS-generally (Ceres wins Jacobian-eval/Schur-BA/threading). | ~600 | ~37 | — |
| ↳ **v7-e-2-bench** ⭐ (DONE 2026-06-08) | **Sparse-Jacobian LM crush vehicle + the CHOLMOD head-to-head.** ✅ THE GATE: `SupernodalCholesky::factorize(…, reuse_symbolic)` + `refactorize()` (symbolic AMD+etree+amalgamation paid ONCE; numeric per λ-trial — Ceres caches symbolic the same way), bit-identical, moat-preserving. ✅ Honest bench vs **CHOLMOD** (= what Ceres-sparse uses), JᵀJ from a 3D-elastic-lattice NLS (`dump_nls_lattice_jtj`), same AMD order + matched accuracy. ⭐ **PERF CRUSH ARC** (all profile-/poison-measured real causes, never guesses; all moat-safe): **(1) SYRK** — symmetric Schur/trailing lower-triangle-only (`syrk_lower_minus` + cmod `col_limit` + (C) balanced two-pass) ⇒ the 1.39× flop excess GONE, **Cerid min-flops == CHOLMOD cc.fl** with ~22% LESS fill; **(2) balanced-triangular primitive** (`detail/parallel_triangular.hpp`, `triangular_bound=round(n·√(k/w))`) + fork **size-gate**; **(3) uninit scratch** (`ubuf.resize_uninitialized`) — surfaced+fixed a real **GEMM beta=0 BUG** (`blas3.cpp` did `0*C`→NaN from uninit/stale pages; now stores 0 = BLAS-spec; bit-identical for finite C; all 4 gemm sites). **8T scoreboard (FACTOR, CHOLMOD/Cerid):** hood **1.53×** · ldoor **1.69×** · bmwcra_1 **1.09×** (was the ADR-0082 wall) · lat20 **0.95×** · **lat24 0.99× = PARITY** · lat28 0.87× · lat32 **0.83×** (0.66× at start). ⭐ **RESIDUAL lat32 gap MEASURED + PINNED:** the SERIAL per-thread GEMM kernel (W=1 0.73×), NOT scaling (Cerid **3.16×** vs CHOLMOD **2.76×** @8T — scales BETTER), NOT Amdahl (cdivA serial-chain=1.5%) = the ADR-0082 intrinsic-vs-OpenBLAS wall (asm ruled out by the user); the {1..16} moat is the standing differentiator. **VALIDATED:** win-debug (dense gemm 359508 + direct moat 598861) · win-asan (no OOB/UAF) · win-shipping (353945/597995) · win-tidy · gcc(WSL bench) · **NaN-poison** (the moat CANNOT catch a UMR — see [[feedback_gemm_beta0_must_store_zero_and_umr_validation]]) · all 6 ctest guards. Session: `docs/sessions/2026-06-07-v7e2-gate-and-nls-cholmod-bench.md`. ⏸ Direct scipy-`least_squares`/Ceres probe deferred (honest indirect: Cerid Cholesky beats CHOLMOD = Ceres-sparse, advisor-endorsed). | ~900 | ~37 | — |
| ↳ **v7-f** (DONE 2026-06-10) | **First-order.** ✅ Steepest descent = v7-a. **Nonlinear CG** (`conjugate_gradient.hpp`, landed with `b261478`): FR / PR⁺ / HS / DY β-variants + Powell + n-step restarts + descent-direction safeguard + strong-Wolfe c2=0.1 (Al-Baali; FR needs c2<½) + the N&W §3.5 CG initial-step rule; SHARP gold-check = finite termination ≤ n on an SPD quadratic under an EXACT line search (nonlinear CG ≡ linear CG there) + all 4 variants on Rosenbrock-N + the `{1,2,4,8,16}` moat. **Momentum** (`momentum.hpp`, this slice): Polyak heavy-ball + Nesterov (FISTA form), fixed-μ or parameter-free FISTA t-sequence + **O'Donoghue-Candès gradient restart** (∇f(y)·(x⁺−x)>0 ⇒ reset t+velocity); μ=0 degenerates to fixed-step GD; Nesterov optimality test at the lookahead (documented proxy; Success-at-tol returns y itself so gradient and iterate agree). SHARP gold-check = **the acceleration THEOREM**: on the κ≈1712 1-D-Laplacian quadratic, optimally-tuned heavy-ball (α=4/(√λmax+√λmin)², μ=((√κ−1)/(√κ+1))²) and parameter-free NAG each reach grad_tol in >4× fewer iters than plain fixed-step GD at the same α=1/L (true ratio ~√κ); + log-cosh convex non-quadratic (L=1) incl. fixed-μ + restart-off branches + `{1,2,4,8,16}` moat + n=0/n=1 boundary. v7-f total: **91 asserts / 7 cases** (file-captured). Verified win-debug + asan + shipping + tidy (module-local per directive; CI owns the sweep). Eval-parity bench vs scipy 'CG' deferred to v7-z per plan. En-route fixes: pre-existing `kTriPanel` tidy naming in `supernodal_cholesky.cpp`; root-caused **+ FIXED** the win-shipping/win-tidy-local silent-stale-obj landmine (mismatched `msvc_deps_prefix` ⇒ `#deps 0`; wiped + reconfigured, deps verified `0→95` — CLAUDE.md Troubleshooting). | ~590 | 91 | — |
| ↳ **v7-g** (DONE 2026-06-10) | **Newton family.** ✅ `Objective<T>` grew `hessian` (dense row-major) + `sparse_hessian` (CSR) — vtable slots 5/6 APPENDED AT END + capability flags; `OptResult::hess_evals` added. **THREE drivers:** `minimize_newton` (`newton.hpp`, umbrella) = dense FULL+MODIFIED Newton — N&W Alg 3.4 τ·I Cholesky-retry (τ=0 ⇒ pure Newton, quadratic rate; escalation ⇒ guaranteed descent through indefinite regions), strong-Wolfe α₀=1, reuses LM's `detail::chol_solve`. `minimize_newton_cg` (`newton_cg.hpp`, umbrella) = matrix-free TRUNCATED Newton (N&W Alg 7.1): inner CG on Hessian-vector products + NEGATIVE-CURVATURE exit + Eisenstat-Walker forcing η=min(0.5,√‖g‖) ⇒ superlinear (the scipy-'Newton-CG' rule, the v7-z eval-parity peer); hess_evals counts H·v products. `minimize_newton_sparse` (`newton_sparse.hpp`, NOT in umbrella — hesap-opt→hesap-direct edge like sparse-LM) = CSR Hessian → **moat-proven supernodal Cholesky** with the v7-e-2 SYMBOLIC-ONCE gate (fixed-sparsity contract; refactorize per iteration/τ-retry) + τ·I via info()!=0. SHARP gates: one-step+one-hess on an SPD quadratic (exact Newton step) · classic Rosenbrock-2 · double-well SADDLE ESCAPE (both dense-τ and inner-CG-exit; |x₀|=1, f*=−¼) · sparse==dense on a convex quartic chain · sparse τ-path through an indefinite coupled-well start (uniform start ⇒ all-ones well) · **TWO `{1,2,4,8,16}` moats** (Newton-CG over parallel-spmv H·v; sparse Newton through the tree-parallel supernodal factor) · n=0 boundary. **97 asserts / 11 cases** (file-captured); full suite 467/49; debug+asan+shipping+tidy green. ⭐ BUG CAUGHT BY THE GATES + driver instrumentation: the first τ-escalation read `τ>β?2τ:β` — STUCK AT β forever when τ==β (Cholesky failed 60× on a healthy indefinite H); fixed to the literal N&W `τ←max(2τ,β)`. ⚠ LESSONS (in test comments): chained Rosenbrock n≥4 has a LOCAL minimizer (f≈3.9859) a local method honestly lands in ⇒ Newton tests use n=2; grad_tol below √(eps·|f*|) hits the f64 line-search resolution floor ⇒ 1e-8 floor on |f*|-large tests. | ~700 | 97 | — |
| ↳ **v7-h** (DONE 2026-06-10) | **Trust-region.** ✅ `trust_region.hpp` (umbrella): the N&W Alg 4.1 framework (ρ-test + radius mgmt + reject-keeps-H + Δ-collapse stall guard) over a **six-solver subproblem ladder**: Cauchy · **dogleg** (PD, Cauchy fallback) · 2D-subspace (span{g,(H+τI)⁻¹g} orthonormalized → the EXACT 2×2) · **Steihaug-CG** (N&W 7.2: boundary + negative-curvature exits, Eisenstat-Walker forcing — scipy 'trust-ncg') · **trust-Krylov (GLTR** 1999: Lanczos w/ FULL reorthogonalization, the k×k tridiagonal subproblem solved EXACTLY per step, β·\|y_k\| residual — scipy 'trust-krylov') · **exact Moré-Sorensen via the v3 `dense::eig_sym`** (interior test + safeguarded secular Newton on 1/Δ−1/‖p(λ)‖ + the HARD CASE closed-form in the eigenbasis — scipy 'trust-exact'/GALAHAD). ⭐ `solve_trust_region_subproblem_exact` is PUBLIC (the certified TR solve; reused by Subspace2D + GLTR inner steps; v7-k QP + eylem consumers). SHARP gates: the **Moré-Sorensen KKT certificate verified DIRECTLY** ((H+λI)p=−g · λ≥max(0,−λ₁) · complementarity) on interior/boundary/indefinite/**hard-case**/pure-saddle(g=0) instances with known spectra · ALL SIX subproblems solve an SPD quadratic (incl. Cauchy, the theory floor — quadratic recentered to f*=0 per the f64-resolution-floor lesson) · Rosenbrock-2 ×5 · saddle escape (Steihaug/GLTR/exact) · `{1,2,4,8,16}` Steihaug moat · n=0. **109 asserts / 6 cases** (file-captured); full suite **576/55**; debug+asan+shipping+tidy green. HONEST scope notes: GLTR is the faithful algorithm without preconditioning/restart engineering (full-memory Lanczos, k≤64 default); eval-parity vs scipy trust-* lives at v7-z. | ~760 | 109 | — |
| ↳ **v7-i** (DONE 2026-06-10, after the v12-PULL) | **Stochastic / ML — COMPLETE, no moat asterisk.** ⭐ The pinned moat-split was resolved by the plan's own "build a minimal RNG" branch (user-chosen): **v12-PULL = new module `crd-hesap-stats`** with the **Philox4x32-10 counter-based RNG** (`philox.hpp`: constexpr pure block fn + PhiloxRng (64-bit seed × 64-bit stream × 64-bit position, O(1) random access) + 53/24-bit uniform converters + unbiased `next_below` + deterministic Fisher-Yates `shuffle`) — **verified against the three PUBLISHED Random123 known-answer vectors** (zero / all-ones / π-digits: together they probe every constant + the round/bump structure) + purity/stream/moment/shuffle tests. ✅ v7-i proper (`stochastic.hpp`, umbrella): ALL TEN steppers to the reference formulas with PyTorch default semantics — SGD(+momentum/Nesterov/dampening) · Adam · **AdamW (decoupled)** · Nadam (the PyTorch μ-product schedule) · RAdam (ρ>5 rectification gate) · RMSprop(+momentum) · Adagrad · Adadelta · **Lion** · **LAMB** (trust ratio; single param group) + LR schedules (step/exp/**cosine-annealing**/linear-warmup) + grad clipping (norm/value, torch semantics). `minibatch.hpp`: the **Philox-backed MinibatchSampler — epoch-keyed streams ⇒ the (seed, epoch) permutation is reproducible BY CONSTRUCTION, in any visit order** (replay jumps straight to epoch 17). SHARP gates: EXACT closed-form first/second steps of every rule (catches bias-correction/state-order transcription bugs — e.g. RAdam's t=1 un-rectified branch is Δx = −lr·g exactly) · all ten converge on a convex quadratic (LAMB under cosine decay — normalized updates orbit at lr scale, the textbook behavior) · sampler permutation + out-of-order epoch reproduction · **TWO moats**: full-batch Adam over the parallel spmv bit-identical {1,2,4,8,16} + the full minibatch-SGD pipeline bit-identical across runs · n=0. Philox 5 cases; v7-i **271 asserts / 9 cases**; opt suite **915/73**; both targets green on 4 configs + guards. Gold parity vs live PyTorch trajectories = v7-z (gated script; the in-tree closed forms ARE torch's documented formulas). | ~1100 | 271 | — |
| ↳ **v7-j** (DONE 2026-06-10) | **Constrained substrate.** ✅ `constraints.hpp`: `Constraints<T>` interface (c_E=0 / c_I≥0, the PINNED N&W sign conventions: L = f − λᵀc_E − μᵀc_I; dense row-major Jacobians + `add_lagrangian_hessian` curvature hook + capability contract + locked vtable w/ reserved sparse slots). `kkt.hpp`: **4-part KktResidual certificate** (stationarity/primal/dual/complementarity — the IPOPT/OSQP stopping quantity, now in `OptResult::kkt_residual` + `multipliers`) · **`solve_kkt_dense`** = the saddle [W J_Eᵀ; J_E 0] via the v0e dense **Bunch-Kaufman LDLᵀ** with the **INERTIA TEST read off D's blocks** ((n+, m−, 0) required) + the IPOPT-style δ·I/γ·I correction ladders · `estimate_eq_multipliers` (lstsq-backed LS multipliers). `merit.hpp`: ℓ1 exact-penalty value + DIRECTIONAL derivative (one-sided kink rules). `sqp_equality.hpp`: the substrate PROVER — line-search Newton-SQP (ν ≥ ‖λ⁺‖∞+margin per N&W 18.36) **with the SECOND-ORDER CORRECTION** (N&W §15.6 — added after measuring ℓ1 creep). SHARP gates: analytic-KKT-point residual checks (eq + ineq complementarity/dual parts) · KKT-solve certificate incl. **inertia-test-does-NOT-over-regularize** (indefinite W, PD reduced Hessian ⇒ δ=0, Sylvester) AND the δ-ladder firing on an indefinite reduced Hessian · multiplier-LS at an analytic point · merit FD check · ONE-step on an equality QP · circle problem + circle projection (nonlinear curvature through the hook; analytic x*, λ*) · `{1,2,4,8,16}` moat (primal AND dual trajectory) · m=0 (≡Newton) + n=0. **68 asserts / 9 cases**; suite **644/64**; 4 configs + guards green. ⚠ HONEST DEFERRALS: HS6 (λ*=0 ⇒ curvature-free exact-W; plain ℓ1-Armijo creeps ~×0.995/iter — MEASURED) deferred to v7-n as a globalization stress gate (N&W 18.3 itself prescribes damped BFGS); FD constraint Jacobians not built. ⭐ EN ROUTE: the `#deps 0` landmine root-caused DEEPER + fixed durably — the VS-bundled CMake fork (4.2.3-msvc3) breaks deps on non-English-locale cl and re-breaks dirs on any in-build regenerate; policy = explicit standalone-CMake path + purged `*-msvc*` detections (CLAUDE.md Troubleshooting updated; a stale-mixed-`OptResult`-layout SIGSEGV was the symptom). | ~900 | 68 | — |
| ↳ **v7-k** ⭐ (DONE 2026-06-10) | **QP — all THREE solvers on ONE canonical form** (`QpProblem`: min ½xᵀPx+qᵀx s.t. l ≤ Ax ≤ u — the OSQP form; eq = l==u; ±inf one-sided) **with UNIFORM OSQP-sign duals so one KKT certificate checks them all.** ✅ `qp.hpp`: **`solve_qp_admm`** = OSQP-class splitting (quasi-definite [P+σI Aᵀ; A −1/ρ] Bunch-Kaufman factored once per ρ; per-constraint ρ w/ 1e3 eq-boost; α=1.6 relaxation; OSQP residual termination; **certified PRIMAL/DUAL INFEASIBILITY** δy/δx tests; **DETERMINISTIC adaptive-ρ** — residual-ratio on a fixed interval, no wall clock; **active-set POLISH** via the v7-j equality-KKT solve, incl. the empty set = exact unconstrained Newton) + **`solve_qp_mehrotra`** = predictor-corrector IPM (affine→σ=(μ_aff/μ)³ corrector, fraction-to-boundary 0.995, reduced saddle [P+GᵀDG A_Eᵀ; A_E 0] through the inertia-corrected Bunch-Kaufman; divergence guard). ✅ `qp_active_set.hpp`: **`solve_qp_goldfarb_idnani`** (1983 dual active-set: G=LLᵀ, J=L⁻ᵀ, Givens-updated R; finite termination; equalities added first/never dropped). GATES: analytic box-projection (exact x*+dual signs) · equality-QP vs the v7-j saddle · **CROSS-ADJUDICATION: three INDEPENDENT algorithms agree on a Philox family** (n=8, m=12, mixed eq/two-sided/one-sided, feasible by construction) + a **400-tiny-instance GI-vs-IPM property scan** (KEPT permanent — it found the GI J-transpose bug) · certified infeasibility both ways · adaptive-ρ ADMM bit-identical across runs + worker counts · m=0/n=0. **1433 asserts / 7 cases**; suite **2348/80**; 4 configs + guards green. ⭐⭐ TWO REAL BUGS GATE-CAUGHT: (1) **GI's J accessor returned L⁻¹ not L⁻ᵀ** — steps in the (LᵀL)⁻¹ metric: still lands FEASIBLE (t2 normalizes) but non-optimal, and INVISIBLE on diagonal-P tests (L=Lᵀ) — found by shrinking to n=2 instances; (2) the quadprog `u[iq−1]=u[iq]` incoming-dual carry on constraint drops was missing. HONEST scope: DENSE v7-k; the sparse backend (multifrontal LDLᵀ behind the same factor seam) lands with the MPC consumer; OSQP/qpOASES/quadprog wall-clock at v7-z. | ~1500 | 1433 | — |
| ↳ **v7-l** (DONE 2026-06-10) | **LP — both members on the v7-k canonical form** (`LpProblem`: min cᵀx s.t. l ≤ Ax ≤ u + optional variable bounds; OSQP-sign row duals). ✅ `lp.hpp`: **`solve_lp_simplex`** = the **bounded-variable REVISED SIMPLEX** — slack working form [A −I]v = 0 (b folded into slack bounds), **TWO-PHASE** (sign-matched artificial basis; Phase-I optimum > 0 = the infeasibility CERTIFICATE), **Dantzig pricing with the Bland anti-cycling fallback** (after a 50-degenerate-pivot streak), bounded ratio test incl. **BOUND FLIPS** (entering hits its own opposite bound — no basis change), free variables (either-direction entering), equality rows (fixed slack columns never enter), explicit dense B⁻¹ eta-updated per pivot + **Gauss-Jordan refactorization every 64 pivots**; unbounded ray ⇒ DualInfeasible. **`solve_lp_mehrotra`** = the v7-k predictor-corrector at **P = 0** (LP is the QP special case; finite variable bounds folded as identity rows) — the smooth member, zero new algorithm. GATES: analytic vertex LP (both) · **EXACT dual recovery on a rows-only LP** (q + Aᵀy = 0 checked DIRECTLY; y* = (1,0,1,0,0) from both members — pins the y = −c_BᵀB⁻¹ sign map) · equality row + FREE variable · the pure bound-flip path (box optimum, slack row, zero row dual) · **Beale's classic cycling LP** (Dantzig cycles forever naively; terminates at obj = −1/20, x = (1/25,0,1,0)) · certified infeasibility (Phase I) + unboundedness + IPM-must-not-claim-success · **60-instance Philox CROSS-ADJUDICATION** (combinatorial vertex pivoting vs the smooth IPM agree to 1e-5 on boxed-feasible-by-construction instances; per-row feasibility of every vertex) · bit-identical determinism (both members, iterations + x + y). **893 asserts / 8 cases**; 4 configs + guards green. HiGHS/GLPK/scipy.linprog wall-clock = v7-z. | ~490 | 893 | — |
| ↳ **v7-m** (DONE 2026-06-10) | **Conic — SCS-class operator splitting** (`ConicProblem`: min cᵀx s.t. **Ax + s = b, s ∈ K** — the SCS data form; dual y ∈ K*, Aᵀy + c = 0, sᵀy = 0). ✅ `conic.hpp`: cone blocks **Zero / Nonneg / SOC (closed-form) / PSD** (symmetrize + **eigenvalue clamp via the v3 `dense::eig_sym`**; full-matrix vectorization — NAMED divergence from SCS's scaled-lower-tri packing). **`solve_conic_admm`** = the v7-k OSQP iteration with the box projection swapped for **Π_C, C = {b} − K** (Π_C(v) = b − Π_K(b − v)): quasi-definite [σI Aᵀ; A −diag(1/ρ)] factored once per ρ (Zero blocks get the 1e3 equality boost), α = 1.6 over-relaxation, deterministic interval adaptive-ρ, **conic infeasibility certificates** on iterate differences (δy ∈ K* ∧ ‖Aᵀδy‖≈0 ∧ bᵀδy < 0 ⇒ primal; −Aδx ∈ K ∧ cᵀδx < 0 ⇒ dual; cone-distance checked via the projections), termination = primal/dual residuals **+ the DUALITY GAP** (the SCS criterion). The duals coincide with the v7-k/l OSQP sign on shared rows ⇒ one certificate convention across QP/LP/conic. GATES: **LP-as-conic reproduces the v7-l LP including the exact duals** + the conic KKT directly (s,y ≥ 0, sᵀy ≈ 0, residuals + gap) · **analytic SOCP** (linear objective over a norm ball: x* = p − r·c/‖c‖, y* = (‖c‖, c), s* ON the cone boundary) · **analytic 2×2 SDP** (min x s.t. [[x,1],[1,x]] ⪰ 0 ⇒ x* = 1; dual Y* = [[½,−½],[−½,½]], trace-complementarity) · mixed Zero+Nonneg vs the simplex · certified primal/dual infeasibility · **25-instance Philox cross-adjudication vs the v7-l simplex (a THIRD independent algorithm family on the same instances)** · bit-identical determinism (x, y, s, iterations). **111 asserts / 7 cases**; opt suite **3432/103**; 4 configs + guards green. HONEST scope: no polish (SCS has none), no homogeneous self-dual embedding (certificates via differences), DENSE like v7-k/l; SCS/ECOS wall-clock = v7-z. ⭐ EN ROUTE: the `#deps 0` landmine's THIRD head — win-shipping's `CMakeCache CMAKE_COMMAND` had FLIPPED BACK to the VS fork (any regenerate executed by the fork rewrites CMAKE_COMMAND to itself ⇒ English prefix vs Turkish cl ⇒ silent stale objs); wiped + standalone-reconfigured (Turkish prefix + `#deps 72 VALID` verified) and added `scripts/{build-target,configure-preset,check-deps,run-ctest}.bat` so every build/configure/ctest goes through the explicit standalone path (CLAUDE.md updated). | ~520 | 111 | — |
| ↳ **v7-n-1** ⭐ (DONE 2026-06-10) | **NLP part 1: SQP + augmented Lagrangian.** ✅ `nlp_sqp.hpp`: **inequality-capable line-search SQP** (N&W Alg 18.3) — the QP subproblem solved by the **v7-k Goldfarb-Idnani** (the consumer edge: finite, exact duals, B≻0 always); **DAMPED BFGS** (Procedure 18.2 — THE cure for the v7-j-measured ℓ1 creep: HS6 now converges < 60 iterations from the classic start, the deferred stress gate CLOSED); ℓ1 merit + ν-rule + **SOC with violated-inequality restoration**; multipliers from the QP duals; 4-part-KKT stopping. ✅ `nlp_auglag.hpp`: **PHR augmented Lagrangian** (LANCELOT-class) — `AuglagObjective` adapter → **v7-d L-BFGS inner solves** (matrix-free, no factorization), first-order multiplier updates + the classical (η,ω) schedule (N&W Framework 17.4). GATES: HS6 (both methods) · HS14 (eq + ACTIVE nonlinear ineq; the published f* = 9−2.875√7 to 1e-9) · **Rosenbrock-in-the-unit-disk** (the scipy reference instance: x*≈(0.78642, 0.61770), active boundary, μ>0) · circle projection (analytic λ* through TWO more algorithms) · run/worker bit-identity · m=0 (≡BFGS) + n=0. **60 asserts / 7 cases**; suite green on 4 configs + guards. HONEST scope: no elastic mode (infeasible linearization ⇒ LineSearchFailed, named). | ~900 | 60 | — |
| ↳ **v7-n-2** ⭐ (DONE 2026-06-10) | **NLP part 2: the IPOPT-class filter interior point.** ✅ `nlp_interior_point.hpp` (`minimize_interior_point`, Wächter-Biegler 2006): slack form (c_I − s = 0, s > 0, −μΣln s) · the (Δs, z⁺)-eliminated primal-dual Newton reduces to **[W + J_IᵀΣJ_I, J_Eᵀ; J_E, 0] through the v7-j inertia-corrected Bunch-Kaufman** (the (n+, m−, 0) test + δ-ladder IS IPOPT's inertia-correction mechanism — already built, consumed as-is) · fraction-to-boundary (τ = max(0.99, 1−μ)) · **the FILTER line search** (W-B margins γ_θ=γ_φ=1e-5, the s_θ/s_φ/δ switching condition, Armijo-on-φ for f-type steps, θ-type filter augmentation, per-barrier reset) · **monotone Fiacco-McCormick μ** (E_μ ≤ κ_ε·μ ⇒ μ ← max(tol/10, min(κ_μμ, μ^1.5)), the IPOPT defaults) · the κ_Σ z-clip safeguard · exact-Hessian W via the v7-j curvature hook (Hessian-free = SQP/auglag's job). GATES: the full battery through the THIRD method — HS6 (the degenerate-barrier mi=0 path) · HS14 (published f* @1e-7, z ≥ 0) · Rosenbrock-in-disk (scipy reference x*, boundary z > 0) · circle projection (**the analytic λ* = 1−√5 now reproduced by a FIFTH independent algorithm**) · bit-identical run-twice. **80 asserts / 8 cases (the whole v7-n battery)**; suite **2428/88**; 4 configs + guards green. ⭐ **IPOPT INSTALL PROBED** (WSL): python3.12 + pip24 present; `coinor-libipopt-dev` (Ipopt 3.11.9) available in apt but needs sudo (ONE user command) → `scripts/setup-ipopt-ref.sh` written (apt-gate + cyipopt + import check); HONEST note: apt's 3.11 is a correctness/iteration peer — the v7-z WALL-CLOCK rows should coinbrew a modern 3.14+MUMPS (recipe pointer in the script). HONEST scope: no restoration phase (backtracking floor ⇒ LineSearchFailed), no SOC inside the IPM, unscaled E — all named. | ~480 | 20 | — |
| ↳ **v7-o** (DONE 2026-06-10) | **Algebraic modeling layer** (the JuMP/CasADi pattern — the ergonomic façade). ✅ `model.hpp`: **`Model<T>`** — declarative variables (`add_variable(s)` + bounds + starts), `minimize(λ)`, `subject_to_eq/ge/le(λ)` where every function is a **scalar-generic lambda** (the v7-b `DiffFunctor` contract — C++ gives the declarativeness for free, no expression graph needed); functions held type-erased (`IModelFn` vtable: T + Dual<T> instantiations) via `crd::memory::construct/destroy`. **AUTO-DERIVATIVES**: exact forward-mode AD — the objective through `FunctorObjective` over an `ErasedFnView`, constraint Jacobians per-row n-pass `forward_ad_gradient`, bound rows ±e_j analytic. **DETERMINISTIC DISPATCH** (`ModelMethod` overridable): general constraints → damped-BFGS SQP · bounds-only → L-BFGS-B · unconstrained → L-BFGS · Auglag selectable; with general constraints, finite bounds FOLD into c_I rows in a pinned order (the `multipliers` layout). GATES: **WIRING EXACTNESS — the unconstrained and bounds-only dispatches are BIT-IDENTICAL to calling the underlying solver directly on the same functor** (same iterations + bit-equal x; any divergence = a wiring bug) · declarative HS14 at the published f* through SQP AND auglag (active-ineq μ > 0) · le + folded-bound at the analytic vertex (0.5, 1.5) · an equality projection recovering the **ANALYTIC MULTIPLIER λ* = (1−2√2)/2 through the model's AD-built Jacobian** · run-twice bit-identity. **39 asserts / 6 cases**; suite **3467/109**; 4 configs + guards green. ⭐⭐ **GATE-CAUGHT a REAL v7-n-1 SQP BUG** (the vertex-in-one-step path): an iterate landing EXACTLY on the solution in one step left the QP returning p = 0 with the EXACT multipliers — but the code fell into `dphi ≥ 0 ⇒ SmallStep` without adopting them or re-certifying; the v7-n battery never tripped it (multipliers converge gradually there). FIX (in `nlp_sqp.hpp`): adopt the QP duals + re-run the 4-part KKT certificate BEFORE the merit machinery (the textbook SQP stopping point); whole suite re-verified, zero regressions. HONEST scope (named): Hessian-free members only (no synthesized second derivatives — exact-Hessian Newton/TR/IPM are raw-API); forward-AD = n passes per function (dense-small ergonomics; large-scale = the raw API with analytic/sparse derivatives). | ~330 | 39 | — |
| ↳ **v7-p-1** (DONE 2026-06-10) | **Derivative-free part 1: the DIRECT-SEARCH trio** (de-slipped by user choice 2026-06-10 — **the FULL-PORT path was chosen for v7-p**: p-2/3/4 port COBYLA/NEWUOA/BOBYQA faithfully from the NLopt C reference with a differential harness, the L-BFGS-B playbook). ✅ `nelder_mead.hpp` = **Nelder-Mead with scipy's exact semantics** (the nonzdelt/zdelt initial simplex, scipy's accept conditions, optional **Gao-Han adaptive parameters**, both-spreads termination; index-tie-broken insertion sort — deterministic, no std::sort). ✅ `powell.hpp` = **Powell's conjugate-direction method** over a **faithful Brent 1-D minimizer** (NR `mnbrak` golden-ratio bracket + Brent 1973 golden/parabolic — the pair scipy's 'Powell' drives) with the f_E extrapolation direction-replacement test. ✅ `pattern_search.hpp` = **GPS + OrthoMADS-style poll** (mesh-size certificate termination; OrthoMads = per-iteration orthonormal Householder basis from a **(seed, iteration)-keyed Philox unit vector** — MADS's fresh directions WITH bit-reproducibility by construction). All value-only (no gradients anywhere). GATES: all four variants at the analytic quadratic minimum · NM + Powell on Rosenbrock-2 (pattern search honestly scoped OUT of curved smooth valleys) · **the NONSMOOTH ℓ1 gate** (gradient methods inapplicable; NM + both polls converge) · **Powell conjugacy** on a cross-coupled quadratic (≤ 12 sweeps, no zigzag crawl) · bit-identical determinism incl. the OrthoMADS Philox stream + different-seed-still-converges · n = 1 / n = 0. **62 asserts / 6 cases**; suite **3529/115**; 4 configs + guards green. ⭐ **p-2..4 ORACLE PROBED**: NLopt cloned (WSL /tmp/nlopt); `cobyla.c` 1872 · `newuoa.c` 2583 · `bobyqa.c` 3278 lines (~7.7K of f2c-style C — confirms the multi-session estimate); sub-function granularity for the differential harness identified (`trstlp` / `trsapp`+`biglag`+`bigden` / `trsbox`+`altmov`+`update`+`prelim`+`rescue`); oracle build = expose statics per-algorithm at p-2 start. | ~640 | 62 | — |
| ↳ **v7-p-2** (DONE 2026-06-10 — **DIFFERENTIALLY VERIFIED: 2050 checks, 0 fail**) | **COBYLA** (Powell 1992; the only DFO member with general constraints). ✅ `cobyla.hpp` = a **FAITHFUL LINE-FOR-LINE PORT of the NLopt C reference** (MIT; Roy's C translation of Powell's COBYLA2 + SGJ's documented mods: ENFORCE_BOUNDS, the deterministic-LCG simplex perturbation seeded (n+m), the SAS ρ-increase rule, nlopt stop semantics) in the f2c idiom — 1-based pointer adjustments, the ORIGINAL goto control flow (restructuring IS the bug farm), the **exact float-literal artifacts** (`.1f`/`.2f`/`1e-6f` keep float-then-promote values for bit-exactness vs the oracle), `relstop` verbatim incl. the inf guard. NAMED deltas: no force-stop/wall-clock stops; iprint stripped. `detail::cobyla_impl::trstlp` keeps the reference signature (the per-routine diff target); public `minimize_cobyla(obj, cons*, x0, lower, upper, alloc, CobylaOptions{rhobeg, rhoend, ftol, max_evals})` — c_I ≥ 0 (COBYLA's own = the pinned v7-j convention), num_eq must be 0 (±pair equalities named), bounds optional. ✅ FUNCTIONAL GATES (23 asserts / 6 cases; suite **3552/121**; 4 configs + guards green): **Powell's own unit-disk product problem** (f* = −½ on the diagonal) · **Rosenbrock-in-the-unit-disk at the scipy reference x* — a CROSS-FAMILY adjudication** (a derivative-free linear-model method agreeing with SQP/auglag/IPM on the same instance to 1e-5, disk active) · active variable bounds (never evaluated outside the box) · bounded Rosenbrock m=0 (slow-crawl on the curved valley at rhoend 1e-10 — COBYLA's documented character; eval-count vs the oracle goes on the harness checklist) · **bit-identical run-twice incl. the LCG** · n = 0. ⭐⭐ **THE DIFFERENTIAL HARNESS ADJUDICATED THE PORT — 2050 checks, 0 failures** (`runtime/examples/cobyla_difftest.cpp`, WSL g++ vs the oracle built by `scripts/setup-nlopt-ref.sh`: stock libnlopt.a + an EXPOSED-STATICS TU [`#define static` + include, no source patch] + an `ar d`-deduped lib + the `crd_cobyla_e2e` shim for the rescaling-free cobyla() layer): per-routine **`trstlp` BIT-EXACT** (dx + vmultc reals, iact ints, ifull, rc) on 5 targeted regimes (feasible-at-zero/violated/DUPLICATE-gradients-L130/m=0/n=1) + 400 randomized instances w/ injected parallel rows ·  **end-to-end bit-exact x and minf with IDENTICAL EVAL COUNTS on all 5 shared problems** (disk-product, rosen-in-disk, unconstrained + boxed rosen, bounds-active sum) — the boxed-Rosenbrock slow-crawl confirmed as the REFERENCE'S OWN behavior (counts match exactly). Reference cached: WSL `~/cerid-deps/nlopt` + repo `external/nlopt-ref/cobyla.c` (gitignored). | ~1100 | 23+2050 | — |
| ↳ **v7-p-3** (DONE 2026-06-10 — **DIFFERENTIALLY VERIFIED: 3773 checks, 0 fail**) | **NEWUOA** (Powell 2004 — quadratic interpolation models over NPT points, the model-based DFO workhorse). ✅ `newuoa.hpp` = the faithful NLopt-reference port (f2c idiom, original gotos, exact literals; Stop/relstop plumbing SHARED with the COBYLA port via `detail::cobyla_impl`). **Scope PINNED: Powell's CLASSIC UNCONSTRAINED algorithm** — the reference's NEWUOA_BOUND variant (`if (lb && ub)` blocks nesting an MMA optimizer in trsapp_/biglag_ + truncation hacks) NOT ported (no nested-solver dependency; bounds are BOBYQA's job — Powell's own position; the e2e diff passes NULL bounds, apples-to-apples). Routines: `trsapp` (truncated-CG TR subproblem + 2-D angle refinement, the ITERC-dispatched inline HD=H·D) · `update` (the BMAT/ZMAT(+IDZ) rank-2 update — ⚠ the reference's `temp < zero` artifact kept VERBATIM + commented, the oracle is the contract) · `biglag` (|Λ_knew| maximizer) · `bigden` (the 5-harmonic denominator maximizer) · `newuob` (the driver: init set, XBASE shift, ρ schedule, the least-Frobenius-norm ITEST swap; rhoend computed as xtol_rel·rhobeg EXACTLY like the reference so e2e bit-matches) · the workspace partition. Public `minimize_newuoa(obj, x0, alloc, NewuoaOptions{rhobeg, rhoend, ftol, npt (0 ⇒ 2n+1), max_evals})`, n ≥ 2 asserted. FUNCTIONAL GATES (19 asserts / 5 cases; suite **3571/126**; 4 configs + guards green): **the QUADRATIC-EXACTNESS property** (npt = (n+1)(n+2)/2 ⇒ the model interpolates a quadratic exactly; f < 1e-13) · Rosenbrock-2 (the curved valley where quadratic models shine vs COBYLA's linear ones) · 4-D sphere at default npt · bit-identical run-twice · n = 0. ⭐⭐ **THE DIFFERENTIAL HARNESS: 3773 checks, 0 failures** (`runtime/examples/newuoa_difftest.cpp` vs `newuoa_exposed.c` — the p-2 oracle recipe extended): per-routine **trsapp/update/biglag/bigden BIT-EXACT** on 250 randomized shared model states (idz/kopt/knew randomized; jl==1/jl>1 + sign-partition branches exercised) + **end-to-end bit-exact x/minf with IDENTICAL EVAL COUNTS on 4 problems** incl. full-npt and default-npt regimes. ⭐ Port bug caught by the COMPILER+harness loop: a dropped 5th workspace arg in the newuob→biglag call (C2672 at build; the harness would have caught any silent variant). | ~1500 | 19+3773 | — |
| ↳ **v7-p-4** (DONE 2026-06-10 — **DIFFERENTIALLY VERIFIED: 3045 checks, 0 fail. v7-p COMPLETE**) | **BOBYQA** (Powell 2009 — bounds-native quadratic models; the reason the NEWUOA port could pin the classic unconstrained scope). ✅ `bobyqa.hpp` = the faithful NLopt-reference port (f2c idiom, original gotos; Stop/relstop shared via `detail::cobyla_impl`; the NLopt RESCALING layer NOT ported — the e2e shim passes EQUAL dx ⇒ identity scaling, apples-to-apples at the `bobyqa()` layer; Powell's own bound-preprocessing/SL-SU block IS ported). All SIX routines: `update` (rank-2, ZTEST small-entry threshold, no IDZ — DENOM stays positive) · `prelim` (bound-aware init set: step flips/shrinks at active bounds, the stepa·stepb<0 point switch, EXACT bound landing) · `altmov` (denominator line search + the constrained-Cauchy XALT tried with both gradient signs) · `trsbox` (the BOUNDED truncated-CG TR subproblem: XBDI active-set fixing + restarts + boundary angle iterations) · `rescue` (the provisional-point re-initialization on denominator degeneracy) · `bobyqb` (the driver: XBASE shift, the RESCUE safeguard, the ITEST least-Frobenius swap with bound-aware projected gradients, Powell's ρ schedule). Public `minimize_bobyqa(obj, x0, lower, upper, alloc, BobyqaOptions)` — bounds REQUIRED (gap ≥ 2·rhobeg asserted, Powell's rule). FUNCTIONAL GATES (17 asserts / 5 cases; suite **3588/131**; 4 configs + guards green): interior quadratic at full npt (f < 1e-12) · **an active-bound optimum pinned EXACTLY (x[0] == 1.0 bit-equal — the SL/SU exact-landing machinery)** · Rosenbrock-in-box · bit-identical run-twice · n = 0. ⭐⭐ **THE DIFFERENTIAL HARNESS: 3045 checks, 0 failures** (`runtime/examples/bobyqa_difftest.cpp` vs `bobyqa_exposed.c` + the `crd_prelim_shim`/`crd_bobyqa_e2e` shims): per-routine **update/altmov/trsbox BIT-EXACT** on 250 randomized model states (HALF with TIGHT bounds firing the active-set paths) + **prelim bit-exact** on interior/near-bound/at-bound starts + **e2e bit-exact x/minf with IDENTICAL EVAL COUNTS on 4 problems** incl. the bound-pinned one. HONEST gap (named in the harness): `rescue` has no targeted per-routine diff (needs a coherent degenerate model state); its coverage is opportunistic via e2e. **v7-p COMPLETE: the direct-search trio + THREE differentially-verified Powell-code ports (COBYLA 2050/0 · NEWUOA 3773/0 · BOBYQA 3045/0 = 8868 oracle checks, 0 failures).** | ~2300 | 17+3045 | — |
| ↳ **v7-q** (DONE 2026-06-10) | **Global / metaheuristic over the Philox stream** — every member bit-identical run-to-run BY CONSTRUCTION (same (seed, stream) ⇒ same trajectory; the reproducibility pycma/scipy can't promise across platforms). Prerequisite shipped first: **`crd-hesap-stats` grew `normal.hpp`** (`NormalSampler` — Box-Muller over PhiloxRng with the pair cache as sampler state; four-moment gate + bit-identity + stream separation; stats suite 230660/7). ✅ `cmaes.hpp` = **CMA-ES faithful to Hansen's tutorial** (arXiv:1604.00772 — the pseudocode pycma implements): the (μ/μ_w, λ) scheme with the standard defaults (λ = 4+⌊3 ln n⌋, ln(μ+½)−ln i weights, the c_σ/d_σ/c_c/c_1/c_μ formulas), CSA with the hσ stall guard, rank-one + rank-μ covariance updates, C eigendecomposed via the v3 `eig_sym` EVERY generation (Hansen's lazy update is a CPU trick, not correctness — named), index-tie-broken insertion-sort selection. ✅ `global_search.hpp`: **differential evolution** (scipy best/1/bin: dithered F ~ U(0.5,1), binomial CR with the guaranteed dimension, clip, greedy, scipy's f-std convergence test) · **PSO** (Clerc constriction defaults, velocity-capped) · **simulated annealing** (classical geometric cooling + T-scaled Gaussian neighborhood + Metropolis; scipy's dual_annealing Tsallis machinery NOT shipped — named) · **basin-hopping** (scipy semantics: NM local minimization + uniform perturbation + Metropolis over LOCAL minima + **scipy's AdaptiveStepsize** — added when the fixed-step default honestly failed to hop a 2.5-wide barrier in the gate) · **multi-start** (the baseline every metaheuristic must beat). GATES (31 asserts / 8 cases; opt suite **3619/139**; 4 configs + guards green): CMA-ES sphere-8 collapse (f < 1e-10) + **Rosenbrock-5 from a bad start to 1e-4** · **DE finds the Rastrigin-4 GLOBAL minimum through ~10⁴ local minima** · PSO sphere · SA + basin-hopping **escape the wrong well of a tilted double-well** · multi-start Rastrigin-2 · **bit-identical run-twice for CMA-ES (through the Philox normal stream + eig_sym) and DE** · n = 0 edges. pycma/scipy eval-parity = v7-z. | ~1100 | 31 | — |
| ↳ **v7-r** (DONE 2026-06-10) | **MIP — BRANCH AND BOUND over the v7-l bounded simplex** (`mip.hpp`: min cᵀx s.t. l ≤ Ax ≤ u, box bounds, x_j ∈ ℤ for the masked set — bound tightening IS branching in the bounded-variable form, the natural pairing). ✅ `solve_mip_branch_and_bound`: LP relaxations via `solve_lp_simplex` (exact vertices + certified per-node infeasibility) · **BEST-BOUND node selection** (lowest-index ties — deterministic tree order) · **MOST-FRACTIONAL branching** (deterministic ties), floor/ceil children · pruning by bound/infeasibility/integrality · exhausted tree = the **PROVEN optimum** (Solved), node cap = best incumbent (MaxIterations), no incumbent + clean tree = PrimalInfeasible (e.g. 2x = 1 over ℤ). GATES (92 asserts / 6 cases; opt suite **3711/145**; 4 configs + guards green): the classic fractional-relaxation integer LP (x+y ≤ const family; relaxation (15/4, 3/2) → integral optimum 5) · 0/1 knapsack vs brute force · **a 25-instance Philox scan of random binary problems vs EXHAUSTIVE 2⁸ enumeration (the absolute oracle) + a permanent simplex-vs-IPM root-relaxation cross-check** · mixed integer/continuous · certified integral infeasibility · bit-identical determinism. ⭐⭐ **THE ORACLE SCAN CAUGHT A REAL BUG**: the branching code read the parent's bounds through pointers INTO the node arrays AFTER `push_node` reallocated them — a dangling read gave the second child a garbage bound and "pruned" the true optimum (B&B returned a worse incumbent with a clean Solved proof). The root-relaxation cross-check first EXONERATED the simplex (bit-matching the IPM), pinning the bug to the B&B; fixed by snapshotting before any push (comment at the site). HONEST scope (named): pure B&B — Gomory cuts need simplex-tableau access (not exposed) and presolve/heuristics are accelerators; both future levers, stated plainly on the v7-z HiGHS/GLPK scoreboard. DENSE small-instance scope like v7-l. | ~230 | 92 | — |
| ↳ **v7-z** (DONE 2026-06-10 — local parts complete; CI sweep + the IPOPT row post-commit) | **CLOSE.** ✅ **CLI `hesap.opt.{qp,lp,mip,conic}.f64`** (`src/cli_register_opt.cpp` + `cli_anchor.hpp`, the v6 eigen pattern) — the DATA-DEFINED families on the command layer (nonlinear members need callables, API-level by design); registration + analytic-answer invocation + error-path tests (63 asserts / 6 cases; suite **3774/151**; 4 configs + guards green). ✅ `docs/systems/hesap-opt.md` rewritten to the full shipped method set. ✅ ADR-0090 stands Accepted. ⭐⭐ **THE GOLD-STANDARD SCOREBOARD** (`scripts/opt_scoreboard.py` + `runtime/examples/opt_scoreboard.cpp` — formula-pinned identical problems, same WSL machine; peers pip-installed: scipy 1.17.1 / osqp / quadprog / scs / highspy / pycma): **objective agreement EVERYWHERE** (QP −0.786540016 across OSQP+quadprog+all-3-Cerid; LP −34.455913325 = HiGHS; MIP −80 = highspy, both proven; SOCP analytic to 1e-9 vs scs) · ⭐ **EXACT TRAJECTORY MATCHES where we implement the same algorithm: Nelder-Mead nfev 219 = scipy's 219 at the same f; Steihaug 31/28 = scipy trust-ncg 31/28 at the same f; exact-TR 27/24 = scipy trust-exact 27/24** (the scipy analog of the NLopt bit-exactness) · **GLTR 39 evals where scipy trust-krylov takes 401** (its wrapper warns + struggles — a genuine win row) · **our DE finds the Rastrigin-4 global (f = 0) where scipy's defaults stall at f = 0.995** · CMA-ES eval-comparable to pycma (1690 vs 1391 sphere-8; 2688 vs 2254 rosen-5 ≈ 1.2×) · GI 0.01 ms vs quadprog 0.03 ms; simplex 0.27 ms vs HiGHS 1.19 ms; B&B 0.02 ms vs highspy 2.23 ms (HONEST caveat: tiny instances are overhead-sensitive, and Python-side per-eval callbacks inflate the interpreted rows — EVAL COUNTS are the honest metric there). Previously-recorded rows incorporated by reference: liblbfgs/scipy-L-BFGS-B eval-parity 0.93–1.02× (v7-d) · CHOLMOD structure-dependent (v7-e-2) · **NLopt COBYLA/NEWUOA/BOBYQA BIT-EXACT same-source (8868 oracle checks)** · L-BFGS-B per-routine bit-identical to the code scipy wraps · PyTorch = the v7-i closed-form steppers ARE torch's documented formulas (the live-trajectory run deferred with the torch wheel — named). **PENDING USER-SIDE (named): the IPOPT row (ONE sudo: `sudo apt-get install -y coinor-libipopt-dev pkg-config` → `bash scripts/setup-ipopt-ref.sh`) + the 18-config sweep (CI owns it post-commit per the standing directive) + the v7 COMMIT.** | ~700 | 63 | — |
| ↳ **v7-crush** ⭐⭐ (DONE 2026-06-11) | **THE FULL-CRUSH PASS — every scoreboard gap closed honestly** (session `2026-06-11-v7-full-crush.md`). **(A) Modified-RUIZ EQUILIBRATION + cost normalization in `solve_qp_admm`** (OSQP §5; default-on, fixed deterministic sweeps, clamps 1e-4..1e4; the old body = `solve_qp_admm_unscaled`): scoreboard QP **54 → 31 iters** (OSQP 25, obj exact). The scaling EXPOSED two latent polish bugs, both ROOT-FIXED (no debt): `qp_finalize`'s NaN-blind max-fold let a singular active-set KKT solve report residual **0** ⇒ NaN accepted (fix: finiteness-tracked folds ⇒ +∞); and polish acceptance never checked **dual SIGNS** — a wrong active set solved exactly beats the certificate on stationarity+feasibility but flips a forced row's multiplier sign (fix: `side[]` tracking + sign-consistency + explicit finiteness; KKT = stationarity + feasibility + signs). **(B) ACTIVE CMA-ES** (negative recombination weights — formulas verified against the INSTALLED pycma source: ln((λ+1)/2)−ln i raw weights over all λ, negatives scaled to −min(1+c1/cμ, 1+2μeff⁻/(μeff+2), (1−c1−cμ)/(n·cμ)), per-vector n/‖z‖² Mahalanobis rescale, un-rescaled-sum covariance decay, the 0.25 rankmu-offset in cμ; default-on like pycma): **ros5 1816 vs pycma 2254 WIN**, sph8 1590 vs 1391 (1.14×). **(C) Powell scipy-exact inner coupling** (the inner Brent at 100·xtol — scipy `_linesearch_powell`; the scoreboard pins scipy's xtol=ftol=1e-4): **494 vs scipy 792 evals WIN** (f 2.4e-30). **(D) BH `LbfgsFd` local mode** (scipy basinhopping's DEFAULT local method: L-BFGS m=10 over 2-point FD via a `CountingObjective` so nfev counts every probe, pgtol 1e-5, **+ the factr flat-f exit (2.22e-9) — gate-caught: without it FD-noise kept ‖∇f‖ above grad_tol and the run burned 2.9M evals**): **9624 vs scipy 8881 parity** (was 15153); NM mode byte-unchanged. **(E) the LIVE TORCH ROW** (CPU wheel installed): Adam AND AdamW **12-digit-identical 200-step trajectories** on rosen2 (f 2.587601739546 / 2.345669750078, both x components identical; ours ~0.00 ms vs torch 21.95/781.88 ms). ⚠ EN ROUTE: **win-shipping found RE-POISONED** by the deps landmine (fork `CMAKE_COMMAND` + English prefix + `#deps 0` — the first shipping "green" was a stale-object lie); wiped + standalone reconfigure ⇒ `#deps 77 VALID`, honest green — the CLAUDE.md audit rule earned its keep. Suite **3782/152** (+1 BH-mode test); win-debug/asan/shipping/tidy + guards green. Remaining non-opt frontier (named, separate): the hesap-direct 3D-lattice panel-TRSM kernel. | ~250 | +8 | — |
| ↳ **lattice-kernel dig** ⭐⭐ (DONE 2026-06-11) | **The hesap-direct lattice frontier moved** (user: "I want the lattice crush too"; session `2026-06-11-lattice-kernel-crush.md`). The v7-e-2 STEP-7 "serial dense-kernel wall" (lat32 W=1 0.73×) fell to a profile for the second time — ⅓ wall, ⅔ fixable: **(1) the `syrk_lower_minus` ELEMENTWISE-pack pathology** (the v0e syrk packed its full m×k operand through `MatrixView::at()` — one TLB miss per element on big-ld panels) → REBUILT as a triangular-output mirror of the Goto gemm driver (same `pack_a`/`pack_b`, same Mc/Kc/Nc blocking, same microkernel; upper tiles skipped, diagonal masked) with the EXACT gemm Kc-grouping ⇒ lower-triangle bits now **identical to gemm-then-subtract**, healing a latent serial-syrk-vs-parallel-gemm value divergence at knc>256 (worker count picked the path); lat32 serial **4283 → 3705 ms from this alone**; **(2) the ColMajor merge stride fix** in `gemm_packed_inner` (j-inner strided every ColMajor C write by ld; i-inner now — bit-identical, lifts every ColMajor gemm engine-wide); **(3) the below-outer TRSM in bit-identical IN-PLACE wide-N RowMajor form** (transpose-VIEWS of the panel; B1 = one beta=1 in-place gemm — no Tᵀ-scratch + subtract passes; whole-obw fused-inverse variant built, measured, REVERTED — the build couldn't amortize on tail blocks). **RESULT: lat32 serial 0.73× → 0.85-class · lat24 8T = 0.99× PARITY · FEA improved (hood 1.57×, bcsstk25 1.85× WIN) · residuals unchanged 9e-15.** Verified: dense **359,508/349** + direct **598,861/190** green on WSL gcc + win-debug + win-shipping (+ASan, tidy). ⭐ THE ONE NAMED REMAINING LEVER for lat28/32 (~0.85): a **dedicated packed-TRSM driver** (B1's M=64-skinny shape re-streams packed B per 6-row a-panel — bandwidth-bound ~50 GF/s in any orientation; OpenBLAS-style fused solve kernel, C-level, NOT asm). ⚠ Probe lesson (sanity): ad-hoc bench TUs must carry `-DCRD_SIMD_TARGET=2` or header templates instantiate the SCALAR microkernel (a 20× phantom); rebuild the .a before probing headers (COMDAT picks the stale lib copy). Also: **the IPOPT row landed** (3.11.9 + cyipopt; disk-NLP: x agreement to 7 decimals, Cerid IPM 22 vs IPOPT 16 iters — parity-class; the last pending v7-z reference row closed). | ~300 | — | — |
| ↳ **solve-crush + full-scoreboard correction** ⭐⭐ (DONE 2026-06-11) | **The SOLVE side of the CHOLMOD board** (session `2026-06-11-solve-crush-full-scoreboard.md`) — driven by the user catching FACTOR-only victory reporting while SOLVE lost 2–3× (standing rule: `feedback_full_scoreboard_no_partial_victory` — ALL metrics in every verdict). THREE measured root fixes: **(1)** the single-RHS level-parallel solve was NET-NEGATIVE and worsened with workers (lat32 69→98 ms at 16T; the old kSolveParallelMinLnz gate sent every big factor there) → single-RHS is ALWAYS serial; **(2)** the backward pass was a scalar FP-add dependency chain (unvectorizable under strict FP; measured fwd 30.5/back 39.6 ms) → shared SIMD solve kernels: `solve_axpy_minus`/`solve_acc_plus` (element-independent maps — BIT-IDENTICAL to the scalar loops) + `solve_dot_conj` → the canonical `simd_dot` (FMA, fixed 4-acc tree, deterministic) in BOTH serial and level-parallel paths (worker-count bit-identity preserved; the parallel backward's per-column re-gather also hoisted) ⇒ **lat32 serial solve 70 → 51 ms**; **(3)** the multi-RHS parallel solve COLLAPSED at high worker counts (lat12 x16 @16T = 0.08×) → measured work gate (lnz·nrhs ≥ 160M) + 8-lane cap (16 lanes strictly worse than 8 at every size). Also en route: the packed-TRSM driver (resident-panel walk, lda-parametrized microkernel, bit-identical) landed but the "B1=1060 ms" target was a MISLABELED TIMER (`cdiv_outertrail` wraps the (C) TRAILING — B was ~165–400 ms all along); (C)'s serial diagonal now merges IN-PLACE via the syrk `col_indexed_out` mode (no ub T-staging); the **lat32@16T ubuf OOM root-fixed** (a single sw·ustride block exceeded TLSF's ~4 GB structural chunk cap at 16 workers → per-worker Array slices); the streaming ceiling MEASURED (26 GB/s on BOTH our and CHOLMOD's factor buffers; our solve at 80% of it, prefetch moved nothing). **THE FULL BOARD (FACTOR · SOLVE · x16 at 1/8/16T, all in the session log): 16T factor lat24 1.02 WIN / lat28 1.00 / lat32 0.96 parity · solve WINS small lattices (→2.67×) + bcsstk25 (→3.82×), hood parity (0.93–0.99) + hood x16 WINS 1.66–1.82× · big-lattice solve 0.69–0.83 · residuals 8.9e-15 (improved).** NAMED remaining gaps (measured): big-lattice solve rate 80%-of-ceiling (perf-counter dig; CHOLMOD ~100% on fewer effective bytes) · within-front parallel gemv solve (CHOLMOD's single-RHS gains 15% at 8T from threaded gemv) · mid-size x16 (lat20 0.52) · tiny-matrix 16T factor fork overhead (lat12 0.37). Verified: dense 359,508 + direct 598,861 on WSL gcc + win-debug + win-shipping + win-asan; win-tidy caught 3 local-constant naming violations (fixed). | ~250 | — | — |
| ↳ **multi-stream dig — THE SOLVE FLIPS TO A WIN** ⭐⭐⭐ (DONE 2026-06-11) | **The deep-research round** ("empirically and 100% confidently determine and fix"; session `2026-06-11-multistream-solve-win.md`) — the causal chain closed with zero unexplained residue: **(1)** read the gold standard's source (SuiteSparse cloned; `t_cholmod_super_solve_worker.c` = dtrsv + dgemv per supernode — structurally identical traffic); **(2)** pinned the BYTES — the true solve trapezoid computed from BOTH data structures is **IDENTICAL (67.87M doubles = 543 MB/pass each)**; the long-standing "Cerid has 22% less fill" claim compared our trapezoid to CHOLMOD's `lnz`(=`xsize`, its RECTANGLE storage) — corrected, true fill equal (same elimination order); the bench now permanently prints both trapezoids + CHOLMOD's L/Lt per-pass split; **(3)** pinned the RATE — CHOLMOD per pass 29.8/28.8 GB/s, ABOVE the 26 GB/s single-stream probe "ceiling" on the same buffers; **(4)** found the MECHANISM — measured **1 stream = 22.7, 2 = 29.7, 4 = 36.9 GB/s** (DRAM bank/page-level parallelism; OpenBLAS dgemv reads 4 columns concurrently = 4 streams; THP ruled out — madvise-only; perf unavailable on WSL, replaced by targeted probes); **(5)** built **4-COLUMN-FUSED solve phase kernels** (`solve_fwd_diag` / `solve_fwd_below_acc` / `solve_fwd_apply_minus` / `solve_back_below` / `solve_back_diag` / `solve_dot4_f64`) — forward fusion is **BIT-IDENTICAL** (ascending-k per element, mul-then-add/sub per term preserved; the diagonal runs in 4-col blocks with the exact sequential recurrence inside), the backward is a new fixed deterministic reduction; the serial and level-parallel paths now SHARE these helpers (the duplicated loop bodies deleted) ⇒ worker-count bit-identity by construction. **RESULT: single-RHS SOLVE WINS ON EVERY MATRIX AT 1T (1.06–1.47×) and on most at 8/16T (to 3.74×); lat32 solve 0.50× (start of day) → 1.14×; hood wins EVERY metric at EVERY thread count (factor 1.91×, solve 1.14×, x16 1.48× at 16T); residuals 8.9e-15.** NAMED remaining (measured): x16 mid-lattices 8/16T 0.51–0.80 (their threaded multi-RHS dgemm — lever: fused multi-RHS kernels) · lat28/32 1-RHS @8T 0.90–1.03 (their threaded gemv on big fronts — lever: within-supernode parallel solve) · FACTOR serial 0.77–0.94 (our gemm ≈90% of OpenBLAS asm; lever candidate: multi-stream PACKING — the same DRAM mechanism) · tiny-matrix 16T factor forks. ⚠ Sanity addition: single-stream load probes UNDERESTIMATE achievable DRAM bandwidth — probe multi-stream too. Verified: dense 359,508 + direct 598,861 on WSL gcc + win-debug + win-shipping + win-asan + win-tidy. | ~350 | — | — |
| ↳ **x16 + factor crush** ⭐⭐ (DONE 2026-06-11) | **The final round** (session `2026-06-11-x16-factor-crush.md`): **(1) 4-way-interleaved `pack_a`/`pack_b`** — the proven multi-stream DRAM mechanism applied to the gemm framework itself (every cold operand pack was a single stream; pure copy reordering ⇒ bit-identical packed bytes) ⇒ lat32 serial factor 3770→3516 ms, factor 8/16T parity-to-WIN on lat20/24/28 (1.02–1.04×); **(2) the x16 split MEASURED** (temp timers: 42% in the DIAGONAL solves — the batched path re-streamed the dscr scratch once per column = quadratic L2 traffic; the below blocks paid per-supernode `dense::gemm` call overhead + single-stream packs) ⇒ **fused mRHS kernels** (`solve_mrhs_fwd_below`/`back_below`/`fwd_diag`/`back_diag`): allocation/pack-free, 4–8-fused column streams, r-BLOCKED (the panel streams once; the wt/acc scratch stays cache-resident — the first version re-streamed it nc× and measurably regressed before the fix), fma-latency unrolls (one chain per accumulator = a measured ~22 GF/s wall → 2-wide r-unroll fwd + 8-column fusion back), per-element k-ascending fma chains exactly matching the gemm microkernel ⇒ serial≡parallel mRHS bit-equality preserved BY SHARED HELPERS (the worker-equality tests are the contract; no test pins mRHS values). **x16 RESULTS: lat24 1.02/1.28/1.21× WIN · lat32 0.97/0.94/1.29× (16T WIN) · hood 0.77/1.67/1.52× WIN · serial lat28 123→87 ms.** THE FULL BOARD lives in the session log. Remaining named (measured causes): lat12/20 x16 0.60–0.77 (sub-25 ms problems; CHOLMOD's smaller call overhead + our 1-chain remainder paths on nc<8 supers) · lat28/32 1-RHS @8T 0.83–0.90 (their threaded gemv; the within-supernode-parallel lever) · lat28/32 factor serial 0.81–0.84 (the last ~10% gemm-rate to OpenBLAS asm) · lat12 factor @16T 0.34 (16 workers on a 9 ms problem — caller guidance). Verified: dense 359,508 + direct 598,861 on WSL gcc + win-debug/shipping/asan/tidy. | ~400 | — | — |
| **v9** | **ODE / DAE — new module `crd-hesap-ode` (ADR-0091 at v9-a).** PLANNED 2026-06-11 (subdivision below). Edges (all acyclic): hesap (LinearOp, cli) + hesap-dense (dense/complex LU for Newton/Radau) + hesap-sparse/-direct (sparse-Jacobian Newton — the v7-e-2 `reuse_symbolic`/`refactorize` gate IS the BDF Jacobian-reuse pattern) + hesap-iterative (matrix-free Krylov Newton = CVODE SPGMR) + jobs. NO edge to hesap-opt (opt consumes ode later for shooting, not reverse). **GOLD STANDARDS (probed/installed 2026-06-11):** SUNDIALS 6.4.1 CVODE/CVODES/IDA/IDAS/ARKODE (apt ✓ — THE stiff/DAE/sensitivity standard, work-precision oracle) · scipy `solve_ivp` (✓ — RK23/RK45/DOP853/Radau/BDF are Python-readable ⇒ **trajectory-EXACT gates**, the v7 NM/trust-ncg playbook) · Hairer Fortran DOPRI5/DOP853/RADAU5/RODAS (canonical refs; external/ gitignored oracle, gfortran probe at v9-b) · Boost.odeint 1.83 (✓ — same-language wall-clock peer) · Bari IVP Testset (published 10+-digit reference solutions = absolute accuracy anchors, fetch at v9-d) · Julia SciMLBenchmarks = published-numbers reference ONLY (no local Julia; Tsit5 from the paper). **THE SCOREBOARD FORMAT = work-precision diagrams** (error-vs-nfev + error-vs-wall at sweeping rtol — the domain's native honest idiom) reported FULL-BOARD (accuracy+work+wall+moat together, per `feedback_full_scoreboard_no_partial_victory`). **MOAT:** controllers = pure deterministic FP functions; bit-identical run-twice everywhere; {1..16} worker bit-identity for parallel RHS/Jacobian + inherited sparse-factor moat; fixed-step symplectic = the eylem replay contract (ADR-0063). **PINNED LANDMINES (day-1 contracts):** dense-output interface designed at v9-a (events/sensitivities/CLI all consume it — retrofit = refactor-everything) · ONE `OdeLinearSolver` seam from v9-d (dense → sparse → Krylov slot in; CVODE's SUNLinSol lesson) · explicit f(t,y) + mass-matrix form through v9-h, full-residual IDA form decided AT v9-h with measurements · sensitivity/checkpoint hooks reserved in the step loop at v9-d (vtable append-at-end discipline) · f64 spine, f32 only where honest (explicit RK; f32 stiff-Newton tolerances near eps(f32) = dishonest, named OUT) · LSODA auto-switching SKIPPED (convenience heuristic; named, revisit on consumer pull). | ~3800 | ~130 | multi-session |
| ↳ **v9-a** ✅ (DONE 2026-06-11) | **Substrate + fixed-step explicit — SHIPPED with the two API layers** (session `2026-06-11-v9a-ode-substrate.md`; ADR-0091 Accepted): (a) **`steppers.hpp` kernels** — `step_euler/midpoint/rk4`, raw-span, allocation-free (caller `*_scratch(n)`), inlined RHS, IN-PLACE SAFE, fixed per-element FP order (the eylem/animation/DAW hot-loop layer; no virtual RHS per body — `project_ode_in_games_layering`); (b) the **driver substrate**: `OdeFunction<T>` (the v7 Objective capability contract; **vtable LOCKED** 0-dtor·1-rhs·2-dim·3-jacobian·4-jacvec, mass appends v9-h, sparse-jac v9-j, events = options not virtuals) + `FunctorOdeFunction` · `OdeWork` deterministic counters (CVODE semantics — the work-precision currency) · `error_norm_wrms` (Hairer (4.11) ≡ scipy, fixed-order serial) · **TWO controllers matching their references exactly** (`ElementaryController` scipy-strict-<1 + post-rejection cap; `PiController` Hairer ≤1 + facold floor — they disagree at 1.0, one parametrization would be dishonest) · the **dense-output contract** (fixed-width coeff blocks, caller storage, static eval) + `hermite_eval` cubic fallback · `integrate_fixed` (recomputed tᵢ, exact t1 landing, NotFinite in the DRIVER — kernels check-free). GATES: hand-computed RK amplification polynomials (bit-equal where representable) · empirical order slopes 1/2/4 · in-place memcmp equality · EXACT nfev counters · Lorenz run-twice bit-identity · full status honesty (blow-up ⇒ NotFinite, backward integration) · controller formula values/clamps/history · Hermite cubic-exactness. **109 asserts / 11 cases**; win-debug+ALL-guards / shipping (CMAKE_COMMAND audit clean) / asan / tidy / WSL-gcc green. ⚠ gotcha: `crd-simd-emission-check` needs vcvars (`dumpbin`) — bare ctest = phantom guard failure; use `run-ctest.bat`. | ~480 | 109 | — |
| ↳ **v9-b** ✅ (DONE 2026-06-12) | **Embedded explicit RK — SHIPPED with scipy-EXACT semantics** (session `2026-06-12-v9bcg-erk-events-symplectic.md`): `erk.hpp` — RK23 + RK45 + DOP853 (incl. its combined 5th/3rd error norm) with **tableaus EXTRACTED from the installed scipy 1.17.1 by `scripts/gen_erk_tableaus.py`** (extraction beats transcription) + Cash-Karp (exact rationals) + Tsit5 (Tsitouras 2011, FSAL frame); the `rk_step`/`select_initial_step`/`_step_impl` semantics read verbatim. ⭐⭐ **STEP-SEQUENCE EXACTNESS PROVEN** (`ode_scipy_difftest` vs `scripts/ode_scipy_ref.py`): VdP RK23 **2177/6533 == scipy** · RK45 **168/1106 ==** (y[0] BIT-identical) · DOP853 **55/926 ==** · decay RK45/DOP853 **130/782, 17/206 == with BIT-identical y**. Order slopes 3/5/5/5/8 certify every tableau; Arenstorf closes at 1e-5 (DOP853, rtol 1e-11); exact counter identities (nfev = 2 + attempts·stages); run-twice bit identity; backward integration. NAMED follow-ups: native interpolants (RK45 quartic/DOP853 7th-order), Verner pairs. | ~650 | — | — |
| ↳ **v9-c** ✅ (DONE 2026-06-12) | **Events + continuous output — SHIPPED.** `solution.hpp` (`OdeSolution`: contiguous (t,y,f) nodes, direction-aware deterministic lookup, Hermite eval — gates encode the h⁴/384·\|y⁗\| HERMITE BOUND explicitly; native interpolants tighten later) + `events.hpp` (`OdeEvent` contract + functor adapter; scipy semantics: zero-endpoint sign changes, direction filter, **brentq at 4·eps** (`detail/brentq.hpp`), terminal truncation with `EventTerminal` + `event_index`) wired into `integrate_erk`. Gates: projectile ground-hit at the ANALYTIC √(2y₀/g) (1e-9) · direction-filtered sin crossings π/2π with hit recording · backward-trajectory dense output. | ~400 | — | — |
| ↳ **v9-d** ✅ (DONE 2026-06-12) | **BDF/NDF — the stiff spine, SHIPPED scipy-EXACT** (session `2026-06-12-v9bcg-erk-events-symplectic.md` §v9-d): `bdf.hpp` — variable-order 1–5 Shampine-Reichelt NDF over the D-difference array, `compute_R`/`change_D`, the 4-iteration rate-predicate Newton, newton_tol = max(10ε/rtol, min(0.03, √rtol)), order selection via the (order−1/order/order+1) factors — `bdf.py` read VERBATIM **including the stale-LU-after-rejection quirk** (a first-draft `c != lu_c` refactor condition was caught and removed — scipy refactors ONLY on LU-absent). **`ode_linear_solver.hpp` = THE SEAM** (factor (I−c·J) + solve; dense hesap-dense partial-pivot LU now — the module's first hesap-dense edge; sparse/Krylov implement the same interface at v9-j). Jacobian policy: analytic via `has_jacobian()` (= scipy `jac=callable`, the exact configuration); plain-FD fallback (NAMED divergence from scipy's adaptive num_jac). ⭐⭐ **TRAJECTORY-EXACT vs scipy BDF with analytic Jacobians — EVERY COUNTER IDENTICAL**: ROBER t=100 **163 accepts / 431 nfev / 5 njev / 37 nlu == scipy exactly** (y to 15 digits) · VdP μ=1000 t=300 **79/174/4/24 == scipy exactly** (y to 14 digits) — Newton, Jacobian-reuse, stale-LU, and order-selection decisions all verified. ⭐ **THE STIFF CRUSH GATE**: VdP μ=1000, same rtol — **BDF 174 evals vs RK45 1,642,370 = 9,439× fewer**. Gates: stiff-linear exact solution · ROBER 1e5-horizon on the SUNDIALS-published decay curve + conservation 1e-7 · FD≡analytic agreement · run-twice bit identity (all counters) · backward + edges. Suite **341/30**; debug/shipping/asan/tidy/WSL-gcc + all guards green (⚠ the non-ASCII guard caught em-dashes in two test names — its job). HIRES/OREGO/Testset anchors + CVODE work-precision = v9-z scoreboard rows. | ~700 | 62 | — |
| ↳ **v9-e** ✅ (DONE 2026-06-12) | **Radau IIA(5) — SHIPPED scipy-EXACT** (session `2026-06-12-v9bcg-erk-events-symplectic.md` §v9-e): `radau.hpp` — the 3-stage collocation in A⁻¹'s eigenbasis (one REAL (μ_R/h·I−J) + one COMPLEX (μ_C/h·I−J) solve per Newton iteration — **the hesap-dense COMPLEX LU edge consumed**), `solve_collocation_system` verbatim (6-iter Newton on W=TI·Z), the LU_real-stabilized embedded error + rejected-step re-stabilization, the **Gustafsson predictive controller** + keep-LU rule (factor<1.2 ⇒ 1), the collocation dense output Q=Zᵀ·P (which IS the next step's Z0 warm start — part of the trajectory contract). Constants extracted from scipy at 17 digits. Exactness subtlety caught in port: `h_abs_old` stores the PRE-step `self.h_abs`. ⭐⭐ **EVERY COUNTER IDENTICAL to scipy Radau** (analytic jac): ROBER **88/726/22/110(nlu!)** == scipy (y to 16 digits) · VdP-1000 **29/233/5/40** == scipy (y[0] BIT-identical). ⭐ stiff crush vs RK45 >100× (gate). Gates: L-stable fast-mode kill (λ=−10⁴ → <1e-12, no ringing) · ROBER 1e5 conservation+curve · run-twice bit identity. Suite **358/34**; debug/shipping/asan/tidy/WSL-gcc + guards green. Hairer RADAU5 oracle + work-precision = v9-z rows. | ~600 | 17 | — |
| ↳ **v9-f** ✅ (DONE 2026-06-12) | **Rosenbrock + SDIRK — SHIPPED, and the reference got CRUSHED ON CORRECTNESS** (session `2026-06-12-v9bcg-erk-events-symplectic.md` §v9-f): `rosenbrock.hpp` = **RODAS4** with Boost.odeint-verbatim semantics (coefficients extracted from the installed 1.83 header; the odeint controller verbatim: fac = clamp(err^0.25/0.9, 1/6, 5) + Gustafsson predictive + err_old floor 0.01; J+LU per attempt as odeint does; solves through the v9-d SEAM via (I−γh·J)g = γh·r). ⭐⭐⭐ **FOUND AND FIXED A REAL BOOST.ODEINT BUG**: odeint's d4 = **+**0.0362…23 vs Hairer rodas.f's **−**0.0362…23 — invisible for autonomous systems, **degrades every non-autonomous problem to ORDER 1** (proven: odeint itself measures p̂ = 1.0385→1.0002 over five h-decades on y′=−2ty²; our port matched it DIGIT-FOR-DIGIT pre-fix e=0.0011252, then the rodas.f sign restored **p̂ = 4.15, error 10,000× smaller at the same h**). The port is verbatim-faithful AND more correct than the reference. `sdirk.hpp` = **TR-BDF2** as a 3-stage stiffly-accurate ESDIRK (γ = 2−√2 exact closed forms, the ARKODE-registered table; both implicit stages share ONE iteration matrix; simplified Newton with the BDF machinery) — the SPICE/ode23tb circuit workhorse (the DAW consumer) and the ESDIRK shape v9-i IMEX reuses. GATES: order slopes RODAS4 autonomous **4.07** + non-autonomous **4.15** + TR-BDF2 2.x · L-stable fast-mode kill (both) · Robertson conservation + **the bounded-cost contracts as ASSERTIONS** (RODAS4: nlu == attempts, nsol == 6·attempts, NO Newton — the real-time property; TR-BDF2: nlu ≪ nsol — the shared-matrix economy) · stiff crush >100× vs RK45 · run-twice bit identity + FD≡analytic (both). Suite **368/40**; debug/shipping/asan/tidy/WSL-gcc + guards green. Hairer RODAS oracle work-precision + odeint wall-clock = v9-z rows (odeint upstream-report candidate noted). | ~900 | 37 | — |
| ↳ **v9-g** ✅ (DONE 2026-06-12) | **Symplectic — SHIPPED** (`symplectic.hpp`, kernel-layer raw-span like steppers.hpp — the eylem hot-loop layer): `step_symplectic_euler` (kick-drift) · `step_velocity_verlet` (FSAL acceleration, 1 force eval/step) · `step_composition` + `yoshida4_w`/`yoshida6_w` (Yoshida 1990 solution A). Gates: Kepler order slopes **2/4/6** · ⭐ the ENERGY gate at the GAME-relevant h=0.2 over ~1590 orbits (Verlet \|ΔE\| bounded, first-tenth max == last-tenth max — textbook no-drift — while same-h RK4 drifts >3×; at h=0.05 RK4 shows no disadvantage over this horizon — MEASURED, the honest framing) · time-reversibility (forward+backward 1000 steps < 1e-9) · Yoshida-6 run-twice bit identity. NAMED later: Yoshida-8 (constants fetched not recalled), position Verlet/Forest-Ruth variants, odeint cross-check at v9-z. | ~300 | — | — |
| ↳ **v9-h** ✅ (DONE 2026-06-13, scoped) | **Mass matrices + index-1 DAE through BDF — SHIPPED** (session `2026-06-12-v9bcg-erk-events-symplectic.md` §v9-h/j): `OdeFunction::mass_matrix` (vtable slot 5, END append; CONSTANT dense M, possibly singular) + `OdeLinearSolver::factor_iteration_matrix_mass` ((M−c·J), END append) + the BDF mass path (residual c·f − M·(ψ+d); D[1] via M-solve when regular, 0 when singular — caller supplies CONSISTENT y0, calc_ic = named follow-up; **the M-less path stays BYTE-identical to the scipy-exact v9-d code**). GATES: M=I via the mass path = IDENTICAL DECISIONS to the plain path (all counters equal; values to 1e-14 — the ψ+d association difference is measured + named) · non-diagonal M exact solution · **singular-M index-1 DAE exact** (constraint < 1e-10) · ⭐ **Robertson ODE-form vs DAE-conservation-form cross-formulation agreement** (1e-6/1e-9) with the DAE form conserving to 1e-12 BY CONSTRUCTION · bit-identical run-twice. SCOPED OUT (named): M(t,y), Radau-mass, mass×sparse, calc_ic, transistor-amplifier/IDA rows (v9-z). | ~250 | 21 | — |
| ↳ **v9-j** ✅ (DONE 2026-06-13, sparse-direct part) | **Sparse Newton — THE STACK SHOWCASE SHIPPED**: `OdeFunction::sparse_jacobian` (CSR, slot 6 END append, the v7-g fixed-pattern convention) + `SparseOdeLinearSolver` (`ode_sparse_solver.hpp`: (I−c·J) assembled by TripletBuilder → **the v5b multifrontal LU**, deterministic; symbolic-reuse = named lever) + the BDF sparse path (n² dense jac NOT allocated — that's the point; explicit-solver contract). GATES: heat-2D MOL **n=1024** against the EXACT discrete-eigenmode decay e^{λ_h t} (1e-7, shape preserved) · sparse≡dense cross-check at n=144 (identical counters, 1e-12) · run-twice bit identity at n=1024. ⭐⭐ **THE SCALE CRUSH** (bench_ode_vs_refs, n=4096 heat-2D, rtol 1e-8, sparse analytic jac both sides): **Cerid BDF+multifrontal 50.4 ms / err 3.7e-9 BEATS CVODE+KLU 56.6 ms / err 1.7e-8** — sparse-vs-sparse at the scale where per-eval work dominates (the tiny-n caveat answered). SCOPED OUT (named): matrix-free Krylov/GMRES mode (CVODE-SPGMR peer) + preconditioner seam + worker knob ({1..16} already proven at the multifrontal level) — the v9-j follow-on. | ~350 | 13 | — |
| ↳ **v9-i** ✅ (DONE 2026-06-13) | **IMEX additive RK — the MOL/CFD pull.** SHIPPED `imex.hpp`: ARK3(2)4L[2]SA / ARK4(3)6L[2]SA / ARK5(4)8L[2]SA (Kennedy-Carpenter, coefficients EXTRACTED from SUNDIALS v6.4.1 by `gen_ark_tableaus.py` → bit-identical to ARKODE); explicit⊕implicit split via OdeFunction slots 7/8/9; shared-iteration-matrix ESDIRK + FSAL. Gates: per-part order slopes (explicit/implicit/split for all three, ~3/4/5) · L-stability · advection-diffusion MOL (IMEX <⅓ RK45 steps) · determinism · FD-vs-analytic J_I (55 asserts). ARKODE work-precision = the v9-z bench. ARS + KenCarp ARK pairs (explicit advection ⊕ implicit diffusion). Gates: Brusselator-1D/2D advection-diffusion MOL vs **ARKODE** (installed ✓) · per-part order verification. | ~350 | ~10 | — |
| ↳ **v9-j (Krylov)** ✅ (DONE 2026-06-13) | **Matrix-free Krylov Newton = CVODE SPGMR.** SHIPPED `ode_krylov_solver.hpp`: `KrylovOdeLinearSolver` drives BDF with NO Jacobian assembly — the inner solve is hesap-iterative FGMRES over `jacobian_vector` on (I−c·J); `OdeKrylovPreconditioner` (PrecSetup/PrecSolve) wired as the FGMRES M⁻¹. New `OdeLinearSolver` virtuals `is_matrix_free`/`factor_iteration_matrix_matfree` + the BDF `use_matfree` branch. Gates: exact discrete heat eigenmode (1.2e-9) · matrix-free == dense BDF (2.8e-16) · tridiagonal PrecSolve 485→206 GMRES iters · determinism (15 asserts). (Sparse-direct part shipped 2026-06-13 above.) [orig plan: sparse + Krylov MOL @ n~10⁴⁻⁵ vs CVODE-KLU/SPGMR + {1..16} moat — sparse-direct done; Krylov done.] The `OdeLinearSolver` seam grows sparse-direct mode (CSR Jacobian → hesap-direct multifrontal with `reuse_symbolic`/`refactorize`) + matrix-free Krylov mode (jac-vec → hesap-iterative GMRES + the v4 preconditioner seam = CVODE SPGMR). Gates: Brusselator-2D/heat-2D MOL at n~10⁴–10⁵ **work-precision + wall vs CVODE-KLU and CVODE-SPGMR** · the {1..16} moat on parallel RHS/Jacobian + the inherited factor moat. THE showcase slice: ode→direct→iterative→jobs, all deterministic. | ~400 | ~12 | — |
| ↳ **v9-k** ✅ (DONE 2026-06-13) | **Sensitivities — the control/opt pull.** SHIPPED `sensitivity.hpp`: `ParametricOdeFunction<T>` + `integrate_forward_sensitivities` (CVODES simultaneous corrector — augmented `[y;S]` through the existing ERK/BDF, block-diagonal `J_y` for the stiff path) + `integrate_adjoint_sensitivities` (CVODES ASA — backward `[λ;q]` over stored dense output; Lagrangian-derived signs). ⭐ THREE-ORACLE gate: forward-sensitivity = adjoint = central-difference FD (non-stiff exchange + stiff Robertson-with-params); determinism (30 asserts). CVODES wall/value comparison = the v9-z bench. Forward (staggered, CVODES pattern: augmented system sharing step sequence + Jacobian) + **adjoint** (checkpointed backward integration over dense output, CVODES ASA pattern). Gates: vs **CVODES** forward/adjoint on Robertson-with-parameters (published values) · FD cross-validation via the v7 `gradient_check` · the v7-b Dual forward-mode cross-check (THREE independent derivative oracles). | ~500 | ~15 | — |
| ↳ **v9-l** ✅ (DONE 2026-06-13 — NOT slipped) | **Higher-index DAE: structural index analysis + index reduction.** SHIPPED `dae_structural.hpp`: Pryce Σ-method (the Pantelides equivalent — signature matrix + HVT + dual offsets) → structural index + differentiation plan (pendulum→3, index-2→2, index-1→1, ODE→0, hand-verified). `dae.hpp`: `ConstrainedMechanicalSystem<T>` + `IndexReducedMechanicalOde` (index-3 multibody → index-1 via the acceleration constraint + KKT λ-elimination = the dummy-derivative selection for mechanics; the eylem consumer). Gate: index-3 pendulum integrates with |constraint| 2.2e-10 + energy 1.7e-9 over 3 s (24 asserts). NAMED bridges: long-horizon GGL/projection · general AD-symbolic auto-reduction (needs an AD-residual layer; ode↛opt forbids reusing the Dual). Structural index analysis (bipartite matching) + Mattsson-Söderlind dummy derivatives ⇒ index-1 ⇒ the v9-h machinery. Gates: index-3 pendulum · car-axis (Testset) · slider-crank. Slip rationale: research-grade; the multibody/Modelica-class consumer is real but downstream (eylem constraints use their own stabilized formulations first). | ~500 | ~12 | — |
| ↳ **v9-z** ✅ (DONE 2026-06-13, local parts) | **CLOSE.** SHIPPED CLI `hesap.ode.solve.f64` (canned problems decay/VdP/Robertson/oscillator × 7 methods RK45/RK23/DOP853/BDF/Radau/RODAS4/TR-BDF2; `register_ode_cli_anchor`; 58 asserts incl. e^{-t}/Robertson-mass/oscillator/error-paths). System doc `docs/systems/hesap-ode.md` rewritten to the full cluster; ADR-0091 finalized. Suite **577/67** on win-debug + win-asan (zero ASan errors on all new code) + win-shipping (LTCG, freshly-recompiled). PENDING USER/CI (the standing pattern): the 18-config CI sweep (win-shipping deps RE-poisoned — wipe+reconfigure before its next local use) + the CVODE/IDA/ARKODE/CVODES work-precision scoreboard rows (the bench harness `bench_ode_vs_refs.cpp` extends to the new methods). | ~350 | 58 | — | CLI `hesap.ode.*` (canned Testset problems + method/tolerance selection; callables = API-level, the v7-z pattern) · **THE WORK-PRECISION SCOREBOARD** vs CVODE/IDA/ARKODE + scipy + odeint at matched tolerances over the canonical corpus (Robertson/VdP/HIRES/OREGO/Arenstorf/Pleiades/Brusselator-MOL/transistor) — FULL BOARD, all metrics together · {1..16} moat sweep · `docs/systems/hesap-ode.md` + ADR-0091 finalized + session logs. | ~350 | ~12 | — |
| **v10** | **FFT — new module `crd-hesap-fft`.** ▶ ACTIVE — **v10-a/b/d/g/f SHIPPED.** v10-a/b/d (2026-06-13): 1D complex Stockham+scheduled codelets ≈ 0.45× MKL / beats PocketFFT everywhere; real FFT crushes numpy/scipy rfft. v10-g (2026-06-14): NUFFT type-1/2 WINS FINUFFT small/mid at superior accuracy, parity 1M, FFT-gated only at n=2^18. v10-f (2026-06-14): **DCT/DST BEATS scipy/PocketFFT at every size** (FFTW/MKL ahead on the shared FFT-kernel wall). Suite **109/20 on 4 configs**. ⭐ HONEST: single-thread 1D-FFT-throughput vs MKL/FFTW is a Spiral-class scheduler project (deferred); the breadth (real FFT, NUFFT, DCT) beats every OTHER peer outright. Remaining c/e/h/z below (memory `project_v10_fft_plan`). Gold standards **FFTW3 + PocketFFT** (scipy/numpy default) + **MKL** (proprietary). ⭐ RATIFIED 2026-06-13 (user): **bar = BEAT MKL**; **scope = a→h** (full toolkit + NUFFT + sparse FFT). Thesis (deliberately OFF the determinism moat — an FFT has no thread-dependent reduction, so {1..16} bit-identity is nearly free AND PocketFFT-batched is equally deterministic): lead with **zero planning overhead + deterministic plan-from-factorization** (FFTW MEASURE/PATIENT varies run-to-run; its wisdom isn't reproducible across builds — sourced) + **typed zero-dependency integration**; speed-crush PocketFFT everywhere + MKL/FFTW on the **AVX2-level i9-14900K** (Raptor Lake has NO AVX-512 ⇒ MKL runs AVX2 here ⇒ level fight; AVX-512 server/CI = the honest caveat). Design: **Stockham autosort radix-2/4/8** + straight-line SIMD **codelet leaves** (N=2..32) + **Bailey four-step/six-step** cache-blocking + the multi-stream DRAM packing that won the lattice solver. ⭐ Correctness gate = **brute-force O(N²) DFT** (NOT round-trip — the FFT edition of the odeint-d4 trap), accuracy ~O(ε·logN). f32 + f64, complex-inherent. | ~4600 | ~146 | multi-session |
| ↳ **v10-a** ✅ (DONE 2026-06-13) | **Substrate + Stockham radix-2/4 (pow-2) + the plan/determinism contract.** SHIPPED `crd-hesap-fft` module; `FftPlan<T>` (algorithm chosen deterministically from the size factorization — NO runtime measurement; one precomputed twiddle table per plan, shared across threads ⇒ cross-THREAD bit-identical, NOT claimed cross-compiler — sin/cos ±1 ULP gcc↔MSVC); complex FFT/IFFT (f32/f64) via Stockham autosort radix-2/4 over split SoA, SIMD via `crd::math::simd`. GATE = brute-force O(N²) DFT cross-check + RMS ~O(ε·logN) (1e-15 f64); round-trip IFFT∘FFT; {1..16} batched bit-identity. First reference shootout established the baseline (radix-2/4 ≈ 0.35× MKL). | ~600 | ~20 | — |
| ↳ **v10-b** ✅ (DONE 2026-06-13) | **Scheduled codelets + mixed-radix planner — the throughput pass.** SHIPPED: the genfft-lite codelet generator `scripts/gen_fft_codelets.py` (straight-line CSE'd SIMD leaves N=2..32 + twiddle codelets 8/16/32, emitted to `detail/codelets.hpp`, numpy self-checked before emit) with a **register-pressure list scheduler** (lifetime-aware key `(1−dying, is_load, fanout, −i)` — prefer short-lived results, Belady-flavoured); the **interleave fold** (radix4_first deinterleave-load over k → middle combine passes → radix4_last over-j with transpose4x4 + per-lane twiddle); and a **mixed-radix size-aware planner** (greedy radix-2/4/8/16/32, `rmax_bits` = 5 for 2¹⁵–2²⁰, 4 for ≥2¹², else 3, with the no-leftover-radix-2 guard). ⭐ FOUR MEASURED WINS over the v10-a baseline: the fold +1.7× · scheduler flips radix-8-over-k from a −17% spill into a win · radix-32 +25% @ L2 · lifetime-scheduler +5–11% @ L1/L2. RESULT: **~0.45× MKL, beats PocketFFT at every size, 1e-15.** ⭐ The structural+source space was **exhaustively measured & refuted**: radix>32 (spills), six-step (loses), cache-oblivious recursion (3.6× loss), Bailey four-step (loses), across-radix-32 codelet (strided), in-place DIT (2.6–3.3× loss — Stockham's no-bit-reversal beats 1× footprint here), FMA (gcc already fuses). HONEST VERDICT: Stockham + the four wins is the structural-AND-source optimum on this AVX2 box; the residual ~2.2× to MKL is the **genfft/Spiral scheduler-as-a-compiler-pass**, a dedicated cooled-box sub-project — NOT a source tweak (deferred, see `project_v10_fft_plan`). | ~700 | ~18 | — |
| ↳ **v10-c** (PLANNED) | **Bluestein (chirp-z) + Rader (primes) → any size O(n log n).** Bluestein convolution (highly-composite padding) for arbitrary/prime sizes; Rader for primes via (p−1)-convolution. Gates: prime 1009/1031 vs naive DFT; shootout on primes vs PocketFFT/FFTW. | ~400 | ~14 | — |
| ↳ **v10-d** ✅ (DONE 2026-06-13) | **Real FFT (RFFT/IRFFT) + Hermitian symmetry (~2×).** SHIPPED `real_fft.hpp`: `RealFftPlan<T>` (n = pow-2 ≥ 4) packs the n reals as n/2 complex, runs ONE size-(n/2) `FftPlan<T>` (the v10-b engine), and Hermitian-recombines with a precomputed W_n^k table (k=0..n/2) — ~½ the work of a complex FFT of the real input. irfft applies 1/n (round-trip exact). Reusing the v10-b engine that already beats PocketFFT ⇒ **Cerid's rfft crushes numpy/scipy/PocketFFT rfft.** Gates: rfft vs full-complex-FFT-of-real to ε + round-trip irfft∘rfft (seed `0x5EED1234 ^ n`); Hermitian symmetry. | ~400 | ~14 | — |
| ↳ **v10-e** (PLANNED) | **Multi-dim 2D/3D/N-D + batched + {1..16} parallel.** Row-column N-D FFT; batched transforms parallelized over INDEPENDENT transforms via `crd-jobs`; {1..16} bit-identity = correctness check (not a moat headline). The 3D real-to-complex shootout (the MKL/FFTW/PocketFFT 3D comparison). | ~450 | ~16 | — |
| ↳ **v10-f** ✅ (DONE 2026-06-14, DCT-II/III + DST-II/III) | **DCT/DST — BEATS scipy/PocketFFT outright.** SHIPPED `dct.hpp` `DctPlan<T>`: DCT-II/III + DST-II/III (Makhoul O(N log N) over FFT; every formula verified vs scipy.fft in `scripts/dct_research.py` BEFORE porting). Forward dct2/dst2 on the **v10-d real FFT** (shuffled Makhoul sequence is real ⇒ half the work; W[k>N/2]=conj(W[N-k])). GATE = direct O(N²) sum (NOT round-trip — odeint-d4 trap) + 2N round-trip + determinism; 43/4, suite **109/20 on 4 configs**. ⭐ **SHOOTOUT** (`bench_dct_vs_refs.cpp`, DCT-II f64, 1T): **BEATS PocketFFT (numpy/scipy backend) at EVERY size** (1.0–1.7×; the real-FFT path flipped the large-N losses 0.86→1.25×). vs **FFTW** 0.55→0.65× (win at 32768, parity at 16384) — FFTW ahead via tuned real-DCT codelets = the SAME inherited FFT-kernel wall as the raw complex FFT, not a DCT gap (MKL same class). Follow-ons (named): inverse real FFT for dct3/dst3, DCT/DST-I/IV, FFT-convolution, FHT. | ~600 | 43 | — |
| ↳ **v10-g** ✅ (DONE 2026-06-14, type-1/2 1D) | **NUFFT (Greengard-Lee) vs FINUFFT — WINS small/mid at SUPERIOR accuracy.** SHIPPED `nufft.hpp`: `NufftPlan<T>` type-1 (nonuniform→uniform spread) + type-2 (interp), ES kernel (exp-of-semicircle, β=2.30w) + σ=2 + width-from-tol, the spread→FFT(v10-b)→deconvolve pipeline (`d_k` = GL-quadratured kernel-FT, precomputed/shared → deterministic), `set_points` shared weights. GATE = **direct O(NM) nonuniform DFT** (NOT round-trip — the odeint-d4 trap) both isign + f32/f64 + tol-monotonicity + spreader run-twice **bit-identical**; 13/5, suite 66/16 on **4 configs (debug+asan+tidy+shipping LTCG)**. ⭐ **THE FINUFFT SHOOTOUT** (`bench_nufft_vs_finufft.cpp`, FINUFFT built from src, 1 thread, EXECUTE-only, Cerid width+2 ⇒ **3× MORE accurate than FINUFFT**): naive 0.5× → 3 measured levers (wrap-split fast path + memset zero; multi-accumulator interp breaking the reduction chain) → **WINS small/mid (4096 T1 1.99× / 16384 T2 1.73×), PARITY at 1M (T2 1.14×), loses only the FFT-bound n=2^18** (262144 modes, 62–91% FFT = the inherited v10-b FFT-engine deficit, the deferred scheduler project — FINUFFT's FFT there is FFTW_ESTIMATE). ⚠ bin-sort MEASURED-REGRESSED (14900K's 36MB L3 hides the scatter) ⇒ reverted. HONEST scoping: serial run-twice determinism (NOT a built {1..16} moat — parallel owner-per-subgrid spread designed, not implemented); EXECUTE-only timing. Follow-ons (named): type-3, 2D/3D, jobs-parallel deterministic spread. | ~600 | 13 | — |
| ↳ **v10-h** (PLANNED) | **Sparse FFT (Hassanieh-Indyk-Katabi-Price 2012).** Sub-linear for k-sparse spectra. Gates: exact k-sparse recovery; sub-linear scaling vs full FFT at large n / small k. Research-grade. | ~450 | ~12 | — |
| ↳ **v10-z** (PLANNED) | **CLOSE.** CLI `hesap.fft.*`; the full reference-class scoreboard vs FFTW + PocketFFT + MKL (throughput GFLOPS + accuracy + planning-time — the beat-MKL verdict on the AVX2-level dev box, with the honest AVX-512 caveat); {1..16} correctness sweep; 4-config DoD + gcc; `docs/systems/hesap-fft.md` + new ADR + session log; `context.md` + phase-doc + the reference-class-policy FFT row updated. | ~400 | ~14 | — |
| **v11** | DSP: FIR (windowed sinc + Parks-McClellan Remez) + IIR (bilinear-transform Butterworth/Cheb-I/Cheb-II/Elliptic/Bessel) + biquad + resampling (polyphase) + spectral analysis (Welch / Bartlett) + Z-transform + complex variants + CLI registration | ~2800 | ~110 | ~2 wk |
| **v12** | Statistics: 50+ distributions (Stan-math reference) + statistical tests (t / chi² / KS / Mann-Whitney / Wilcoxon / Friedman / Kruskal-Wallis / ANOVA) + special functions (gamma / beta / erf / Bessel J/Y/I/K / Legendre / Hermite / Chebyshev) + RNGs (splittable PCG + Xoshiro256** + **Philox** + **Threefry** for parallel-deterministic) + bootstrap / jackknife + CLI registration | ~3500 | ~130 | ~2.5 wk |
| **v13** | Polynomial / interpolation (linear / cubic spline / Akima / Hermite / monotone / Chebyshev / barycentric / RBF) + quadrature (Gauss-Legendre / Hermite / Laguerre / Lobatto / Radau / Clenshaw-Curtis / adaptive Simpson / Romberg) + numerical differentiation (FD / Richardson extrapolation / Hermite extrapolation) + CLI registration | ~2300 | ~90 | ~1.5 wk |
| **v14** | N-dim tensors + broadcasting + einsum + reductions + reshape/transpose/slice/gather/scatter + tensor LinearOp + complex variants + CLI registration | ~2800 | ~110 | ~2 wk |
| **v15** | Autodiff v1 forward mode: dual numbers (Jet types) + hyper-dual (2nd order) + sparse Jacobian (Curtis-Powell-Reid coloring) + **per-op manual VJP / JVP rules for entire BLAS surface** (JAX pattern — custom `solve` VJP avoids AD-through-LU) + CLI registration | ~3000 | ~120 | ~2.5 wk |
| **v16** | Autodiff v2 reverse mode: tape-based + **operator-level structural AD** (JAX expression-graph pattern) + higher-order (forward-over-reverse / reverse-over-reverse for Hessian-vector) + **checkpointing** (Griewank 1992) + CLI registration | ~3500 | ~120 | ~3 wk |
| **v17** | GPU mirror via `crd-rhi-compute` (Phase 3.1.7.6): dense BLAS L1/L2/L3 + sparse spmv/spmm (CSR5 + Merge-CSR GPU kernels) + GPU FFT (Stockham radix-mix) + GPU iterative (CG/PCG/GMRES with GPU spmv) + GPU autodiff + complex variants + CLI registration | ~4500 | ~120 | ~3 wk |
| **v18** | **Phase 4.0 dependency: requires `crd-cli` substrate**. Notebook + interactive plotting + plug-in surface. **C++ hot-reload cell engine** (cells are `.crds.cpp` files compiled to hot-reload DLLs per ADR-0081). `'CNBK'` notebook FourCC. **MATLAB-class syntax facade is a stretch goal**, not Phase 3.1.6 scope (a C++-source-transformer would emit C++ from MATLAB syntax; reserved). | ~2500 | ~50 | ~2 wk |

**v0 NEW TOTAL**: ~5400 LOC engine + ~445 tests across v0a-f sub-slices.

**Full Phase 3.1.6 elite-tier**:
- ~57 KLOC engine + ~2 200 tests across 18 slices (was 52 KLOC / 580 tests in 2026-05-10 plan; the ~10% LOC bump comes from full matrix-type catalog + complex variants + CLI plumbing baked into every slice; the test-count growth from ~30/slice → ~100-150/slice is the elite-tier verification discipline same as Phase 3.1.7 geometry).
- **~10-12 months calendar** (was 6-8 mo) elite-tier, comparable to the Phase 3.1.7 `crd-geometry` schedule (22 KLOC over 6 mo elite).

> v17 (GPU) is independently shippable. If schedule pressure
> surfaces, v17 can defer to a "Phase 3.1.6 follow-up" without
> blocking the CPU substrate's usefulness. v18 explicitly depends on
> Phase 4.0 `crd-cli` substrate landing — v18 cannot ship before
> Phase 4.0.
| v18 | REPL + notebook (`.cnb` `'CNBK'` format) + plot integration + plug-in C ABI (per ADR-0034 pattern) + ImGui frontend panel | ~2500 | ~24 | ~2 wk |

**Total: ~52 KLOC, ~580 tests, ~38 weeks (~8 months) for the full
substrate.** Comparable in scope to eylem itself.

> v17 (GPU) and v18 (REPL) are independently shippable. If schedule
> pressure surfaces, they can defer to a later "Phase 3.1.6 follow-up"
> without blocking the CPU substrate's usefulness.

---

## v5 — Sparse DIRECT factorization — DETAILED PLAN (planned 2026-05-28)

> **The COMPLETE elite direct-solver family** — the direct twin of the v4
> iterative spine. Mandate (user-directed 2026-05-28): "fully elite path …
> outstanding, unusually world-class and complete." Never-defer
> (`feedback_hesap_substrate_never_defer_features`): four exact
> factorizations + the rank-structured family (HSS *and* BLR) +
> mixed-precision refinement, all four type instantiations, CLI per op.
> Three pillars, all mandatory: **full cross-thread bit-determinism +
> correctness + performance (beat Eigen; match the SuiteSparse floor).**
> Deep-research dossier: `docs/research/cerid-hesap-v5-sparse-direct.md`.

### v5 close gates — LOCKED 2026-05-28 (apply to EVERY v5 sub-slice)

> User directive 2026-05-28. These are HARD close gates, additional to the
> standard per-slice DoD (build clean / unit tests / cross-config / tidy):
>
> 1. **Benchmarks are mandatory at slice close.** No sub-slice closes without
>    running its `bench_hesap_*_vs_reference` when a benchmark is applicable.
>    Unit-tests-green ≠ closed. If no bench exists for an op with a frontier
>    peer, writing one IS part of the slice.
> 2. **Crush / push to the limits.** The result must beat the frontier peer
>    (Eigen / SuiteSparse CHOLMOD-UMFPACK-SPQR / LAPACK), or match the true
>    hardware floor. "Passes but slower" is not done.
> 3. **NEVER regress existing performance.** A change must not make any
>    previously-measured benchmark slower/worse (time, fill, iterations,
>    throughput). Compare against the PRIOR baseline, not only the reference.
>    A change that regresses a previously-winning benchmark **does not ship —
>    revert or fix.** (Case: v5a-0 ND-compression — passed all unit tests, but
>    regressed ND fill on bcsstk13/24/25; reverted in full.)
> 4. **Benchmark the PREMISE before building an optimization.** If the existing
>    path already wins, there may be nothing to fix — a quick bench of the
>    current state beats writing speculative machinery. (See `feedback_benchmarks_mandatory_at_slice_close`.)

### Framing — the substrate thesis made concrete

A sparse direct factorization = **symbolic analysis (already shipped in
v2c) + a tree of *dense* panel operations.** `crd::hesap::ordering::SymbolicFactor`
hands v5 the elimination tree, postorder, column counts, full L-pattern
(CSC), and the fundamental supernode partition. So v5 is not "write a
sparse factorizer from scratch" — it is **"orchestrate v0's dense BLAS-3
kernels over v2c's assembly tree, deterministically."** Every node's work
is a dense TRSM/SYRK/GEQRF/GETRF on a supernode or frontal matrix — kernels
we already beat LAPACK with. Primary consumers: structural mechanics,
implicit FEM (eylem v7), CFD pressure-Poisson, circuit/aerospace stiffness,
and the inner solves of optimization (v7/v8) + implicit ODE (v9).

### Architecture — new module `crd-hesap-direct`

One-way deps (no cycles): `crd-hesap` (LinearOp, CLI) · `crd-hesap-sparse`
(CSC/CSR, spmv for refinement residual) · `crd-hesap-ordering`
(`SymbolicFactor`, AMD/CAMD/ND, MC64, postorder) · `crd-hesap-dense`
(dense panel kernels + `rsvd`/range-finder for HSS) · `crd-hesap-sched`
(`DependencyGraph` — the PLASMA/PaRSEC task DAG, already shipped) ·
`crd-jobs`, `crd-memory`.

**Two foundational data structures, specced once in v5a-1, reused by every
family:**

1. **`Factorization<T>`** — factored representation + a cheap, re-callable,
   **multi-RHS-from-day-1** `solve(F, B)`. Factor-once/solve-N is the
   FEM/eylem/opt access pattern; never bolt multi-RHS on later. Carries the
   v4z factor-vs-solve break-even reporting hooks for benches.
2. **Frontal matrix + `extend_add`** — dense `Matrix<T,RowMajor>` + relative
   row/col index map into the parent's pattern + the scatter-add of a child's
   Schur-complement block into its parent. The central reusable kernel for
   multifrontal QR (v5c), LDLᵀ (v5d), and rank-structured fronts (v5e);
   defined in v5a-1 so v5c+ consume a settled surface.

### The determinism moat — claimed per family (the differentiator)

No frontier sparse-direct library (CHOLMOD / UMFPACK / SPQR / MUMPS /
PARDISO / SuperLU) offers bit-exact factors across thread counts — they
reduce frontal updates in completion order. The v5 universal discipline
(v4 §26 D(iter)-1): **serial reductions within a front; disjoint-slab
parallelism over independent subtrees only; never a fork-join sum across
threads.** Sibling subtrees touch disjoint data until their parent
assembles them, and `extend_add` iterates children in a **fixed (postorder)
order** → the assembled front is identical regardless of which worker
finished first. This exactly satisfies the `DependencyGraph` contract
(ready tasks run in any order; the order that matters is encoded in the
deterministic parent-assembly step, not in dispatch).

| Family | Bit-exact across {1,2,4,8,16} | Mechanism |
| --- | --- | --- |
| **Cholesky** | L | No pivoting; static supernodal etree schedule. |
| **LDLᵀ** | L, D, pivot + delayed-pivot sequence | Static 1×1/2×2 threshold on deterministic front data + deterministic tie-break; delayed count is a function of the front. |
| **LU** | pivot sequence, L, U | **MC64 + threshold partial pivot** (dynamic partial pivot is order-dependent → serial reference only, v5b-1). |
| **QR** | R, Q (Householder vectors) | Per-front Householder local + deterministic; assembly-tree order is global determinism; rank-reveal uses deterministic column-norm reduction + tie-break. |

### Per-family algorithm decisions (canonical + reference)

- **v5a Supernodal Cholesky (SPD/HPD)** — left-looking supernodal
  (Davis 2006; Ng-Peyton 1993; CHOLMOD-class), relaxed supernode
  amalgamation (CHOLMOD `nrelax`/`zrelax`, deterministic merge rule),
  complex Hermitian LLᴴ, tree-parallel on `DependencyGraph`.
- **v5b Sparse LU** — Gilbert-Peierls `cs_lu` (Davis 2006, DFS-reachability,
  serial reference) → supernodal LU (Demmel-Eisenstat-Gilbert-Li 1999,
  SuperLU) with MC64 (v4j-1a) + threshold partial pivot for deterministic
  parallel pivoting.
- **v5c Multifrontal QR** — column etree + frontal Householder + `extend_add`
  (Davis 2011, SuiteSparseQR), implicit Qᵀ for least-squares, rank-revealing
  in-front column pivot.
- **v5d Multifrontal LDLᵀ** — Duff-Reid 1983 (MA57-class), 1×1/2×2 +
  deterministic delayed pivots, complex-symmetric + Hermitian-indefinite.
- **v5e Rank-structured fronts** — HSS (Xia 2010) + STRUMPACK randomized-
  sampling front construction (Ghysels-Li 2016) + BLR (Amestoy 2015,
  MUMPS-BLR). Counter-based RNG keyed by block index so the sampled basis
  is thread-independent (moat-safe). **v5e-1 (ID + range-finder + HSS + ULV)
  and v5e-2 (global-sample construction) ✅ 2026-06-04 — vs STRUMPACK serial
  rank=4 machine-eps: compress CRUSH 1.39–3.41× (QR-then-tiny-SVD replacing
  the tall full-SVD), factor CRUSH 2.7–4.1×, solve PARITY/WIN 0.99–1.06×, +
  the bit-identical-{1,2,4,8} determinism moat STRUMPACK lacks.** **v5e-3 BLR
  ✅ 2026-06-04** — BLR substrate built (correct/moat-safe) but the gate proved it
  net-NEGATIVE vs Cerid's own elite dense path at sim sizes (densifies the Schur at
  each tree edge ⇒ no flop/storage win) ⇒ kept for very-large-N, NOT the crush vehicle.
  The win was the **dense multifrontal Cholesky serial-crush** (factor kernel 49-53 GF/s
  BEATS MUMPS-full's ~44 via blocked-syrk + direct-CSC L assembly; 1.44-1.53× behind
  MUMPS-full all-in, gap localized to non-factor overhead). Parallel hybrid deferred
  (node-parallel sub-1×; can't out-scale MUMPS — parity + the determinism moat is the
  honest ceiling). Found+fixed a latent gemm FrameArena-exhaustion bug en route.
- **v5f Mixed-precision iterative refinement** — HPL-AI / Carson-Higham
  2018, opt-in factor-f32 + refine-f64 `solve` policy across all families.

### Bench / reference strategy

`CRD_BUILD_HESAP_VS_SUITESPARSE` set up in v5a-1 (WSL, gitignored
`external/`, never CI — same pattern as `CRD_BUILD_HESAP_VS_ILUPACK`),
reused by v5b/c/d. Apples-to-apples header peer = Eigen
`SimplicialLLT`/`SparseLU`/`SparseQR`/`SimplicialLDLT`; reference floor =
SuiteSparse CHOLMOD/UMFPACK/SPQR (+ MA57-class for LDLᵀ). Report
`‖b−Ax‖/‖b‖` after refinement (matched-true-residual + correct-peer
discipline). Corpus: SuiteSparse Matrix Collection (SPD `bcsstk*`/`ldoor`/
`af_shell`/`nd24k` 3D for HSS; unsymmetric circuit/CFD; least-squares) +
the v4 corpus for continuity.

### Open scope (pinned at the §27 lock)

- **BLR (v5e-3) ships in v5** (LOCKED 2026-05-28), expanding ADR-0065's
  BLR-reserve; **HSS (v5e-2) is the full STRUMPACK feature set** (adaptive
  rank + dense-fallback + ND-aware clustering). Both pinned at the §27 lock.
- Calendar honesty: ~14 elite sub-slices ≈ **multi-month**, ~7000+ LOC /
  ~165 tests (revised up from the table's ~4500/~140 — fuller HSS+BLR +
  mixed-precision IR + weighted-ND). Accepted per
  `feedback_hesap_clean_structure_over_calendar`.
- **Start gate:** Phase 2.2 **S8** (streaming-allocators cluster close +
  ADR-0085 lock + 18-config sweep) per `context.md`. v5a-0 execution
  begins after S8; this plan can be reviewed now.

---

## v4 — Iterative solvers + preconditioners + AMG — DETAILED PLAN (planned 2026-05-25)

> **The COMPLETE family. Never-defer (`feedback_hesap_substrate_never_defer_features`):**
> every Krylov method, every preconditioner, block/multi-RHS forms, inner-Krylov nesting,
> and the full AMG family ship in v4. The ONLY thing in v5 is sparse-**direct**
> factorization. Three pillars, all mandatory: **full bit-determinism + correctness +
> performance (beat Eigen + any frontier library).**

### Framing — the substrate thesis made concrete

Every solver consumes a **`LinearOp<T>`** (`apply: y=A·x`, PETSc-`Mat`/Trilinos-`Operator`
shaped, already shipped in core `crd-hesap`) + an optional preconditioner **`LinearOp<T>`**
(`apply: M⁻¹`). The solver never sees a matrix → the same CG/FGMRES/BiCGSTAB drives a sparse
matrix, a dense matrix, a **matrix-free PDE stencil**, or a **physics/optimisation Jacobian**
(eylem, a sim, a tool) unchanged. v4 builds the Krylov + preconditioner + multigrid machinery
once; every domain plugs its operator in.

### Architecture — 4 modules (dependency-clean)

- **`crd-hesap-resources`** (matrix corpus bridge; the **`v4-corpus`** slice, runs BEFORE v4a per
  the 2026-05-25 user directive) — depends `crd-resources` + `crd-hesap-sparse`. One-way: neither
  depends on it (a loader producing a `SparseMatrix<T>` payload needs both, so it lives in a bridge
  module). Makes SuiteSparse matrices **first-class cooked engine resources** (`'HMTX'` FourCC +
  CRDR chunks + cooker `.mtx`→binary CSR + runtime `ILoader` + fetch/cook/load/info CLI). The
  v4a framework + every vs-reference bench then loads the corpus through `ResourceManager`
  (eviction/hot-reload/deterministic-replay for free) instead of an ad-hoc bench-only read.
  **New ADR-0084.**
- **`crd-hesap-iterative`** (solvers) — depends core `crd-hesap` (LinearOp) + `crd-hesap-dense`
  (Arnoldi Hessenberg least-squares via Givens; recycling eigenproblems). A consumer wanting
  just solvers + a matrix-free operator + Jacobi pulls in NO sparse dependency.
- **`crd-hesap-preconditioners`** — depends `crd-hesap-sparse` (pattern/symbolic from v2) +
  `crd-hesap-dense`. IC/ILU/SPAI/polynomial/Schwarz/multilevel-ILU.
- **`crd-hesap-amg`** — depends sparse (Galerkin `Pᵀ A P` via spgemm) + iterative (the cycle
  is a smoother+coarse-solve recursion) + preconditioners. AMG is BOTH a standalone solver
  AND a `LinearOp` preconditioner (one V-cycle = `M⁻¹`).

Solver × preconditioner are **orthogonal** (any × any; a preconditioner IS a `LinearOp`).
Output `IterativeResult{iterations, residual_norm, converged, breakdown}`. **FGMRES is
flexible from the start** (variable preconditioner — near-zero cost over GMRES; future-proofs
inner-Krylov-as-preconditioner + AMG-as-preconditioner). Complex variants fold in PER-SLICE.

### THE DETERMINISM MOAT (gated, not a footnote)

Every Krylov inner product `⟨x,y⟩` + norm `‖r‖` routes through the **KBN-pairwise `blas1`
reductions** (shipped v0b); parallel spmv is already bit-exact across threads (v1b). So every
solve yields a **bit-identical {iteration count, residual sequence, final vector} across
thread counts {1,2,4,8,16}**. AMG setup tie-breaks are fixed (deterministic coarsening, like
the v2 AMD/ND ordering). **No frontier library (Eigen, PETSc, AMGCL, Trilinos) ships
deterministic iterative solving** — this is hesap's identity and a hard gate on every slice.

### Benchmark corpus — fetch from the internet (SuiteSparse)

Reuse the v1d gated `file(DOWNLOAD)` + Matrix-Market reader: pull real **SuiteSparse**
matrices spanning SPD (CG/IC/AMG — bcsstk*, Boeing), nonsymmetric (GMRES/ILU — sherman,
gemat), symmetric-indefinite (MINRES), rectangular (LSQR — illc/well), and CFD/circuit
(AMG hard cases). Every kernel benches **vs Eigen + the appropriate frontier**.

### Per-method beat target (honest, populated empirically)

| Method | Reference | Target |
|---|---|---|
| CG / PCG | Eigen CG + DiagonalPrecond | 1.1–1.5× iters×wall (better spmv DRAM-bound + determinism) |
| BiCGSTAB | Eigen BiCGSTAB | beat |
| FGMRES / MINRES / SYMMLQ / LSQR / LSMR / QMR / IDR(s) | Eigen lacks most | existence + breadth; bench vs AMGCL / published |
| GCRO-DR + recycling | frontier mostly lacks | **iters saved across a solve sequence** (the algorithmic + substrate-for-domains win) |
| block-Krylov | Eigen lacks | multi-RHS throughput vs N single solves |
| IC(0)/ILU(0)/ILU(p)/ILUT | Eigen IncompleteCholesky/IncompleteLUT | beat/match (note ILUT≠level-0 — compare like-for-like) |
| SPAI / polynomial / Schwarz / multilevel-ILU | AMGCL / ILUPACK | convergence quality on hard matrices |
| AMG (RS / SA / AGMG / bootstrap) | AMGCL / PETSc-GAMG / hypre-BoomerAMG | grid-complexity + convergence-factor + setup+solve wall |
| **determinism (ALL)** | **nobody** | bit-exact iters + vector across {1,2,4,8,16} threads — GATED |

### Slice ledger (complete — every item ships; advisor-vet each before implementing)

**`crd-hesap-resources` (the matrix corpus — runs FIRST):**
- **v4-corpus** — SuiteSparse matrices as first-class cooked resources. `crd-hesap-resources`
  bridge module + `'HMTX'` FourCC + CRDR chunks: `MXHD` header + `MXOP` (outer_ptr u32[rows+1]) +
  `MXII` (inner_idx u32[nnz]) + `MXVL` (values, raw `T` bytes). Loader uses `crdr_find_chunk`
  (order-agnostic — CrdrWriter FourCC-sorts at finish). Cooker `MatrixArtifactBuilder`
  (`from_matrix_market` reuses v1g `read_matrix_market`; `from_csr<T>`). Runtime `ILoader` →
  `SparseMatrixResource` (rows/cols/nnz/variant + 3 byte blobs; typed `build_csr<T>(alloc)` asserts
  variant↔`T`). `register_hesap_matrix_loader`. **Single loader, variant-in-header** (one FourCC; 4
  type variants distinguished by the header tag, not 4 FourCCs).
  - **`MatrixFileInfo` pinned at 40 bytes** (advisor): `{u32 rows, u32 cols, u64 nnz, u8 variant,
    u8 format, u8 reserved[6], u64 topology_hash, u64 frame_stamp}`. `nnz` is **u64** (SuiteSparse
    approaches 4G entries). `topology_hash`/`frame_stamp` stored so the loader can assert-on-mismatch
    (free corruption detector). `format` byte reserved for BSR/ELL/DIA/SELL later (CSR=0 today).
  - **`variant` enum pinned APPEND-ONLY** in the public header (`0=f32, 1=f64, 2=c32, 3=c64`;
    do-not-renumber) — recorded in ADR-0084, else a reorder silently mis-types every cooked `.crdr`.
  - Little-endian-host CRDR posture (matches Profile/Mesh `memcpy`); a future ARM/macOS target is a
    project-wide concern, not this slice's.
  - **CLI `hesap.matrix.{cook,load,info}` at runtime; `fetch` is DEV-TIME** (Cerid has no HTTP
    client — the SuiteSparse download stays the build-time gated `file(DOWNLOAD)`; runtime CLI works
    on the cooked/on-disk corpus). Avoids signing the numerical slice up for an HTTP/TLS dependency.
  - **Corpus delivery:** cook each matrix to a `.crdr`, assemble a small PACK manifest (reuse
    `manifest_write`), `mount_manifest`, `load_sync` — **no new `ResourceManager` API.**
  - **v4-corpus DoD:** cook+load round-trip byte-exact (`SparseMatrix<T,Csr>`→cook→load→`build_csr<T>`
    == original; topology_hash matches) + `cook/load/info` CLI works on a real SuiteSparse `.mtx` +
    smoke `smoke_hesap_matrix_resource` exercises `ResourceManager::mount_manifest + load_sync<
    SparseMatrixResource>`. (The "drive a solver end-to-end from CLI" guardrail lands in **v4a**:
    smoke loads a corpus matrix via `hesap.matrix.load` then runs `hesap.iterative.cg`.)
  - ADR-0084 records the bridge-module dependency direction + the cooked-CSR chunk format + the
    pinned 40-byte header + append-only variant enum + the authoring-text/runtime-binary honouring.

**`crd-hesap-iterative`:**
- **v4a** — Krylov framework (`LinearOp` consumer, `IterativeResult`, deterministic stopping
  criteria, the `blas1`-routed Krylov vector-helper layer) + **CG / PCG** + **Jacobi /
  block-Jacobi / SSOR**. Establishes the architecture + SPD path + the first determinism gate
  + first Eigen head-to-head. (real+complex/Hermitian)
- **v4b** — **FGMRES(m)** (flexible) + restart + Arnoldi + Givens Hessenberg least-squares
  (consumes dense). 
- **v4c** — **BiCGSTAB** + **MINRES** + **SYMMLQ** + breakdown detection.
- **v4d** — **LSQR** + **LSMR** + **QMR** + **IDR(s)** (LSQR/LSMR consume the Golub-Kahan
  bidiag substrate from v3b).
- **v4e** — **GCRO-DR** + **M-CG / M-GMRES Krylov subspace recycling** (deflation-subspace
  reuse across a solve sequence — the algorithmic frontier).
- **v4f** — **Block-Krylov / multi-RHS** (block-CG, block-GMRES, block-BiCGSTAB) +
  **inner-Krylov-as-preconditioner** (the nested-solver-as-`LinearOp` driver; FGMRES already
  supports the variable-preconditioner hook from v4b).

**`crd-hesap-preconditioners`:**
- **v4g** — **IC(0) + ILU(0)** (level-0 incomplete Cholesky/LU; consume v2 etree/symbolic).
- **v4h** — **ILU(p)** (level-of-fill) + **ILUT** (dual-threshold, Saad) + the leveled/
  threshold dropping machinery.
- **v4i** — **SPAI** (sparse approx inverse, Frobenius-min; naturally parallel) +
  **polynomial / Chebyshev** (matrix-free, determinism/GPU-friendly) + **additive +
  restricted Schwarz** (domain decomposition).
- **v4j** — **Multilevel ILU (ILUPACK-class)** — split: **j-1** scaffold (MC64) ✅, **j-2**
  inverse-based pivoting + recursion (2a/2b ✅, **2c** complex+CLI+adjoint ⬜), **j-3** genuine
  multilevel for COMPLETENESS (improved ICE + M-version Schur — NOT the convection crush; that's
  v4k-b AGMG, proven in v4k-a-2). (Research-grade; its own mini-cluster.)

**`crd-hesap-amg` — the AMG family (one cluster, value-ordered):**
- **v4k-a** ✅ — **SA-AMG** (smoothed aggregation, Vaněk 1996): strength + aggregation + smoothed
  prolongator + Galerkin `Pᵀ A P` + V/W-cycle + smoother enum. Crushes ILUPACK on diffusion.
- **v4k-b** ⬜ — **AGMG K-cycle** (Notay 2010): the robust convection crush (Krylov-accelerated
  cycle stabilizes the W-cycle wall found in v4k-a-2). **Next; highest value.**
- **v4k-c** ⬜ — polish + CLI (closes the AMG CLI-per-op debt) + complex + F-cycle.
- **v4k-d** ⬜ — **classical Ruge-Stüben** (C/F splitting + direct interpolation) for the textbook
  isotropic case + breadth.
- **v4k-e** ⬜ — **bootstrap / adaptive αSA AMG** for hard, anisotropic, or unknown-near-nullspace
  problems. Head-to-head vs AMGCL/PETSc-GAMG/hypre-BoomerAMG.

**Close:**
- **v4z** — complex-completeness audit (every solver/preconditioner/AMG has its complex/
  Hermitian variant) + CLI-completeness audit (every op a command) + **ADR-0065 §lock**
  (D(iter)-N determinism pins — deterministic reductions, fixed coarsening tie-breaks,
  breakdown handling, recycling subspace selection) + vs-reference rollup + 18-config sweep.

### Honest sizing

At the v3 elite bar (faithful ports + ADR locks + bench-vs-reference + 18-config gates +
per-pin determinism), v4 is **~9000 LOC / ~320 tests / multi-month** — the multilevel-ILU
(v4j) and AMG (v4k–m) are each mini-clusters. Per `feedback_hesap_clean_structure_over_calendar`
this is stated honestly; calendar is not the constraint, completeness + the three pillars are.

### v4a framework — concrete skeleton (planned 2026-05-25, advisor-vetted)

`crd-hesap-iterative` files:
- `iterative_result.hpp` — `IterativeResult{iterations, final_residual_norm, converged,
  StopReason}` (`StopReason ∈ {Converged, MaxIter, Breakdown, Stagnation}`) +
  `IterativeOptions{rel_tol, abs_tol, max_iter, record_residuals}`. Residual **trajectory** is
  opt-in (`record_residuals` → allocator-owned `Array<RealType<T>>`); scalar always present. The
  trajectory is what the determinism gate diffs across thread counts.
- `krylov_workspace.hpp` — **`KrylovWorkspace<T>`**: pre-allocated vector bank (r, z, p, Ap…),
  allocator-fed, reused across iterations (zero per-iteration alloc). Owns the deterministic
  `kdot`/`knrm2` wrappers routing to the v0b `blas1` KBN-pairwise reductions — **the determinism
  moat** (fixed accumulator shape ⇒ identical across {1,2,4,8,16} threads).
- `stopping.hpp` — deterministic stop criteria (rel/abs residual, max-iter, breakdown, stagnation).
- `cg.hpp` — CG + PCG. PCG takes the preconditioner as a `LinearOp` (`M⁻¹`); CG = identity precond.

`crd-hesap-preconditioners` files (created at v4a): Jacobi / block-Jacobi / SSOR, each a
`LinearOp` exposing `M⁻¹`, built from a concrete matrix (a matrix-free op can't expose its
diagonal/blocks/triangle). **Determinism:** Jacobi + block-Jacobi are parallel AND bit-exact
(disjoint); SSOR ships **sequential** (deterministic by construction) — multicolor-parallel SSOR
is a perf variant reusing the v2 ordering/colouring substrate (coloring determinism spec pinned now).

Design pins (advisor-vetted): preconditioner owns its setup/factor scratch, solver workspace
provides the `z=M⁻¹r` target; no new `LinearOp` virtual at v4a (block-apply for v4f appends AT END);
CLI ≈ 20 (`cg`/`pcg` ×4 + `precond.{jacobi,block_jacobi,ssor}` ×4); benches vs Eigen
`ConjugateGradient` + `DiagonalPreconditioner` on the v4-corpus SPD matrices.

**v4a DoD (the gold-plating guardrail for v4-corpus):** smoke `smoke_hesap_solve_cli` loads a
corpus matrix via `hesap.matrix.load` then runs `hesap.iterative.cg` end-to-end and asserts
convergence — proving the matrix-as-resource path actually drives a solver, not just benches.

### First move

**v4-corpus** then **v4a**. v4-corpus lands the matrix-as-resource pipeline so v4a's CG/PCG +
every vs-reference bench consume the SuiteSparse corpus through `ResourceManager` from day one.
v4a then establishes the framework + SPD path + the determinism gate + the first Eigen head-to-head,
and every other slice hangs off its abstractions. Advisor-vet each slice design before implementing
(project rule).

---

## v1 — Sparse storage + kernels — DETAILED PLAN (planned 2026-05-20)

**Goal:** an elite, multi-threaded, deterministic sparse-matrix substrate that
**beats Eigen's sparse module** on the kernels that matter (spmv, spgemm), with
the pattern/values/analysis-cache architecture as its spine. v1 is **storage +
kernels only** — iterative solvers (v4), reorderings (v2), sparse direct (v5),
sparse eig (v6) consume it later. Keeping that boundary is the first
anti-rabbit-hole rule.

### Why we can beat Eigen (the cross-check)

Eigen's `SparseCore` (studied: `SparseMatrix.h` 4-array storage, `AmbiVector.h`
SPA accumulator, `ConservativeSparseSparseProduct.h` Gustavson spgemm,
`SparseDenseProduct.h`) is **single-threaded, scalar spmv, SPA-based spgemm**.
Three structural openings, confirmed by 2023–2025 SOTA research:
- **spmv:** SELL-C-σ (SPC5 2023) + AVX (Vec8f/Vec4d) + row-balanced parallel
  (ALBUS) over `crd-jobs`. Eigen has no SELL and no parallelism here.
- **spgemm:** parallel hash-accumulator + symbolic/numeric two-phase beats
  SPA/`AmbiVector`; BRMerge (2022) / MAGNUS (2025) / SaSpGEMM as elite
  refinements. Eigen is single-threaded SPA.
- **everything else Eigen lacks:** deterministic parallel reductions
  (ADR-0063), two-layer typed API, allocator discipline, CLI/agent-native
  protocol.
Reference corpus: **SuiteSparse Matrix Collection** `.mtx` fixtures (the v0f
deferred reference fixtures land here, with a real consumer) + our own
generated patterns — never overfit to one corpus.

### Slice template — applied to EVERY v1 sub-slice (the v0-close lessons, made structural)

1. **Cross-config from day 1** ([[feedback_full_sweep_catches_cross_config_simd]]).
   Each slice's iterate-loop builds **win-debug + win-debug-scalar +
   win-debug-sse2 + win-clang-cl + one `linux-gcc-*`** — NOT just MSVC-AVX2. The
   `simd::fma` / scalar-fallback / `-Werror` / Linux-guard breakage class is
   caught per-slice, not at v1-close.
2. **Numeric perf stop-criterion written in the DoD BEFORE coding** (the v0e
   small-N lesson). Each perf slice names a threshold (e.g. "median ≥ 1.3× Eigen
   on the SuiteSparse corpus"); when hit, **ship** — do not chase edge-case
   parity.
3. **4 type variants** (f32/f64/c32/c64) where applicable; complex from day 1
   (ADR-0065 §13 D2), never deferred.
4. **Determinism** (ADR-0063): see the v1a determinism spec — it is pinned up
   front, not discovered in v1d.
5. **CLI commands for EVERY op, registered in the SAME slice that ships the op**
   (ADR-0065 §13 D16 — agent-native, non-negotiable). Not batched at v1-close:
   each sub-slice adds its own `cli::register_module_commands` entries + anchor
   symbol ([[feedback_static_lib_anchor_symbol]]) for every public op × type
   variant it introduces, with typed `CommandSchema` + structured
   `CommandResult` output + MCP descriptor — exactly as v0b/v0c/v0d/v0e-g did
   (28 + 17 + 14 + 8 commands respectively). A sparse op without a CLI command
   is an incomplete slice. v1g only runs the final completeness **audit**, not
   the bulk registration.
6. Allocator propagation (never `default_allocator()` —
   [[feedback_hesap_propagate_allocator]]).

### Sub-slices

| Slice | Topic | Perf stop-criterion (where applicable) | Tests |
| :---: | --- | --- | :---: |
| **v1a** | **Substrate + COO builder + CSR/CSC core + determinism spec.** The pattern/values/analysis-cache trinity: `SparsePattern` (rows/cols/outer_ptr/inner_idx/format/`topology_hash`), `SparseValues` (values + `frame_stamp`), `AnalysisHandle` (cached `topology_hash` + recommended exec format — "the heart" per `sparsematrices.md`). `SparseMatrix<T,Format,Layout>` core; CSR + CSC compressed + uncompressed modes (Eigen 4-array). `TripletBuilder` (COO) → `compress()` with sort + dedup-merge + row-nnz preallocation (PETSc's >50× assembly lever). `'HSPM'` CRDR pin. `SparseId` handles. Structural queries. **Determinism spec pinned here:** row-parallel spmv = disjoint-row ownership; spgemm numeric = canonical column-sorted output before write; no fixed-order-free cross-thread float reductions. **CLI:** `hesap.sparse.build`/`from_triplets`/`to_csr`/`to_csc`/`nnz`/`density`/`structural_query` × {f32,f64,c32,c64}. | — (storage) | ~25 |
| **v1b** | **spmv — SELL-C-σ primary + CSR fallback + `SparseLinearOp` + transpose-spmv.** SELL-C-σ (slice height = SIMD width, σ sort window) as primary; CSR-spmv irregular fallback. AVX2 `Vec8f`/`Vec4d` + scalar + row-balanced parallel (disjoint slices → deterministic). `y=αAx+βy`, `y=αAᵀx+βy` (complex: conj/non-conj). `SparseLinearOp<T>` (first sparse consumer of v0a `LinearOp<T>`). CSR→SELL convert. **CLI:** `hesap.sparse.spmv`/`spmv_transpose` × {f32,f64,c32,c64}. | **≥ Eigen-ST single-thread AND ≥ 2.5× parallel; ≥ 60% STREAM-triad bandwidth bound.** Ship when hit. | ~25 |
| **v1c** | **Element-wise + structural + format-conversion graph.** A±B (matched + symbolic-union patterns), αA, A.*B (Hadamard), sparse transpose (CSR↔CSC), diagonal extract/set/scale, triangular extract. Conversion hub = CSR: COO↔CSR↔CSC↔BSR↔ELL↔SELL↔DIA. Submatrix/row/col slice views. **CLI:** `hesap.sparse.add`/`sub`/`scale`/`hadamard`/`transpose`/`convert`/`diag`/`triu`/`tril` × {f32,f64,c32,c64}. | — | ~20 |
| **v1d** | **spgemm core — hash accumulator, parallel, f32/f64.** Two-phase: symbolic (row-nnz bound via hash-set) + numeric (per-row private hash accumulator → column-sorted write = deterministic). Gustavson baseline → parallel hash. C=A·B + C=A·Aᵀ (normal equations). **f32/f64 only — complex deferred to v1g** (advisor split: spgemm is the v1 rabbit-hole risk). **CLI:** `hesap.sparse.spgemm`/`spgemm_ata` × {f32,f64} (c32/c64 land in v1g with the complex impl). | **median ≥ 1.3× Eigen (AmbiVector) on SuiteSparse corpus — ship at threshold, do NOT chase extreme-sparsity edge cases.** | ~20 |
| **v1e** | **spmm (sparse×dense) + SDDMM.** C(dense)=A(sparse)·B(dense) — multiple RHS for block-Krylov / batched solve; row-wise, dense-RHS blocked, parallel. SDDMM (sampled dense·dense→sparse mask, GNN/ML). 4 variants. **CLI:** `hesap.sparse.spmm`/`sddmm` × {f32,f64,c32,c64}. | **≥ 2× Eigen on multi-RHS (Eigen loops spmv).** | ~15 |
| **v1f** ✅ 2026-05-21 | **Block + structured formats: BSR + ELL + DIA.** BSR (b×b dense blocks — **dedicated small-block GEMV, D(sparse)-6, NOT the v0d microkernel**; FEM/physics). ELL (regular, interop/base — SELL stays the irregular perf path). DIA (banded/structured-grid). Each: storage + spmv (serial+parallel) + CSR↔X convert. (SELL already shipped in v1b.) **36 CLI** (`to_*`/`from_*`/`*_spmv` × {bsr,ell,dia} × 4). | **CRUSHED: BSR 3.4–6.7× Eigen-CSR + 3.2–6.5× our CSR; ELL 4.9–5.2×; DIA 4.8–5.9× — all on native patterns.** | ~36 |
| **v1g** | **Matrix-Market I/O + spgemm perf-attack/cap-lift + v1-close.** (spgemm c32/c64 already shipped in v1d.) `.mtx` reader/writer engine-side + SuiteSparse corpus [v1g-1 ✅]. spgemm adversarial stress (won all 2.5–5.5×, no BRMerge/MAGNUS refinement needed) + **hash-accumulator path lifting the 4M-col dense-SPA ceiling** + `crd::containers::sort` hidden-malloc fix (in-place introsort + caller-scratch `stable_sort`) [v1g-2 ✅]. **CLI:** `mtx_read`/`mtx_write` ×4 + **CLI completeness audit** (grep-diff every op×variant vs registered commands) [v1g-3]. **v1-close:** sandbox-showcase TU split (kill LTCG ICE) + ADR-0065 §15 amendment (lock D1–D8) + 18-config full sweep [v1g-3]. | each op×4 has a CLI command; 18-config sweep PASS | ~15 |

**Dropped from v1 (2026-05-20 scope review, user-confirmed):** CSR5 + Merge-CSR
(GPU-shaped formats → **v17 GPU slice**, built with their real GPU consumer);
JDS + SkyLine (legacy, "rare modern use" → **filed follow-on**, built only on
consumer pull). HYB (ELL+COO hybrid) folds into v1f if ELL's padding proves
wasteful on a real corpus, else also a follow-on. Rationale: shipping CPU
storage of GPU/legacy formats with no consumer is the speculative pattern v0f
learned to defer ([[feedback_ship_at_consumer_template_from_day_one]]).

**Ordering note:** v1d (spgemm core) lands mid-cluster, but its complex variants
+ perf-attack (v1g) are deliberately sequenced LAST, after v1f gives real
block-format consumer data — so the spgemm perf-attack optimizes against
representative workloads, not a guess.

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

## v3 — SVD + dense eigenvalue — MAX-AMBITION DETAILED PLAN (planned 2026-05-21)

**Ambition (user directive 2026-05-21, memory `project_hesap_v3_max_ambition_gate`):**
the **full LAPACK-elite tier**. We beat Eigen everywhere and **beat LAPACK on
MRRR / D&C / AED** — "if LAPACK and Eigen can do it, we can do at least how they
did." **MRRR and Aggressive Early Deflation HARD-GATE the v3 cluster close**
(not filed-in-cluster aspirational — they block close). Realistic envelope:
~6–8 KLOC, ~200 tests, **8–12+ weeks** (the hardest numerical code in the whole
module — clustered-eigenvalue orthogonality, dqds bit-stability, AED).

**Method (per sub-slice, non-negotiable):** *deep-research pass FIRST* — read
the Eigen internals AND the LAPACK reference source for the routine, attack each
sub-problem granularly and in small pieces, design carefully, THEN implement.
Reference source lives in `build/_deps/` once `CRD_BUILD_HESAP_VS_REFERENCE=ON`
(Eigen headers + OpenBLAS's f2c'd LAPACK `SRC/*.c` + the netlib `*.f`).

### Goal

- **Symmetric eigensolver** — Householder tridiagonalization + implicit QL/QR
  (Wilkinson shift) + **divide-and-conquer (Cuppen)** + **MRRR** (`stemr`).
- **SVD** — Householder bidiagonalization + Demmel-Kahan implicit-zero-shift QR
  + **divide-and-conquer bidiagonal SVD** + **randomized SVD (Halko 2011)**.
- **Non-symmetric eigensolver** — balancing + Hessenberg reduction + Francis
  double-shift QR → real Schur with **Aggressive Early Deflation** + eigenvectors.
- **Least squares** — `lstsq` (QR / SVD) + `pinv` (Moore-Penrose) + **NNLS**
  (Lawson-Hanson) + **TLS** (via SVD).
- **Complex variants** folded into each real slice (Hermitian → v3a, complex
  Schur → v3d), not batched at close.

### Shared substrate (built FIRST, in v3a-1)

`detail::householder_block_reduction` — the blocked-WY Householder reduction
that `dsytrd` (→ symmetric tridiagonal), `dgebrd` (→ bidiagonal), and `dgehrd`
(→ upper Hessenberg) all share, with the trailing update routed through v0d
`gemm_parallel`. Lifting it once keeps the three reductions bit-identical by
construction and saves ~30% LOC (the role v0a played for the dense catalog).

### Public surface

```cpp
namespace crd::hesap::dense
{
    // --- SVD ---
    template <typename T> struct SVD { Matrix<T> U; Vector<T> S; Matrix<T> V; };
    template <typename T> [[nodiscard]] SVD<T> svd       (IAllocator*, const Matrix<T>& A);  // Golub-Reinsch / D&C dispatch
    template <typename T> [[nodiscard]] SVD<T> rsvd      (IAllocator*, const Matrix<T>& A, usize rank, usize oversample = 10, usize power_iters = 2);  // randomized (Halko 2011)

    // --- symmetric eigenvalue ---
    template <typename T> struct EigSym { Vector<T> values; Matrix<T> vectors; };  // ascending values
    template <typename T> [[nodiscard]] EigSym<T> eig_sym (IAllocator*, const Symmetric<T>& A);  // QL / D&C / MRRR dispatch

    // --- non-symmetric eigenvalue ---
    template <typename T> struct EigNon { Vector<Complex<T>> values; Matrix<Complex<T>> vectors; };
    template <typename T> [[nodiscard]] EigNon<T> eig     (IAllocator*, const Matrix<T>& A);  // balance + Hessenberg + Schur(AED) + vectors

    // --- least squares family ---
    template <typename T> [[nodiscard]] Matrix<T> pinv    (IAllocator*, const Matrix<T>& A, T rcond = T(0));  // Moore-Penrose via SVD
    template <typename T> [[nodiscard]] Vector<T> lstsq   (IAllocator*, const Matrix<T>& A, const Vector<T>& b);  // QR full-rank / SVD min-norm
    template <typename T> [[nodiscard]] Vector<T> nnls    (IAllocator*, const Matrix<T>& A, const Vector<T>& b);  // Lawson-Hanson active-set
    template <typename T> [[nodiscard]] Vector<T> tls     (IAllocator*, const Matrix<T>& A, const Vector<T>& b);  // total least squares via SVD
}
```

All factories `[[nodiscard]]`, take `IAllocator*` from the input matrix (no
`default_allocator()` in library code — memory `feedback_hesap_propagate_allocator`),
f32 + f64 + complex variants, CLI per op.

### Determinism pins to draft up front (D(dense-eig)-N → ADR-0065 §17)

- **D&C parallel recursion bit-identical across worker counts** — deterministic
  merge order on the recursion tree; secular root-finder (`dlaed4`) uses
  safeguarded **fixed-iteration** bisection+Newton with deterministic bracket
  selection (no RNG-dependent / convergence-dependent early-exit).
- **QR iteration** uses the fixed Wilkinson-shift formula + a capped max-iter.
- **MRRR RRR shift selection deterministic** (pin the tie-break rule); commit to
  the **Gram-Schmidt re-orthogonalization fallback** for clustered eigenvalues
  (LAPACK `dlarrv` does this — don't hope clusters never occur).
- **Two-rounded reductions** per ADR-0063 throughout (no order-free cross-thread
  float reductions).

### Sub-slice ledger (see the summary table for LOC/test/gate per leaf)

- **v3a** symmetric eig — **✅ CLOSED 2026-05-23.** v3a-1 (substrate + **blocked** tridiag +
  QL/QR, **real f32/f64**) → **v3a-1b/2.5 (complex Hermitian `zhetrd`)** → v3a-2 (D&C) →
  **v3a-3 (MRRR)**. Beats Eigen + LAPACK (full eig 1.4–1.95× Eigen / 2.1–3.7× LAPACK).
- **v3b** SVD — **✅ CLOSED 2026-05-23.** v3b-1 (bidiag + blocked dgebrd/dorgbr + Demmel-Kahan
  dbdsqr) + **v3b-2 (Gu-Eisenstat D&C — beats Eigen BDCSVD 1.6–3.2× + LAPACK dgesdd 1.4–4.6×)**
  + v3b-3 (randomized rsvd/rsyev, Halko) + v3b-1c (complex SVD, reuses the real D&C crush).
- **v3c** least-squares family — lstsq + pinv + NNLS + TLS.
- **v3d** non-sym eig — **v3d-1 real Schur** = **v3d-1a** balance + Hessenberg (✅
  beats Eigen+LAPACK) → **v3d-1b** Francis double-shift QR + `dlanv2` → real Schur
  (✅) → **v3d-1c** AED (the HARD-GATE) = **1c-1** `dlasy2`+`dlaexc`+`dtrexc`
  (Schur reorder) → **1c-2** `dlaqr2/3` AED-deflation window → **1c-3** `dlaqr5`
  multishift + `dlaqr0` driver (3-way bench dlahqr/AED/dhseqr + sweep-count
  crossover ≈ N75) — then **v3d-2** eigenvectors (`dtrevc`) + 3-stage back-transform
  + complex Schur.
- **v3e** CLI audit + vs-reference rollup + ADR-0065 §17 lock + 18-config close.

### v3d-2c-2b — complex AED — DETAILED PLAN (cold-context handoff, written 2026-05-24)

> **This is the next slice.** Written as a self-contained plan so a fresh session
> can execute without re-deriving context. Read this section + the v3d-2c rows in
> the table above + the 2026-05-24 session log. **Call `advisor` on the per-piece
> plan before implementing each sub-subslice** (project rule).

**Where we are.** v3d-2c-1 (complex Hessenberg `zgehd2` + unitary Q) and v3d-2c-2
(complex `balance` + single-shift Schur `complex_schur`/`zlahqr`) are shipped and
4-config-DoD green. `complex_schur` produces an upper-triangular Schur form
`h_in = Z·T·Zᴴ` (eigenvalues on the diagonal, no 2×2 blocks) and **beats Eigen
`ComplexSchur` 1.16×/1.10× + crushes `zhseqr` 6.58×/1.45× at n≤128**. The known
gap: single-shift `complex_schur`'s QR **sweep count grows super-linearly with n**
(n=256 ≈ 117 ms, ~9.2× of n=128) — that is the regime AED fixes.

**Goal.** Complex Aggressive Early Deflation (LAPACK `zlaqr0`-class) — the
production complex Schur. Converges a whole trailing window per inner Schur
instead of one eigenvalue per O(n) sweep, collapsing the sweep count at scale.
This is the complex analog of the real **v3d-1c** HARD-GATE (read its table rows
+ the `schur_aed`/`aed_deflate`/`reorder_schur`/`dlaqr5_sweep` code in
`eig_nonsym.cpp` as the structural template — the complex version mirrors it).

**GATE FRAMING — important, differs from the real path.** The real AED beat
LAPACK `dhseqr` at scale (real refs run fine). **For complex, BOTH references AV
at n≥256** — Eigen `ComplexSchur` AND LAPACK `zhseqr` (confirmed; see memory
`reference_eigen_complex_hessenberg_av_at_large_n`). So there is **no external
reference to "crush" at the scale AED matters.** Gate complex AED against **our
own single-shift `complex_schur` baseline**: (a) AED-Schur == single-shift
spectrum + recon `‖h_in−Z·T·Zᴴ‖ < 1e-7`, and (b) **AED beats single-shift
`complex_schur` at n≥256** (measure the sweep-count reduction, like the real
`schur_aed` measured AED vs pure-`dlahqr`). Cap any external-ref bench at n≤128.
This is still the right *elite/complete* choice (single-shift-only would be a
known-incomplete production Schur) — just measured against our own baseline.

**The big simplification vs real v3d-1c: NO 2×2 blocks.** Complex eigenvalues sit
directly on the triangular diagonal, so there is **no `dlanv2`, no `dlasy2`
(2×2/4×4 Sylvester), no `dlaexc` 2×2-block swap.** Every reorder/deflation step is
a 1×1 operation = a single complex Givens. This collapses the real 1c-1 (~870
LOC) to ~100 LOC.

**Shipped substrate to REUSE (do not re-derive):**
- `complex_schur` (`zlahqr`) — the window Schur inside AED + the NMIN-crossover
  fallback for small blocks.
- `complex_givens` (`zlartg`, `detail/householder.hpp`) — every reorder/sweep
  rotation.
- `crd::hesap::sqrt(Complex)` + `make_householder_complex` (`zlarfg`) — spike
  reflection + re-Hessenbergization.
- `hessenberg<Complex<T>>` (`zgehd2`) — re-Hessenbergize the leading window block.
- `gemm` (complex) — global H/Z slab updates (the BLAS-3 arithmetic-intensity
  lever, same accumulate-into-U → gemm pattern the real `aed_deflate`/`dlaqr5`
  use via `slab_left_t`/`slab_right`).
- The fused complex SIMD (`detail/dot_simd_complex.hpp`) for any new inner
  kernels — and heed memory `feedback_complex_split_simd_must_be_wide_unrolled`
  (8-wide, FMA-port ILP) if you write one.

**Subdivision (3 sub-subslices, each its own focused session, each advisor-vetted
+ gateable):**

- **v3d-2c-2b-1 — complex Schur reorder (`ztrexc`). ✅ CLOSED 2026-05-24.**
  `reorder_complex_schur(T, Z, ifst, ilst)`: move the diagonal eigenvalue at
  `ifst` to `ilst` by a sequence of **adjacent 1×1 swaps**. Each swap of adjacent
  diagonal entries `t(p,p)`, `t(p+1,p+1)` of an upper-triangular T is a single
  complex Givens — faithful `zlartg(t(p,p+1), t(p+1,p+1)−t(p,p))` (overflow-safe;
  reuses `detail::complex_givens`) applied as a unitary similarity `G·T·Gᴴ` over
  the full block region (left rows over `[p,n-1]`, right cols over `[0,p+1]`) then
  the `(p+1,p)` subdiagonal forced to 0. Z updated by `Z·Gᴴ`. (No `dlasy2`/
  `dlaexc` — the no-2×2 simplification.) **Backward move uses a `here` cursor, not
  `for p≥ilst` — the `usize` underflow at `ilst==0` is a real bug class (advisor
  flag).** **Gate MET:** reorder a complex Schur (T,Z) of a random complex
  Hessenberg → `Z'·T'·Z'ᴴ` = SAME H `<1e-9` (c64; `<1e-3` c32), Z' unitary, T'
  upper-triangular, chosen eigenvalue moved `ifst→ilst` (incl. forward, backward,
  and `ilst==0`). **~95 LOC, 2 cases.** 4-config DoD green (debug/shipping-LTCG/
  asan/tidy; full `[nonsym]` 247 706 assertions / 34 cases; win-tidy fixed 5
  pre-existing violations in the file — 3× local `constexpr n`→`const` per
  `LocalConstexprVariableCase: CamelCase`, 2× usize→double narrowings).

- **v3d-2c-2b-2 — complex AED deflation window (`zlaqr2`/`zlaqr3`). ✅ CLOSED
  2026-05-24.** `complex_aed_deflate(...)`: trailing window `[kwtop, kbot]` of
  size `nw` → Schur via `complex_schur`; deflate from the bottom — 1×1 test
  `|s|·|V(1,j)| ≤ max(smlnum, ulp·cabs1(T(j,j)))` (no 2×2 component pair), else
  `reorder_complex_schur` moves it up; eigenvalue restore = the diagonal of T (no
  `dlanv2`). Spike reflected (`make_householder_complex`) + leading block
  re-Hessenbergized (`hessenberg<Complex>` + `form_hessenberg_q<Complex>`) +
  **global H/Z updates via complex `gemm` slabs** (`H := Vᴴ·H·V`). Returns
  `{ns, nd}`. **Three complex divergences from the real `aed_deflate` that the
  faithful port required** (all `zlaqr2`-faithful): (i) LEFT spike apply uses
  `conj(tau)`, RIGHT uses `tau` (new `apply_hc_left`/`apply_hc_right` conjugating
  helpers — the real `apply_h_*` don't conjugate); (ii) the left slab is `Vᴴ·H`
  not `Vᵀ·H` (new `slab_left_h` with `Trans::ConjTranspose`; `slab_right` `C·V`
  reused as-is); (iii) the coupling restore is `H(kwtop,kwtop−1) = s·conj(V(1,1))`
  (the real path's `V(1,1)` is real so the conj is invisible there). **Gate MET:**
  decoupled trailing window (coupling zeroed ⇒ `s=0`) deflates fully (`nd==nw`,
  `ns==0`); general window (`nw=8`, `kwtop=12>ktop=0` ⇒ coupling exercised) keeps
  `z·H·zᴴ == H0` over the WHOLE matrix `<1e-8` (catches a coupling-conj error)
  AND spectrum invariant `eig(H)==eig(H0) <1e-7` (catches a similarity sign error
  the in-window recon misses); `nd+ns==nw`; c32 `<1e-3`. **~330 LOC, 3 cases.**
  4-config DoD green (full `[nonsym]` 247 715 assertions / 37 cases; `[aed]`
  43 806 assertions ASan-clean).

- **v3d-2c-2b-3 — complex AED driver (`zlaqr0`) + multishift sweep (`zlaqr5`).
  ✅ CLOSED 2026-05-24.** `complex_schur_aed(...)`: driver loop = split active
  block → `complex_aed_deflate` → nibble → complex small-bulge multishift QR sweep
  (`complex_dlaqr5_sweep` = `zlaqr5`: NBMPS=ns/2 bulges via `complex_dlaqr1` +
  `make_householder_complex` 3-vectors, KACC22=1 accumulate-into-U + BLAS-3 `gemm`
  slab far-updates with `slab_left_h` = Uᴴ horizontal / `slab_right` = U vertical +
  Z) using undeflated AED eigenvalues as shifts → **NMIN=150 crossover** to
  single-shift `complex_schur`. Wired as the production complex Schur (2c-3's
  `eig` calls this). **The `zlaqr5` conj rule (ported VERBATIM from `zlaqr5.f`,
  not reconstructed): RIGHT/U-accum `T={tau, tau·conj(v2), tau·conj(v3)}` +
  plain-v gather; LEFT `T={conj(tau), conj(tau)·v2, conj(tau)·v3}` + conj-v
  gather; similarity is Hᴴ·A·H.** **Gate MET:** AED-Schur == single-shift spectrum
  + recon `<1e-9` (~1e-13) at n=40/160/260; **beats single-shift `complex_schur`
  (our own baseline — refs AV at n≥256): 1.10× (n=256) / 1.12× (400) / 2.14×
  (512), the win WIDENS with N; sweeps collapse to 3/4/4** (vs single-shift's
  per-eigenvalue O(n) sweeps). **D(non-sym)-7** (window `nw=min(nh,max(2,nh/3))`),
  **D(non-sym)-8** (undeflated AED eigenvalues as consecutive shifts). NMIN
  **measured** (crossover ~200: n=128 loses 0.90×, n≥256 wins) — NOT the real
  path's 200. **~700 LOC, 6 cases** (incl. implicit-Q sweep isolation + random-
  matrix repros). 4-config DoD green.
  - **THE BUG (found via systematic isolation — a model debugging trail):** first
    cut passed all unit tests (smooth sin/cos matrices, recon ~1e-13) but the
    bench's **random** matrices gave recon ~1 (single-shift was clean on them, so
    not the bench). Isolation: implicit-Q proved the multishift sweep is an exact
    similarity for ALL block positions + shift counts (~1e-15); `complex_schur`
    crossover-only (NMIN=∞) was clean; per-step running-recon pinned the jump to
    **`complex_aed_deflate` deflates with `nd>0`** (the spike-reflection path,
    which only runs on *partial* deflation — never hit by the small-window unit
    tests). Root cause: `zlaqr2.f` **conjugates the spike row** (`WORK(I) =
    DCONJG(V(1,I))`) before `zlarfg`; my port copied it plain. The advisor flagged
    this exact conjugation in the 2b-2 review and I'd wrongly concluded "plain
    copy". One-line fix (`work[k] = conj(v.at(0,k))`). Lesson: **test eigensolvers
    on generic/random matrices, not just smooth analytic ones — smooth spectra
    deflate without hitting the spike path.**

**Determinism pins (continue the numbering — real/2b/2c-2 used D(non-sym)-1..6):**
- **D(non-sym)-7** — complex AED window-size formula (`nw = min(nh, max(2, nh/3))`
  or whatever measurement settles, mirror real D(non-sym)-2).
- **D(non-sym)-8** — complex AED shift order (undeflated AED eigenvalues as shifts,
  consecutive; mirror real D(non-sym)-3).
- Pin these + verify the v3d-2c-2 **D(non-sym)-6** `zlahqr` exceptional constants
  character-for-character against `zlahqr.f` at the v3e §17 lock (currently
  reasoned-from-memory + recon-gated).

**Files:** `eig_nonsym.{hpp,cpp}` (declarations + impl), `test_eig_nonsym.cpp`
(gates), `bench_hesap_eig_nonsym_vs_reference.cpp` (AED vs single-shift `complex_schur`
at n≥256; external refs capped at n≤128). **Total ~1000 LOC + ~7 cases across the
3 sub-subslices.** After 2c-2b: **2c-3** (`ztrevc` + back-transform + public complex
`eig(Matrix<Complex<T>>)` + CLI `eig.nonsym.{c32,c64}`), then v3d close + v3e.

### v3d-2c-3 — complex `ztrevc` + back-transform + public complex `eig` + CLI ✅ CLOSED 2026-05-24

- **`ztrevc_right<T>`** (complex right eigenvectors of upper-triangular Schur T):
  per-column triangular back-solve with the `smin` near-defective floor + inline
  overflow scaling (the `cnorm`/`bignum` guard the real `dtrevc_right` uses — no
  `zlatrs`). All-scalar (no 2×2/`dlaln2`). Back-solve verbatim-faithful to
  `ztrevc.f` (fetched). NO normalization (deferred to `eig`).
- **`eig<T>` complex branch** (`if constexpr (is_complex_v<T>)` → `eig_complex_impl`;
  real stays `eig_real_impl`, dispatched by a thin `eig`): balance → hessenberg +
  form unitary Q → `complex_schur_aed` → `ztrevc_right` → `V = D⁻¹P·Q·Z·V_schur`
  (two `gemm`s + complex `gebak_right`) → normalize once per **D(non-sym)-4**
  (‖·‖₂=1, largest-**modulus** component phase-real-positive). `gebak_right`
  generalized to `<V, S>` (complex vectors, real scale) — real callsite deduces.
- **CLI** `hesap.dense.eig.nonsym.{c32,c64}` — interleaved `[re,im]` n×n in,
  interleaved `[re,im]` eigenvalues out.
- **Gate MET:** per-eigenpair residual `‖A·vₖ−λₖ·vₖ‖₁/‖vₖ‖₁` ~**1e-13** on random
  c64 (n=20/60) + a **non-triangular near-defective** matrix (duplicated 2.0
  eigenvalue via a Givens similarity → exercises the `smin` floor) + c32; ‖v‖₂=1;
  largest-modulus component real-positive (asserted). **Bench (c64): BEATS Eigen
  `ComplexEigenSolver` 1.13× (n=64) / 1.32× (n=128), crushes `zgeev` 4.79×/2.80×;
  refs AV at n≥256 (confirmed empirically — capped n≤128), Cerid alone resid
  3.2e-13 @ 256.** 4-config DoD green. **Test-bug caught:** the phase assertion
  first used `cabs1` to pick the pivot component while `eig` uses **modulus**
  (`re²+im²`, the LAPACK convention) — different component; fixed the test, not
  `eig`. **Follow-on filed `v3d-eig-fully-reducible-input`:** a fully-reducible
  (e.g. triangular) input makes `balance` isolate everything → empty active block
  → `ihi = l−1` underflows (usize) → `hessenberg` asserts; affects real `eig` too;
  narrow edge case, deferred per `feedback_crush_mandate_bounded_by_importance`.
  **🎉 v3d-2c (complex non-sym eig) CLOSED.** NEXT = v3d close + v3e §17 lock
  (D(non-sym)-1..8; verify zlahqr/zlaqr5 exceptional constants vs the .f sources).

### v3a-1 locked design (advisor-vetted + user-approved 2026-05-21)

**Scope:** real f32/f64 symmetric eigensolver, **blocked from the start**.
Complex Hermitian is **v3a-1b** (different algorithm-class — `zhetrd` real-tridiagonal
+ phase back-transform); v3a-1 interfaces are designed complex-aware (`tau` carries
`T`; `D`/`E` are `RealType<T>`) so v3a-1b drops in. Files: `detail/householder.hpp`
(shared reflector substrate) + `eig_sym.hpp`/`.cpp` + `cli_register_eig.cpp` +
`tests/hesap-dense/test_eig_sym.cpp` + `bench_hesap_eig_vs_reference.cpp`.

**Algorithm:** blocked `dsytrd` (`dlatrd` panel: per-column `dlarfg` + `symv` + the
4-`gemv` deferred-update forming `W`, then ONE `syr2k` trailing update
`A₂₂ -= V·Wᵀ + W·Vᵀ` — the BLAS3 beat-LAPACK lever; last ≤NB block via unblocked
`dsytd2`, also kept as the test oracle) → `steqr` QL/QR implicit-shift (block-split,
Wilkinson shift, Givens bulge-chase, 2×2 via `dlae2`/`dlaev2`, max-iter `30n`,
Givens accumulated into `Z`) → back-transform `V = Q·Z` (`dormtr`) → ascending sort +
sign convention.

**Four faithful-port fixes (advisor):**
1. `make_householder` is **`dlarfg`-faithful incl. the `safmin = min()/eps` rescaling
   loop** (QR's reflector omits it; tridiag's symmetric rank-2 update needs it or
   badly-scaled inputs lose precision / hit `beta=0, tau≠0` → NaN). QR's reflector
   stays a separate internal shortcut. → **D(dense-eig)-5**.
2. Eigenvalue sort = **permutation array applied once** (bit-identical, faster than
   in-place selection-swap). → **D(dense-eig)-3**.
3. Eigenvector sign = **lowest-index largest-magnitude component positive** (matches
   D(ord)-1); LAPACK doesn't pin sign, so the vs-LAPACK gate compares `|V|` /
   `‖A·v−λv‖` / `‖VᵀV−I‖`, never raw `V`. → **D(dense-eig)-4**.
4. Block-split threshold = LAPACK's **`|E(m)| ≤ √(|d_m·d_{m+1}|)·eps`** + the
   bottom-loop `EPS²·|d_m·d_{m+1}| + SAFMIN` (NOT Eigen's additive form; the
   `+SAFMIN` survives zero-eigenvalue tridiagonals). → **D(dense-eig)-2**.

**D(dense-eig)-1** (overarching): production tridiag = the blocked path, pinned;
all reductions deterministic (two-rounded, RNG-free). Allocator from the input
`Symmetric<T>` everywhere; no `default_allocator()` in library code.

**Approved implementation steps — ALL DONE 2026-05-21 (DoD confirming):**
1. ✅ `detail/householder.hpp` — `make_householder` (`dlarfg`-faithful, safmin guard, `hypot2`).
2. ✅ `eig_sym.hpp` — `EigSym<T>` (`Vector<RealType<T>>` values ascending + `Matrix<T>` vectors) +
   `eig_sym` + complex-aware `tridiagonalize`/`steqr` declarations.
3. ✅ `eig_sym.cpp` — blocked `dsytrd` (`dlatrd` panel + **single-pass SIMD symmetric matvec** +
   ONE `gemm_parallel` `P=V·Wᵀ` trailing then `A_lo -= P+Pᵀ`; `dsytd2` last block) → faithful
   `steqr` QL/QR (**column-major SIMD Givens** — ADR-0083 layout escape hatch) → SIMD `form_q`
   back-transform → perm-array sort → pinned sign.
4. ✅ `test_eig_sym.cpp` (9 cases: `make_householder` `H·x=βe₀`, diagonal/2×2/tridiagonal,
   SPD N=16..200 incl. blocked, indefinite, determinism, f32, CLI) — full suite **186 / 68,263**.
5. ✅ `bench_hesap_eig_vs_reference.cpp` — vs Eigen `SelfAdjointEigenSolver` + LAPACK `dsyev`
   (QL/QR, same algorithm class). **GATE MET — beats both at every N=64..1024:** C/Eigen
   1.20/1.44/1.48/1.24/1.26×, C/LAPACK 2.03/3.49/2.96/2.13/1.46×; accuracy ~1e-12, residual ~1e-14.
   (9.8× speedup at N=1024 from the layout fix + SIMD `form_q` + SIMD panel `symv`.)
6. ✅ `cli_register_eig.cpp` — `hesap.dense.eig.sym.{f32,f64}` + anchor.
7. ⏳ 5-config DoD (`-IncludeRelease`) running. (Fixed: shipping/release C4189 `steqr_info` unused
   when asserts-off → `[[maybe_unused]]`; tidy `misc-unused-alias-decls` on `rot_cols` two-param
   template → fully-qualify; local `nb` `constexpr`→`const`.)

**`v3a-1-perf` — RE-SCOPED 2026-05-22 after a flop recount.** The originally-filed "half-flop
triangular `syr2k`" is a **non-issue**: our trailing `P = V·Wᵀ` (full `m×m`, `m²·kb` mults) +
`A_lo -= P+Pᵀ` is **flop-equivalent** to a triangular `syr2k` (`m²/2` entries × `2kb`), and runs
on the fast parallel `gemm_parallel` rather than our scalar `syr2k`/`syrk` shells — a triangular
kernel would be *slower*. The genuine dominant cost at large N is the **`steqr` QL/QR eigenvector
rotation accumulation (~6n³, serial, fine-grained)** — which is precisely what **v3a-2 (D&C)** and
**v3a-3 (MRRR, O(n²) vectors)** eliminate architecturally. So QL/QR vector-perf is **low-leverage
and largely obsoleted by v3a-2**; do not invest there. `v3a-1-perf` is therefore deferred to a
data trigger: revisit only if the Linux-CI fair-BLAS-LAPACK shootout shows a scale gap that v3a-2
does not already close. (A clean-but-marginal option if ever wanted: blocked-WY parallel `form_q`
via the QR compact-WY machinery — but `form_q` is the smaller `~2/3 n³` term, not the bottleneck.)

### v3a-2 locked design (Cuppen divide-and-conquer; advisor-vetted 2026-05-22)

**Sequencing:** v3a-1b (complex Hermitian) deferred to AFTER v3a-2 (interfaces already
complex-aware). **Reuses** `tridiagonalize` + `steqr` (base case, blocks ≤ SMLSIZ≈25) +
`form_q` (back-transform) + `simd_dot`/`simd_axpy`. Bottom-up, each piece standalone-testable:

- **v3a-2.1 — secular root-finder** (`dlaed4`+`dlaed5`/`dlaed6`): solve `1 + ρ·Σ wⱼ²/(δⱼ−λ)=0`
  for each root in bracket `(δᵢ,δᵢ₊₁)`. **Port the post-1995 Gu-Eisenstat `dlaed4` FAITHFULLY —
  do NOT simplify the bracket-safeguard branches** (the older Bunch-Nielsen-Sorensen interpolation
  can converge outside the bracket; the safeguards prevent it). Any deviation = a numbered Dxxx
  note. Test: roots satisfy secular eqn + interlace δ.
- **v3a-2.2 — deflation + merge** (`dlaed2`+`dlaed3`): deflate negligible/near-equal components
  (Givens). **LÖWNER FORMULA IS LOAD-BEARING** — after `dlaed4` returns roots `λ̂`, DISCARD the
  original `w` and reconstruct `ŵᵢ = sign(wᵢ)·√(∏ⱼ(λ̂ⱼ−δᵢ)/∏_{j≠i}(δⱼ−δᵢ))`, build eigenvectors
  from `ŵ` (else orthogonality dies exactly on clustered inputs — same risk class as v3a-3 MRRR
  GS-fallback). **Eigenvector formation expressed as a `gemm` → `gemm_parallel` from the START**
  (Gu-Eisenstat is BLAS-like; this is what unlocks the parallel beat-LAPACK story — do NOT ship
  scalar Löwner then add gemm; that is the QL/QR perf trap already paid once). **Gate test:
  `‖VᵀV−I‖ ≤ n·eps`** (not a side-check).
- **v3a-2.3 — D&C recursion** (`dlaed0`): cut at `n/2` (rank-1 correction ρ·vvᵀ), recurse,
  merge via 2.2. **Parallelism = FLAT TASK-LIST PER LEVEL** (collect both subproblems at each
  recursion level, `parallel_for` over the list; tree levels sequential — sidesteps the
  frame-arena risk [[feedback_jobs_parallel_for_frame_arena_exhaustion]] and nested-spawn
  complexity). **Join order fixed: parent consumes left subproblem then right, regardless of
  which worker finished first** → bit-identical across worker counts.
- **v3a-2.4 — `eig_sym` D&C dispatch + gate**: D&C for N>threshold (QL/QR small-N).
  Bench vs Eigen QL + LAPACK `dsyevd` (D&C, apples-to-apples). Gate: beat Eigen QL at scale +
  ≥ LAPACK `dsyevd`.
- **v3a-2.5 — complex Hermitian** (folds in former v3a-1b per user directive 2026-05-22):
  `zhetrd`/`zhetd2` reduce a complex Hermitian matrix to a **REAL** symmetric tridiagonal `(D,E)`
  via complex Householder reflectors, accumulating a **phase diagonal** so the off-diagonals come
  out real. The eigenvalues + tridiagonal eigenvectors come from the **SAME real solver** (`steqr`
  now, D&C after 2.3 — the `dlaed*` core is real, unchanged). Complex back-transform
  `V = D_phase · Q · Z` (`zunmtr`-class), where `Q` is the complex reduction matrix and `Z` the
  real tridiagonal eigenvectors. New `eig_herm(IAllocator*, const Hermitian<T>&)` + CLI
  `hesap.dense.eig.herm.{c32,c64}`. **Gate: vs Eigen complex `SelfAdjointEigenSolver`.** Reuses
  the v3a-1 complex-aware interfaces (`tau:T`, `D`/`E`:`RealType<T>`) — no re-plumbing.

  **✅ SHIPPED 2026-05-22.** `eig_herm` in `eig_sym.cpp`: **SIMD complex reduction**
  (`tridiagonalize_hermitian_simd`) — the working Hermitian is carried as two REAL arrays
  `ar`/`ai` (lower triangle) so the zhetd2 Hermitian matvec + rank-2 update are contiguous-row
  real SIMD (`simd_dot`/`simd_axpy`), no scalar complex arithmetic. Reflectors land in `ar`/`ai`;
  the back-transform (`apply_q_zsplit`) reads them directly (no complex materialization of the
  working matrix). Real `dc_base_steqr` (n≤256) / `dc_tridiag_eig` (n>256) on `(D,E)`; complex
  back-transform `V = Q·D_phase·Z` via real-split gemms; phase-normalized eigenvectors (squared-
  magnitude pivot, one sqrt/column). **The SIMD reduction was the decisive perf fix** — it moved
  N=128 from 0.94× → **1.11×** and lifted every larger size. **Bench (c64) vs Eigen
  `SelfAdjointEigenSolver`: N=64 1.00× (tie — floor; Eigen's hand-inlined small-matrix `zhetrd`),
  N=128 1.11×, N=256 1.56×, N=512 1.89×, N=1024 2.22×; beats LAPACK `zheev` everywhere
  (2.5–4.4×); accuracy ~1e-14.** Tests `[herm]` 5 cases/20,117 assertions (incl. `eig.herm` CLI round-
  trip `[[2,1+i],[1-i,3]]`→{1,4}); full hesap-dense suite 206 cases/98,716 assertions green. Dead
  scalar paths removed (`tridiagonalize_hermitian`, `form_q_hermitian`, `cgemm_split`,
  `apply_q_to_z_complex`). CLI `hesap.dense.eig.herm.{c32,c64}` registered (A as interleaved
  `[re,im]`, lower triangle).

**New pins:** **D(dense-eig)-6** — D&C bit-identical across worker counts (fixed left-then-right
merge order; `dlaed4` fixed-iteration safeguarded bisection+Newton, deterministic bracket;
deflation tie-break ascending original index). **D(dense-eig)-7** — cut always at `n/2`; rank-1
vector sign fixed. **+ `dlaed4-port-faithful`** (no safeguard simplification).

### v3a-3 locked design (MRRR — `dstemr`; deep-research pass 2026-05-23)

**The hardest routine in the module.** MRRR (Multiple Relatively Robust Representations,
Dhillon-Parlett 1997-2006) computes the symmetric-tridiagonal eigendecomposition in **O(n²)**
(vs QL/QR O(n³) and D&C O(n³) worst-case) **without Gram-Schmidt in the common case** —
orthogonality of eigenvectors comes from the *relatively robust representation* (a shifted
`LDLᵀ` factorization whose small eigenvalues are determined to high *relative* accuracy) plus a
*twisted factorization* per eigenvector. Reference family (all local under
`build/win-debug/_deps/openblas-src/lapack-netlib/SRC/`, 5,566 lines of Fortran):
`dstemr → dlarre → {dlarra split, dlarrc/dlarrd/dlarrk/dlarrb/dlaneg Sturm-bisection, dlasq1-6 dqds}
→ dlarrv → {dlarrf child-RRRs, dlar1v twisted factorization}`.

**`dstemr` top-level flow** (read 2026-05-23, `dstemr.f:430-710`):
1. **Scale** `(D,E)` into the safe range `[RMIN, RMAX]` (`SCALE = RMIN/TNRM` or `RMAX/TNRM`),
   undo at the end. `TNRM = ‖T‖_max`.
2. `dlarrr` → does `T` warrant the relative-accuracy path? Sets `THRESH = ±EPS`.
3. Form `E2[j] = E[j]²`.
4. **`dlarre`** — split + compute eigenvalues to `RTOL` accuracy + form root RRRs + Gershgorin
   bounds + per-eigenvalue block/shift bookkeeping. **This is the v3a-3.1 eigenvalue engine.**
5. If `JOBZ='V'`: **`dlarrv`** — eigenvectors via the RRR tree + twisted factorizations
   (**v3a-3.2/.3**). Else: shift eigenvalues back by the root-representation shifts.

**v3a-3.1 substrate pinned (the leaf shipped this session — `detail/sturm_count.hpp`):**
- **Tridiagonal Sturm recurrence** (`dlarrk.f:219-233`, `dlarrc.f:183-203`):
  `negcount(x)` = #{eigenvalues `< x`} via `t₀ = d₀−x` then `tᵢ = (dᵢ−x) − e²ᵢ₋₁/tᵢ₋₁`,
  with the `|t| < pivmin ⇒ t = −pivmin` guard (avoids div-by-zero AND fixes the count at an
  exact pivot — **the determinism-critical line**). `t ≤ 0 ⇒ neg++`.
- **Two-pivot interval count** (`dlarrc.f` `JOBT='T'`): one pass carrying `LPIVOT(vl)` and
  `RPIVOT(vu)` → `EIGCNT = RCNT − LCNT` = #{eigenvalues in `(vl, vu]`}.
- **`dlarra` split**: zero any `|E(i)| ≤ tol` (relative form `tol = SPLTOL·√(|dᵢ·dᵢ₊₁|)`),
  emit block boundaries `ISPLIT`. Splits decouple the problem into independent unreduced blocks.
- **Gershgorin global bracket** + `pivmin` (the per-block min safe pivot).
- **`dlarrk` single-eigenvalue bisection** (the end-to-end validator this session): given index
  `IW` and bracket `[GL,GU]`, bisect to `RELTOL` width, return `(W, WERR)`. Iteration cap
  `ITMAX = ⌈log₂((TNORM+pivmin)/pivmin)⌉ + 2` → deterministic.
- **NOT in .1**: `dlaneg` (the `LDLᵀ` *twisted* Sturm count — needed only once RRRs exist) lands
  in v3a-3.2; `dlasq2` dqds (whole-block fast eigenvalues) lands at .1 completion; `dlarrd`/`dlarrb`
  all-eigenvalues block driver completes .1 next session.

**Determinism pins to lock at v3a-3 close (D(dense-eig)-9..12 → ADR-0065 §17):**
- **D(dense-eig)-9 — `pivmin` Sturm guard is exact + fixed.** The `|t|<pivmin ⇒ t=−pivmin`
  substitution makes the Sturm count a deterministic step function of `x`; `pivmin` is derived
  from the block (not RNG / not host-tuned). This is what makes bisection bit-reproducible.
- **D(dense-eig)-10 — RRR shift selection deterministic.** `dlarrf`'s shift = a fixed rule
  (try the cluster endpoints, accept the first that yields a relatively-robust factorization by
  the element-growth test); pin the tie-break, no convergence-dependent branch.
- **D(dense-eig)-11 — cluster tie-break + GS-fallback trigger fixed.** Eigenvalues within the
  relative-gap tolerance `MINRGP` form a cluster processed in fixed ascending order; the
  Gram-Schmidt re-orthogonalization fallback fires on a fixed residual-gap predicate (not "if it
  looks bad"). Commit to the GS fallback — `dlarrv` has it; clustered inputs WILL occur.
- **D(dense-eig)-12 — dqds (`dlasq2`) fixed-iteration termination** (deterministic shift
  strategy + capped sweeps), bit-reproducible across runs.
- **All bisection iteration-capped** (fixed `ITMAX`), all reductions two-rounded per ADR-0063.

**Stress fixtures — pinned per phase, not deferred:** **v3a-3.1 (eigenvalues, shipped)** uses the
**known-spectrum Toeplitz** fixture (`[a,b]` symmetric Toeplitz → `λₖ = a + 2|b|·cos(kπ/(n+1))`,
closed form) + the `eig_sym` D&C/QL oracle on random + reducible tridiagonals. **v3a-3.3
(eigenvector orthogonality) pins UP FRONT** the glued-Wilkinson `W₂₁⁺`-class matrices (tight
clusters by construction) + Demmel-Kahan adversarial tridiagonals (relative-accuracy killers) —
these are *eigenvector-orthogonality* stress tests (`‖VᵀV−I‖`), so they belong with the vector
machinery, not the .1 eigenvalue leaf. The phase rule "build the stress tests BEFORE declaring it
works" is honored per-leaf: .1's gate is the closed-form spectrum, .3's gate is the clustered-
orthogonality corpus.

**Benchmark protocol pinned (D so the .4 hard-gate is falsifiable):** the vs-LAPACK comparison is
`dstegr`/`dstemr` with `JOBZ='V'` on the **identical** scaled `(D,E)` with the **identical**
`RTOL1/RTOL2` tolerances; the gate metrics are `max‖A·vₖ−λₖvₖ‖`, `‖VᵀV−I‖`, and wall-time at
equal accuracy — never raw `V` (LAPACK doesn't pin eigenvector sign; same convention as
D(dense-eig)-4). Eigenvalues-only (.1) gates vs `dstebz`.

**dqds port decisions (locked 2026-05-23, advisor-vetted; the whole-block fast path):**
- **`dlasq2` route for whole-block eigenvalues** = shift the block by a STRICT Gershgorin lower
  bound `σ = gl − fudge·pivmin` (so `T−σI` is positive definite), build the LDLᵀ **qd array**
  (`q_i` = pivots, `e_i` = `lld_i` = `e[i−1]²/q_{i−1}`), run `dlasq2`, add `σ` back. **Pivot
  guard (precondition):** if any `q_i ≤ 0` during the LDLᵀ build the shift was unsafe (Gershgorin
  can underestimate for ill-conditioned inputs) → fall through to the v3a-3.1 bisection driver.
  Never recover dqds from a degenerate qd.
- **D(dense-eig)-MRRR-Z1base** — the `Z` qd workspace is accessed **1-based** via a thin
  pointer-minus-one wrapper (`Z1`), so the `4*N0+PP−3` ping-pong index arithmetic ports
  **line-for-line** from `dlasq2/3/4/5/6.f`. Hand-translating those indices to 0-based is the
  classic dqds-port failure mode; the 1-based mirror is what every correct C port (CLAPACK, the
  f2c'd OpenBLAS LAPACK in `build/_deps/`) does. Asserts compiled out in release.
- **D(dense-eig)-MRRR-dqds-ieee-only** — only the `IEEE=.TRUE.` branches of `dlasq5`/`dlasq6` are
  ported (ADR-0063 mandates IEEE-754; this is a contract simplification, not a corner-cut). Drops
  the non-IEEE early-`RETURN`-on-negative-`d` paths — the most error-prone surface in dqds.
- **Two-leaf delivery** (advisor — never write all five dlasq routines before the first test):
  **.1-dqds-a** = `dlasq6` + `dlasq5` IEEE kernels + `Z1` layout + qd build + an **unshifted dqd
  driver** (basic bottom deflation), gated standalone vs Toeplitz + `eig_sym` on small PD
  tridiagonals — proves the Z-layout BEFORE shift logic. **.1-dqds-b** = `dlasq4` (shift) +
  `dlasq3` (stepper + 1/2-eigenvalue deflation + reversal) + `dlasq2` (full driver + split) +
  wire as the primary whole-block path + per-slice DoD → closes .1.
- **D(dense-eig)-12 (dqds determinism)** — fully deterministic: `dlasq2`'s `N+1` outer-while cap
  + `dlasq3`'s `NBIG = 100·(n0−i0+1)` per-block cap + the deterministic `dlasq4` shift selection +
  IEEE NaN/Inf handling in `dlasq3`'s `DISNAN(DMIN)` branch (deterministic given IEEE-754).

**Benchmark protocol — operational tolerance pin (so .4's hard-gate is falsifiable):** "beat
LAPACK `stegr` on accuracy at O(n²)" = wall-time compared **at equal accuracy**, with the
**identical** `RTOL1/RTOL2` bisection tolerances, the **identical** strict shift bounds
(`σ = gl − fudge·pivmin`), and the **identical** convergence threshold fed to both paths; gate
metrics `max‖A·vₖ−λₖvₖ‖`, `‖VᵀV−I‖`, wall-time. Two O(n²) impls can differ 3× purely from looser
tolerances — locking the tolerances is what makes the gate real.

**LOC realism (recorded per user directive 2026-05-23):** the phase summary's original ~700 LOC
for v3a-3 was optimistic against a 5,566-line Fortran port surface. Re-estimated to
**~1,600–2,200 LOC** of C++ (idiomatic, sharing `detail/sturm_count.hpp` across the bisection
routines). No calendar pressure (`feedback_hesap_clean_structure_over_calendar`); the LOC range is
the contract so the slice is not pinched at close.

### v3b locked design (SVD — advisor-vetted 2026-05-23)

**Reuse map (what v3a already gives v3b):** (1) **`detail/dqds.hpp::dlasq2`** — its
NATIVE purpose is bidiagonal singular values; the values-only SVD path feeds B's qd array
(`q_i = d_i²`, `e_i = e_i²`) straight to `dlasq2` → squared singular values → sqrt. Free.
(2) **`detail/householder.hpp::make_householder`** (`dlarfg`-faithful) + the blocked-WY
reduction pattern from v3a-1's `dsytrd` → reused by `dgebrd`. (3) `gemm_parallel` for the
trailing updates + back-transforms.

**v3b-1 leaf split (port the foundation first, crush at v3b-2):**
- **v3b-1a — `dgebrd` blocked bidiagonalization. ✅ SHIPPED 2026-05-23.** Reduce A (m×n) to upper
  bidiagonal `B=(d,e)` via left/right Householder, **blocked** (`dlabrd` panel accumulating
  the X/Y update matrices + ONE trailing `gemm` per block — the BLAS-3 lever, same as v3a-1's
  blocked `dsytrd`; the unblocked `dgebd2` per-column logic is the panel's inner kernel + the
  ≤NB tail). Reflectors stored in A; `tauq`/`taup`. **Gate (isolation): `‖A − Q B Pᵀ‖ ≤ n·eps·‖A‖`,
  `‖QᵀQ−I‖`/`‖PᵀP−I‖` orthogonality, B actually bidiagonal.** Real testable foundation.
- **v3b-1b — `dbdsqr` + `svd` driver + bench. ✅ SHIPPED 2026-05-23.** Demmel-Kahan
  implicit-zero-shift QR on the bidiagonal accumulating U/V rotations (high relative
  accuracy); values-only path dispatches to `dlasq2`; back-transform `U = Q·U_b`,
  `V = P·V_b`; descending sort + sign pin. CLI. `detail/bdsqr.hpp` (dlartg-f90 / dlas2 /
  dlasv2 / dlasr-RowMajor / drot / dbdsqr, all faithful ports); `svd` + `svdvals` drivers;
  CLI `hesap.dense.{svd,svdvals}.{f32,f64}`; 4-column bench. ADR-0065 §18 (D(svd)-1..5).
  **Bench (i9 f64): beats Eigen JacobiSVD 3–15×, ties/beats `dgesvd` at small N, LOSES to
  D&C (BDC/`dgesdd`) at scale (C/BDC 0.14 @512) — exactly the serial-baseline gap this leaf
  was meant to MEASURE.** Honest finding: at N≥256 the dominant cost is the **unblocked
  `dgebd2` bidiagonalization** (svdvals @512 = 156 ms vs `dgesvd`-N 46 ms; `dlasq2` is
  O(n²) ~free), NOT dbdsqr — so **v3b-1a-perf (blocked `dlabrd`) is likely the larger lever
  than v3b-1b-perf** for full SVD at scale. ([[project_serial_iterative_qr_loses_to_dc_reduction_is_bottleneck]].)
- **v3b-1a-perf — blocked `dlabrd` bidiagonalization (BLAS-3). ✅ SHIPPED 2026-05-23 (ADR-0065 §19; svdvals beats LAPACK + Eigen at all N).** Promoted from a v3b-1a
  follow-on to a first-class leaf by the v3b-1b bench evidence above: the unblocked `dgebd2`
  reduction is the dominant full-SVD cost at scale (same shape as blocked `dsytrd` carrying
  v3a-1). `dlabrd` panel accumulates the X/Y update matrices + ONE trailing `gemm` per block;
  helps BOTH `svd` and `svdvals`. (Sequencing resolved: 1a-perf → 1b-perf → v3b-2, all ✅ 2026-05-23.)
- **v3b-1b-perf — vector-path crush via blocked `dorgbr`. ✅ CLOSED 2026-05-23 (ADR-0065 §20).**
  Profiling the full-SVD vector path at N=512 split it into form_q+form_pt **49% (335 ms, serial
  scalar ~1.5 GFLOPS)** and `dbdsqr` **49%**. The elite fix for the forming-half is blocked
  `dorgbr` (BLAS-3 compact-WY), NOT a parallel scalar loop — `detail/orgbr.hpp`
  (`orgbr_q`/`orgbr_p`) + shared `detail/block_reflector.hpp`. **form_q+form_pt 335 → 12 ms (28×);
  full SVD 690 → 378 ms; C/`dgesvd` 0.81 → 1.45 — beats LAPACK's dbdsqr-class routine.**
  PARALLEL `dbdsqr` was **SKIPPED** (advisor + user): dorgbr alone won the leaf, and per-`dlasr`
  parallelism (a) exhausts the 1 MB frame arena (~1000–2000 sweeps × 2 `dlasr`) and (b) at best
  TIES BDC/`dgesdd` — parallelizing an O(n³) memory-bound sweep cannot beat an O(n²) D&C. The
  BDC/`dgesdd` crush is v3b-2; `dbdsqr` stays the small-N fallback. (The GK-MRRR fork hits EXACT
  ±σ multiplicity → GS-fallback defeats O(n²) → needs a perfect-shuffle extraction → deferred
  follow-on `v3b-2-svd-via-mrrr`, pursued only if Gu-Eisenstat D&C misses the crush.)
- **v3b-1c — complex SVD** (`zgesvd`-class): complex bidiagonalization (real bidiagonal +
  phase) reusing v3b-1a; reuse the real bidiagonal solver on `(d,e)`.

**v3b-2 — Gu-Eisenstat D&C bidiagonal SVD. ✅ SHIPPED + CRUSHES 2026-05-23 (ADR-0065 §21).**
The fork was decided (advisor + user): **Gu-Eisenstat D&C (`dbdsdc`-class) chosen; the novel
SVD-via-MRRR route DEFERRED** (`v3b-2-svd-via-mrrr`) — forming `J=[[0 Bᵀ][B 0]]` gives EXACT
±σ multiplicity (every σ twice), which defeats MRRR's cluster loop → GS-fallback for every
value → loses the O(n²) win without a bespoke perfect-shuffle extraction = real rabbit-hole
risk. D&C is the references' own algorithm (we beat them on the lever they lack: cores).
Full chain `detail/svd_secular.hpp` (`dlasd5`/`dlasd4`/`dlaed6`) + `detail/svd_dc.hpp`
(`dlasd2` deflation + `dlasd3` secular-solve/vectors + `dlasd1` merge + `dlasdq` base +
`dlasdt` tree + `dlasd0` recursion), wired into `svd()` at n≥64 (smlsiz=25); back-transform
+ dlasd3 assembly on `gemm_parallel_auto`. **Beats EVERY reference at ALL N=128–1024: Eigen
`BDCSVD` 1.59–3.21× (fair gate), LAPACK `dgesdd` 1.37–4.55×, `dgesvd` 4.8–10.5×, `JacobiSVD`
11–28×; recon ~1e-14. @512 full SVD 690→52.9ms (13×), C/BDCSVD 0.17→1.76.** Reconstruction-
gated (`‖B−UΣVᵀ‖<1e-9` multi-level) + debug/asan/shipping/tidy green. Layout bridge
(column-major D&C ↔ tested row-major dbdsqr via `dlasdq` adapter) pinned D(svd)-11; D(svd)-10
(dlasd4 ψ/φ split) / -12 (deflate both U+VT) / -13 (interleaved-Löwner Z) / -14 (col-major
GEMM swap). Filed `v3b-2-parallel-merges` (parallelize independent same-level merges — pure
margin; already winning). **Remaining to formally close: 5-config DoD + commit.**

**Benchmark protocol (FOUR columns — `feedback_always_bench_both_eigen_and_lapack`):** every
SVD bench section reports **Eigen `JacobiSVD`** (O(n³) Jacobi — the easy crush) **AND Eigen
`BDCSVD`** (D&C — the real target) **AND LAPACK `dgesvd`** (dbdsqr — direct algorithmic peer)
**AND LAPACK `dgesdd`** (D&C — the harder target), with ratios printed. Gate metrics:
reconstruction `‖A−UΣVᵀ‖`, `‖UᵀU−I‖`/`‖VᵀV−I‖`, singular values vs reference, wall-time.

### Tests (~200 across leaves)

- SVD reconstruction `A == U·diag(S)·Vᵀ` within `n·eps·|A|`; singular values
  non-negative + descending; randomized-SVD low-rank error bound.
- Symmetric: eigenvalues of a diagonal/known matrix recovered; eigenvectors
  orthonormal (`VᵀV == I`); **D&C and MRRR bit-identical across worker counts**;
  **clustered-eigenvalue orthogonality** (the MRRR stress case).
- Non-symmetric: real Schur `A == Q·T·Qᵀ`; complex-conjugate pairs recovered;
  eigenvector residual `‖A·v − λ·v‖` small; AED reduces iteration count vs naive.
- Pseudo-inverse `A·pinv(A)·A == A`; lstsq minimises `‖Ax − b‖`; NNLS solution
  non-negative + KKT; TLS vs ordinary LS on errors-in-variables data.
- Per-op CLI round-trip bit-equal to the engine call.
- vs-reference accuracy oracle: our values/vectors match LAPACK + Eigen within
  tolerance on every gate matrix.

**Definition of done:** every leaf passes 4/5-config DoD; **MRRR beats LAPACK
`stegr` AND AED is in the non-sym path** (the two hard-gates); the cluster
closes on the 18-config sweep + numerical-accuracy fixtures green.

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
