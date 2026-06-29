# crd-hesap-special — special functions (Phase 3.1.6 v12)

> **Status: SHIPPED (v12 a–d).** The special-function leaf that powers the statistics cdf/ppf engine, Gauss
> quadrature roots/weights, and DSP filter design: gamma/beta/erf families (+ incomplete + inverses), Bessel/Airy,
> classical orthogonal polynomials, and the transcendental tail (elliptic, hypergeometric, Lambert-W, zeta, …).
> Gated ≤ 1e-12 vs scipy.special + Boost + analytic identities + the {1,4,16} determinism moat + f32. Detailed
> story + crush board: `docs/phases/phase-3.1.6-hesap.md` (§ v12-a…d); ADR-0094. Header-only templates (f32/f64/
> complex), lower-layer raw scalars (ADR-0078).

## What it contains

- **Gamma / beta / erf** (`gamma.hpp`/`incomplete.hpp`/`erf.hpp`): gamma/lgamma/digamma/trigamma/polygamma · beta ·
  regularized incomplete gamma `gammainc_p/q` + inverses `gammainc_p_inv/q_inv` · incomplete beta `betainc` + inverse ·
  erf/erfc/erfcx/erfinv/erfcinv · Dawson/Faddeeva/Voigt · **`ndtri`** (Wichura AS-241 probit — the normal ppf). The
  incomplete-gamma/beta inverses ARE the distribution quantile (ppf) engine; cached-`gln`/`lbeta` overloads amortize lgamma.
- **Bessel / Airy** (`bessel.hpp`/`airy.hpp`): cylindrical J/Y/I/K + derivatives + negative & complex orders (Steed/Temme
  continued fractions) · spherical j/y · Hankel · Airy Ai/Bi + deriv · Kelvin ber/bei/ker/kei.
- **Orthogonal polynomials** (`orthopoly.hpp`): Legendre (+associated) · Hermite (physicist/probabilist) · Laguerre
  (+generalized) · Chebyshev T/U/V/W · Gegenbauer · Jacobi — stable 3-term recurrences. (Gauss nodes/weights live in
  `crd-hesap-quadrature` via Golub-Welsch + `dense::eig_sym`.)
- **Transcendental tail** (`expint.hpp`/`elliptic.hpp`/`fresnel.hpp`/`lambertw.hpp`/`zeta.hpp`/`struve.hpp`/`hypergeom.hpp`/
  `marcum.hpp`): E₁/Eₙ/Ei/Si/Ci · Carlson R_F/R_D/R_C/R_J + complete/incomplete elliptic K/E/F/Π + Jacobi sn/cn/dn (the
  **canonical** elliptic home; hesap-dsp delegates here) · Fresnel S/C · Lambert-W₀/W₋₁ · Hurwitz/Riemann ζ (+ζ′) ·
  Struve H/L · ₀F₁/₁F₁/₂F₁ + general pFq · Marcum-Q.

## Crush highlights (single-thread ns/call, accuracy preserved ≤ 1e-12)

**Bessel 24/24** — every function beats Boost + scipy + MATLAB at every threading. **Transcendental tail vs Boost all 7
WIN** (E1 7.98× · Ei 6.55× · zeta 20.5× · Lambert-W 1.17× · ellint_K/E 1.06× · Carlson 1.78×) via generated minimax
rationals. gamma/erf hot paths crushed. The lever: dedicated fast paths (J-only, direct Airy Maclaurin) + minimax
rationals replacing `std::pow` chains.

## CLI (`hesap.special.*`, v12-z)

A curated agent-facing subset: `hesap.special.gamma.f64` (Γ element-wise) · `hesap.special.erf.f64` (erf element-wise).
Registered via `CRD_HESAP_CLI_REGISTER_MODULE` + `cli_anchor.hpp`.

## Edges (acyclic — special is a LEAF)

`crd-core` / `crd-containers` / `crd-memory` / `crd-math` (deterministic transcendentals) · `crd-hesap` (Complex + CLI).
**No edge to crd-hesap-stats** — stats/quadrature/dsp depend on special, never the reverse.
