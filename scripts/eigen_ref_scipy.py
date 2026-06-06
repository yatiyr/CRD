#!/usr/bin/env python3
# v6-close crush oracle — the gold-standard SYMMETRIC sparse eigensolver baseline for crd-hesap-eigen.
#
# Peer = ARPACK shift-invert (scipy.sparse.linalg.eigsh with sigma=), the config a competent user actually runs
# for the SMALLEST eigenvalues of an SPD matrix (plain/SM ARPACK is glacial and nobody uses it). The honest
# cross-library metrics are (1) the SuperLU factor fill-in = nnz(L)+nnz(U) — LANGUAGE-INDEPENDENT memory, the
# heart of the "direct factorization fill-in is the killer in 3D" crush argument; and (2) wall-clock (ARPACK is
# Fortran + SuperLU is C, so the Python driver overhead is negligible at scale). Regime = the 3D Laplacian
# (7-point), where 3D fill-in grows ~O(n^(4/3)) and direct factorization blows up — the regime where an
# AMG-preconditioned matrix-free eigensolver is expected to win.
#
# This prints a table; the Cerid AMG-LOBPCG bench prints the matching rows, and the comparison is done by hand
# (eigenvalues must match = same accuracy; then compare factor-nnz/row and time). NOT linked into the C++ build.

import sys
import time

import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla


def tri(m):
    return sp.diags([-1.0, 2.0, -1.0], [-1, 0, 1], shape=(m, m))


def laplacian_3d(mx, my, mz):
    # 7-point 3D Laplacian on a RECTANGULAR mx*my*mz grid (distinct dims ⇒ NON-degenerate smallest modes,
    # so eigsh and LOBPCG resolve identical distinct eigenvalues). diag 6, 6 neighbours -1. n = mx*my*mz.
    return sp.kronsum(sp.kronsum(tri(mx), tri(my)), tri(mz)).tocsr()


def lam1d(m):
    return [2.0 - 2.0 * np.cos((i + 1) * np.pi / (m + 1)) for i in range(min(m, 6))]


def analytic_smallest(mx, my, mz, k):
    vals = sorted(a + b + c for a in lam1d(mx) for b in lam1d(my) for c in lam1d(mz))
    return vals[:k]


def main():
    ss = [int(x) for x in sys.argv[1:]] or [16, 20, 24, 28, 32]
    k = 4
    tol = 1e-7  # MATCHED to the Cerid LOBPCG residual tolerance (fair-fight)
    print(f"{'s':>4} {'n':>9} {'eigsh_SI_s':>11} {'splu_s':>8} {'factor_nnz':>13} {'nnz/row':>9} {'smallest4'}")
    for s in ss:
        mx, my, mz = s, s + 3, s + 7  # rectangular ⇒ non-degenerate
        a = laplacian_3d(mx, my, mz)
        n = a.shape[0]
        t0 = time.perf_counter()
        vals = spla.eigsh(a, k=k, sigma=0.0, which="LM", tol=tol, return_eigenvectors=False)
        t1 = time.perf_counter()
        # the factorization fill-in eigsh-SI pays internally (one splu of A - sigma*I); time it alone to show
        # eigsh-SI is FACTORIZATION-dominated in 3D ⇒ the wall-clock is robust to the eigen tolerance.
        ts = time.perf_counter()
        lu = spla.splu(a.tocsc())
        te = time.perf_counter()
        fill = int(lu.L.nnz + lu.U.nnz)
        got = sorted(float(v) for v in vals)
        exp = analytic_smallest(mx, my, mz, k)
        err = max(abs(g - e) for g, e in zip(got, exp))
        flag = "OK" if err < 1e-6 else f"ERR={err:.1e}"
        print(f"{s:>4} {n:>9} {t1 - t0:>11.3f} {te - ts:>8.3f} {fill:>13} {fill / n:>9.1f} "
              f"{[round(g, 5) for g in got]} {flag}")


if __name__ == "__main__":
    main()
