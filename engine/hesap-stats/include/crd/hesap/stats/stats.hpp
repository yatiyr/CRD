#pragma once

// crd-hesap-stats umbrella — Phase 3.1.6. STATISTICS substrate (the v12 cluster's module, created EARLY as a
// v12-pull: the Philox counter-based RNG was needed by v7-i for reproducible minibatch sampling with the full
// determinism moat, and pulling the tiny frozen-interface primitive forward beat shipping v7-i with an
// asterisk). The rest of the v12 stats cluster (distributions, estimators, hypothesis tests, ...) grows here.

// v12-e — the BitGenerator RNG suite (counter-based + classic), all bit-exact-gated vs published KATs / NumPy.
#include <crd/hesap/stats/bitgen.hpp>
#include <crd/hesap/stats/mt19937.hpp>
#include <crd/hesap/stats/pcg64.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/stats/sfc64.hpp>
#include <crd/hesap/stats/splitmix64.hpp>
#include <crd/hesap/stats/threefry.hpp>
#include <crd/hesap/stats/xoshiro256.hpp>

// v12-f — fast distribution samplers (ziggurat normal/exp · gamma · beta · Poisson · binomial · alias · reservoir).
#include <crd/hesap/stats/samplers.hpp>
#include <crd/hesap/stats/ziggurat.hpp>

// v12-g — QMC low-discrepancy sequences (Sobol · Halton · lattice · LHS) + ChaCha20 crypto RNG.
#include <crd/hesap/stats/chacha.hpp>
#include <crd/hesap/stats/qmc.hpp>

// v12-h/i — the Distribution<T> framework + ~25 univariate continuous + 12 discrete distributions (full
// pdf/logpdf/cdf/sf/ppf/rvs/moments/entropy surface, gated vs scipy.stats; CDFs ride hesap-special).
#include <crd/hesap/stats/continuous.hpp>
#include <crd/hesap/stats/discrete.hpp>
#include <crd/hesap/stats/distribution.hpp>

// v12-j — heavy-tail / extreme-value / noncentral distributions (GEV · GPD · Lévy · BetaPrime · NoncentralChiSquared).
#include <crd/hesap/stats/heavy_tail.hpp>

// v12-k — multivariate distributions over the shipped Cholesky (MVN · MVt · Dirichlet · Wishart · invWishart · LKJ · multinomial).
#include <crd/hesap/stats/multivariate.hpp>
