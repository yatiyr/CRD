# crd-hesap-autodiff — the differentiation layer (Phase 3.1.6 v15 forward + v16 reverse)

> **Status: v15 forward-mode SHIPPED (2026-07-06), slices a–h + z.** Header-only forward surface; one src TU anchors
> the static lib + one registers the `hesap.ad.*` CLI. ADR-0097. Plan: `docs/phases/phase-3.1.6-v15.md` (slice
> contract + verdicts); master rows in `phase-3.1.6-hesap.md`. Scoreboard: `docs/bench/2026-07-06-v15z-scoreboard.md`
> (links every per-slice board). v16 (reverse mode + differentiable solvers + tape→codegen) is planned in the same
> module, not yet built. Close verification: 1249 assertions / 72 cases green on win-debug; the full 6-config DoD +
> `{1..16}` moat sweep is batched with v16 per the close plan.

## What it is

The engine's **differentiation layer** — the capability that turns every scalar-generic functor (and, via the suite
JVPs, every solver) into a source of **exact** derivatives, with a determinism moat no incumbent ships. Forward mode
(v15) buys exact gradients/Jacobians (FD→exact) for opt and the ODE integrators, matrix-calculus sensitivities via
factor-reuse, Taylor-mode high-order derivatives + a high-precision ODE integrator, and complex/Wirtinger
sensitivities through the FFT/DSP surface. Reverse mode (v16) will add ∂(scalar loss)/∂(millions of params) in one
backward pass. **The moat: bit-identical `{1..16}`-worker gradients** — deterministic differentiation, which
PyTorch/JAX cannot deliver (their reverse pass scatters through non-associative atomic adds).

## Architecture (ADR-0097)

- **One module, forward (`crd::hesap::autodiff::forward`) + later reverse (`::reverse`).** Header-only sub-headers so
  a scalar-`Dual`-only consumer drags neither the tensor JVPs nor the solver bridges.
- **Autodiff is LOWER than the solvers** — the edge is always solver→autodiff. `hesap-opt` re-exports the migrated
  canonical `Dual` (zero-regression). The matrix-calculus rules (v15-f) are **self-contained** (take the caller's
  stored factor + inline gemm/trisolve) precisely because the LA solvers sit above.
- **Determinism moat**: single-rounded `crd::math::simd::fma`, `-ffp-contract=off`/`/fp:precise`, `crd::math`
  throughout (no `std::` transcendental), per-direction-independent forward chains ⇒ bit-identical across worker
  count / tile width / platform.

## The surface (`include/crd/hesap/autodiff/`)

| header | what |
|---|---|
| `dual.hpp` | `Dual<T>` — the canonical forward scalar (value + one tangent); full `crd::math` JVP surface |
| `jet.hpp` / `jet_simd.hpp` | `Jet<T,N>` compile-time N; `JetPackD<W>` SIMD-across-directions carrier (SROA + FMA-order) |
| `hyperdual.hpp` | `HyperDual<T>` {f0,f1,f2,f12} — exact 2nd order; `hessian`/`curvature` drivers |
| `drivers.hpp` | `gradient<W>`/`jacobian<W>`/`jvp`/`directional` — runtime-n tiling over the SIMD carrier, allocation-free |
| `sparsity*.hpp`, `sparse_*.hpp` | index-set tracer → distance-2 coloring → O(nnz) CSR recovery (Jacobian + Hessian) |
| `matrix_jvp.hpp` | factor-reuse matrix-calculus JVPs (gemm/solve/cholesky/logdet/eigvals/svdvals); value-only degeneracy-robust |
| `suite_jvp.hpp` | FFT (linear ⇒ jvp = transform), DSP filtering (bilinear), spline Thomas-factor reuse |
| `taylor.hpp` / `taylor_ode.hpp` / `taylor_tape.hpp` | Taylor-mode jets (O(K²)); the O(K²)-taped high-precision ODE integrator |
| `complex_dual.hpp` | holomorphic dual `Dual<std::complex<T>>` + Wirtinger `(∂/∂z,∂/∂z̄)` + Cauchy-Riemann gate |
| `cli_anchor.hpp` | the `hesap.ad.*` CLI anchor symbol |

## Agent-facing CLI (`hesap.ad.*`)

Derivatives operate on callables, so agents reach the drivers through **canned named functions + a point** (the
data-vs-callable split, as for `hesap.ode.*`). **Forward (v15-z):** `hesap.ad.gradient.f64` (∇f, func 0 Rosenbrock /
1 sphere / 2 cubes, x an F64Array n≤32), `hesap.ad.hessian.f64` (exact ∇²f via hyper-dual, n≤6), `hesap.ad.taylor.f64`
(order-K∈{4,8,12,16} normalized coefficients of a canned 1-D f: exp/sin/1÷(1−x)/√(1+x)). **Reverse + implicit
(v16-z):** `hesap.ad.rgradient.f64` (∇f in ONE reverse pass, func 0 Rosenbrock / 1 sphere / 2 cubes / 3 Σexp, 2≤n≤256 —
the O(n) advantage over forward), `hesap.ad.jacobian.f64` (reverse Jacobian of a coupled map f_j=x_j²+x_{(j+1)mod n},
2≤n≤64), `hesap.ad.hvp.f64` (forward-over-reverse ∇²f·v — grad + H·v in one pass, never forms H; x + v, 2≤n≤64),
`hesap.ad.implicit.f64` (the IFT VJP through a canned root F=x²−θ ⇒ x*=√θ and dL/dθ without unrolling any solver,
1≤n≤16). All gated ≡ analytic in `test_ad_cli.cpp`. This is a step toward the v18 agent-drivable MATLAB.

## Crush summary

**Forward (v15, a–h)** crushes its peers on each axis at matched accuracy (scoreboard
`docs/bench/2026-07-06-v15z-scoreboard.md`) — batched throughput vs Ceres/CoDiPack (a), exact rules (b), fused 2nd
order (c), SIMD Jacobians (d), O(nnz) sparse recovery **13.5×** (e), factor-reuse **up to 19.3×** vs AD-through (f),
Taylor jets exact + O(K²)-taped ODE **10×** at 1e-12 (g), exact complex sensitivities real-only AD can't do (h).

**Reverse + differentiable solvers (v16, a–k)** crushes PyTorch/JAX/torchdiffeq/jaxopt/cvxpylayers/top88/efficient-kan
(full scoreboard `docs/bench/2026-07-07-v16z-scoreboard.md`) — full ∇f in one O(n) pass (a), machine-exact rule VJPs
(b), NN VJPs **1.6–2.7×** vs torch + sparse-autodiff torch lacks (c), matrix VJPs **3.77×** + FFT-VJP=IFFT (d), HVP
**crushes functorch 32–640×** (e), ODE-adjoint **607–777×** vs torchdiffeq + O(log T) memory (f), implicit-diff
**214× / 4900×** vs jaxopt/cvxpylayers + solves what SCS can't (g), tape→C++ codegen **13.7×** vs JAX-jit (h), the
**deterministic-training moat** (i), adjoint topopt **8.1×** vs top88 (j), neural-ODE **4.9×** + KAN **31×** (k). The
`{1..16}` bit-identical-gradient + bit-reproducible-training + deterministic-codegen moat runs through every slice —
the certification column no AD framework carries.

## Cross-refs

ADR-0097 · plan `docs/phases/phase-3.1.6-v15.md` · research `docs/research/2026-07-06-v15-forward-ad-crush.md` ·
scoreboard `docs/bench/2026-07-06-v15z-scoreboard.md` · crush playbook `docs/hints/crush-playbook.md`.
