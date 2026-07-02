#!/usr/bin/env python3
# v6-e-d GENERALIZED same-class floor — pyamg(smoothed-aggregation AMG on K) + scipy.sparse.linalg.lobpcg with
# the generalized mass B=M is EXACTLY Cerid's eigs_sym_gen_lobpcg (AMG-on-K-preconditioned generalized LOBPCG).
# The honest expectation (advisor-gated) is PARITY + the determinism MOAT, NOT a crush: same algorithm ⇒ the fai
# metric is ITERATIONS-TO-TOLERANCE (comparable across implementations). WALL-CLOCK is NOT comparable (scipy's
# lobpcg loop is pure Python) — printed for context only.
#
# CRITICAL: the matrices are reproduced BIT-FOR-BIT identically to bench_hesap_eigen_gen_lobpcg.cpp — the SAME
# node ordering id = (i*my+j)*mz+l (built explicitly, NOT via scipy kronsum, whose ordering would mismatch the
# per-node mass) and the SAME diagonal mass m_node = 1 + ((node*37+11) % 100)/100. So (K, M) is the identical
# generalized problem ⇒ the eigenvalues must match Cerid's printed smallest-4, and the iteration counts are an
# apples-to-apples same-class comparison. NOT linked into the C++ build.

import sys
import time

import numpy as np
import scipy.sparse as sp
import pyamg
from scipy.sparse.linalg import lobpcg


def stiffness_3d(mx, my, mz):
    # 7-point 3D Dirichlet Laplacian, node id = (i*my+j)*mz+l (SAME ordering as the C++ bench). diag 6, nbrs -1.
    n = mx * my * mz

    def idx(i, j, l):
        return (i * my + j) * mz + l

    rows, cols, data = [], [], []
    for i in range(mx):
        for j in range(my):
            for l in range(mz):
                d = idx(i, j, l)
                rows.append(d); cols.append(d); data.append(6.0)
                if i + 1 < mx:
                    rows += [d, idx(i + 1, j, l)]; cols += [idx(i + 1, j, l), d]; data += [-1.0, -1.0]
                if j + 1 < my:
                    rows += [d, idx(i, j + 1, l)]; cols += [idx(i, j + 1, l), d]; data += [-1.0, -1.0]
                if l + 1 < mz:
                    rows += [d, idx(i, j, l + 1)]; cols += [idx(i, j, l + 1), d]; data += [-1.0, -1.0]
    return sp.csr_matrix((data, (rows, cols)), shape=(n, n))


def lumped_mass(n):
    # m_node = 1 + ((node*37+11) % 100)/100 ∈ [1, 2). integer/100.0 ⇒ identical IEEE doubles to the C++ bench.
    d = np.array([1.0 + ((node * 37 + 11) % 100) / 100.0 for node in range(n)])
    return sp.diags(d).tocsr()


def main():
    ss = [int(x) for x in sys.argv[1:]] or [16, 20, 24, 28, 32]
    k_count = 4
    print(f"{'s':>4} {'n':>9} {'pyamg_gen_lobpcg_iters':>22} {'py_s(ctx)':>10} {'smallest4'}")
    for s in ss:
        mx, my, mz = s, s + 3, s + 7  # rectangular ⇒ non-degenerate
        kk = stiffness_3d(mx, my, mz)
        mm = lumped_mass(mx * my * mz)
        n = kk.shape[0]
        ml = pyamg.smoothed_aggregation_solver(kk)  # AMG on the stiffness K = Cerid SaAmg's class
        m_pre = ml.aspreconditioner(cycle="V")
        rng = np.random.default_rng(0)
        x0 = rng.standard_normal((n, k_count))
        t0 = time.perf_counter()
        # GENERALIZED: A=K, B=M (the mass), M=preconditioner (AMG on K) — EXACTLY Cerid's eigs_sym_gen_lobpcg.
        vals, _vecs, hist = lobpcg(kk, x0, B=mm, M=m_pre, tol=1e-7, largest=False, maxiter=500,
                                   retResidualNormsHistory=True)
        t1 = time.perf_counter()
        iters = len(hist)
        got = sorted(float(v) for v in vals)
        print(f"{s:>4} {n:>9} {iters:>22} {t1 - t0:>10.3f} {[round(g, 6) for g in got]}")


if __name__ == "__main__":
    main()
