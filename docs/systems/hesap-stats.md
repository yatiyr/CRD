# crd-hesap-stats — statistics substrate (Phase 3.1.6 v12)

> **Status: SHIPPED (v12 cluster a→r).** The full statistics stack: counter-based RNG, ~50 probability
> distributions, descriptive statistics, the classical hypothesis-test suite, resampling, KDE/robust/streaming,
> MCMC, and regression/GLM/multivariate. Gold-standard gated vs scipy/statsmodels/sklearn/ArviZ/PyMC and bit-for-bit
> where deterministic. Detailed slice-by-slice story + crush board: `docs/phases/phase-3.1.6-hesap.md` (§ v12);
> ADR-0094. Header-only templates (f32/f64) over `crd-containers`/`crd-memory`.

## What it contains

- **RNG suite** (`philox.hpp`/`pcg64.hpp`/`threefry.hpp`/`xoshiro256.hpp`/`sfc64.hpp`/`splitmix64.hpp`/`mt19937.hpp`/
  `chacha.hpp`): counter-based (Philox4x32 / Threefry4x64 — seekable) + classic generators, all KAT/NumPy-state gated,
  with AVX2 bulk fill bit-identical to scalar. **The determinism moat**: a counter generator is a pure function of
  (counter, key) — same seed ⇒ bit-identical, independent of execution order or worker count.
- **Samplers + QMC** (`samplers.hpp`/`ziggurat.hpp`/`qmc.hpp`): Ziggurat normal/exp · Marsaglia-Tsang gamma · beta · χ² ·
  Poisson (Knuth+PTRS) · binomial (BINV+BTPE, stateful `BinomialSampler`) · Vose alias · reservoir; Sobol/Halton/lattice/LHS.
- **Distributions** (`distribution.hpp` + `continuous.hpp` 25 / `discrete.hpp` 12 / `heavy_tail.hpp` 8 / `multivariate.hpp` 7):
  the `Distribution<T>` CRTP surface (pdf/logpdf/cdf/sf/ppf/isf/rvs + moments/entropy). CDFs ride hesap-special
  erf/gammainc/betainc + inverses; multivariate rides `dense::factor_cholesky`. **Analytic log-density gradients**
  (`log_density_grad.hpp`) ∂logp/∂x + ∂logp/∂θ for every distribution — the HMC/NUTS + MLE enabler.
- **Descriptive** (`descriptive.hpp`): moments (scipy conventions + robust MAD/trimmed) · 9 R quantile types + Harrell-Davis ·
  ECDF · histogram bin-rules · weighted · covariance/correlation.
- **Hypothesis tests** (`hypothesis.hpp`): the full classical suite — t/z/Welch/ANOVA(1-way·2-way·repeated)/Bartlett/Levene ·
  Mann-Whitney/Wilcoxon/Kruskal-Wallis/Friedman/sign/Mood · KS/AD/CvM/Shapiro/JB/D'Agostino/Lilliefors · χ²/Fisher/McNemar/G ·
  Pearson/Spearman/Kendall/dCor · Tukey-HSD/Games-Howell/Scheffé/Dunnett/Holm/BH/Bonferroni · Cohen's d/η²/Cramér's V.
- **Resampling** (`resampling.hpp` + `resampling_parallel.hpp`): bootstrap (percentile/basic/BCa/studentized) · block bootstrap ·
  jackknife (+delete-d) · permutation · CV — parallel over crd-jobs, bit-identical to serial (the moat under threading).
- **KDE / robust / streaming** (`kde.hpp`/`robust.hpp`/`cov_robust.hpp`/`streaming.hpp`/`tdigest.hpp`): Gaussian/Epanechnikov KDE ·
  Theil-Sen/Hodges-Lehmann/Huber/Tukey-M · Ledoit-Wolf/OAS/exact-MCD covariance · Welford/P²/t-digest/HyperLogLog/count-min.
- **MCMC** (`mcmc.hpp` + `mcmc_diagnostics.hpp`): Metropolis/adaptive-Haario/Gibbs/HMC/**NUTS (+dual-averaging = full Stan)**/
  slice/SMC + diagnostics (rank-normalized R-hat / Geyer bulk-ESS / autocorrelation / Geweke, bit-for-bit vs ArviZ).
- **Regression / GLM / multivariate** (`regression.hpp`): OLS/WLS/GLS · ridge/lasso/elastic-net · GLM-IRLS (logistic/Poisson/
  gamma) · robust-Huber/quantile/RANSAC · PCA/LDA/QDA/factor-analysis — riding the shipped `dense::lstsq`/`pinv`/`eig_sym`
  (SANITY 8: reuse, no reimplementation).

## Crush highlights (native C++ vs the Python/MATLAB peers)

RNG every-same-generator win vs NumPy+MATLAB · distributions vs scipy 16/16 + MATLAB 16/16 (to 20×) · multivariate all-7
crush · bootstrap 2.08× scipy serial / 5.05× parallel · KDE 5872× / OAS 437× · **NUTS 104× effective-samples/sec vs PyMC** ·
regression **Ridge 47× / PCA 23× / OLS 17× vs sklearn**. The lever throughout: ctor/precompute-amortized special functions
and shipped factorizations with zero per-call interpreter overhead.

## CLI (`hesap.stats.*`, v12-z)

A curated agent-facing subset (the v7-z/v10-z/v11-z data-vs-callable split): `hesap.stats.describe.f64`
(mean/var/skew/kurt) · `hesap.stats.ttest_1samp.f64` (statistic/pvalue/df). Registered via
`CRD_HESAP_CLI_REGISTER_MODULE` + `cli_anchor.hpp`.

## Edges (acyclic)

`crd-core` / `crd-containers` / `crd-memory` / `crd-math` · `crd-hesap` (CLI + Complex) · `crd-hesap-special`
(gamma/erf/Bessel + CDFs; special is a leaf) · `crd-hesap-dense` (Cholesky/lstsq/pinv/eig_sym — dense never references
stats) · `crd-hesap-quadrature` (Gauss-Hermite/Laguerre for the studentized range) · `crd-jobs` (parallel resampling).
