# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0e-e + v0e-f: LinearOp / condition / iterative refinement

Combined session log for v0e-e (LinearOp wrappers + Hager 1-norm
condition estimator) and v0e-f (iterative refinement, same-precision
MVP). Both shipped on 2026-05-20 in the same session as v0e-a/b/c/d.

## v0e-e — LinearOp wrappers + Hager 1-norm condition estimator

The `crd::hesap::LinearOp<T>` abstract interface was set up in v0a
(`engine/hesap/include/crd/hesap/linear_op.hpp`). v0e-e implements
concrete dense wrappers and a condition estimator on top.

**Surface**:

| Symbol | Form |
|---|---|
| `MatrixLinearOp<T, L>` | Wraps `const Matrix<T, L>&`; `apply` = gemv, `apply_transpose` = gemv-T |
| `SymmetricLinearOp<T>` | Wraps `const Symmetric<T>&`; `apply = apply_transpose = symv` |
| `compute_1norm(Matrix/Symmetric)` | Exact ||A||_1 (column-sum max) |
| `hager_1norm_estimate(n, apply_op, apply_op_transpose, alloc)` | Template; takes any callables. LAPACK xLACON pattern. |
| `condition_estimate_1norm_symmetric(Symmetric, Cholesky)` | Convenience: κ_1(A) = ||A||_1 · ||A^-1||_1 for SPD via Cholesky |

**Hager's 1-norm estimator** (5-iteration power iteration):

```
v = (1/n, 1/n, ..., 1/n)
for iter = 0..max_iter-1:
  v := op · v                   # apply_op (e.g., A^-1 for ||A^-1||_1)
  γ_new = ||v||_1
  if iter > 0 and γ_new <= γ: return γ
  γ = γ_new
  v := sign(v)                  # construct sign vector
  v := op^T · v                 # apply_op_transpose
  jmax = argmax |v_i|
  v := e_{jmax}                 # restart as unit vector
return γ
```

For symmetric A factored via Cholesky, `apply_op == apply_op_transpose`
(both = `solve_cholesky`), simplifying the closure setup.

**Tests** (8 cases / 26 assertions PASS):

- `MatrixLinearOp::apply` matches manual gemv on 3×3 textbook.
- `MatrixLinearOp::apply_transpose` matches manual Aᵀ·x.
- `SymmetricLinearOp::apply` matches manual symmetric·x.
- `compute_1norm(Matrix)`: 3×3 = 18 (max col sum).
- `compute_1norm(Symmetric)`: 3×3 = 12.
- κ_1 for tridiag SPD: finite, well-conditioned (< 100).
- κ_1 for identity matrix: exactly 1.0.
- κ_1 for scaled identity (`7.5·I`): exactly 1.0 (the scale cancels).

**Filed**: `v0e-e2` — LU + LDLT + QR condition estimators (need
solve_transpose paths; LU's solve_lu doesn't yet have a transpose
variant).

## v0e-f — Iterative refinement (same-precision MVP)

Wilkinson 1948 iterative refinement. Drives a factor-solved initial
guess to backward-stable accuracy via repeated residual-correction.

**Algorithm**:

```
x_0 = solve(b)                  # initial solve (caller does this)
for k = 0, 1, 2, ...:
  r_k = b - A · x_k             # residual via LinearOp
  if ||r_k||_2 / ||b||_2 < tol: return
  dx  = solve(r_k)              # correction via the factor
  x_{k+1} = x_k + dx
```

**Surface**:

| Symbol | Form |
|---|---|
| `RefinementResult<T>` | Struct: iterations, initial_rel_residual, final_rel_residual, converged |
| `refine_lu<T, L>(a, lu, b, x, max_iter=5, tol=1e-13)` | LU variant |
| `refine_cholesky<T, L>(a, chol, b, x, max_iter=5, tol=1e-13)` | Symmetric Cholesky variant |

Both variants take the original matrix (needed for residual product
via LinearOp) plus the factor (for the corrections via solve). Initial
guess is supplied by the caller (typically `solve_lu(b) → x_0`).

**Tests** (4 cases / 35 assertions PASS):

- N=16 well-conditioned LU: residual doesn't increase post-refinement,
  x recovered to 1e-10.
- Trivial b=0: converges immediately at iteration 0.
- N=12 SPD Cholesky: residual doesn't increase, x recovered to 1e-10.
- Already-exact initial solve (diagonal matrix): converges at iteration 0.

**Filed**: `v0e-f2` — mixed-precision HPL-AI variants. Factor in f32,
refine in f64. 3-4× speedup pattern from HPL-AI benchmark. Needs a
cross-precision solve wrapper (cast b to TF, solve in TF, cast back
to TR, accumulate in TR).

## Verification matrix (combined v0e-e + v0e-f)

| Config | Build | Run | Notes |
|---|---|---|---|
| win-debug | ✓ PASS | ✓ 164 cases / 60,969 assertions | Full hesap-dense suite |
| win-asan  | ✓ PASS | ✓ 12 v0e-e+f cases | ASan-clean across both slices |
| win-tidy  | ✓ PASS | n/a | Both lib + tests clean |

## Files touched

**Engine** (v0e-e):
- `engine/hesap-dense/include/crd/hesap/dense/linear_op_dense.hpp` — NEW
- `engine/hesap-dense/include/crd/hesap/dense/condition.hpp` — NEW

**Engine** (v0e-f):
- `engine/hesap-dense/include/crd/hesap/dense/refinement.hpp` — NEW

**Tests**:
- `tests/hesap-dense/test_linear_op_condition.cpp` — NEW (8 cases)
- `tests/hesap-dense/test_refinement.cpp` — NEW (4 cases)
- `tests/hesap-dense/CMakeLists.txt` — added both

## What this consumed from v0a-v0e-d

- `crd::hesap::LinearOp<T>` interface (v0a) — base class.
- `gemv` (v0c) — `MatrixLinearOp::apply`.
- `symv` (v0c) — `SymmetricLinearOp::apply`.
- `solve_cholesky` (v0e-b) — Hager closure target.
- `solve_lu` (v0e-a) — refinement correction step.
- `Matrix<T, L>`, `Symmetric<T>` (v0c).

## Next

v0e-g: CLI registration for every solver + `bench_hesap_solvers_vs_reference.cpp`
shootout vs Eigen FullPivLU / PartialPivLU / LLT / LDLT / HouseholderQR /
ColPivHouseholderQR. Per continuous-benchmarking policy.

After v0e-g: v0e-close (5-config DoD + ADR-0065 §14 decisions queue +
rollup session log).
