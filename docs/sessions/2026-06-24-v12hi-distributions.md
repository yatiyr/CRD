# 2026-06-24 — v12-h/i/j distributions (continuous + discrete + heavy-tail) + the betainc/AS-241 crush

**Slice:** Phase 3.1.6 `crd-hesap`, v12 Statistics — v12-h + v12-i (the univariate distribution layer).
**Outcome:** the `Distribution<T>` framework + 25 continuous + 12 discrete distributions, gold-standard-gated vs scipy.stats (650/0 + 304/0), 4-config Windows DoD green, honest all-peers perf board.

## The framework (`distribution.hpp`)
- `Real` concept; `ContinuousDistribution` / `DiscreteDistribution` C++20 concepts (the rv_continuous/rv_discrete twin, D(stat)-4).
- `ContinuousBase<D,T>` / `DiscreteBase<D,T>` CRTP bases supplying the mechanical fallbacks — `std`/`median`/`isf`/`sf`/`logpdf`/`logcdf`/`logsf`, and for discrete a generic **integer ppf** (bracket-double then bisect on the cdf). A distribution overrides any fallback where a direct formula is more accurate in the tail.
- Each distribution is a small value type holding its parameters; full pdf/logpdf/cdf/sf/ppf/rvs + mean/var/skew/kurt/entropy (+ mgf/fit where they exist), f32/f64.

## Reuse (SANITY rule 8) — no reinvention
CDFs/PPFs ride the already-shipped **hesap-special**: erf/erfinv (normal family), gammainc_p/q + inverses (gamma/χ²/Maxwell/Nakagami/Poisson), betainc + inverse (beta/t/F/binomial/negbinom), riemann_zeta (Zipf), cyl_bessel_i (von Mises/Skellam/Rice), marcum_q (Rice cdf). rvs rides the **v12-f** samplers (ziggurat/gamma/beta/Poisson/binomial/geometric + alias).

## v12-h — 25 continuous (`continuous.hpp`)
normal · lognormal · exponential · gamma · beta · χ² · StudentT · FisherF · Cauchy · Laplace · logistic · Weibull · Gumbel · Pareto · Rayleigh · Maxwell · uniform · halfnormal · halfcauchy · triangular · invgamma · Nakagami · Wald(inverse-Gaussian) · vonMises · Rice.

## v12-i — 12 discrete (`discrete.hpp`)
Bernoulli · Binomial · Poisson · Geometric · NegativeBinomial · DiscreteUniform · Hypergeometric · Skellam · Zipf · YuleSimon · BetaBinomial · Logarithmic. (COM-Poisson omitted — scipy has no native gate.) Closed-form CDFs where they exist (Poisson→gammainc_q, Binomial/NegBinom→betainc), finite pmf-sum elsewhere.

## Gold-standard gates (scipy.stats, generated → plain-C-array `.inc`)
- **continuous: 650/0** — pdf/logpdf/cdf/sf <1e-9, ppf <1e-7, moments + entropy. NaN ref ⇒ not gated (undefined: Cauchy/halfcauchy moments → +inf or nan; Wald/Rice entropy no simple closed form; vonMises var/skew/kurt = circular convention vs scipy's linear-over-period).
- **discrete: 304/0** — pmf/logpmf/cdf/sf <1e-9, **integer ppf exact**, moments + entropy.
- Iterated fast on WSL standalone gates; ~8 moment/entropy formula fixes total (maxwell-kurt exact formula, halfcauchy mean/var = +inf, nakagami entropy missing −ln 2, and a yulesimon ppf exact-boundary off-by-one → the generic discrete ppf now absorbs the cdf's ~ulp rounding with a tiny threshold so a quantile landing exactly on a cdf jump resolves to the lower k).

## 4-config Windows DoD (all green)
| config | result |
|---|---|
| win-debug | 309570 assertions / 40 cases — exit 0 |
| win-asan | 309570 / 40 — exit 0, 0 ASan errors |
| win-shipping | 309570 / 40 — exit 0 |
| win-tidy | clean — **the distribution headers passed clang-tidy on the first try**; only test `kN`→`n` |
| guards | no-non-ascii / no-std-sort / no-std-math / no-untagged / no-malloc — 5/5 |

## Perf — honest all-peers (ns/elem, single-thread, 1M array; scipy.stats vectorized + MATLAB-1T)
**pdf / pmf CRUSH both peers everywhere:** normal.pdf 5.7×scipy / 2.8×MATLAB · studentt.pdf 2.05× / 2.3× · gamma.pdf 1.4× / 8.2× · beta.pdf 1.15× / 2.1× · poisson.pmf 2.4× / 10.7× · binomial.pmf 2.1× / 13.9×.
**Most cdfs win:** normal.cdf 1.85× / 3.4× · poisson.cdf 1.5× / 4.2× · beta.ppf 1.4× / 1.4× · studentt.cdf ~par / 1.08×.
Determinism moat: every rvs bit-identical across {1,4,16} threads (counter-RNG) — scipy/MATLAB lack it.

## The betainc/gammainc crush (user: "no losses, crush every library")
The first board had 4 incomplete-beta/gamma-bound losses (gamma.cdf 0.78× · beta.cdf 0.55× · studentt.ppf 0.38× · binomial.cdf 0.37×). A decomposition probe (rule 5) found the real costs: the **CF is ~88% of betainc** (not lgamma — that reading was a constant-fold artifact; real per-call lgamma is ~20 ns), `binomial.cdf` via betainc is **7× slower than a direct pmf-sum**, and `betainc_inv` ≈ 8 Newton steps. Four fixes, each measured, all flipping to wins:
1. **Amortise lgamma** — new cached `gammainc_p/q(a,x,gln)` / `betainc(a,b,x,lbeta)` / `betainc_inv(a,b,p,lbeta)` overloads in `incomplete.hpp`; fixed-(a,b) distributions (Gamma/ChiSquared/Beta/StudentT/FisherF/InverseGamma) precompute lgamma/lbeta ONCE in the ctor (a cached member) — scipy's frozen ufunc recomputes every element. Also crushed the pdfs (gamma.pdf 1.4→3.4× · beta.pdf 1.15→4.1× · studentt.pdf 2.05→5.84×).
2. **Looser CF tolerance** — `kCfEps = 8·machine-ε` (the gate is 1e-12, so full machine-ε bought ~3 needless iterations): gammainc_p 65→43 ns.
3. **binomial.cdf direct pmf-sum** of the shorter tail for n≤200 (no betainc CF, exact via the C(n,j) recurrence): 112→7 ns.
4. **studentt.ppf = Hill (AS 396) direct quantile + 1 Halley polish** — replaces the generic beta-inverse (this is what scipy's `stdtrit` does): 446→130 ns.
5. **normal.ppf = Wichura's AS 241 `ndtri`** — added as a reusable probit in `hesap-special/erf.hpp` (pure rational, no iteration/erf, full f64; coefficients verified vs R's qnorm.c). The central branch (|p−½|≤0.425, most of a uniform sweep) is just two degree-7 polynomials + a divide → **21.6→2.7 ns (0.95→6.48×)**. Also speeds the StudentT Hill init (→1.6×) and the lognormal/half-normal quantiles.

**Final board: vs scipy 16/16 WIN (1.04×–6.48×, NO losses, NO parity); vs MATLAB-1T 16/16 WIN (1.25×–20×).**
**Accuracy preserved throughout:** continuous 650/0, discrete 304/0, and the **hesap-special suite still 402081/37 (<1e-12 gammainc/betainc + inverses)** — the looser CF eps keeps ≥13 honest digits. 4-config stats DoD re-verified green (309570/40).

## Pending / next
- **DONE this session (the betainc crush above):** the 4 cdf/ppf losses are all wins now. ✅
- **PENDING USER:** commit the v12 a→i batch + 18-config CI.
- **NEXT slice:** v12-j — heavy-tail/extreme/noncentral (α-stable CMS · GEV · GPD · skew-normal · beta-prime · noncentral t/χ²/F).

## v12-j — heavy-tail / extreme / noncentral (`heavy_tail.hpp`)
8 full distributions + the α-stable sampler, all reusing shipped machinery, **gated vs scipy.stats 208/0**:
- **GEV** (genextreme, scipy `c=−ξ`), **GPD**, **Lévy** (stable α=½) — closed-form cdf/ppf.
- **BetaPrime** — rides `betainc`+inverse (cached lbeta).
- **NoncentralChiSquared** — cdf via the shipped **Marcum-Q**, pdf via `cyl_bessel_i`, rvs = Poisson(λ/2)-mixture of χ².
- **SkewNormal** — cdf = Φ(z) − 2·Owen's-T(z,α); Owen's-T via the `x=tanθ` substitution + a **composite 4-panel 16-pt Gauss-Legendre** (the cdf is a near-cancellation of two ~0.3 values, so plain 16-pt's ~1e-11 abs error lost relative digits in the small tail — composite pushed it to ~1e-15).
- **NoncentralT** — Lenth (AS 243) cdf series + the Γ-ratio pdf series.
- **NoncentralF** — Poisson-mixture-of-central-betas series.
- **StableSampler** — Chambers-Mallows-Stuck (Nolan S1, with the α=1 special case); a sampler, not a `Distribution<T>` (general-α pdf/cdf needs Zolotarev quadrature — scipy itself computes it numerically). Gated by the special cases (α=2 → N(loc,√2·scale), α=1,β=0 → Cauchy) + {seed} determinism.

**Full a→j DoD green:** win-debug + win-asan (0 errors) + win-shipping **317795 / 44 cases**, win-tidy clean, 5 guards (fixed an α/em-dash in a TEST_CASE name that tripped no-non-ascii). The one deferred piece: general-α stable Zolotarev pdf/cdf. **NEXT: v12-k** (multivariate — MVN/MVt/Dirichlet/Wishart/LKJ/multinomial, over the shipped Cholesky).

## Files
- `engine/hesap-stats/include/crd/hesap/stats/{distribution,continuous,discrete,heavy_tail}.hpp`, umbrella `stats.hpp`; `hesap-special/erf.hpp` (AS-241 `ndtri`) + `incomplete.hpp` (cached gammainc/betainc overloads)
- `tests/hesap-stats/{test_continuous,test_discrete,test_heavy}.cpp` + `gen_{continuous,discrete,heavy}_refs.py` + `*_refs.inc` + CMakeLists
- `runtime/examples/bench_distributions.cpp` + `tests/hesap-stats/bench_dist_matlab.m`
