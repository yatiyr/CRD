# 2026-07-06 — v16-b: the scalar VJP rule library — crush (exact + full coverage)

**What shipped:** `rules_reverse.hpp` — the COMPLETE scalar VJP surface over the reverse `Var` (v16-a tape): the whole
`crd::math` unary family (asin/acos/atan · sinh/cosh · asinh/acosh/atanh · exp2/exp10/expm1 · log2/log10/log1p · tan ·
cbrt/rsqrt, on top of v16-a's exp/log/sin/cos/sqrt/tanh/pow), the binary rules (atan2/hypot/pow-dual), and control
flow (abs subgradient · min/max/select carry the taken branch · value-comparisons). **The crush is correctness: a VJP
is the TRANSPOSE of the JVP, so every local partial is the SAME audited `forward::detail` slope the v15 forward mode
uses — there is no second rule library to drift or mis-derive.**

## ★ Correctness — 3-oracle gate (`test_reverse_rules.cpp`, win-debug green, 66 assertions)
Every rule: **reverse VJP ≡ forward JVP** (the transpose identity — exact, `~1e-11`, both use the same slope) **≡
central FD** (`~1e-6`, independent). Binary rules gated on both partials; control flow verified to route the adjoint to
the active branch (max→larger gets 1, other gets 0; select; abs→sign).

## ★ Crush — machine-EXACT gradients vs finite difference (`crd_v16b_rules_bench`)
Gradient of a transcendental loss `Σ softplus(x_i) + atan(x_i)` (n=64), max error vs the analytic gradient:

| method | max error | evals |
|---|--:|--:|
| **Cerid reverse (1 backward pass)** | **2.2e-16 (EXACT, machine, deterministic)** | **1 pass** |
| central FD h=1e-4 | 3.3e-9 (FD's best) | 128 (2n) |
| central FD h=1e-2 | 3.3e-5 | 128 |
| central FD h=1e-10 | 4.9e-5 | 128 |

**★ The reverse VJP library is machine-exact in ONE pass** where FD saturates at ~3e-9 with a step-choice and 2n
evaluations. Exact + one-pass + deterministic vs approximate + step-tuned + O(n) evals.

## Verdict — crush + coverage
- **Full crd::math VJP surface + binary + control flow**, each 3-oracle gated (broader than autograd's core; matches
  Stan Math / Adept on the math, adds the transpose-of-a-proven-JVP guarantee — no independent rule bugs).
- **Machine-exact gradients** (2.2e-16) vs FD's ~3e-9, in one pass, deterministic (crd::math cores).
- Full autodiff suite 1342 asserts/79 GREEN (win-debug); 6-config + {1..16} moat sweep batched after v16 (per plan).
