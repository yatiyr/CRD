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
| **v1** | Sparse storage (CSR / CSC / BSR / COO / ELL / HYB / DIA / CSR5 / Merge-CSR / Sliced ELL / JDS / SkyLine) + spmv + spmm + spgemm + format-conversion graph + sparse `LinearOp` + complex variants + CLI registration | ~3000 | ~120 | ~3 wk |
| **v2** | Fill-reducing reorderings: AMD (Amestoy 1996) + RCM (Cuthill-McKee) + METIS-class nested dissection + symbolic factorisation phase + CLI registration | ~2200 | ~80 | ~2 wk |
| **v3** | SVD (Golub-Reinsch + **randomized**, Halko 2011) + dense eigenvalue (MRRR symmetric + QR-double-shift non-symmetric + **randomized variants**) + least squares (LS / NNLS / TLS) + complex variants + CLI registration | ~2800 | ~130 | ~2.5 wk |
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
