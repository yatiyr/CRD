# v12-p peer bench: per-call cost of KDE / Theil-Sen / Huber-M / Ledoit-Wolf / OAS in scipy/statsmodels/sklearn.
import numpy as np
import timeit
from scipy.stats import gaussian_kde, theilslopes
import statsmodels.api as sm
from statsmodels.robust.norms import HuberT
from sklearn.covariance import ledoit_wolf, oas

n = 100
a = 2.0 + 0.7 * np.sin(0.30 * np.arange(n))
x = np.arange(50.0)
y = 2.0 * x + np.sin(x)
mat = (np.sin(0.7 * np.arange(250) + 1.0) + 0.1 * (np.arange(250) % 5)).reshape(50, 5)
ones = np.ones((n, 1))


def bench(name, fn, number=1000):
    for _ in range(50):
        fn()
    t = timeit.timeit(fn, number=number) / number
    print("%-14s %9.1f ns/call" % (name, t * 1e9))


kde = gaussian_kde(a, bw_method="scott")
bench("kde_eval", lambda: kde.evaluate([3.0]))
bench("theil_sen", lambda: theilslopes(y, x))
bench("huber_loc", lambda: sm.RLM(a, ones, M=HuberT()).fit().params[0], number=500)
bench("ledoit_wolf", lambda: ledoit_wolf(mat))
bench("oas", lambda: oas(mat))
