#!/usr/bin/env python3
# v6 SAME-CLASS FLOOR — pyamg(smoothed-aggregation AMG) + scipy.sparse.linalg.lobpcg is EXACTLY Cerid's method
# (AMG-preconditioned LOBPCG). The honest expectation (advisor-gated) is PARITY + the determinism MOAT, NOT a
# crush: same algorithm ⇒ the fair metric is ITERATIONS-TO-TOLERANCE (comparable across implementations).
# WALL-CLOCK is NOT comparable here — scipy's lobpcg loop is pure Python — so it is printed for context only,
# never as a claim. Same 3D rectangular Poisson grid (s, s+3, s+7) as bench_hesap_eigen_lobpcg.cpp.
#
# Reads: Cerid AMG-LOBPCG converges the smallest 4 in ~20-25 iters (its bench prints `iters`). This script
# prints pyamg+scipy's iteration count on the identical problem; comparable counts ⇒ Cerid's AMG-LOBPCG is a
# sound, competitive implementation (the floor), and Cerid additionally carries the {1,2,4,8} moat scipy/pyamg
# do not. NOT linked into the C++ build.

import sys
import time

import numpy as np
import scipy.sparse as sp
import pyamg
from scipy.sparse.linalg import lobpcg


def tri(m):
    return sp.diags([-1.0, 2.0, -1.0], [-1, 0, 1], shape=(m, m))


def laplacian_3d(mx, my, mz):
    return sp.kronsum(sp.kronsum(tri(mx), tri(my)), tri(mz)).tocsr()


def lam1d(m):
    return [2.0 - 2.0 * np.cos((i + 1) * np.pi / (m + 1)) for i in range(min(m, 6))]


def analytic_smallest(mx, my, mz, k):
    return sorted(a + b + c for a in lam1d(mx) for b in lam1d(my) for c in lam1d(mz))[:k]


def main():
    ss = [int(x) for x in sys.argv[1:]] or [16, 20, 24, 28, 32]
    k = 4
    print(f"{'s':>4} {'n':>9} {'pyamg_lobpcg_iters':>18} {'py_s(ctx)':>10} {'smallest4'}")
    for s in ss:
        mx, my, mz = s, s + 3, s + 7
        a = laplacian_3d(mx, my, mz)
        n = a.shape[0]
        ml = pyamg.smoothed_aggregation_solver(a)  # = Cerid SaAmg's class
        m_pre = ml.aspreconditioner(cycle="V")
        rng = np.random.default_rng(0)
        x0 = rng.standard_normal((n, k))
        t0 = time.perf_counter()
        vals, _vecs, hist = lobpcg(a, x0, M=m_pre, tol=1e-7, largest=False, maxiter=500,
                                   retResidualNormsHistory=True)
        t1 = time.perf_counter()
        iters = len(hist)
        got = sorted(float(v) for v in vals)
        exp = analytic_smallest(mx, my, mz, k)
        err = max(abs(g - e) for g, e in zip(got, exp))
        flag = "OK" if err < 1e-6 else f"ERR={err:.1e}"
        print(f"{s:>4} {n:>9} {iters:>18} {t1 - t0:>10.3f} {[round(g, 5) for g in got]} {flag}")


if __name__ == "__main__":
    main()
