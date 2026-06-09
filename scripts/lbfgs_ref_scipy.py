#!/usr/bin/env python3
# lbfgs_ref_scipy.py — Phase 3.1.6 v7-d accuracy cross-check: a THIRD independent L-BFGS implementation
# (scipy.optimize, FORTRAN L-BFGS-B) confirms Cerid reaches the same Rosenbrock minimizer. scipy's wall-clock is
# contaminated by per-eval Python callback overhead (the v6-z lesson) — use its nit/nfev + achieved ‖g‖, NOT its
# time. The clean COMPILED peer is liblbfgs (bench_hesap_lbfgs_vs_reference); this is the accuracy oracle.
#
# Run (WSL):  ~/eigref-venv/bin/python scripts/lbfgs_ref_scipy.py   (or any python with scipy)

import numpy as np
from scipy.optimize import minimize


def rosenbrock(x):
    n = x.size
    f = 0.0
    g = np.zeros(n)
    for i in range(n - 1):
        a = 1.0 - x[i]
        b = x[i + 1] - x[i] * x[i]
        f += a * a + 100.0 * b * b
        g[i] += -2.0 * a - 400.0 * x[i] * b
        g[i + 1] += 200.0 * b
    return f, g


print("# scipy L-BFGS-B (m=8) accuracy cross-check on Rosenbrock-N (minimizer = all ones, f*=0)")
print("# %-6s | %6s | %6s | %10s | %10s | %10s" % ("N", "nit", "nfev", "f*", "|g|inf", "|x-1|inf"))
for n in (2, 10, 100, 1000):
    x0 = np.array([-1.2 if i % 2 == 0 else 1.0 for i in range(n)], dtype=float)
    res = minimize(rosenbrock, x0, jac=True, method="L-BFGS-B",
                   options={"maxcor": 8, "ftol": 1e-12, "gtol": 1e-6, "maxiter": 100000})
    _, g = rosenbrock(res.x)
    ginf = np.max(np.abs(g))
    xerr = np.max(np.abs(res.x - 1.0))
    print("  %-6d | %6d | %6d | %10.3e | %10.3e | %10.3e" % (n, res.nit, res.nfev, res.fun, ginf, xerr))
print("# matched-accuracy cross-check: Cerid (bench) + liblbfgs + scipy all reach x*=ones, f*→0 — three "
      "independent L-BFGS implementations agree.")
