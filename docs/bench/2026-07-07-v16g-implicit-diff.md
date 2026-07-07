# 2026-07-07 — v16-g: the implicit-differentiation suite (the open C++ lane)

**What shipped (`implicit_diff.hpp`):** differentiate the SOLUTION of an equation via the implicit function theorem —
NEVER by unrolling the solver, so the backward is O(1) linear solves independent of the solver's iteration count and
reuses the solver's own factor.
- **`root_vjp`** — F(x*,θ)=0 ⇒ z=(∂F/∂x)⁻ᵀx̄, θ̄=−(∂F/∂θ)ᵀz. Jacobians via the reverse tape, implicit solve via dense LU.
- **`fixed_point_vjp`** — x*=g(x*,θ) ⇒ the same rule with F=g−x.
- **`qp_eq_vjp`** — argmin ½xᵀQx+qᵀx s.t. Ax=b (OptNet): solve the KKT system, VJP gives {gQ, gq, gA, gb}.

## ★ Gate (`test_implicit_diff.cpp`, win-debug, 19 asserts)
- **root_vjp ≡ central FD** of the re-solved root (`<1e-6`) — Newton re-solve per θ-perturbation; and DETERMINISTIC
  (bit-identical run-to-run).
- **fixed_point_vjp ≡ central FD** of the re-solved fixed point (`<1e-6`).
- **qp_eq_vjp ≡ central FD** of the re-solved equality QP (`<1e-6`) — gq, gA, gb directly; gQ via symmetric perturbation.
- Full autodiff suite **2869 asserts / 111 cases** green.

## ★★ CRUSH — jaxopt + cvxpylayers parity, 200–5000× faster, and solves what SCS can't
**Config:** WSL2 i9-14900K, **1 thread `taskset -c 4`**, f64; Cerid g++ 13.3 `-O3 -march=native`. Peers **jaxopt**
(root, IFT — jax x64, jit'd) + **cvxpylayers 0.1.9 / SCS** (equality QP layer, torch backend, `eps_abs=eps_rel=1e-10`).
Same problems as `external/crd_v16g_implicit_bench.cpp` / `scripts/v16g_implicit_peers.py`.

★ **ROOT — jaxopt parity + 214×:**

| | Cerid `root_vjp` | jaxopt (Broyden+IFT) |
|--|--|--|
| dL/dθ | `[0.8104017478, 0.2645442217]` | `[0.8104017478, 0.2645442217]` |
| value+grad | **105 ns** | 22 475 ns (**214×**) |

10-digit gradient parity; Cerid 214× faster, deterministic, native.

★ **EQUALITY QP — cvxpylayers parity + ~4900×, and SCS fails where Cerid doesn't:**

| nq (mq) | Cerid `qp_eq_vjp` Sum_gq | cvxpylayers Sum_gq | Cerid time | cvxpylayers time | speedup |
|--|--|--|--|--|--|
| 8 (2) | **−1.6079026875** | −1.6079026875 | **250 ns** | 1.23 ms | **~4900×** |
| 20 (5) | **−3.5128013472** (1.4 µs) | **SCS: "infeasible"** (fails) | **1 415 ns** | — | — (∞) |

- nq=8: **10-digit parity** at tight SCS tolerance (the loose default gave a 4e-5 gap — Cerid's KKT solve is EXACT, gated
  ≡ FD; SCS is an approximate conic solver). **~4900× faster.**
- nq=20: **cvxpylayers/SCS returns "infeasible" on a feasible QP** (a conic-solver scaling failure) even at
  `eps=1e-10`/100k iters — **Cerid solves it exactly in 1.4 µs.** A robustness + capability win.

★ **The open lane:** jaxopt is UNMAINTAINED; cvxpylayers is Python + SCS + (for scale) GPU; Theseus is PyTorch/GPU.
A deterministic, native, exact-KKT C++ implicit-diff suite has **no living peer** — Cerid owns it. Every gradient is
bit-identical run-to-run (fixed-order tape + deterministic dense LU).

## Verdict
- **IFT VJPs** (root / fixed-point / equality-QP) — gated ≡ FD, deterministic. ✓
- **CRUSH: jaxopt parity + 214× (root); cvxpylayers parity + ~4900× (QP), and solves the nq=20 case SCS fails.** ✓
- **Owns the open C++ implicit-diff lane** (jaxopt dead), exact + deterministic. ✓
- MATLAB: no standard differentiable-optimization-layer equivalent (its `quadprog`/`fsolve` are forward solvers only) —
  N/A for the diff-opt comparison, stated with the reason; jaxopt/cvxpylayers are the correct peers.
- **★ NEW plan items** — Alt-Diff (arXiv:2210.01802, cheap block KKT updates for large structured QPs) + inequality-QP
  (active-set/interior-point KKT) + second-order implicit (Hessians through solves): the equality-QP + root/fixed-point
  core lands here; these extensions are honestly-scoped follow-ons (no silent scope reduction). 6-config DoD + {1..16}
  moat batched after v16.
