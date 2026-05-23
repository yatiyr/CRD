# 2026-05-23 — hesap v3c (least-squares family) CLOSE

**Phase:** 3.1.6 `crd-hesap` v3 (SVD + dense eig). **Slice:** v3c (least-squares family) — CLOSED.
**Module:** `crd-hesap-dense`.

## What shipped

v3c builds the least-squares family on the shipped factorizations (QR / SVD that already beat
Eigen + LAPACK), adding one genuinely new factorization (column-pivoting QR + COD) and a BLAS-3
reflector-application lever that makes the dense-inverse path crush.

### v3c-1 — `lstsq` + `pinv`
- **`QRColPiv` / `factor_qr_colpiv`** — column-pivoting Householder QR (`dgeqp3`/`dlaqp2`
  faithful): Businger-Golub pivot on a transposed SIMD scratch + LAPACK partial-column-norm
  downdate-with-recompute. Reveals numerical rank.
- **`COD` / `factor_cod` / `solve_cod`** — complete orthogonal decomposition (LAPACK dgelsy):
  RZ reflectors (`dtzrzf`/`dlatrz`) reduce the r×n trapezoid to r×r `T11`; `solve_cod` =
  min-norm least-squares.
- **`lstsq`** (Auto=COD real / SVD complex; QR full-rank; SVD max-accuracy) + multi-RHS + lazy
  residual + rcond + {X, rank, residual}. **`pinv`** (Auto=COD / SVD).
- **Blocked-reflector-apply attack** (user-directed): `detail/apply_q_block.hpp` BLAS-3 `dlarfb`
  (all 4 side×transpose modes) + blocked recursive `trtri`. Took **pinv 0.11× → 1.60× Eigen** —
  the scalar O(r³) T⁻¹ back-sub (strided column access) was the large-r bottleneck, NOT the
  col-piv QR (the COD-solve path ties Eigen with the same factor).

### v3c-2 — NNLS + TLS
- **`nnls`** (real f32/f64) — Lawson-Hanson active-set with an INCREMENTAL thin QR of the
  passive columns (Björck §5.8): re-orthogonalised 2-pass MGS add + Givens re-triangularisation
  downdate (no full refactor per active-set change). D(lstsq)-1 ascending-index tie-break.
- **`tls`** (4 type variants via the complex SVD) — augmented `[A|b]` SVD, `X = −V12·V22⁻¹`,
  type-generic Gauss-Jordan V22 inverse, `exists` flag, multivariate d≥1.
- +14 v3c CLI commands; every op has a command.

## Benchmark (i9-14900K AVX2, f64, win-vs-ref)

| | vs Eigen | vs LAPACK |
|---|---|---|
| pinv (COD) | **1.12–1.60×** (↑ with n) | — |
| lstsq COD (`Auto`) | 0.84–**1.07×** | **1.88–12.19×** (dgelsy) |
| lstsq SVD | 0.75 → **2.40×** (n≥128) | **1.07–4.01×** (dgelsd) |
| lstsq QR | 0.69–0.93× | **1.70–8.69×** (dgels) |

The QR-tall small-n loss is the already-accepted ADR-0083 row-major-vs-column-major layout-fit
(filed `v3c-1-qr-tall-blocked`). NNLS/TLS gated by correctness (KKT + exact recovery + textbook;
the optimum is unique for full-rank A) — head-to-head bench filed `v3c-2-nnls-vs-eigen-bench`.

## Verification

- Full `crd-hesap-dense` suite **279 cases / 105 764 assertions** (was 206 at v3a close).
- 4-config DoD (debug / shipping / tidy / asan) green for the touched module; CI owns the
  18-config sweep (per `feedback_local_test_only_ci_owns_sweep`).
- vs-reference bench `bench_hesap_lstsq_vs_reference` validated with
  `CRD_BUILD_HESAP_VS_REFERENCE=ON` (build/win-vs-ref), then reset OFF for the default build.

## Decisions

- **ADR-0065 §24** — v3c least-squares decision lock + D(lstsq)-1..3 (ascending-index tie-break;
  Auto=COD/SVD dispatch; pinv dense inverse via blocked dlarfb + trtri, never per-reflector
  scalar). User scope call: "Full blocked-reflector-apply attack" over the CholeskyNormalEq
  pinv fast-path or accept-and-defer.

## Engineering notes

- `volatile auto x = <move-only>` is malformed (AV); use a scalar `volatile` sink in benches.
- `using L = Layout::RowMajor;` is a compile error (`RowMajor` is an enum value) — use
  `constexpr Layout kL = ...` (and clang-tidy wants the `k` prefix / ConstantCase).
- A "rank-deficient ground truth" test trap: same-frequency sinusoid columns are rank-deficient,
  so the NNLS/LS optimum is non-unique — use a Vandermonde for full-rank exact-recovery tests.

## Filed follow-ons

`v3c-1-qr-tall-blocked`, `v3c-1-blocked-rz-apply`, `v3c-2-nnls-vs-eigen-bench`.

## Next

**v3d** — non-symmetric eigensolver (balance `dgebal` + Hessenberg `dgehrd` + Francis
double-shift Schur `dhseqr` + Aggressive Early Deflation + eigenvectors `dtrevc`).
