# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0-close: dense foundation closed

v0-close is the cluster-close for Phase 3.1.6 v0 (the dense foundation:
substrate + BLAS L1/L2/L3 + dense direct solvers + property/bench
infrastructure). Two deliverables: the **ADR-0065 §14 decision lock** and the
**18-config full sweep**.

## What v0 was (v0a–f rollup)

| Slice | Surface | Headline result |
|---|---|---|
| v0a | Substrate: `Complex<T>` + ~30-type matrix catalog + `LinearOp<T>` + typed handles + `'HDV0'` pin + CLI protocol scaffolding | shipped day 1 |
| v0b | BLAS L1 × 4 type variants + KBN pairwise reduction + 8-acc SIMD `dot`/`nrm2` | 20–30× SIMD speedup; axpy 4/5 WINS vs Eigen |
| v0c | BLAS L2 (17 ops × variants): single-pass symv + 8-row tiled gemv + prefetch | symv N=4096 0.02→0.99×; gemv N=4096 1.25× WIN |
| v0d | BLAS L3: Goto/BLIS task-DAG gemm + AVX2 microkernel + `gemm_parallel` + sched DAG | **10/10 WINS over Eigen-MT** |
| v0e | Dense direct: LU / Cholesky / LDLT / QR + LinearOp + Hager κ₁ + Wilkinson IR | LU WINS N≥128; ADR-0083 (row-major) |
| v0f | `RandomMatrix` property-test factory + 5 property tests + `crd_add_hesap_vs_ref_bench()` CMake helper (slimmed) | suite 176 / 66,703 |

The single-rounded `simd::fma()` (distinct from two-rounded `mul_add`) is the
through-line that beats Eigen-MT at GEMM and propagates into every solver.

## Deliverable 1 — ADR-0065 §14 decision lock (Accepted)

Formalises the v0 implementation decisions that were validated empirically:

- **L50–L53** (v0a–d): substrate-from-day-1; KBN L1 reduction; single-pass L2
  SIMD shape; Goto/BLIS intrinsics-microkernel L3 with `gemm_parallel` over
  `crd::jobs` (no separate thread pool) and `simd::fma` accumulation.
- **v0e-D1…D8** (now formally locked): the solver algorithm choices +
  **ADR-0083 Accepted** (row-major storage + per-factor column-major escape
  hatch).
- **L54–L55** (v0f): property-based test framework; vs-reference bench CMake
  dedup (validated only with `CRD_BUILD_HESAP_VS_REFERENCE=ON`).

§14 also records every deviation from the §13 elite plan in a table:
`crd-hesap-bench` sub-module + committed FFTW/SuiteSparse fixtures deferred to
the FFT/sparse consumers; `bench_common.hpp` deferred to the third vs-reference
bench; microkernel AVX-512/NEON/SVE2 hardware-gated; mixed-precision IR filed
`v0e-f2`; per-µarch asm deferred behind ADR-0082's untriggered revisit gate.

## Deliverable 2 — 18-config full sweep + six latent cross-config bugs

`scripts/full-sweep.ps1`: 11 Windows (debug / relwithdebinfo / release / asan /
clang-cl / debug-scalar / debug-sse2 / shipping / shipping-profile /
clang-cl-shipping + tidy build-only) + 7 Linux-via-WSL
(gcc debug / relwithdebinfo / release / asan / debug-scalar / debug-sse2 /
shipping).

**The first full-sweep run failed 11/18** — and that is the whole point of the
gate. The 5-config per-slice DoD only ever builds **MSVC + AVX2**, so a class of
breakage introduced back in **v0d** (the `simd::fma()` primitive) had been latent
since then. The full sweep was the first build of an `fma()`-using TU under
gcc / clang-cl / scalar / SSE2. Six distinct latent bugs, all fixed:

| # | Bug | Affected configs | Fix |
|---|---|---|---|
| A | `_mm256_fmadd_{ps,pd}` needs `-mfma` on gcc/clang (MSVC `/arch:AVX2` bundles it) | all 7 Linux | add `-mfma` to the gcc/clang AVX2 branch of `CrdSimd.cmake` |
| B | `Vec4f` had no `fma()`, so `Vec8f::fma`'s scalar/SSE2 half-decomposition didn't compile | scalar + SSE2 (Win+Linux) | add `Vec4f::fma` (per-lane `std::fma`, single-rounded, bit-exact with the 256-bit hardware FMA) |
| C1 | unused `sp` lambda capture in `syrk_microkernel.hpp` | clang-cl | drop the capture |
| C2 | unused `void_result` in `cli_register.cpp` | clang-cl + gcc | delete the dead helper |
| D | `blas2.cpp` prefetch locals (`use_prefetch`, `A_next_row`) unused when the `_mm_prefetch` block is `#if`-compiled-out | scalar + SSE2 | `[[maybe_unused]]` |
| E | `complex.hpp::arg()` used `std::atan2` (ADR-0063 determinism violation; caught by the Linux `.sh` no-std-math guard) | all (guard) | `crd::math::deterministic::atan2` + new `crd-hesap → crd-math` link edge |

The `-mfma` change (A) does **not** weaken ADR-0063: `mul_add` stays two-rounded.
It is `(a*b)+c` built from separate `_mm256_mul_pd` + `_mm256_add_pd` intrinsics,
and `-ffp-contract=off` (carried on `crd-simd-flags`, PUBLIC-linked through
`crd-math` to every consuming TU) blocks the compiler from contracting them. Only
the explicit `simd::fma()` emits the hardware FMA. The SIMD-vs-scalar bit-exact
parity tests (run in every config's ctest) are the standing guard that `mul_add`
was not contracted.

Bug E added the only new module edge: **`crd-hesap → crd-math`** (PUBLIC). It is
acyclic (`crd-math` depends only on core/units/simd-flags) and `crd-hesap-dense`
already had it; the umbrella simply needed it so `complex.hpp` can reach the
deterministic `atan2` substitute.

**Local validation after the fixes:** clang-cl + win-debug-scalar + win-debug-sse2
build clean; the gcc build compiles clean (Bug A confirmed) with the no-std-math
guard now PASS; win-release + linux-gcc-release build+ctest run as the LTO +
determinism-parity confidence gate. **The authoritative 18-config full sweep is
delegated to CI** (this push) — CI is the canonical record of `RESULT: PASS`.

## Definition of Done

- **18-config full sweep PASS** (cluster-close gate).
- **Suite**: 176 hesap-dense cases / 66,703 assertions.
- **Guards**: no-non-ascii / no-std-math / simd-emission green.
- **Determinism**: factors bit-identical across worker counts.
- **ADR-0065 §14 Accepted**; ADR-0083 Accepted.

## Post-close CI hardening (second push, same day)

The first CI run on the pushed fixes surfaced two more cross-config issues —
neither a regression from the v0-close fixes; both pre-existing, exposed once the
build got far enough to reach them. CI's MSVC is **14.44**; local is **14.50**.

- **C4127 "conditional expression is constant"** (`/W4 /WX`). Local 14.50 relaxed
  it; CI 14.44 still emits it. Hit `smoke_hesap_substrate` (a constexpr FourCC
  check → fixed properly as a `static_assert`) and `geometry-curves/frames.hpp`
  (`if (!closed || i < n)` where `curve.closed` is `static constexpr` for one
  curve type and runtime for another — no clean per-site fix). **Resolution:**
  globally suppress via `/wd4127` on `crd-warnings`, matching the existing
  `/wd4714` precedent. → memory `feedback_msvc_c4127_ci_local_version_skew`.
- **clang-cl FMA-contraction determinism hole.** clang-cl reports as MSVC and
  honors `/fp:precise`, but — unlike MSVC `cl` — maps it to `-ffp-model=precise`
  which leaves within-statement FMA contraction **on**. So clang-cl contracted
  `a -= b*c` into a single-rounded FMA, producing different bits than cl/gcc and
  **breaking the ADR-0063 no-contraction contract**. Surfaced as two failing
  tests on win-clang-cl-shipping (`LU: detects exactly singular`, `CLI: solver
  dispatch rejects singular`): `4 - (2/3)*6` is exactly 0 with separate mul+sub
  but ~2.2e-16 under FMA, so the `== 0` singular detector missed. **Resolution:**
  for clang-cl skip `/fp:precise` (its default model is already IEEE-precise) and
  pass `/clang:-ffp-contract=off`. Validated on win-clang-cl-shipping: both tests
  pass; full suite otherwise green (a lone `crd-simd-emission-check` + two
  `_NOT_BUILT` "failures" in local runs were partial/stale-build artifacts, not
  present in CI's clean build). Both singular tests **hardened** with a
  structurally-singular (zero-column) matrix; the rank-deficient matrix is kept
  as a determinism canary.

## Next

**v1 — sparse storage + kernels.** Detailed 7-sub-slice plan in
`docs/phases/phase-3.1.6-hesap.md` (§ "v1 — Sparse storage + kernels — DETAILED
PLAN"). Core-tier scope (Tier-3 CSR5/Merge-CSR/JDS/SkyLine dropped 2026-05-20):
COO + CSR/CSC + BSR/ELL/SELL-C-σ/DIA + spmv (SELL primary) + spmv-T + spmm +
spgemm (parallel hash) + element-wise + conversion graph + `SparseLinearOp` +
Matrix-Market I/O + complex + CLI-per-op. First slice **v1a**. Then the strategic
sequencing (`feedback_strategic_execution_plan_2026_05_15`) resumes **Phase 3.1
eylem v1c+**, which consumes geometry + hesap-dense from day 1.
