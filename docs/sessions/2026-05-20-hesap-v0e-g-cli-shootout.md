# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0e-g: CLI registration + reference-class solver shootout vs Eigen

## What shipped

**CLI registration** (`engine/hesap-dense/src/cli_register_solvers.cpp`):
8 single-shot factor+solve commands, one per (solver, precision) combo:

| Command | Inputs | Output |
|---|---|---|
| `hesap.dense.solver.lu.{f32,f64}` | `n`, `A (n*n)`, `b (n)` | `x (n)` via factor_lu+solve_lu |
| `hesap.dense.solver.cholesky.{f32,f64}` | `n`, `A (n*n lower)`, `b` | `x` via factor_cholesky+solve_cholesky |
| `hesap.dense.solver.ldlt.{f32,f64}` | `n`, `A (n*n lower)`, `b` | `x` via factor_ldlt+solve_ldlt |
| `hesap.dense.solver.qr.{f32,f64}` | `m`, `n`, `A (m*n)`, `b (m)` | `x (n)` via factor_qr+solve_qr (LS for m>n) |

Each impl error-checks dimensions, returns `ResultError` on singular /
not-PD input. Anchor symbol `register_solvers_cli_anchor()` exported
from `cli_anchor.hpp` per ADR-0081 §7.

**Factor-only / solve-only / refine commands** — deferred. The factor
objects (`Permutation` + `block_kinds` for LDLT, `taus` for QR) are
not directly JSON-serializable; the single-shot factor+solve is the
useful primitive. A future v0e-g2 can ship factor-object-as-opaque-handle
when consumers need to reuse a factor across many RHS calls.

**Reference-class bench harness** (`runtime/examples/bench_hesap_solvers_vs_reference.cpp`,
gated by `CRD_BUILD_HESAP_VS_REFERENCE=ON`):
- LU vs Eigen `PartialPivLU<MatrixXd>`
- Cholesky vs Eigen `LLT<MatrixXd>`
- LDLT vs Eigen `LDLT<MatrixXd>`
- QR vs Eigen `HouseholderQR<MatrixXd>`
- Sizes: N = {32}, 64, 128, 256, 512, 1024 (LDLT skips 1024 due to unblocked cost)
- P-core affinity (`SetProcessAffinityMask(0xFFFF)` on i9-14900K)
- Best-of-3 measurement with 3 warmup iters
- `crd::jobs::frame_reset()` between iters (per
  `memory/feedback_jobs_parallel_for_frame_arena_exhaustion`)
- Validates max element-wise error vs Eigen reference

## Honest reference-class numbers (2026-05-20, i9-14900K AVX2)

**LU factor+solve, f64**:

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)   C/Eigen   max|err|
64     6.84            12.94            0.53x     7.22e-16
128    8.92            6.51             1.37x WIN 1.67e-15
256    13.49           0.80             16.84x WIN 1.78e-15
512    22.84           1.49             15.36x WIN 1.89e-15
1024   34.55           4.10             8.43x WIN 3.66e-15
```

LU **WINS at N ≥ 128**, dramatically at N = 256–512 (16×). The
catch-22 at N=64 is overhead-dominated (single panel, no trailing-
update GEMM). At N=128+, Cerid's `gemm_parallel` trailing update
parallelizes across 16 P-threads while Eigen-default appears to run
serial — filed `v0e-g-eigen-mt-config` to investigate fair MT setup.

**Cholesky factor+solve, f64 SPD**:

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)   C/Eigen
64     5.98            10.25            0.58x
128    7.86            20.98            0.37x
256    12.73           30.66            0.42x
512    22.76           32.99            0.69x
1024   34.66           33.91            1.02x WIN (tied)
```

Cholesky only wins at N = 1024 (and only barely). Eigen's LLT has
significantly tighter inner loops at small/medium N. Two follow-ons:
- `v0e-b-syrk-optim` (halve trailing-update FLOPs via true syrk)
- `v0e-g-eigen-mt-config` (apples-to-apples MT comparison)

**QR factor+solve, f64 square**:

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)   C/Eigen
64     6.72            7.97             0.84x
128    2.57            17.87            0.14x
256    1.57            23.38            0.07x
512    1.54            39.19            0.04x
```

**Significant gap** — our unblocked Householder QR vs Eigen's blocked
WY representation. `v0e-d-blocked` is now BLOCKING-priority: ship a WY
blocked variant with `gemm_parallel` trailing update before any
consumer slice depends on QR perf. The 25× gap at N=512 is the
unblocked-O(n³)-FMA pipeline vs Eigen's panel-then-GEMM-trailing-update
pattern.

**LDLT factor+solve, f64 symmetric**:

```
N      Cerid (GFLOPS)  Eigen (GFLOPS)   C/Eigen
32     4.04            3.70             1.09x WIN
64     5.33            7.41             0.72x
128    1.97            9.25             0.21x
256    1.03            12.18            0.08x
```

Same story — unblocked Bunch-Kaufman vs Eigen's blocked LDLT.
`v0e-c-blocked` is now BLOCKING-priority.

## Test surface

`tests/hesap-dense/test_solvers_cli.cpp` — **6 cases / 32 assertions**:

1. All 8 solver commands registered.
2. LU.f64 solves diagonally-dominant 4×4 to 1e-10.
3. Cholesky.f64 solves SPD tridiagonal 3×3 to 1e-12.
4. LDLT.f64 solves 2×2 indefinite [[1,2],[2,1]] exactly.
5. QR.f64 solves over-determined 4×2 polynomial-fit LS to 1e-10.
6. Singular matrix → error result (not crash).

## Verification matrix

| Config | Build | Run | Notes |
|---|---|---|---|
| win-debug | ✓ PASS | ✓ 170 cases / 61,001 assertions | Full hesap-dense suite |
| win-tidy | ✓ PASS | n/a | No new violations |
| win-vs-ref (`CRD_BUILD_HESAP_VS_REFERENCE=ON`) | ✓ PASS | ✓ Shootout ran | Numbers above |

## Frame arena fix

Per `memory/feedback_jobs_parallel_for_frame_arena_exhaustion`: the
shootout's tight loop initially crashed with `FrameArena::alloc: arena
exhausted` at the third LU N=1024 iter — `gemm_parallel` inside the
trailing update accumulates JobDecls in the per-thread 1 MB frame
arena, which only reclaims on `crd::jobs::frame_reset()`. Added
`crd::jobs::frame_reset()` between iters in the `time_loop` helper.

## Files touched

**Engine**:
- `engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp` — anchor decl.
- `engine/hesap-dense/src/cli_register_solvers.cpp` — NEW (~370 LOC).

**Tests**:
- `tests/hesap-dense/test_solvers_cli.cpp` — NEW (6 cases).
- `tests/hesap-dense/CMakeLists.txt` — added.

**Runtime**:
- `runtime/examples/bench_hesap_solvers_vs_reference.cpp` — NEW (~370 LOC).
- `runtime/CMakeLists.txt` — bench target added under
  `CRD_BUILD_HESAP_VS_REFERENCE` gate.

**Docs**: this session log + context.md + phase doc.

## Filed follow-ons (now BLOCKING for consumer slices)

| Task | Status | Why |
|---|---|---|
| `v0e-c-blocked` | **BLOCKING** when any consumer hits LDLT perf | Eigen LDLT is 12× ours at N=256 |
| `v0e-d-blocked` | **BLOCKING** when any consumer hits QR perf | Eigen QR is 25× ours at N=512 |
| `v0e-g-eigen-mt-config` | Investigation | Eigen-default appears unparallel at large N; explicit `setNbThreads` may flip Cholesky/LU ratios; need apples-to-apples |
| `v0e-b-syrk-optim` | Filed | Half-FLOP Cholesky trailing update via true syrk |
| `v0e-f2` mixed-precision | Filed (from v0e-f) | HPL-AI pattern |
| `v0e-e2` LU/LDLT/QR condition estimators | Filed (from v0e-e) | Need solve_transpose paths |

The QR and LDLT gaps are **expected outcomes of v0e-c/d MVP scope** —
unblocked algorithms cannot match blocked reference implementations.
The blocked follow-ons are filed (with consumer-slice trigger) and
documented in the phase doc.

## Next

v0e-close: 5-config DoD (`scripts/per-slice-check.ps1 -IncludeRelease`)
+ rollup session log + ADR-0065 §14 decisions queue + update
`docs/systems/hesap-dense.md` with the solver scorecard.
