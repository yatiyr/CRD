#!/usr/bin/env python3
# v16f_ode_adjoint_peers.py — Phase 3.1.6 v16-f: torchdiffeq (+ diffrax) ODE-adjoint peers. Same ODE / RK4 / loss as
# external/crd_v16f_ode_adjoint_bench.cpp. Demonstrates the exactness-vs-memory tradeoff the peers force and Cerid
# avoids: `odeint` (backprop = DTO) is EXACT but O(T) memory; `odeint_adjoint` (continuous adjoint) is O(1) memory but
# INCONSISTENT with the discrete RK4 (a gradient error). Cerid's DTO+revolve is exact AND O(log T) AND deterministic.
# Run pinned: taskset -c 4 python3 scripts/v16f_ode_adjoint_peers.py
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
import time
import numpy as np

X0 = [1.0, 0.5]
THETA = [0.8, 1.2]
LG = [1.0, 0.5]  # loss_grad; L = LG·x_T


def median_ns(fn, reps):
    for _ in range(3):
        fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter_ns(); fn(); ts.append(time.perf_counter_ns() - t0)
    ts.sort(); return ts[reps // 2]


def torch_peer():
    import torch
    from torchdiffeq import odeint, odeint_adjoint
    torch.set_num_threads(1); torch.set_default_dtype(torch.float64)
    print(f"[torch {torch.__version__} + torchdiffeq]")

    class ODE(torch.nn.Module):
        def __init__(self, th):
            super().__init__()
            self.theta = torch.nn.Parameter(torch.tensor(th, dtype=torch.float64))
        def forward(self, t, x):
            return torch.stack([self.theta[0] * x[1] - 0.5 * x[0] ** 2,
                                -self.theta[1] * x[0] + 0.3 * torch.sin(x[1])])

    for T in (100, 500):
        h = 2.0 / T
        tgrid = torch.tensor([0.0, 2.0], dtype=torch.float64)
        opt = {"step_size": h}
        lg = torch.tensor(LG, dtype=torch.float64)

        # FD reference (of the discrete RK4 forward), no grad
        def loss_at(theta_np):
            with torch.no_grad():
                ode = ODE(theta_np)
                x0 = torch.tensor(X0, dtype=torch.float64)
                sol = odeint(ode, x0, tgrid, method="rk4", options=opt)
                return float((lg * sol[-1]).sum())
        fd = np.zeros(2)
        for j in range(2):
            thp = list(THETA); thp[j] += 1e-6; fp = loss_at(thp)
            thm = list(THETA); thm[j] -= 1e-6; fm = loss_at(thm)
            fd[j] = (fp - fm) / 2e-6

        # DTO: backprop through odeint (stores all states → O(T) memory)
        ode = ODE(THETA); x0 = torch.tensor(X0, dtype=torch.float64)
        sol = odeint(ode, x0, tgrid, method="rk4", options=opt)
        (lg * sol[-1]).sum().backward()
        dto = ode.theta.grad.detach().numpy().copy()

        # CTO: odeint_adjoint (continuous adjoint → O(1) memory, but inconsistent)
        ode2 = ODE(THETA); x02 = torch.tensor(X0, dtype=torch.float64)
        sol2 = odeint_adjoint(ode2, x02, tgrid, method="rk4", options=opt)
        (lg * sol2[-1]).sum().backward()
        cto = ode2.theta.grad.detach().numpy().copy()

        dto_err = float(np.max(np.abs(dto - fd)))
        cto_err = float(np.max(np.abs(cto - fd)))
        print(f"T={T:<4d}  DTO(odeint) grad θ=[{dto[0]:.10f}, {dto[1]:.10f}]  |DTO−FD|={dto_err:.2e} (exact, O(T) mem)  "
              f"|CTO−FD|={cto_err:.2e} (odeint_adjoint: O(1) mem, INCONSISTENT)")

        def dto_call():
            o = ODE(THETA); xx = torch.tensor(X0, dtype=torch.float64)
            s = odeint(o, xx, tgrid, method="rk4", options=opt)
            (lg * s[-1]).sum().backward()
        print(f"  >> torchdiffeq DTO value+grad: {median_ns(dto_call, 300 if T <= 100 else 100):.0f} ns (median) <<")


if __name__ == "__main__":
    try:
        torch_peer()
    except Exception as e:  # noqa
        print("torch peer FAILED:", e)
