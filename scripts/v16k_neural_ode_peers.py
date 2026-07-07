#!/usr/bin/env python3
# v16k_neural_ode_peers.py -- Phase 3.1.6 v16-k: torchdiffeq neural-ODE peer. Same MLP RHS / true damped-spiral data /
# init / lr / epochs as external/crd_v16k_neural_ode_bench.cpp. Reports final loss + wall time for the speed crush.
# Cerid's per-sample DTO adjoint + fixed-order fold ALSO guarantees bit-identity across {1..16} workers (v16-i);
# torch offers no such guarantee under parallelism. Run: taskset -c 4 python3 scripts/v16k_neural_ode_peers.py
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
import math
import time
import numpy as np


def main():
    import torch
    from torchdiffeq import odeint
    torch.set_num_threads(1); torch.set_default_dtype(torch.float64)
    nb, nt, h, epochs = 32, 20, 0.05, 300
    lr = 0.05 / nb  # Cerid updates theta -= 0.05 * grad/nb on the SUM loss ⇒ match with lr/nb here
    khid = 8

    # data: x0_k -> xT_k under the true spiral dx/dt = [[-0.1,-1],[1,-0.1]] x (RK4, matched to Cerid)
    def true_rhs(x):
        return torch.stack([-0.1 * x[..., 0] - x[..., 1], x[..., 0] - 0.1 * x[..., 1]], dim=-1)
    x0 = torch.tensor([[math.sin(1.0 + k), math.cos(0.5 + k)] for k in range(nb)])
    x = x0.clone()
    for _ in range(nt):  # explicit RK4
        k1 = true_rhs(x); k2 = true_rhs(x + 0.5 * h * k1); k3 = true_rhs(x + 0.5 * h * k2); k4 = true_rhs(x + h * k3)
        x = x + (h / 6.0) * (k1 + 2 * k2 + 2 * k3 + k4)
    xt = x.detach()

    init = torch.tensor([0.2 * math.sin(0.3 + i) for i in range(khid * 2 + khid + 2 * khid + 2)])

    class Ode(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.l1 = torch.nn.Linear(2, khid); self.l2 = torch.nn.Linear(khid, 2)
            with torch.no_grad():
                self.l1.weight.copy_(init[0:khid * 2].reshape(khid, 2)); self.l1.bias.copy_(init[khid * 2:khid * 3])
                self.l2.weight.copy_(init[khid * 3:khid * 3 + 2 * khid].reshape(2, khid)); self.l2.bias.copy_(init[khid * 3 + 2 * khid:])
        def forward(self, t, x):
            return self.l2(torch.tanh(self.l1(x)))

    def train():
        ode = Ode(); opt = torch.optim.SGD(ode.parameters(), lr=lr)
        tgrid = torch.tensor([0.0, nt * h])
        loss = None
        for _ in range(epochs):
            opt.zero_grad()
            pred = odeint(ode, x0, tgrid, method="rk4", options={"step_size": h})[-1]
            loss = ((pred - xt) ** 2).sum()
            loss.backward(); opt.step()
        return float(loss)

    t0 = time.perf_counter()
    final = train()
    ms = (time.perf_counter() - t0) * 1000.0
    print(f"[torch {torch.__version__} + torchdiffeq]  final loss={final:.10f}  >> torchdiffeq neural-ODE train ({epochs} epochs): {ms:.0f} ms <<")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa
        print("torchdiffeq peer FAILED:", repr(e)[:200])
