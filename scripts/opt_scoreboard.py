#!/usr/bin/env python3
# v7-z — the gold-standard scoreboard, REFERENCE side. Runs each peer on the SAME formula-pinned problems as
# runtime/examples/opt_scoreboard.cpp (the Cerid side) and prints objective / eval-count / wall-clock rows.
# Peers (all pip --user): scipy (NM/Powell/trust-*/linprog-HiGHS/DE/basinhopping), osqp, quadprog, scs,
# highspy (MIP), cma (pycma). IPOPT row pending the sudo install (scripts/setup-ipopt-ref.sh).
# Run on WSL: python3 scripts/opt_scoreboard.py
import time

import numpy as np

PI2 = 2.0 * np.pi


def timeit(fn, repeats=3):
    best = float("inf")
    out = None
    for _ in range(repeats):
        t0 = time.perf_counter()
        out = fn()
        best = min(best, time.perf_counter() - t0)
    return out, best * 1e3  # ms


# ---------------------------------------------------------------- the formula-pinned QP (n=30, m=40)
def qp_data():
    n, m = 30, 40
    P = np.zeros((n, n))
    for i in range(n):
        for j in range(n):
            P[i, j] = 10.0 if i == j else 1.0 / (1.0 + abs(i - j))
    q = np.array([np.sin(i + 1.0) for i in range(n)])
    A = np.array([[np.cos((k + 1.0) * (j + 1.0) / 10.0) for j in range(n)] for k in range(m)])
    l = np.array([-1.0 - (k % 3) for k in range(m)])
    u = np.array([1.0 + (k % 5) * 0.5 for k in range(m)])
    return P, q, A, l, u


def row_qp():
    import osqp
    import quadprog
    from scipy import sparse

    P, q, A, l, u = qp_data()
    prob = osqp.OSQP()
    prob.setup(sparse.csc_matrix(P), q, sparse.csc_matrix(A), l, u, verbose=False,
               eps_abs=1e-8, eps_rel=1e-8)
    res, ms = timeit(lambda: prob.solve())
    print(f"QP  osqp      : obj {res.info.obj_val:+.9f}  iters {res.info.iter:5d}  {ms:8.2f} ms")

    # quadprog: min 1/2 x'Gx - a'x s.t. C'x >= b. Two-sided rows become [A; -A] with [l; -u].
    G = P.copy()
    a = -q
    C = np.vstack([A, -A]).T
    b = np.concatenate([l, -u])
    (xq, fq, *_), ms = timeit(lambda: quadprog.solve_qp(G, a, C, b))
    obj = 0.5 * xq @ P @ xq + q @ xq
    print(f"QP  quadprog  : obj {obj:+.9f}  (dual active-set)        {ms:8.2f} ms")


# ---------------------------------------------------------------- the formula-pinned LP (n=40, m=25)
def lp_data():
    n, m = 40, 25
    c = np.array([np.cos(j + 1.0) for j in range(n)])
    A = np.array([[np.sin((k + 2.0) * (j + 1.0) / 7.0) + 1.1 for j in range(n)] for k in range(m)])
    u = np.array([5.0 + (k % 7) for k in range(m)])
    return c, A, u


def row_lp():
    from scipy.optimize import linprog

    c, A, u = lp_data()
    n = len(c)
    res, ms = timeit(lambda: linprog(c, A_ub=A, b_ub=u, bounds=[(-1.0, 2.0)] * n, method="highs"))
    print(f"LP  scipy-HiGHS: obj {res.fun:+.9f}  nit {res.nit:5d}            {ms:8.2f} ms")


# ---------------------------------------------------------------- the SOCP norm ball (analytic)
def row_socp():
    import scs
    from scipy import sparse

    # min c'x s.t. ||x - p|| <= r, p=(1,2), r=0.5, c=(1,1): obj* = 3 - 0.5*sqrt(2).
    A = sparse.csc_matrix(np.array([[0.0, 0.0], [-1.0, 0.0], [0.0, -1.0]]))
    b = np.array([0.5, -1.0, -2.0])
    c = np.array([1.0, 1.0])
    data = {"A": A, "b": b, "c": c}
    cone = {"q": [3]}
    solver = scs.SCS(data, cone, verbose=False, eps_abs=1e-9, eps_rel=1e-9)
    res, ms = timeit(lambda: solver.solve())
    print(f"SOCP scs      : obj {res['info']['pobj']:+.9f}  iters {res['info']['iter']:5d}  {ms:8.2f} ms"
          f"   (analytic {3.0 - 0.5 * np.sqrt(2.0):+.9f})")


# ---------------------------------------------------------------- MIP knapsack (n=12, formula data)
def mip_data():
    n = 12
    v = np.array([5.0 + ((j * 7) % 13) for j in range(n)])
    w = np.array([3.0 + ((j * 5) % 11) for j in range(n)])
    W = float(np.sum(w)) / 3.0
    return v, w, W


def row_mip():
    import highspy

    v, w, W = mip_data()
    n = len(v)
    h = highspy.Highs()
    h.setOptionValue("output_flag", False)
    inf = highspy.kHighsInf
    h.addVars(n, np.zeros(n), np.ones(n))
    h.changeColsIntegrality(n, np.arange(n, dtype=np.int32), np.array([highspy.HighsVarType.kInteger] * n))
    h.changeColsCost(n, np.arange(n, dtype=np.int32), -v)  # min -v.x
    h.addRow(-inf, W, n, np.arange(n, dtype=np.int32), w)
    _, ms = timeit(lambda: h.run())
    print(f"MIP highspy   : obj {h.getInfo().objective_function_value:+.9f}                      {ms:8.2f} ms")


# ---------------------------------------------------------------- nonlinear rows (scipy + pycma)
def rosen2(x):
    return (1.0 - x[0]) ** 2 + 100.0 * (x[1] - x[0] ** 2) ** 2


def rosen2_grad(x):
    return np.array([-2.0 * (1.0 - x[0]) - 400.0 * x[0] * (x[1] - x[0] ** 2),
                     200.0 * (x[1] - x[0] ** 2)])


def rosen2_hess(x):
    return np.array([[2.0 - 400.0 * (x[1] - 3.0 * x[0] ** 2), -400.0 * x[0]],
                     [-400.0 * x[0], 200.0]])


def rastrigin4(x):
    return 10.0 * 4 + np.sum(x * x - 10.0 * np.cos(PI2 * x))


def row_nonlinear():
    from scipy.optimize import basinhopping, differential_evolution, minimize

    x0 = np.array([-1.2, 1.0])
    for method in ("Nelder-Mead", "Powell"):
        res, ms = timeit(lambda m=method: minimize(rosen2, x0, method=m,
                                                   options={"xatol": 1e-8, "fatol": 1e-8}
                                                   if m == "Nelder-Mead" else {"xtol": 1e-8, "ftol": 1e-10}))
        print(f"DFO scipy {method:11s}: f {res.fun:.3e}  nfev {res.nfev:5d}      {ms:8.2f} ms")

    for method in ("trust-ncg", "trust-krylov", "trust-exact"):
        res, ms = timeit(lambda m=method: minimize(rosen2, x0, method=m, jac=rosen2_grad, hess=rosen2_hess,
                                                   options={"gtol": 1e-9}))
        print(f"TR  scipy {method:11s}: f {res.fun:.3e}  nfev {res.nfev:4d} njev {res.njev:4d}  {ms:8.2f} ms")

    bounds = [(-5.12, 5.12)] * 4
    res, ms = timeit(lambda: differential_evolution(rastrigin4, bounds, seed=1, tol=1e-8, maxiter=2000,
                                                    polish=False))
    print(f"GLB scipy DE  : f {res.fun:.3e}  nfev {res.nfev:6d}     {ms:8.2f} ms   (Rastrigin-4 global = 0)")

    res, ms = timeit(lambda: basinhopping(rosen2, x0, niter=100, seed=1))
    print(f"GLB scipy BH  : f {res.fun:.3e}  nfev {res.nfev:6d}     {ms:8.2f} ms")


def row_cmaes():
    import cma

    x0 = 2.0 * np.ones(8)
    es, ms = timeit(lambda: cma.fmin(lambda x: float(np.sum(np.asarray(x) ** 2)), x0, 1.0,
                                     {"ftarget": 1e-10, "verbose": -9, "seed": 7}), repeats=1)
    print(f"GLB pycma sph8: f {es[1]:.3e}  evals {es[2]:6d}    {ms:8.2f} ms")
    x0r = -2.0 * np.ones(5)
    es, ms = timeit(lambda: cma.fmin(cma.ff.rosen, x0r, 0.5,
                                     {"ftarget": 1e-8, "verbose": -9, "seed": 7}), repeats=1)
    print(f"GLB pycma ros5: f {es[1]:.3e}  evals {es[2]:6d}    {ms:8.2f} ms")


def row_torch():
    # Live optimizer-trajectory parity: torch.optim.{Adam, AdamW} on rosen2 from (-1.2, 1), float64, EXACT
    # autograd gradients, lr=0.05, 200 pinned steps. The Cerid side runs the same recurrence (stochastic.hpp);
    # torch rounds sqrt(v)/sqrt(bc2)+eps where Kingma/Cerid round sqrt(v/bc2)+eps, so agreement is ~1e-12-class,
    # not bit-exact — that difference is torch's, not ours.
    import torch

    def run(decoupled):
        x = torch.tensor([-1.2, 1.0], dtype=torch.float64, requires_grad=True)
        cls = torch.optim.AdamW if decoupled else torch.optim.Adam
        opt = cls([x], lr=0.05, weight_decay=0.01 if decoupled else 0.0)
        for _ in range(200):
            opt.zero_grad()
            f = 100.0 * (x[1] - x[0] ** 2) ** 2 + (1.0 - x[0]) ** 2
            f.backward()
            opt.step()
        with torch.no_grad():
            f = 100.0 * (x[1] - x[0] ** 2) ** 2 + (1.0 - x[0]) ** 2
        return f.item(), x[0].item(), x[1].item()

    for name, dec in (("Adam ", False), ("AdamW", True)):
        (f, x0v, x1v), ms = timeit(lambda d=dec: run(d), repeats=1)
        print(f"OPT torch {name}: f {f:.12e}  x [{x0v:.12f}, {x1v:.12f}]   {ms:8.2f} ms (200 steps lr=0.05)")


def row_ipopt():
    # The NLP row: IPOPT (3.11 via cyipopt) on the pinned Rosenbrock-in-the-unit-disk instance
    # (min rosen2 s.t. 1 - x^2 - y^2 >= 0, from (0,0); scipy reference x* = (0.78642, 0.61770), active
    # boundary). The Cerid side runs its Waechter-Biegler filter IPM on the identical instance.
    from cyipopt import minimize_ipopt

    x0 = np.array([0.0, 0.0])
    cons = [{"type": "ineq",
             "fun": lambda x: 1.0 - x[0] ** 2 - x[1] ** 2,
             "jac": lambda x: np.array([-2.0 * x[0], -2.0 * x[1]])}]
    res, ms = timeit(lambda: minimize_ipopt(rosen2, x0, jac=rosen2_grad, constraints=cons,
                                            options={"tol": 1e-9, "print_level": 0}), repeats=1)
    print(f"NLP ipopt disk: f {res.fun:.9f}  x [{res.x[0]:.7f}, {res.x[1]:.7f}]  nit {res.nit:4d}   {ms:8.2f} ms")


def main():
    print("== v7-z gold-standard scoreboard — REFERENCE side (WSL) ==")
    for name, fn in (("qp", row_qp), ("lp", row_lp), ("socp", row_socp), ("mip", row_mip),
                     ("nonlinear", row_nonlinear), ("cmaes", row_cmaes), ("torch", row_torch),
                     ("ipopt", row_ipopt)):
        try:
            fn()
        except Exception as exc:  # graceful per-row skip
            print(f"-- {name}: SKIPPED ({exc})")
    try:
        import cyipopt  # noqa: F401
        print("IPOPT: cyipopt present")
    except Exception:
        print("IPOPT: pending (sudo apt-get install -y coinor-libipopt-dev pkg-config; "
              "then bash scripts/setup-ipopt-ref.sh)")


if __name__ == "__main__":
    main()
