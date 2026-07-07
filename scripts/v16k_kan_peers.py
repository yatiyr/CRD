#!/usr/bin/env python3
# v16k_kan_peers.py -- Phase 3.1.6 v16-k (part 2): efficient-kan peer. Trains a 2-layer KAN [2,4,1] (grid_size=5,
# spline_order=3) to fit sin(2x+1.5y) on the same 6x6 grid as external/crd_v16k_kan_bench.cpp; reports final loss +
# wall time. Cerid's KAN is native C++ + bit-reproducible (efficient-kan's B-spline restructuring is the SAME idea
# Cerid uses -- the crush is native speed + determinism). Run: taskset -c 4 python3 scripts/v16k_kan_peers.py
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
import math
import time
import numpy as np


def main():
    import torch
    from efficient_kan import KAN
    torch.set_num_threads(1); torch.set_default_dtype(torch.float64)
    pts = [(-1.0 + 0.4 * a, -1.0 + 0.4 * b) for a in range(6) for b in range(6)]
    x = torch.tensor(pts)
    y = torch.tensor([[math.sin(2.0 * a + 1.5 * b)] for (a, b) in pts])

    model = KAN([2, 4, 1], grid_size=5, spline_order=3).double()
    opt = torch.optim.Adam(model.parameters(), lr=0.02)
    epochs = 3000
    t0 = time.perf_counter()
    loss = None
    for _ in range(epochs):
        opt.zero_grad()
        loss = ((model(x) - y) ** 2).sum()
        loss.backward(); opt.step()
    ms = (time.perf_counter() - t0) * 1000.0
    print(f"[efficient-kan / torch {torch.__version__}]  2-layer KAN [2,4,1] fit sin(2x+1.5y): final loss={float(loss):.6f}"
          f"  >> efficient-kan train ({epochs} epochs): {ms:.0f} ms <<")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa
        print("efficient-kan peer FAILED:", repr(e)[:200])
