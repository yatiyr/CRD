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
| **v5** 🚧 IN PROGRESS (Cholesky ✅ · LU active) | **Sparse DIRECT — the COMPLETE elite family** (detailed plan ↓). **STATUS 2026-06-01: v5a Cholesky ✅ COMPLETE — supernodal LLᵀ/LLᴴ CRUSHES CHOLMOD (hood 1.33× / ldoor 1.28× factor + multi-RHS solve, complex, CLI, moat held). v5b LU ✅ ACTIVE/landing — GP-LU serial oracle ✅, supernodal-LU crush vs Eigen ✅, multifrontal-LU + ADAPTIVE-MC64 ✅ (WIP, not committed): near-parity-to-winning vs MUMPS on the CFD targets (af23560 1.04–1.06× win · wang3/ns3Da 0.93–0.98×) with the determinism moat + saddle-point FIXED. v5c QR / v5d LDLᵀ / v5e HSS+BLR / v5f mixed-precision IR = FUTURE.** Originally planned 2026-05-28; multi-month as forecast. Supernodal Cholesky (CHOLMOD-class) + sparse LU (Gilbert-Peierls reference + supernodal/multifrontal **adaptive-MC64 static pivot**) + multifrontal QR (SPQR-class) + multifrontal LDLᵀ (Duff-Reid indefinite) + **rank-structured fronts (FULL HSS STRUMPACK + BLR MUMPS — both ship in v5, user-directed 2026-05-28)** + **mixed-precision iterative refinement** + complex variants + CLI per op. New module `crd-hesap-direct`; consumes v2c `SymbolicFactor` + v0 dense panels + `crd-hesap-sched::DependencyGraph`. **Cross-thread bit-determinism moat held per family** (Cholesky/LDLᵀ/LU-via-static-pivot/QR). *(AMG moved to v4 — iterative-family.)* | ~7200 | ~170 | multi-month |
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
| ↳ **v5c-1** | **Multifrontal QR** — column etree + frontal Householder QR + `extend_add` + R + least-squares solve. | ~550 | ~16 | — |
| ↳ **v5c-2** | Rank-revealing (deterministic in-front column pivot + R-diag detection) + implicit Qᵀ-apply + complex + CLI `hesap.direct.qr.*` + bench vs Eigen `SparseQR` / SPQR. | ~450 | ~14 | — |
| ↳ **v5d** | **Multifrontal LDLᵀ** (symmetric indefinite, static Duff-Reid 1×1/2×2 + deterministic delayed pivots) + complex-symmetric / Hermitian-indefinite + CLI `hesap.direct.ldlt.*` + bench (Eigen `SimplicialLDLT` = fixed-pivot breadth gap; MA57-class floor). | ~550 | ~16 | — |
| ↳ **v5e-1** | **Low-rank substrate + HSS kernel** — ID + randomized range-finder generalized to `LinearOp` sampling (extends v3b-3 `rsvd`/`rsyev`) + HSS representation + ULV factorization/solve. | ~600 | ~12 | — |
| ↳ **v5e-2** | **HSS-embedded multifrontal — FULL STRUMPACK feature set** — compress large fronts above a size threshold via randomized sampling (never form the dense front) + **adaptive rank** + **dense-fallback** + ND-aware clustering; validate 3D-Poisson asymptotic + indefinite/ill-conditioned robustness. | ~700 | ~14 | — |
| ↳ **v5e-3** | **BLR-embedded multifrontal (MUMPS-BLR)** — flat block-low-rank fronts (robust production default; complements HSS). **Ships in v5, expanding ADR-0065 BLR-reserve (locked 2026-05-28; pinned at §27).** | ~450 | ~10 | — |
| ↳ **v5f** | **Mixed-precision iterative refinement** (HPL-AI / Carson-Higham) — opt-in factor-in-f32 + refine-in-f64 `solve` policy across all four factorizations (~2× factor speed + memory at f64 accuracy). | ~350 | ~8 | — |
| ↳ **v5z** | **CLOSE** — complex-completeness audit + CLI-completeness audit + **end-to-end determinism moat {1..16}× all families** + ADR-0065 §27 lock D(direct)-1..N + `docs/systems/hesap-direct.md` + **18-config full sweep**. | ~250 | ~8 | — |
| **v6** | Sparse eigenvalue (Lanczos with restart + IRA Arnoldi + **LOBPCG Knyazev 2001** + **FEAST Polizzi 2009** + **Jacobi-Davidson** + **IRLBA Baglama-Reichel 2005**) + complex variants + CLI registration | ~2500 | ~110 | ~2.5 wk |
| **v7** | Optimisation v1 unconstrained: gradient / Newton / L-BFGS / **trust-region Steihaug** / BFGS + line search (Wolfe / Armijo / strong-Wolfe) + **stochastic optimization** (Adam / AdamW / Lion) + sensitivity (FD / forward-AD) + CLI registration | ~3200 | ~120 | ~2.5 wk |
| **v8** | Optimisation v2 constrained: QP (**OSQP** + **SCS O'Donoghue 2016**) + LP (revised simplex + Mehrotra IPM) + NLP (IPOPT-class + SQP) + algebraic modelling layer (JuMP / CasADi pattern) + CLI registration | ~3800 | ~110 | ~3 wk |
| **v9** | ODE / DAE: non-stiff (DOPRI5/8 + Cash-Karp + Verner + **Tsitouras 2011**) + stiff (BDF 1-6 + Rosenbrock-Wanner + **SDIRK** + RADAU5) + **symplectic** (Verlet + Yoshida 4/6/8) + **IMEX** + **sensitivity analysis** (forward + adjoint, CVODES/IDAS pattern) + DAE Pantelides index reduction + CLI registration | ~3500 | ~120 | ~3 wk |
| **v10** | FFT (Cooley-Tukey mixed-radix + Stockham + Bluestein + Rader for primes) + **RFFT** + 2D/3D + multidim + DCT/DST/Hartley + **NUFFT Greengard-Lee 2004** + **sparse FFT Hassanieh 2012** + convolution + complex inherently + CLI registration | ~2800 | ~110 | ~2.5 wk |
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
  is thread-independent (moat-safe). v5e-3 BLR expands the ADR-0065 reserve.
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
