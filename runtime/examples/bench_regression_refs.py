# v12-r regression peer bench: sklearn / statsmodels per-fit throughput on the same 200x8 design.
import time
import numpy as np
from sklearn.linear_model import LinearRegression, Ridge, Lasso, LogisticRegression
from sklearn.decomposition import PCA

n, p = 200, 8
s = np.uint64(12345)
def rnd():
    global s
    s = np.uint64(s * np.uint64(6364136223846793005) + np.uint64(1442695040888963407))
    return float(s >> np.uint64(11)) / float(1 << 53)
X = np.zeros((n, p)); y = np.zeros(n); yb = np.zeros(n)
for i in range(n):
    yi = 0.5
    for j in range(p):
        v = rnd() * 2 - 1; X[i, j] = v; yi += 0.3 * v
    y[i] = yi + 0.1 * (rnd() * 2 - 1); yb[i] = 1.0 if yi > 0.5 else 0.0

def bench(name, reps, fn):
    fn()
    t0 = time.perf_counter()
    for _ in range(reps):
        fn()
    us = (time.perf_counter() - t0) / reps * 1e6
    print("%-12s %9.2f us/fit   %12.0f fits/s" % (name, us, 1e6 / us))

bench("OLS", 2000, lambda: LinearRegression(fit_intercept=False).fit(X, y).coef_)
bench("Ridge", 2000, lambda: Ridge(alpha=1.0, fit_intercept=False).fit(X, y).coef_)
bench("Lasso", 500, lambda: Lasso(alpha=0.01, fit_intercept=False, max_iter=100000).fit(X, y).coef_)
bench("GLM-logit", 500, lambda: LogisticRegression(fit_intercept=False, C=1e12, max_iter=1000).fit(X, yb).coef_)
bench("PCA", 2000, lambda: PCA(n_components=p).fit(X).explained_variance_)
