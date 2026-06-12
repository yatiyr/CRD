#!/usr/bin/env python3
"""scipy reference for runtime/examples/ode_scipy_difftest.cpp (v9-b trajectory-exactness gate).

Run (WSL): python3 scripts/ode_scipy_ref.py — compare line-for-line with the C++ runner's output.
"""

import numpy as np
from scipy.integrate import solve_ivp


def vdp(t, y):
    return [y[1], (1.0 - y[0] * y[0]) * y[1] - y[0]]


def dec(t, y):
    return [-y[0]]


def run(name, fun, t_span, y0, method, rtol, atol):
    r = solve_ivp(fun, t_span, y0, method=method, rtol=rtol, atol=atol)
    naccept = len(r.t) - 1
    y = ", ".join(f"{v:.17g}" for v in r.y[:, -1])
    print(f"{name}: naccept={naccept} nfev={r.nfev} y=[{y}]")


run("vdp_rk23", vdp, (0.0, 10.0), [2.0, 0.0], "RK23", 1e-8, 1e-8)
run("vdp_rk45", vdp, (0.0, 10.0), [2.0, 0.0], "RK45", 1e-8, 1e-8)
run("vdp_dop853", vdp, (0.0, 10.0), [2.0, 0.0], "DOP853", 1e-8, 1e-8)
run("dec_rk45", dec, (0.0, 5.0), [1.0], "RK45", 1e-10, 1e-12)
run("dec_dop853", dec, (0.0, 5.0), [1.0], "DOP853", 1e-10, 1e-12)


# --- v9-d: BDF with analytic Jacobians (jac=callable = the trajectory-exact configuration) ---
def rober(t, y):
    return [-0.04 * y[0] + 1e4 * y[1] * y[2],
            0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] ** 2,
            3e7 * y[1] ** 2]


def rober_jac(t, y):
    return [[-0.04, 1e4 * y[2], 1e4 * y[1]],
            [0.04, -1e4 * y[2] - 6e7 * y[1], -1e4 * y[1]],
            [0.0, 6e7 * y[1], 0.0]]


def vdp1000(t, y):
    return [y[1], 1000.0 * ((1.0 - y[0] ** 2) * y[1]) - y[0]]


def vdp1000_jac(t, y):
    return [[0.0, 1.0],
            [1000.0 * (-2.0 * y[0] * y[1]) - 1.0, 1000.0 * (1.0 - y[0] ** 2)]]


def run_bdf(name, fun, jac, t_span, y0, rtol, atol):
    r = solve_ivp(fun, t_span, y0, method="BDF", jac=jac, rtol=rtol, atol=atol)
    naccept = len(r.t) - 1
    y = ", ".join(f"{v:.17g}" for v in r.y[:, -1])
    print(f"{name}: naccept={naccept} nfev={r.nfev} njev={r.njev} nlu={r.nlu} y=[{y}]")


run_bdf("rober_bdf", rober, rober_jac, (0.0, 100.0), [1.0, 0.0, 0.0], 1e-6, 1e-10)
run_bdf("vdp1000_bdf", vdp1000, vdp1000_jac, (0.0, 300.0), [2.0, 0.0], 1e-6, 1e-8)


def run_radau(name, fun, jac, t_span, y0, rtol, atol):
    r = solve_ivp(fun, t_span, y0, method="Radau", jac=jac, rtol=rtol, atol=atol)
    naccept = len(r.t) - 1
    y = ", ".join(f"{v:.17g}" for v in r.y[:, -1])
    print(f"{name}: naccept={naccept} nfev={r.nfev} njev={r.njev} nlu={r.nlu} y=[{y}]")


run_radau("rober_radau", rober, rober_jac, (0.0, 100.0), [1.0, 0.0, 0.0], 1e-6, 1e-10)
run_radau("vdp1000_radau", vdp1000, vdp1000_jac, (0.0, 300.0), [2.0, 0.0], 1e-6, 1e-8)
