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
| ↳ **v3a** 🔄 (v3a-1/2/2.5 ✅) | **Symmetric eigensolver.** Shared reflector substrate + tridiag + QL/QR + **D&C** + **MRRR** + complex Hermitian. **4 sub-subslices ↓ — real symmetric + D&C + complex Hermitian SHIPPED; v3a-3 MRRR is the remaining hard-gate.** | ~2200 | ~70 | — |
| ↳ **v3a-1** ✅ 2026-05-22 | **Real f32/f64, blocked from the start.** Shared `detail/householder.hpp` (`make_householder` = `dlarfg`-faithful incl. safmin guard; shared by tridiag/bidiag/Hessenberg) + **blocked** symmetric tridiagonalization (`dsytrd` = `dlatrd` panel + `syr2k` trailing update; last block via `dsytd2` oracle) + implicit QL/QR Wilkinson shift (`dsteqr`: faithful `√(\|d·d\|)·eps`+SAFMIN split) + eigenvector accumulation + back-transform (`dormtr`) + perm-array ascending sort + pinned sign. **Column-major SIMD eigenvector rotation (ADR-0083 escape) + single-pass SIMD panel symv.** CLI `eig_sym.{f32,f64}`. Interfaces complex-aware (`tau:T`, `D`/`E`:`RealType<T>`). D(dense-eig)-1..5. **Gate MET: beats Eigen `SelfAdjointEigenSolver` 1.21–1.94× + LAPACK `dsyev` 1.90–4.02× at N=64–1024, accuracy ~1e-12; 5-config DoD PASS.** | ~750 | ~25 | — |
| ↳ **v3a-1b → v3a-2.5** ✅ 2026-05-22 | **Complex Hermitian eig** (folded into v3a-2.5 per user directive 2026-05-22; reuses the v3a-2 D&C solver). `zhetd2` reduce Hermitian → REAL tridiagonal (complex reflectors + accumulated phase-diagonal) + reuse real D&C/`steqr` on `(D,E)` + complex back-transform `V=Q·D_phase·Z`. **SIMD complex reduction: working matrix carried as two REAL arrays `ar`/`ai` → contiguous-row real-SIMD zhetd2 matvec + rank-2; back-transform reads reflectors directly (no complex materialization).** c32/c64. CLI `hesap.dense.eig.herm.{c32,c64}`. **Gate MET: beats Eigen complex `SelfAdjointEigenSolver` N≥128 (1.11× → 2.22× at N=1024), ties N=64 (1.00× — floor); beats LAPACK `zheev` everywhere (2.5–4.4×); accuracy ~1e-14.** `[herm]` 5 cases/20,117 assertions; 5-config DoD PASS. | ~450 | ~15 | — |
| ↳ **v3a-2** ✅ 2026-05-22 | Divide-and-conquer (Cuppen 1981 / `dstedc`): rank-1 tearing + deflation (`dlaed2` negligible-weight + equal-pole Givens) + secular-equation root-finder (`detail/secular.hpp` bracket-confined Newton+bisection, **D(dense-eig)-8**) + Löwner/Gu-Eisenstat eigenvectors (interleaved product — N=512 overflow fix [[feedback_lowner_product_overflow_interleave]]) back-transformed via `gemm_parallel`; fused Q·V merge. **Serial recursion + deterministic parallel merge-gemm = bit-identical across worker counts (D(dense-eig)-6/-7).** `eig_sym` dispatches QL/QR ≤256, D&C >256. **Gate MET: beats Eigen + LAPACK at every N (see v3a-1 row).** Subslices v3a-2.1..2.4 (secular / rank1 / dc_tridiag / dispatch) + 2.5 (complex Hermitian) all closed. | ~800 | ~22 | — |
| ↳ **v3a-3** | **MRRR** (`dstemr`: dqds eigenvalues `dlasq`, relatively-robust representations + twisted factorizations `dlarrv`, Gram-Schmidt re-orthogonalization for clustered eigenvalues). **HARD-GATE: O(n²) + accuracy beats LAPACK `stegr`.** | ~700 | ~23 | — |
| ↳ **v3b** | **SVD.** Bidiagonalization + Demmel-Kahan QR + **D&C bidiagonal** + **randomized**. **3 sub-subslices ↓.** | ~1600 | ~50 | — |
| ↳ **v3b-1** | Householder bidiagonalization (`dgebrd`, reuses v3a-1 substrate) + Demmel-Kahan implicit-zero-shift QR on bidiagonal (`dbdsqr`) + singular-vector accumulation + descending sort + complex SVD (`zgesvd`). **Gate: reconstruction `A=UΣVᵀ` + vs Eigen `JacobiSVD`.** | ~600 | ~18 | — |
| ↳ **v3b-2** | Divide-and-conquer bidiagonal SVD (Gu-Eisenstat / `dbdsdc`, BDCSVD-class), jobified bit-identical. **Gate: beat/tie Eigen `BDCSVD` at scale.** | ~700 | ~18 | — |
| ↳ **v3b-3** | **Randomized SVD** (Halko-Martinsson-Tropp 2011: Gaussian sketch + range-finder + power/subspace iteration → small dense SVD) + randomized Nyström symmetric-PSD eig. **Gate: Eigen has no randomized path — low-rank accuracy + speed win.** | ~300 | ~14 | — |
| ↳ **v3c** | **Least-squares family.** `lstsq` (QR full-rank / SVD min-norm rank-deficient) + `pinv` (Moore-Penrose + rcond) + **NNLS** (Lawson-Hanson active-set) + **TLS** (via SVD). **Gate: vs Eigen LS solvers + textbook minimiser.** | ~700 | ~25 | — |
| ↳ **v3d** | **Non-symmetric eigensolver (full).** Balance + Hessenberg + Francis double-shift Schur + **AED** + eigenvectors + complex Schur. **2 sub-subslices ↓.** | ~1500 | ~40 | — |
| ↳ **v3d-1** | Balancing (`dgebal`) + Hessenberg reduction (`dgehrd`, blocked, reuses substrate) + Francis double-shift implicit QR → real Schur (`dhseqr`) with **Aggressive Early Deflation** (Braman-Byers-Mathias) — **HARD-GATE**; eigenvalues incl. complex-conjugate 2×2 blocks. | ~900 | ~22 | — |
| ↳ **v3d-2** | Eigenvectors via Schur back-substitution (`dtrevc`) + **3-stage back-transform** (balancing-diag⁻¹ · Householder Q · Schur vectors) + complex eigenvector assembly + complex non-sym Schur. **Gate: vs Eigen `EigenSolver`/`RealSchur`.** | ~600 | ~18 | — |
| ↳ **v3e** | **CLI audit + close.** CLI-completeness audit (every op a command) + vs-reference rollup (Eigen + LAPACK) + **ADR-0065 §17** lock (D(dense-eig) determinism pins) + 18-config sweep. | ~200 | — | — |
| **v4** | Iterative solvers (CG / PCG / BiCGSTAB / GMRES / MINRES / LSQR / IDR(s) / **GCRO-DR + M-CG Krylov subspace recycling**) + modern preconditioners (Jacobi / IC(0) / ILU(0) / **SPAI** / **ILUPACK multilevel ILU** / polynomial / block-Jacobi / additive Schwarz) + `LinearOp` consumer surface + complex variants + CLI registration | ~3500 | ~150 | ~3 wk |
| **v5** | Sparse direct (supernodal Cholesky — CHOLMOD-class + left-looking LU — Gilbert-Peierls + multifrontal QR + LDLT) + **AMG variants** (classical Ruge-Stüben + **SA-AMG Vaněk 1996** + **AGMG Notay 2010** + bootstrap AMG) + **HSS-augmented (STRUMPACK pattern) reserve** + complex variants + CLI registration | ~4500 | ~140 | ~3.5 wk |
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

- **v3a** symmetric eig — v3a-1 (substrate + **blocked** tridiag + QL/QR, **real
  f32/f64**) → **v3a-1b (complex Hermitian `zhetrd`)** → v3a-2 (D&C) →
  **v3a-3 (MRRR — hard-gate vs LAPACK `stegr`)**.
- **v3b** SVD — v3b-1 (bidiag + Demmel-Kahan + complex) → v3b-2 (D&C bidiagonal
  SVD) → v3b-3 (randomized Halko).
- **v3c** least-squares family — lstsq + pinv + NNLS + TLS.
- **v3d** non-sym eig — **v3d-1 (balance + Hessenberg + Schur + AED — hard-gate)**
  → v3d-2 (eigenvectors + 3-stage back-transform + complex Schur).
- **v3e** CLI audit + vs-reference rollup + ADR-0065 §17 lock + 18-config close.

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
