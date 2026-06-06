#!/usr/bin/env python3
# v6-z gold-standard oracle — PRIMME (Stathopoulos-McCombs), the STATE-OF-ART symmetric sparse eigensolver, the
# real crush target for the v6 close. Same 3D Poisson (kronsum, rectangular s,s+3,s+7 ⇒ non-degenerate) and
# matched tol as bench_hesap_eigen_lobpcg.cpp / eigen_ref_scipy.py — so the printed smallest-4 must match Cerid's
# (the accuracy GATE before any cost comparison).
#
# TWO honest configs (the metric depends on the peer's machinery — advisor-locked):
#   (A) PRIMME, NO preconditioner — pure C library ⇒ WALL-CLOCK is comparable to a compiled Cerid run; also
#       prints matvecs. (Smallest SPD with no preconditioner is the hard end ⇒ matvec-heavy.)
#   (B) PRIMME + pyamg AMG preconditioner (OPinv) — the SAME-CLASS head-to-head vs Cerid AMG-LOBPCG. The pyamg
#       V-cycle is Python-driven ⇒ WALL-CLOCK is NOT cross-language comparable; the fair metric here is MATVECS.
#       ⚠ matvecs are not perfectly apples-to-apples either: a LOBPCG iter applies A to a BLOCK of nev vectors,
#       PRIMME's JDQMR/GD+k apply A per inner step — so this is "competitive with the state-of-art", while the
#       pyamg-lobpcg floor (eigen_floor_pyamg.py, identical algorithm AND precond) is the clean "parity" claim.
#
# Honest expectation (advisor-locked, pre-committed as the WIN): PARITY vs same-class PRIMME+AMG (matching a
# mature C library at f64, WITH a cross-thread determinism moat it lacks) + the established CRUSH vs DIRECT
# shift-invert ARPACK. NOT a speed win over PRIMME+AMG. NOT linked into the C++ build.

import sys
import time

import numpy as np
import scipy.sparse as sp
import pyamg
import primme


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
    tol = 1e-7
    print(f"{'s':>4} {'n':>9} {'noprec_mv':>10} {'noprec_s':>9} {'amg_mv':>7} {'amg_s(ctx)':>11} "
          f"{'err':>9} {'smallest4'}")
    for s in ss:
        mx, my, mz = s, s + 3, s + 7
        a = laplacian_3d(mx, my, mz)
        n = a.shape[0]
        exp = analytic_smallest(mx, my, mz, k)

        # (A) PRIMME, no preconditioner — wall-clock comparable to a compiled run.
        t0 = time.perf_counter()
        vals_np, _, st_np = primme.eigsh(a, k=k, which="SA", tol=tol, return_stats=True)
        t_np = time.perf_counter() - t0
        mv_np = st_np["numMatvecs"]

        # (B) PRIMME + pyamg AMG preconditioner — same precond class as Cerid; report MATVECS (pyamg is Python).
        ml = pyamg.smoothed_aggregation_solver(a)
        m_pre = ml.aspreconditioner(cycle="V")
        t1 = time.perf_counter()
        vals_amg, _, st_amg = primme.eigsh(a, k=k, which="SA", tol=tol, OPinv=m_pre, return_stats=True)
        t_amg = time.perf_counter() - t1
        mv_amg = st_amg["numMatvecs"]

        got = sorted(float(v) for v in vals_np)
        got_amg = sorted(float(v) for v in vals_amg)
        err = max(max(abs(g - e) for g, e in zip(got, exp)),
                  max(abs(g - e) for g, e in zip(got_amg, exp)))
        flag = "OK" if err < 1e-6 else f"ERR={err:.1e}"
        print(f"{s:>4} {n:>9} {mv_np:>10} {t_np:>9.3f} {mv_amg:>7} {t_amg:>11.3f} {err:>9.1e} "
              f"{[round(g, 5) for g in got]} {flag}")


if __name__ == "__main__":
    main()
