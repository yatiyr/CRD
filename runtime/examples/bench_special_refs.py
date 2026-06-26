#!/usr/bin/env python3
"""scipy.special (vectorized) timing for the v12-a special-function board. Same N + ranges as the C++ bench.
Run: python3 runtime/examples/bench_special_refs.py"""
import time
import numpy as np
from scipy import special as sp

N = 4_000_000
rng = np.random.default_rng(12345)
xe = rng.uniform(-4, 4, N)
xg = rng.uniform(0.1, 50, N)
xy = rng.uniform(-0.99, 0.99, N)
xa = rng.uniform(0.05, 15, N)


def t(name, f, x, reps=3):
    best = 1e30
    acc = 0.0
    for _ in range(reps):
        t0 = time.perf_counter()
        r = f(x)
        t1 = time.perf_counter()
        acc += float(np.sum(r))
        best = min(best, (t1 - t0) / N * 1e9)
    print(f"{name:<16}  scipy {best:8.2f} ns/elem")
    return best


print("# scipy.special vectorized, N =", N)
t("erf", sp.erf, xe)
t("erfc", sp.erfc, xe)
t("erfinv", sp.erfinv, xy)
t("lgamma", sp.gammaln, xg)
t("tgamma", sp.gamma, xg)
t("digamma", sp.digamma, xg)
t("gammainc_p", lambda x: sp.gammainc(2.5, x), xa)
