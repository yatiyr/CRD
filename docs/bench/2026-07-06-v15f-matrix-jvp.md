# 2026-07-06 — v15-f: matrix-calculus + suite forward differentials (JVPs) — crush + honesty

**What shipped:** the matrix-calculus JVP surface (`matrix_jvp.hpp`) + the DSP/FFT/spline suite (`suite_jvp.hpp`),
self-contained (autodiff sits BELOW the LA solvers, so the rules take the caller's STORED FACTOR + dense row-major
matrices and use inline gemm / triangular-solve — they never call a factorization). Rules verified vs Giles NA-08/01,
Murray arXiv:1602.07527, JAX `jax/_src/lax/linalg.py`.

## Rules (`matrix_jvp.hpp`, all == FD in `test_matrix_jvp.cpp`)
- `gemm_jvp`  `dC = dA·B + A·dB`  ·  `solve_spd_jvp`  `dX = A⁻¹(dB − dA·X)` (reuses L)  ·  `cholesky_jvp`
  `dL = L·Φ(L⁻¹dA L⁻ᵀ)` (halved diag)  ·  `logdet_spd_jvp`  `Tr(A⁻¹dA)`  ·  `eigvals_jvp`  `dλ=diag(QᵀdA Q)`  ·
  `svdvals_jvp`  `dσ=diag(Uᵀ dA V)`.
- Suite (`suite_jvp.hpp`): FFT `jvp(fft)=fft(dx)` (linear); filtering `dy=dh⊛x+h⊛dx` (bilinear); spline control-value
  JVP = one Thomas back-solve reusing the build's tridiagonal factor (`thomas_solve`).

## ★ HONESTY EDGE — value-only degeneracy-robust drivers
`logdet` / `eigvals` (dλ) / `svdvals` (dσ) are the trace/diagonal tangents — they **never divide** by (λ_i−λ_j) or σ,
so they stay **finite and exact at repeated/zero spectra**, where JAX/PyTorch (which compute the eigenVECTOR
derivative via the F-matrix `1/(λ_j−λ_i)`) return **NaN**. Tested: `eigvals_jvp` exact + `isfinite` on a degenerate
(equal-eigenvalue) construction. On the rule *math* we MATCH JAX/PyTorch (same Giles/Murray); the finite-at-degeneracy
value-only path is the honesty win.

## ★ CRUSH — factor-reuse (`external/crd_v15f_matrix_jvp_bench.cpp`, fairness-gated bit-exact)
Full `∂x/∂b = A⁻¹` for an SPD solve (n directions). Cerid factorizes ONCE and back-solves; the naive AD-through peer
(a Jet library differentiating the solve function) re-runs Cholesky+solve on `Dual` PER DIRECTION (O(n⁴), because it
doesn't know A is constant across the b-directions). Both use the SAME templated kernels — only the reuse differs.

| n | AD-through-Cholesky (ns) | **Cerid factor-REUSE** | crush | agree |
|--:|--:|--:|--:|--:|
| 16 | 14964 | **4349** | **3.4×** | 7e-18 |
| 32 | 156760 | **18979** | **8.3×** | 1e-17 |
| 64 | 2083508 | **108138** | **19.3×** | 3e-18 |

**★ Factor-reuse CRUSHES AD-through 3.4–19.3× and rising (O(n³) vs O(n⁴)), bit-exact.** This is the v15-f principle
in one number: differentiate the SOLUTION reusing the factor, never AD-through the factorization loop.

## Verdict — FULL CRUSH + honesty
- Match JAX/PyTorch on rule math; **crush the Jet libs on factor-reuse flops (19× at n=64, growing)**; **value-only
  drivers stay finite where JAX/PyTorch NaN** (repeated λ/σ). Self-contained, allocation-free, deterministic.
- 6-config DoD green; opt zero-regression. (`dU/dV` full eigen/SVD-vector JVPs with F-damping = a future `Dxxx` if
  ever forced; we deliberately ship the degeneracy-robust value-only path instead.)
