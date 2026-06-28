# v12-o resampling peer bench: scipy.stats.bootstrap wall-time for a percentile CI of the mean (n=100, B resamples).
import numpy as np
import timeit
from scipy.stats import bootstrap

n = 100
data = 2.0 + 0.7 * np.sin(0.30 * np.arange(n))
B = 100000


def run():
    rng = np.random.default_rng(12345)
    return bootstrap((data,), np.mean, n_resamples=B, method="percentile", random_state=rng).confidence_interval


run()  # warmup
t = timeit.timeit(run, number=10) / 10
ci = run()
print("scipy.stats.bootstrap   %8.3f ms  CI=[%.6f, %.6f]" % (t * 1000, ci.low, ci.high))
