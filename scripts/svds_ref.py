#!/usr/bin/env python3
# v6-z SVD verdict oracle — scipy.sparse.linalg.svds (ARPACK) + primme.svds (state-of-art) vs Cerid IRLBA
# (runtime/examples/bench_hesap_svds.cpp) for the largest singular triplets of the SAME 2D rectangular-grid
# edge-node incidence matrix B (built bit-identically: same edge order = horizontal first then vertical, node
# id = i*gy + j). B's singular values are sqrt(graph-Laplacian eigenvalues). Matched tol; the printed largest-4
# must match Cerid's (the accuracy GATE) before comparing cost.
#
# Honest metric (advisor-locked): WALL-CLOCK (all peers compiled C/Fortran) + matvecs for context. Expected:
# PARITY (all three are Krylov-bidiagonalization) + the {1..16} determinism moat Cerid alone carries. NOT linked.

import sys
import time

import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
import primme


def incidence_2d(gx, gy):
    # node id = i*gy + j; horizontal edges (i outer, j inner) then vertical — SAME order as the C++ bench.
    n = gx * gy
    rows, cols, data = [], [], []
    e = 0

    def nid(i, j):
        return i * gy + j

    for i in range(gx):
        for j in range(gy - 1):
            rows += [e, e]; cols += [nid(i, j), nid(i, j + 1)]; data += [1.0, -1.0]; e += 1
    for i in range(gx - 1):
        for j in range(gy):
            rows += [e, e]; cols += [nid(i, j), nid(i + 1, j)]; data += [1.0, -1.0]; e += 1
    m = e
    return sp.csr_matrix((data, (rows, cols)), shape=(m, n))


def main():
    gs = [int(x) for x in sys.argv[1:]] or [32, 48, 64, 96]
    k = 4
    tol = 1e-7
    print(f"{'gx':>4} {'m':>9} {'n':>9} {'scipy_mv':>9} {'scipy_s':>8} {'primme_mv':>10} {'primme_s':>9} "
          f"{'err':>9} {'largest4'}")
    for gx in gs:
        gy = gx + 3
        b = incidence_2d(gx, gy)
        m, n = b.shape

        t0 = time.perf_counter()
        # scipy.svds (ARPACK on the augmented/normal system); largest by default.
        _u, sa, _vt = spla.svds(b, k=k, tol=tol, which="LM")
        t_sa = time.perf_counter() - t0
        scipy_sv = sorted((float(x) for x in sa), reverse=True)

        t1 = time.perf_counter()
        _us, sp_vals, _vs, stats = primme.svds(b, k=k, tol=tol, which="LM", return_stats=True)
        t_pr = time.perf_counter() - t1
        primme_sv = sorted((float(x) for x in sp_vals), reverse=True)
        primme_mv = stats["numMatvecs"]

        err = max(abs(a - p) for a, p in zip(scipy_sv, primme_sv))
        flag = "OK" if err < 1e-5 else f"ERR={err:.1e}"
        print(f"{gx:>4} {m:>9} {n:>9} {'-':>9} {t_sa:>8.4f} {primme_mv:>10} {t_pr:>9.4f} {err:>9.1e} "
              f"{[round(x, 5) for x in scipy_sv]} {flag}")


if __name__ == "__main__":
    main()
