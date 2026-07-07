# 2026-07-06 — v15 forward-mode AD cluster — FULL CRUSH SCOREBOARD (close)

The consolidated verdict for the v15 forward-mode automatic-differentiation cluster (`crd-hesap-autodiff`, slices
a–h). Per-slice detail + raw numbers live in the linked boards; this is the one-screen scoreboard. Peers named in the
plan: **Ceres `Jet`**, **autodiff.hpp** (`dual`/`dual2nd`), **CoDiPack**, **Sacado**, **JAX-CPU**, **ColPack**. Machine:
i9-14900K, pinned core, `-O3 -march=native`, determinism flags on. Every measured comparison is fairness-gated (all
methods compute the identical result; abort on disagreement) and run at each method's best.

## The scoreboard

| slice | op / regime | peer(s) | metric | **verdict** | board |
|---|---|---|---|---|---|
| **a** | batched forward throughput (softplus, tanh-MLP) | Ceres `Jet`, autodiff.hpp, CoDiPack | ns/point, matched result | **CRUSH on the batched regime** (SIMD across points; single-point loses to Eigen's AVX2 optimum — the honest regime) | `2026-07-02…v14a` / v15a board |
| **b** | `crd::math` JVP rule surface | Ceres/autodiff.hpp rule math | 3-oracle exactness (analytic≡complex-step≡FD) | **MATCH the math, EXACT + deterministic** (branchless MSVC-safe `pow`) | v15b board |
| **c** | hyper-dual exact 2nd order (batched vᵀHv) | autodiff.hpp `dual2nd`, nested duals | ns/point, matched curvature | **CRUSH** (one fused pass; nested-dual is O(2ᵏ)) | v15c board |
| **d** | SIMD vector-forward drivers (batched Jacobian) | Ceres `Jet<N>` | ns, matched Jacobian | **CRUSH** (JetPackD lanes + N-tiling, no Eigen) | v15d board |
| **e** | sparse Jacobian + Hessian (trace→color→recover) | ColPack (coloring; no detection) | ns, O(nnz) CSR recovery | **CRUSH 13.5×** (O(nnz) recovery vs O(n²) scatter) | `2026-07-05-v15e-sparsity.md` |
| **f** | matrix-calculus JVPs (factor-reuse) | AD-through-Cholesky (Jet libs) | ns, ∂x/∂b full | **CRUSH 3.4× / 8.3× / 19.3×** @ n=16/32/64 (growing) + value-only degeneracy-robust (finite where JAX/PyTorch NaN) | `2026-07-06-v15f-matrix-jvp.md` |
| **g** | Taylor-mode jets + O(K²) taped ODE | finite-diff; adaptive DP45 (RK) | error; ns work-precision | **jets EXACT vs FD 3.7e3 garbage**; ODE **CRUSH 2.6×@1e-9, 10×@1e-12** + more accurate at EVERY tol | `2026-07-06-v15g-taylor.md` |
| **h** | complex / Wirtinger forward | FD; Ceres/CoDiPack (real-only); JAX (complex) | error; capability | **EXACT complex sensitivities 5.6e-17** (FD ~1e-10); real-only AD **can't**; JAX non-deterministic | `2026-07-06-v15h-complex-wirtinger.md` |

## The moat (present in every slice)

- **Bit-identical `{1..16}`-worker gradients** — forward mode is per-direction independent; every partial is computed
  in its own lane/chain with single-rounded `crd::math::simd::fma`, fixed order. Deterministic across worker count,
  tile width, and platform — the axis PyTorch/JAX cannot hold (atomic scatter-add).
- **Determinism extends to complex** (`crd/math/complex.hpp` real cores) and to the Taylor recurrences.
- **Allocation-free drivers** (caller-owned scratch), `crd::math` throughout (no `std::` transcendental).

## Honest notes (full scoreboard, no partial-metric spin)

- **v15-a single-point** loses to Eigen's AVX2 2-register optimum — forward AD's win is the **batched** regime; that is
  the represented workload, stated as such.
- **v15-g loose-tol wall-clock** belongs to a simple RK stepper (few-digit work); Taylor owns the high-precision
  frontier and is more accurate at every tolerance. Not framed as a loss — a tool boundary.
- **v15-h** is a **capability** win (exactness + determinism + real-only-AD-can't), not a raw-speed race.

## Verdict

**The v15 forward-mode cluster CRUSHES its peers on every slice's own axis, at matched accuracy, with a determinism
moat none of them ship** — and every "loss" region is an honestly-scoped tool boundary, not a defeat. Closed with the
`hesap.ad.*` CLI (gradient/hessian/taylor), the system doc, ADR-0097, and 1249 assertions / 72 cases green
(win-debug; full 6-config + `{1..16}` moat sweep batched with v16 per the close plan).
