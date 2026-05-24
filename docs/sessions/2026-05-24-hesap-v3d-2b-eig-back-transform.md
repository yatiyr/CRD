# 2026-05-24 — hesap v3d-2b: public real `eig()` + 3-stage back-transform

Shipped the consumer-facing non-symmetric eigensolver: `eig(Matrix<T>) →
EigNonsym<T>` assembling the already-shipped real-Schur pipeline into the public
API, with the 3-stage back-transform from Schur basis to the original matrix.
**Beats Eigen `EigenSolver` at every measured N and crushes LAPACK `dgeev`.**

## What shipped

- **`EigNonsym<T>`** (`eig_nonsym.hpp`): `Vector<Complex<RealType<T>>> values` +
  `Matrix<Complex<RealType<T>>> vectors` (column k = eigenvector for values[k];
  Schur order). Real input → real + conjugate-pair complex eigenpairs.
- **`eig(Matrix<T>)`** (`eig_nonsym.cpp`): clone → `balance` → `hessenberg` +
  `form_hessenberg_q` → `schur_aed` (multishift-train AED; `A_bal =
  (Q·Z)·T·(Q·Z)ᵀ`) → `dtrevc` (`detail::schur_right_eigvecs`) → 3-stage
  back-transform `V = D⁻¹·P · Q · Z · V_schur`:
  - (i) `Z·V_schur` + (ii) `Q·` via two `gemm`s,
  - (iii) **`gebak_right`** — faithful LAPACK `dgebak` (SIDE='R', JOB='B'): row
    scaling over `[ilo,ihi]` + the isolating row permutation at the corners
    (1-based loop mirroring `dgebak.f`).
  - All three stages are real-linear ⇒ the `dtrevc` real-packed re/im column
    layout survives; complex eigenpairs are assembled + normalized only at the
    end.
- **`gebak_right`** — new anonymous-namespace helper (the dgebak port).
- **CLI** `hesap.dense.eig.nonsym.{f32,f64}` — full n×n row-major matrix in,
  interleaved `[re,im]` eigenvalues (Schur order) out (values-only, matching the
  `eig_sym` CLI precedent; vectors via the engine API).
- **Bench** `bench_hesap_eig_nonsym_vs_reference.cpp` extended with a full-eig
  section: Cerid `eig` vs Eigen `EigenSolver` vs LAPACK `dgeev` + residual +
  eigenvalue match.

## Determinism pins (→ ADR-0065 §17, locked at v3d close)

- **D(non-sym)-4** — eigenvector normalization: Euclidean norm 1, then the
  lowest-index largest-magnitude component phase-rotated real-positive (the
  LAPACK `dgeev` / Eigen `EigenSolver` convention). **Supersedes the v3d-2 plan
  row's "max-abs = 1" wording** — 2-norm=1 keeps the head-to-head gate clean.
- **D(non-sym)-5** — eigenpair order = Schur order from `schur_aed`
  (deterministic; complex spectra have no natural total order, so no sort).

## Gate — MET decisively

- Per-eigenpair residual `‖A·vₖ − λₖ·vₖ‖∞ / ‖vₖ‖∞` ~**1e-13** (≪ 1e-9 gate),
  for real + complex eigenpairs; eigenvalues match Eigen to ~1e-13.
- The `dgebak` permutation + scaling branches are exercised by dedicated
  fixtures: a corner-isolated matrix (`ilo>0`/`ihi<n-1`) and a badly-scaled
  `A = D·B·D⁻¹` (D spanning 2⁻¹⁵…2¹²).
- Bench (win-vs-ref, i9-14900K AVX2, f64, full eig values+vectors):
  beats Eigen `EigenSolver` **1.09× (n=100) / 1.08× (n=200) / 1.65× (n=400)**;
  crushes LAPACK `dgeev` **2.03× / 3.04× / 1.57×**.

## The perf lever — `form_hessenberg_q` was the only stage losing to Eigen

First full-eig bench showed Cerid *losing* to Eigen at small N (0.90× @ n=200)
despite the Schur step itself **winning** (1.12×). Rather than hand-wave it as
"the accepted v3d-1c-3 NMIN=200 regime", a stage-timing diagnostic attributed
the n=200 cost:

```
bal=0.14  hess=1.11  formQ=3.35  schur=13.04  trevc=0.47  gemms=0.57   (ms)
```

`form_hessenberg_q` (dorghr) was **3.35ms** — the entire non-Schur overhead, and
the only thing tipping the full pipeline to a loss. Its reflector apply was
**scalar column-strided** (`q[(i+1+r)*ld + j]` for fixed j varying r = stride-ld
jumps, no SIMD). Rewrote it **row-wise**: accumulate `w[:] = Σ_r v[r]·Q[row_r][:]`,
scale by τ, then `Q[row_r][:] −= v[r]·w[:]` — both contiguous row ops via
`detail::simd_axpy`. Same per-element accumulation order over r (bit-stable up to
the standard hesap single-rounded-fma convention; no D-pin or golden mandates
bit-exact Q — checked). Result: **3.35ms → 0.58ms (5.8×)**, flipping n=100/200
from loss to win. This is the same SIMD-row-wise lever that won the v3d-1a
Hessenberg reduction ([[feedback_simd_rowwise_unblocked_beats_blocked_smallk]]).

dtrevc (0.47ms) and the two back-transform gemms (0.57ms) were already cheap;
`gemm_parallel_auto` would not move them at this size.

## No-debt call

We beat Eigen + LAPACK at **every** measured N. Eigen's fused dhseqr-Z
accumulation (one combined Q·Z back-transform vs our two gemms) would only
*widen* the win — there is no loss to fix, and the two gemms are 0.57ms
(negligible vs the Schur cost). **Not filed as a follow-on** (would be a phantom
slice; per `feedback_never_defer_solve`, only losses get tracked).

## Verification

- Full `[nonsym]` suite **247 624 assertions / 26 cases** + 4 new `eig:` cases.
- 4-config DoD green (touched module): **win-debug** + **win-shipping (LTCG)** +
  **win-asan** each 247 624 assertions; **win-tidy** clean on the changed
  sources. CI owns the 18-config sweep (`feedback_local_test_only_ci_owns_sweep`).
- Local clang-format is **v22.1.1**, repo pinned **v19**; `--lines`-scoped check
  of the added ranges flagged only the same categories that also flag
  pre-existing v19-clean code (bare `template <typename T>` lines, aligned
  trailing comments, unicode-in-comment). Not churned with v22; **CI's v19 is
  authoritative** (`feedback_clang_tidy_ci_local_version_skew`).

## v3d-2c-1 — complex Hessenberg reduction + unitary Q (CLOSED, same day)

Opened v3d-2c (the complex non-sym path). Subdivision advisor-vetted: 2c-1
(Hessenberg + Q) / 2c-2 (zgebal + single-shift zlahqr) / 2c-2b (complex AED —
expected) / 2c-3 (ztrevc + back-transform + complex eig + CLI). User directive:
**full elite path, max performance, cleanness over code size.**

**Shipped:** `zgehd2` + `zunghr` on the two-real-array (`ar`/`ai`) SIMD path (the
`eig_herm` v3a-2.5 idiom). Unified `hessenberg<T>` / `form_hessenberg_q<T>` now
dispatch real-vs-complex via `if constexpr`; the complex branch splits
`Matrix<Complex<R>>` → `(ar, ai)` once (O(n²), ADR-0078 §5 lower layer), reduces,
recombines. `make_householder_complex` (`zlarfg`, real beta) **promoted to the
shared `detail/householder.hpp`**, deduping `eig_herm`'s inline copy (the
`[herm]` suite stayed green — proof the dedup is faithful).

**Fused complex SIMD substrate** `detail/dot_simd_complex.hpp` —
`simd_cdot_nc` / `simd_caxpy` / `simd_caxpy_conjx`, bit-identical to the
4-separate-`sdot`/`saxpy`-pass form but reads each operand row ONCE. Reused by
2c-2's `zlahqr`.

**The perf arc (measure-don't-guess, the hard lesson):**
- First cut lost to Eigen (0.74×/0.82× @ n=64/128). I hypothesised memory
  traffic and wrote *4-wide* fused kernels → **REGRESSED to 0.56×/0.54×**. Wrong:
  the wall was FMA-port ILP, not memory — the 4-wide fused kernel had less unroll
  than the 8-wide-unrolled `sdot`.
- Rewrote the kernels **8-wide (2× Vec4d, 8 independent FMA accumulators)** →
  **flipped to a WIN: 1.05× (n=64) / 1.21× (n=128) vs Eigen**, and the 8-wide
  reduction is now bit-identical to the `sdot`-combo (2-accumulator/8-wide). vs
  LAPACK `zgehrd` 2.21×/11.91×.

**Bench-harness crash — found and SOLVED, not waved off.** The bench SEGV'd at
n=256. Marker isolation (`cerid-done eigen-start` printed, no `eigen-done`)
proved it was **Eigen's `HessenbergDecomposition<MatrixXcd>::compute` access-
violating at n≥256** (its `Packet2cd` AVX complex path; the real `MatrixXd` path
at 256 is fine), NOT Cerid — confirmed by Cerid's recon-clean run to n=512 and
ASan-clean run to n=256. The reuse `.compute()` pattern still crashed → genuine
Eigen-reference fault. Fixed the bench to time the references only in their
stable regime (n≤128, same as the existing OpenBLAS `zgehrd`/`dhseqr` caps) and
Cerid alone at 256/512; bench now completes EXIT=0.

**Gate MET:** `A=Q·H·Qᴴ` recon <1e-12 (c64 n=6/32/64/160/256), Q unitary, H
upper-Hessenberg with real subdiagonal; c32 n=24 <5e-4. 4-config DoD green
(debug/shipping-LTCG/asan each **353 406 assertions / 308 cases** / tidy clean).

## v3d-2c-2 — complex balance (zgebal) + single-shift Schur (zlahqr) (CLOSED, same day)

**Shipped:** `balance<T>` unified for real+complex via `if constexpr` +
`RealType<T>` scalars + `bal_abs`/`bal_nsq` helpers; **`scale` changed to
`Array<RealType<T>>`** (advisor call — storing real values in `Complex.re` would
force a Complex-multiply in the 2c-3 `gebak` hot loop). `complex_schur`
(`zlahqr`): single Wilkinson-shift implicit QR on a complex upper-Hessenberg →
upper-triangular `T` (eigenvalues on the diagonal, **no 2×2 blocks, no
`dlanv2`** — the structural win over real) + unitary `Z` + eigenvalues;
complex-Givens bulge-chase, Ahues-Tisseur deflation, faithful zlahqr Wilkinson
shift (`U=√h(i-1,i)·√h(i,i-1)`, scaled `Y`, `T=h(i,i)−U²/(X+Y)`) + exceptional
shifts at its 10/20 (**D(non-sym)-6**, `dat1=0.75`). New primitives: complex
`sqrt` (`complex.hpp`) + `complex_givens` (`zlartg`, `detail/householder.hpp`,
overflow-safe via `hypot2` — reused by 2c-3 `ztrevc`). `Complex<T>` arithmetic
(the `ar`/`ai` SIMD boundary ends at 2c-2 entry, per the advisor).

**Gate MET:** `H=Z·T·Zᴴ` recon <1e-8 (c64 n=8/20/50/128, actually ~1e-13), Z
unitary, T upper-triangular, eigenvalues = diag(T); c32 n=24 <1e-3; complex
balance isolation + trace-invariance. **Bench (c64): BEATS Eigen `ComplexSchur`
1.16× (n=64) / 1.10× (n=128), crushes LAPACK `zhseqr` 6.58×/1.45×; recon
~1e-13.** Eigen `ComplexSchur` ALSO AVs at n≥256 — confirmed the
`reference_eigen_complex_hessenberg_av_at_large_n` prediction; refs capped at
128. 4-config DoD green (debug/shipping-LTCG/asan **353 435 assertions / 311
cases** / tidy clean; a stale-PCH C2859 in win-shipping/tidy after the
`complex.hpp`/`householder.hpp` edits was fixed by `--clean-first` on
`crd-hesap`, a known MSVC incremental-PCH fragility, not a code defect).

## State / next

v3d-2b + v3d-2c-1 + v3d-2c-2 are 4-config-DoD green; no open debt. The whole
v3c + v3d body remains in the working tree, uncommitted since `v3b finished`.

**NEXT = v3d-2c-2b** — complex AED (`zlaqr`-class — EXPECTED, LAPACK `zhseqr`
uses AED at n≥75, single-shift loses at scale). Then 2c-3 (`ztrevc` +
back-transform + public complex `eig` + CLI), v3d close + v3e CLI audit (locks
ADR-0065 §17 incl. D(non-sym)-1..6).
