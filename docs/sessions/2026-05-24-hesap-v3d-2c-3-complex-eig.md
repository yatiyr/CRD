# 2026-05-24 — hesap v3d-2c-3: complex ztrevc + public complex `eig` + CLI

Closes **v3d-2c (complex non-symmetric eigensolver)**. The public
`eig(Matrix<Complex<T>>)` assembles the shipped complex pipeline (balance →
Hessenberg → `complex_schur_aed` → ztrevc → unitary back-transform) and **beats
Eigen `ComplexEigenSolver` + crushes `zgeev`**.

## What shipped

- **`ztrevc_right<T>`** (complex right eigenvectors of an upper-triangular complex
  Schur form): per-column triangular back-solve with the `smin` near-defective
  floor + inline overflow scaling (the `cnorm`/`bignum` guard `dtrevc_right` uses —
  no `zlatrs`). All-scalar (complex Schur has no 2×2 blocks ⇒ no `dlaln2`).
  Back-solve ported VERBATIM from `ztrevc.f` (fetched, like `zlaqr5`/`zlaqr1`). NO
  normalization here (deferred to `eig`, advisor call — one normalization, one
  rounding, one phase convention).
- **`eig<T>` → dispatcher**: `eig_real_impl` (the v3d-2b body, renamed — zero
  re-indent) + new `eig_complex_impl`, dispatched by a thin `if constexpr`
  `eig<T>`. The complex pipeline: balance → hessenberg + form unitary Q →
  `complex_schur_aed` (A_bal = (Q·Z)·T·(Q·Z)ᴴ) → `ztrevc_right` → `V = D⁻¹P·Q·Z·
  V_schur` (two `gemm`s + complex `gebak_right`) → normalize once per
  **D(non-sym)-4** (‖·‖₂=1, largest-**modulus** component phase-real-positive).
  All-complex columns — no real-packed/conjugate-pair logic (the real branch's
  delicate conjugate-pair assembly has no complex analog; kept side-by-side, not
  deduplicated, per advisor).
- **`gebak_right` generalized to `<V, S>`** (complex vectors `V`, real scale `S`) —
  the real callsite deduces both, no churn.
- **CLI** `hesap.dense.eig.nonsym.{c32,c64}` — interleaved `[re,im]` n×n in,
  interleaved `[re,im]` eigenvalues (Schur order) out (values-only, matching the
  real `eig.nonsym` CLI; vectors via the engine API).

## Gate — MET

- Per-eigenpair residual `‖A·vₖ − λₖ·vₖ‖₁ / ‖vₖ‖₁` ~**1e-13** on random c64
  (n=20/60), ‖v‖₂=1, largest-modulus component real-positive (asserted) — plus a
  **non-triangular near-defective** fixture (duplicated 2.0 eigenvalue introduced
  by a Givens similarity, then the matrix kept non-triangular so `balance` doesn't
  fully isolate → drives `ztrevc` into the `smin` floor) and c32.
- **Bench (c64): BEATS Eigen `ComplexEigenSolver` 1.13× (n=64) / 1.32× (n=128),
  crushes `zgeev` 4.79×/2.80×.** Refs **AV at n≥256 — confirmed empirically** (the
  bench probes 256, both refs access-violate, same fragility as `ComplexSchur`);
  capped n≤128. Cerid alone resid 3.2e-13 @ 256.

## Two findings

1. **Test-bug (not an eig bug):** the phase assertion first picked the pivot
   component by `cabs1` (`|re|+|im|`) while `eig` normalizes by **modulus**
   (`re²+im²`, the LAPACK/Eigen convention) — different component ⇒ the test
   checked one `eig` hadn't rotated. Residual was already ~1e-14 (vectors correct);
   fixed the test's pivot metric to modulus. Lesson reinforced:
   [[feedback_test_eigensolvers_on_random_not_smooth]].
2. **Follow-on filed `v3d-eig-fully-reducible-input`:** a fully-reducible input
   (e.g. triangular) makes `balance` isolate every eigenvalue → empty active block
   → `ihi = l−1` underflows (`usize`) → `hessenberg` asserts `ihi < n`. Pre-existing
   (affects the real `eig` too); narrow edge case (only fully-reducible inputs),
   deferred per `feedback_crush_mandate_bounded_by_importance`. The original
   triangular near-defective fixture tripped this; made the fixture non-triangular.

## Verification

- 4-config DoD green (touched module): win-debug + win-shipping (LTCG) each full
  `[nonsym]` **248 181 assertions / 47 cases**; win-asan `[complex][eig]` clean;
  win-tidy exit 0. Complex eig bench builds + runs with the vs-reference flag
  (reset OFF by default).

## Full 18-config sweep — RESULT: PASS (and what it caught)

User asked for the full local sweep (`full-sweep.ps1`, 11 Windows + 7 Linux/WSL),
not the lighter local check. It earned its keep — it caught **real cross-config
defects every per-slice check had missed**, because per-slice runs MSVC-only and
uses the test binary directly (not `ctest`). All were in the just-committed
v3c+v3d body; **zero logic bugs.** Two classes:

1. **18 non-ASCII TEST_CASE names** (`·ᵀᴴλ⁺`, em-dash) in `test_eig_nonsym.cpp` +
   `test_lstsq.cpp` (from v3d-1/2a/2b/2c-1/2c-2 + lstsq, NOT the 2c-3 names) →
   Windows `ctest` mojibakes the argv filter → "No test cases matched" → instant
   fail. Invisible to binary-direct runs. ASCII-ized (`·`→`*`, `ᵀ`→`^T`, `ᴴ`→`^H`,
   `λ`→`lambda`, `⁺`→`+`, `⁻¹`→`^-1`, em-dash→`-`). The `crd-no-non-ascii-test-names`
   guard is `ctest`-registered, so it only fires under `ctest` — never run in
   per-slice binary-direct checks.
2. **9 clang/gcc `-Werror` build-fails** MSVC's `/WX` doesn't flag: unused `z`
   accessor lambdas in both `dlaqr5` sweeps (real + complex), `deflate2`
   set-but-unused in `dqds.hpp` (→ `[[maybe_unused]]`, faithful-port artifact),
   unused `max3` lambda in `svd_secular.hpp`, a dead `zt` in `test_eig_nonsym.cpp`,
   and **5 `double→float` narrowings** (`dqds.hpp` ×4 + `bdsqr.hpp` ×1 → `static_cast<R>`,
   the `feedback_gcc_linux_double_to_float_narrowing` hazard). Verified by building
   the module on clang-cl (exit 0) + gcc-linux keep-going (exit 0) before re-sweeping.

**Re-run: 18/18 PASS, 0 failed, 52:16.** First run was 9-failed (the above) +
2 false starts (a `vswhere`/PATH env bug in the clean background session — fixed by
prepending the VS Installer dir so `vcvars` finds `vswhere`).

**Process lesson** ([[feedback_per_slice_binary_direct_misses_ctest_and_crossconfig]]):
the iterate-on-the-binary-with-tag-filters workflow structurally misses BOTH the
`ctest`-registered guards AND the clang/gcc toolchains. One `ctest --preset` run +
one clang-cl + one gcc build before slice close would have caught all of this
locally without the multi-hour sweep detour.

## State / next

## v3e — close (CLI audit + constant verification + ADR-0065 §25 lock) ✅ 2026-05-25

The v3 cluster close. Three legs (the 18-config sweep was already PASS):

- **Constant verification (the bug-finding leg).** Diffed the exceptional-shift constants vs
  `zlahqr.f`/`dlahqr.f`/`zlaqr5.f`/`zlaqr1.f`: `dat1=0.75`, `itmax=30·max(10,nh)`, the
  zlaqr5/zlaqr1 conj forms — all exact. **One divergence found + fixed:** `complex_schur` used
  the **classic `zlahqr` 2-kick** schedule (`its==10/20`) while `real_schur` already uses the
  **modern `dlahqr` `KEXSH=10` `kdefl`-continuous** kicks. Upgraded `complex_schur` to the
  modern continuous-kick schedule (`its%(2·KEXSH)`=bottom / `its%KEXSH`=top) for consistency +
  robustness on pathological spectra. **Behaviour-neutral:** converging spectra never reach
  `its≥10`, so `[nonsym]` stays at 248 181 assertions, 0 changed. (D(non-sym)-6 updated.)
- **CLI audit — CLEAN.** Every dense-eig op has a command, including `rsvd`/`rsyev` (my first
  audit grep used an incomplete pattern and false-flagged them; they ARE registered as
  `hesap.dense.rsvd/rsyev.{f32,f64}`, matching §22's claim).
- **ADR-0065 §25** (NOT §18 — §13–§24 were already used; §17 MRRR, §18–23 SVD, §24
  least-squares; only v3d was unlocked). Locks **D(non-sym)-1..8** + the NMIN-measured
  crossover (real 200 / complex 150) + the faithful-port divergences (zlaqr5 conj-verbatim,
  zlaqr2 spike-conjugation, ztrevc inline-scaling, `gebak_right<V,S>`, the complex-ref AV cap,
  the `v3d-eig-fully-reducible-input` follow-on) + the vs-reference rollup. **§25 closes v3d
  AND the whole v3 family.**

**🎉🎉 Phase 3.1.6 v3 (dense SVD + eigensolvers + least-squares) CLOSED** — v3a (sym/herm) +
v3b (SVD) + v3c (least-squares) + v3d (non-sym), §17–§25, beats Eigen + LAPACK across the
dense-decomposition surface; 18-config sweep PASS.

## v3d-2c state

**🎉 v3d-2c (complex non-symmetric eigensolver) CLOSED** — complex Hessenberg
(2c-1) + balance & single-shift Schur (2c-2) + AED `zlaqr0`/`zlaqr5` (2c-2b) +
ztrevc + public complex `eig` + CLI (2c-3), all beating Eigen + LAPACK (or
crushing our own single-shift baseline where the refs AV). **NEXT = v3d close +
v3e §17 lock** — ADR-0065 §17 records D(non-sym)-1..8 + verifies the
zlahqr/zlaqr5/ztrevc exceptional constants character-for-character against the .f
sources; CLI audit (every nonsym op has a command). One open follow-on:
`v3d-eig-fully-reducible-input`.
