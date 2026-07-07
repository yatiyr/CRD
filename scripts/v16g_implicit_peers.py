#!/usr/bin/env python3
# v16g_implicit_peers.py — Phase 3.1.6 v16-g: implicit-diff peers. jaxopt (root, IFT — the dead-but-still-the-peer
# lane) + cvxpylayers (equality QP layer). Same problems as external/crd_v16g_implicit_bench.cpp. Cerid differentiates
# the solution natively + deterministically in µs; the peers pay jit/canonicalisation overhead.
# Run pinned: taskset -c 4 python3 scripts/v16g_implicit_peers.py
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
import math
import time
import numpy as np


def median_ns(fn, reps):
    for _ in range(3):
        fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter_ns(); fn(); ts.append(time.perf_counter_ns() - t0)
    ts.sort(); return ts[reps // 2]


def jaxopt_root():
    import jax
    import jax.numpy as jnp
    import jaxopt
    jax.config.update("jax_enable_x64", True)
    theta = jnp.array([1.3, 0.7]); xbar = jnp.array([1.0, 0.5])

    def F(x, theta):  # residual whose root is x*(θ)
        return jnp.array([x[0] + 0.3 * jnp.sin(x[0]) + 0.2 * x[1] - theta[0],
                          0.2 * x[0] + x[1] + 0.3 * jnp.sin(x[1]) - theta[1]])
    solver = jaxopt.Broyden(fun=F, tol=1e-13, maxiter=100)  # implicit_diff on by default

    def loss(theta):
        xs = solver.run(jnp.zeros(2), theta).params
        return jnp.dot(xbar, xs)
    g = jax.grad(loss)
    gv = np.asarray(g(theta))
    print(f"[jaxopt] ROOT  dL/dθ=[{gv[0]:.10f}, {gv[1]:.10f}]")
    gj = jax.jit(g)
    gj(theta).block_until_ready()
    print(f"  >> jaxopt root value+grad (jit, IFT): {median_ns(lambda: gj(theta).block_until_ready(), 200):.0f} ns (median) <<")


def cvxpylayers_qp():
    import torch
    import cvxpy as cp
    from cvxpylayers.torch import CvxpyLayer
    torch.set_num_threads(1); torch.set_default_dtype(torch.float64)
    for nq in (8, 20):
        mq = nq // 4
        Q = np.zeros((nq, nq)); q = np.zeros(nq); xbar = np.zeros(nq); A = np.zeros((mq, nq)); b = np.zeros(mq)
        for i in range(nq):
            for j in range(nq):
                Q[i, j] = (2.0 + 0.1 * i) if i == j else 0.15 * math.sin(1.0 + i + j)
            q[i] = math.sin(0.7 + i); xbar[i] = 0.5 + 0.3 * math.cos(0.2 + i)
        for i in range(mq):
            for j in range(nq):
                A[i, j] = math.cos(0.3 + i * nq + j)
            b[i] = 0.2 * (i + 1)
        # DPP QP with q,b as parameters (Q,A constant); differentiate wrt q,b (match Cerid Sum_gq, Sum_gb)
        x = cp.Variable(nq)
        q_p = cp.Parameter(nq); b_p = cp.Parameter(mq)
        prob = cp.Problem(cp.Minimize(0.5 * cp.quad_form(x, cp.psd_wrap(Q)) + q_p @ x), [A @ x == b_p])
        layer = CvxpyLayer(prob, parameters=[q_p, b_p], variables=[x])
        xbar_t = torch.tensor(xbar)

        sargs = {"eps_abs": 1e-10, "eps_rel": 1e-10, "max_iters": 100000}

        def run():
            qt = torch.tensor(q, requires_grad=True); bt = torch.tensor(b, requires_grad=True)
            (xs,) = layer(qt, bt, solver_args=sargs)
            (xbar_t @ xs).backward()
            return qt.grad, bt.grad
        gq, gb = run()
        print(f"[cvxpylayers] QP nq={nq:<3d} mq={mq}  Sum_gq={float(gq.sum()):.10f} Sum_gb={float(gb.sum()):.10f}")
        print(f"  >> cvxpylayers QP value+grad: {median_ns(run, 30):.0f} ns (median) <<")


if __name__ == "__main__":
    for name, fn in (("jaxopt", jaxopt_root), ("cvxpylayers", cvxpylayers_qp)):
        try:
            fn()
        except Exception as e:  # noqa
            print(f"{name} peer FAILED:", repr(e)[:200])
