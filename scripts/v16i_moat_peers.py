#!/usr/bin/env python3
# v16i_moat_peers.py -- Phase 3.1.6 v16-i: the deterministic-training MOAT, measured against torch-CPU. Cerid's
# batch_gradient folds per-sample gradients in a FIXED order, so its batched gradient (and thus a whole training run)
# is BIT-IDENTICAL across worker counts {1..16} -- a formal guarantee (gated in test_determinism_moat.cpp). torch has
# no such guarantee: this script measures whether torch's batched gradient / trained weights are reproducible across
# intra-op THREAD COUNTS. We report exactly what torch does -- the moat is the GUARANTEE, so honesty about torch's
# actual drift matters. Run: python3 scripts/v16i_moat_peers.py
import os
os.environ.setdefault("MKL_CBWR", "")  # do NOT force MKL conditional-reproducibility -- measure torch's DEFAULT
import numpy as np


def torch_reproducibility():
    import torch
    torch.set_default_dtype(torch.float64)
    # a reduction-heavy linear regression: large batch so intra-op parallel reduction order can matter.
    n_samp, d, epochs, lr = 200000, 4, 60, 0.02
    rng = np.random.default_rng(0)
    x = rng.standard_normal((n_samp, d))
    y = rng.standard_normal(n_samp)
    xg = torch.tensor(x)
    yg = torch.tensor(y)

    def one_grad(nthreads):
        torch.set_num_threads(nthreads)
        th = torch.zeros(d, requires_grad=True)
        loss = ((xg @ th - yg) ** 2).sum()
        loss.backward()
        return th.grad.detach().numpy().copy()

    def train(nthreads):
        torch.set_num_threads(nthreads)
        th = torch.zeros(d, requires_grad=True)
        for _ in range(epochs):
            if th.grad is not None:
                th.grad = None
            loss = ((xg @ th - yg) ** 2).sum()
            loss.backward()
            with torch.no_grad():
                th -= lr * th.grad / n_samp
        return th.detach().numpy().copy()

    print(f"[torch {torch.__version__}]  batched gradient reproducibility across intra-op thread counts:")
    g1 = one_grad(1); g8 = one_grad(8)
    print(f"  single batched gradient:  max|g(1thr) - g(8thr)| = {float(np.max(np.abs(g1 - g8))):.3e}")
    w1a = train(1); w1b = train(1); w8 = train(8)
    print(f"  after {epochs}-epoch training:  max|w(1thr,runA) - w(1thr,runB)| = {float(np.max(np.abs(w1a - w1b))):.3e}"
          f"  (same thread count, run-to-run)")
    print(f"                                 max|w(1thr) - w(8thr)|            = {float(np.max(np.abs(w1a - w8))):.3e}"
          f"  (ACROSS thread counts -- the moat axis)")
    print("  Cerid batch_gradient: 0.0 across ALL of {1..16} workers -- GATED bit-identity (test_determinism_moat.cpp).")


if __name__ == "__main__":
    try:
        torch_reproducibility()
    except Exception as e:  # noqa
        print("torch peer FAILED:", repr(e)[:200])
