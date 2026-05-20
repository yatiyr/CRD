# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0e-close: dense direct solvers cluster closed

Rollup for the **v0e dense direct solvers** cluster — 7 sub-slices (a–g)
shipped in one day plus a perf-attack session, all closed 2026-05-20.

## What v0e delivered

| Sub-slice | Surface | Tests |
|---|---|---|
| v0e-a | LU partial-pivoting (right-looking blocked; first `gemm_parallel` consumer) | 10 / 37,156 |
| v0e-b | Cholesky SPD | 10 / 18,725 |
| v0e-c | LDLT Bunch-Kaufman indefinite | 7 / 132 |
| v0e-d | Householder QR (blocked compact-WY) | 10 / 4,266 |
| v0e-e | LinearOp wrappers + Hager 1-norm condition | 8 / 26 |
| v0e-f | Iterative refinement (Wilkinson) | 4 / 35 |
| v0e-g | 8 CLI solver commands + Eigen shootout | 6 / 32 |
| perf-attack | blocked compact-WY QR + SIMD LDLT + packed parallel syrk | (rolled into above) |

Plus the reusable `syrk_lower_minus` packed register-tiled primitive and
the `LinearOp`/condition/refinement support layer.

## Reference-class result (vs Eigen-MT, i9-14900K AVX2, factor+solve)

```
Solver  N=64    N=128   N=256   N=512   N=1024
LU      0.52x   1.46x   ~70x    11x     21x
Chol    0.94x   0.70x   0.55x   0.84x   1.41x
QR      1.19x   0.76x   0.96x   1.04x   --
LDLT    1.00x   0.96x   1.18x   1.13x   0.94x
```

Journey highlights from the perf-attack: LDLT 0.08→1.16× (14×), QR
0.04→1.06× (26×), Cholesky N=512 0.65→0.84× / N=1024 0.96→1.41× via the
packed parallel syrk. Small-N dense factorization residual (≤256)
investigated to root cause and settled in **ADR-0083** (row-major vs
Eigen's column-major layout fit; not a kernel-quality gap; proven by 3
controlled experiments + Eigen source reading).

## Definition of Done

- **5-config DoD**: `scripts/per-slice-check.ps1 -IncludeRelease -Parallel`
  — debug + asan + shipping + release + tidy all PASS.
- **Suite**: 172 hesap-dense cases / 65,098 assertions PASS.
- **Guards**: `crd-no-non-ascii-test-names`, `crd-no-std-math-check`,
  `crd-simd-emission-check` green. (Caught + fixed 6 TEST_CASE names with
  a `·` middle-dot — exactly what running binaries directly misses.)
- **Determinism**: factors bit-identical across worker counts.
- **Allocator propagation**: no `default_allocator()` in library code.

## Decisions

- **ADR-0083 Accepted** — hesap-dense row-major storage with per-factor
  column-major escape hatch.
- v0e decisions (D1–D8) queued in ADR-0065 for the §14 lock at v0-close.

## Filed follow-ons (consumer-/hardware-gated, NOT blocking)

`v0e-a2` (LU complex), `v0e-b-hpd` (Hermitian Cholesky), `v0e-c-blocked`
(LDLT gemm trailing update), `v0e-d-colpiv` (rank-revealing column-
pivoting QR), `v0e-e2` (LU/LDLT/QR condition estimators — need
solve_transpose), `v0e-f2` (mixed-precision HPL-AI iterative refinement),
`v0e-b-syrk-optim` (subsumed: packed syrk now used for n>256).

## Next

**v0f** — `crd-hesap-bench` sub-module + reference-fixture replay
infrastructure + property-based test framework (`RandomMatrix<T>`). Then
**v0-close** (ADR-0065 §14 decision lock for v0a–f + 18-config full sweep).
After Phase 3.1.6 closes: Phase 3.1 eylem v1c+ resumes (consumes geometry
+ hesap from day 1).

Session logs: `2026-05-20-hesap-v0e-a-lu.md`, `-v0e-b-cholesky.md`,
`-v0e-c-d-ldlt-qr.md`, `-v0e-e-f-linearop-condition-refinement.md`,
`-v0e-g-cli-shootout.md`, `-v0e-perf-attack.md`.
