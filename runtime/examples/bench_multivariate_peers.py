#!/usr/bin/env python3
"""Peer bench for v12-k multivariate — scipy.stats + NumPy, ns/op, matching bench_multivariate.cpp params.
logpdf/logpmf timed VECTORIZED over the whole batch (scipy's strength) / N; rvs timed as .rvs(size=N) / N."""
import time
import numpy as np
from scipy import stats

N = 500000
Nm = 200000


def best_ns(fn, reps=5, n=N):
    fn()  # warm
    t = min(time.perf_counter_ns() and (lambda: (lambda s: (fn(), time.perf_counter_ns() - s)[1])(time.perf_counter_ns()))() for _ in range(reps))
    return t / n


def timed(fn, n, reps=5):
    fn()
    best = float("inf")
    for _ in range(reps):
        s = time.perf_counter_ns()
        fn()
        best = min(best, time.perf_counter_ns() - s)
    return best / n


mean = np.array([1.0, -2.0, 0.5])
cov = np.array([[2.0, 0.5, 0.3], [0.5, 1.5, -0.2], [0.3, -0.2, 1.0]])
rng = np.random.default_rng(1)

# MVN logpdf (batch) + sampling
mvn = stats.multivariate_normal(mean, cov)
pts = rng.multivariate_normal(mean, cov, size=N)
print(f"scipy MVN_logpdf_ns {timed(lambda: mvn.logpdf(pts), N):.3f}")
print(f"numpy MVN_rvs_ns {timed(lambda: rng.multivariate_normal(mean, cov, size=N), N):.3f}")

# MVt logpdf (scipy takes shape == cov here)
mvt = stats.multivariate_t(loc=mean, shape=cov, df=5)
print(f"scipy MVt_logpdf_ns {timed(lambda: mvt.logpdf(pts), N):.3f}")

# Dirichlet logpdf (on the simplex) + sampling
alpha = np.array([2.0, 1.5, 3.0, 0.8])
dpts = rng.dirichlet(alpha, size=N)
dir_ = stats.dirichlet(alpha)
print(f"scipy Dirichlet_logpdf_ns {timed(lambda: dir_.logpdf(dpts.T), N):.3f}")
print(f"numpy Dirichlet_rvs_ns {timed(lambda: rng.dirichlet(alpha, size=N), N):.3f}")

# Multinomial logpmf
p = np.array([0.4, 0.3, 0.2, 0.1])
counts = rng.multinomial(20, p, size=N).astype(float)
mn = stats.multinomial(20, p)
print(f"scipy Multinomial_logpmf_ns {timed(lambda: mn.logpmf(counts), N):.3f}")

# Wishart / inverse-Wishart rvs (sampling-bound)
scale = np.array([[2.0, 0.3, 0.1], [0.3, 1.0, 0.2], [0.1, 0.2, 1.5]])
wis = stats.wishart(df=8, scale=scale)
iwis = stats.invwishart(df=8, scale=scale)
print(f"scipy Wishart_rvs_ns {timed(lambda: wis.rvs(size=Nm), Nm):.3f}")
print(f"scipy InverseWishart_rvs_ns {timed(lambda: iwis.rvs(size=Nm), Nm):.3f}")
