#!/usr/bin/env python3
# v14-k TT peer harness — tntorch 1.1.2 (torch 2.12.0+cpu), matched problems with
# runtime/examples/bench_tt.cpp. 1T (torch.set_num_threads(1)), best-of-5; run pinned:
#   taskset -c 4 python3 scripts/v14k_tt_peers.py
# ttpy: N/A-WITH-CHECK — pip install fails metadata generation on py3.12 (its setup.py
# imports numpy.distutils, removed from numpy for py>=3.12); attempted 2026-07-05.
import time

import numpy as np
import torch
import tntorch as tn

torch.set_default_dtype(torch.float64)
torch.set_num_threads(1)
np.random.seed(0)
torch.manual_seed(0)


def best_of(reps, fn):
    best = 1e300
    out = None
    for _ in range(reps):
        t0 = time.perf_counter()
        out = fn()
        dt = (time.perf_counter() - t0) * 1e3
        best = min(best, dt)
    return best, out


# ---- matched tensors --------------------------------------------------------
def hilbert4(n):
    idx = np.indices((n,) * 4).sum(axis=0)
    return torch.tensor(1.0 / (1.0 + idx))


def smooth4(n):
    sx = np.array([-1.0 + 2.0 * i / (n - 1) for i in range(n)])
    X = np.meshgrid(*([sx] * 4), indexing="ij")
    s = 0.1 + X[0] * X[0]
    for x in X[1:]:
        s = s + x * x
    return torch.tensor(1.0 / np.sqrt(s))


print("== v14-k TT peers: tntorch (1T, best-of-5) ==")

# ---- tt_svd -----------------------------------------------------------------
for name, A, eps in [("hilbert 20^4", hilbert4(20), 1e-8),
                     ("smooth 16^4", smooth4(16), 1e-10),
                     ("smooth 32^4", smooth4(32), 1e-8)]:
    ms, t = best_of(5, lambda: tn.Tensor(A, eps=eps))
    print(f"[tntorch tt_svd {name} eps={eps:g}] {ms:8.2f} ms  ranks={[int(x) for x in t.ranks_tt[1:-1]]}")

# ---- add + round ------------------------------------------------------------
t16 = tn.Tensor(smooth4(16), eps=1e-10)


def add_round():
    t2 = t16 + t16
    t2.round_tt(1e-10)
    return t2


ms, _ = best_of(5, add_round)
print(f"[tntorch (t+t).round_tt smooth 16^4 @1e-10] {ms:8.2f} ms")

# ---- eval throughput: 1M random indices on the smooth 16^4 TT ----------------
idx = [torch.randint(0, 16, (1000000,)) for _ in range(4)]
ms, out = best_of(5, lambda: t16[idx[0], idx[1], idx[2], idx[3]].torch())
print(f"[tntorch eval smooth 16^4, 1M pts] {ms:8.2f} ms  ({ms * 1e6 / 1e6:.1f} ns/pt)")

# ---- tt_cross smooth 4D (frozen oracle budget) --------------------------------
sx16 = torch.tensor(np.array([-1.0 + 2.0 * i / 15 for i in range(16)]))


def f4(x1, x2, x3, x4):
    return 1.0 / torch.sqrt(0.1 + x1 * x1 + x2 * x2 + x3 * x3 + x4 * x4)


def cross4():
    return tn.cross(function=f4, domain=[sx16] * 4, ranks_tt=10, max_iter=6, eps=1e-9,
                    val_size=200, verbose=False, return_info=True, suppress_warnings=True)


ms, (t_cr, info) = best_of(5, cross4)
print(f"[tntorch cross smooth 16^4 r=10] {ms:8.2f} ms  val_err={float(info['val_epss'][-1]):.3e} "
      f"evals={info['nsamples']}")

# ---- 6D kernels --------------------------------------------------------------
# OLD overlapping-domain kernel (soften 0.05, both bodies on [-1,1]^3): the
# convergence probe — is rank 12 enough for ANY implementation?
ax = torch.tensor(np.array([-1.0 + 2.0 * i / 15 for i in range(16)]))


def f6_overlap(x0, x1, x2, x3, x4, x5):
    return 1.0 / torch.sqrt(0.05 + (x0 - x3) ** 2 + (x1 - x4) ** 2 + (x2 - x5) ** 2)


def cross6_overlap():
    return tn.cross(function=f6_overlap, domain=[ax] * 6, ranks_tt=12, max_iter=6, eps=1e-7,
                    val_size=500, verbose=False, return_info=True, suppress_warnings=True)


ms, (t6o, info) = best_of(3, cross6_overlap)
print(f"[tntorch cross 16^6 OVERLAP kernel r=12] {ms:8.2f} ms  "
      f"val_err={float(info['val_epss'][-1]):.3e} evals={info['nsamples']}")

# SEPARATED two-body gravitational kernel (the ephemeris regime: bodies never
# collide -> analytic 1/|r1-r2|, no softening): r1 in [-1,-0.2]^3, r2 in [0.2,1]^3
ax1 = torch.tensor(np.array([-1.0 + 0.8 * i / 15 for i in range(16)]))
ax2 = torch.tensor(np.array([0.2 + 0.8 * i / 15 for i in range(16)]))


def f6_sep(x0, x1, x2, x3, x4, x5):
    return 1.0 / torch.sqrt((x0 - x3) ** 2 + (x1 - x4) ** 2 + (x2 - x5) ** 2)


def cross6_sep(r, eps):
    return tn.cross(function=f6_sep, domain=[ax1] * 3 + [ax2] * 3, ranks_tt=r, max_iter=6,
                    eps=eps, val_size=500, verbose=False, return_info=True, suppress_warnings=True)


for r, eps in ((8, 1e-7), (12, 1e-7), (12, 1e-3)):  # (12, 1e-3) = the MATCHED demo budget
    ms, (t6s, info) = best_of(3, lambda: cross6_sep(r, eps))
    print(f"[tntorch cross 16^6 SEPARATED kernel r={r} eps={eps:g}] {ms:8.2f} ms  "
          f"val_err={float(info['val_epss'][-1]):.3e} evals={info['nsamples']}")

# eval throughput on the separated-kernel 6D TT
idx6 = [torch.randint(0, 16, (1000000,)) for _ in range(6)]
ms, _ = best_of(5, lambda: t6s[idx6[0], idx6[1], idx6[2], idx6[3], idx6[4], idx6[5]].torch())
print(f"[tntorch eval 16^6 TT, 1M pts] {ms:8.2f} ms  ({ms * 1e6 / 1e6:.1f} ns/pt)")
