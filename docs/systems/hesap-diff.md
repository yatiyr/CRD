# crd-hesap-diff — numerical differentiation

> Phase 3.1.6 v13 (l–m). ADR-0095. Plan: `docs/phases/phase-3.1.6-v13.md`.
> Status: **SHIPPED (2026-07-01)** — linux-gcc-green + Windows 4-config; 42 assertions. Sessions:
> `2026-07-01-v13-jk-oscillatory-cubature.md` (diff shipped alongside), `2026-07-02-v13z-windows-close.md`.
> Fornberg arbitrary-stencil weights · Richardson/Ridders · **★★machine-exact complex-step** · Savitzky-Golay ·
> Chebyshev/Fourier differentiation matrices · CLI `hesap.diff.*`.

## What it is

Derivatives — gradients, Jacobians, Hessian-vector products — for sensitivity analysis, IK Jacobians at kHz, and
noise-robust velocity estimation from encoders/IMUs. Held to the v13 certification bar (ADR-0095): deterministic,
allocation-free kernels, status/bool returns (never exceptions).

## Shipped surface

- **v13-l — finite differences done right.** **★Fornberg arbitrary-stencil weights** (`fornberg_weights` — any node
  distribution, any derivative order, the numerically-stable recurrence) · central/forward/backward with a
  **per-magnitude optimal step** (`optimal_h`, never a hard-coded h) · **★Richardson / Ridders** (`derivative_ridders`
  — error-killing extrapolation returning a value *and* an error estimate) · FD gradient / Jacobian / Hessian-vector
  (`gradient_central`, `hessian_vector`).
- **v13-m — exact + spectral + noisy.** **★★complex-step** `Im[f(x+ih)]/h` (`complex_step` — machine-exact, *zero
  subtractive cancellation*, no h-tuning; the modern best gradient when f accepts complex args) · **★Chebyshev /
  Fourier differentiation matrices** (spectral accuracy, via `crd-hesap-fft` / barycentric) · **★Savitzky-Golay**
  differentiation (`savgol_coeffs` / `savgol_filter` — noisy telemetry rates; coeffs bit-match scipy).

## Determinism

Pure `crd::math` kernels, fixed evaluation order ⇒ bit-identical across compilers/opt-levels/threads; the FD/complex-
step/savgol kernels are `noexcept`/bool-returning and allocation-free (the `{1,4,16}` moat applies).

## Crush

- **complex-step gradient 48.5 ns = 57× JAX-jit / 835× numpy central-difference — AND machine-exact** (matches JAX's
  autodiff accuracy to 0–1e-16; numpy-FD is not exact). The headline: the modern-best gradient, faster than the
  autodiff frameworks *and* exact.
- **`savgol_coeffs` 142 ns = 204× scipy.signal.savgol_coeffs.**
- Fornberg weights reproduce the analytic FD stencils to machine precision; Ridders matches NR `dfridr`; spectral
  differentiation shows exponential convergence.

## CLI (`hesap.diff.*`)

The sample-driven differentiators cross the boundary as data (a function integrand cannot; the callable complex-step/
Ridders drivers stay in-process).

| command | params | output |
|---|---|---|
| `hesap.diff.savgol.f64` | `y`, `window`, `polyorder`, `deriv`, `delta` | blob = filtered/derivative signal |
| `hesap.diff.fornberg.f64` | `nodes`, `z`, `max_deriv` | blob = weight table `c[k*nnodes + i]` |

Anchor `register_diff_cli_anchor()`; `test_cli.cpp` checks the Fornberg central stencils ([0,1,0] / [−½,0,½] /
[1,−2,1]) and the Savitzky-Golay derivative of a line.

## Module edges (acyclic)

`crd-hesap-diff` → core/containers/memory · math · **crd-hesap** (CLI) · **crd-hesap-dense** (Savitzky-Golay normal
equations) · **crd-hesap-fft** (spectral differentiation).

## Tests

`tests/hesap-diff/` — `test_diff` · `test_cli`. 42 + CLI assertions, green on win-debug + the full win-tidy check set.
