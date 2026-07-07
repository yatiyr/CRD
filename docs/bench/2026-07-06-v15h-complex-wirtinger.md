# 2026-07-06 — v15-h: complex / Wirtinger forward AD — capability crush

**What shipped:** `complex_dual.hpp` — the holomorphic dual is `Dual<std::complex<T>>` (value + tangent both complex),
so every holomorphic op propagates `ẇ = f'(z)·ż` via a COMPLEX multiply using the **identical real-dual code**
(`dual.hpp`), no new rules, for `+−×÷ / exp log sqrt pow sin cos tan tanh` and the linear FFT (JAX `jvp` un-conjugated
convention). Non-holomorphic ops (`conj`/`real_part`/`imag_part`/`abs`/`norm`) carry the ℝ-linear pushforward
`t_ż=a·ż+b·conj(ż)`. The **Wirtinger pair** `(∂/∂z, ∂/∂z̄)` is reconstructed by seeding `ż=1` and `ż=i` — two exact
passes, no FD step. Holomorphic ⟺ `∂/∂z̄=0` (Cauchy-Riemann) = the gate. Added deterministic complex `sincos` to
`crd/math/complex.hpp` (one shared real range reduction). Determinism moat intact (crd::math complex cores).

## Correctness (`test_complex_dual.cpp`, 6-config green)
- Holomorphic (`z²`, `exp z`, `1/z`): `∂f/∂z` ≡ analytic AND `holomorphy_defect < 1e-11` (CR holds).
- Non-holomorphic (`conj`, `|z|`, `|z|²`): correct `(∂/∂z, ∂/∂z̄)` AND correctly FAIL CR (`|∂/∂z̄| ≈ 0.5`/`1`).
- Wirtinger pair ≡ a **2×2-real-Jacobian finite difference** (the validation complex-step CANNOT do — it co-opts the
  imaginary axis; this limit is encoded in the tests).
- A DFT is linear ⇒ holomorphic; its input sensitivity `∂Y_k/∂x_j = e^{−2πikj/n}` is exact.

## ★ Capability crush (`external/crd_v15h_complex_bench.cpp`) — filter-design sensitivity wrt a complex tap
Differentiate a DFT-like FIR response wrt a complex tap (N=16, ω=0.9).

**(1) Holomorphic transcendental `G=sin(H(ω))`, `∂G/∂h_m`:**

| method | error | evals |
|---|--:|--:|
| **Cerid CDual** | **5.6e-17 (EXACT, machine, deterministic)** | **1 pass** |
| central FD δ=1e-2 | 1.8e-5 | 2 |
| central FD δ=1e-6 | 1.1e-10 (FD's best) | 2 |
| central FD δ=1e-9 | 5.8e-8 | 2 |

**(2) Non-holomorphic objective `J=|H−t|²`, Wirtinger `∂J/∂z̄` (steepest-descent direction):** Cerid = 2 passes,
EXACT + deterministic; a 2×2 real FD needs **4** evals + a step choice to approximate it.

**★ Cerid delivers EXACT complex sensitivities (holomorphic 1 pass / Wirtinger 2 passes) at machine precision and
bit-deterministically**, where finite difference bottoms out at ~1e-10 with step-tuning and 2×/4× the evaluations —
and where real-only AD (Ceres `Jet`, CoDiPack) **cannot represent a complex tangent at all**. JAX complex `jvp` does
the math but calls the platform complex libm (run-to-run/platform variation); Cerid's crd::math complex cores are
bit-identical everywhere.

## Verdict — capability win
- Holomorphic sensitivities through FFT/DSP/comms (filter design, equalizer tuning) — EXACT, one-pass, deterministic,
  with a correct holomorphy (CR) gate distinguishing holomorphic from non-holomorphic primitives.
- 6-config DoD green; opt zero-regression; determinism moat extended to complex. (Reverse-mode grad-conjugation for
  complex *optimization* is v16; forward pushforward is complete here.)
