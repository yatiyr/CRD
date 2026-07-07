#!/usr/bin/env python3
# v14-j peer rows: TensorLy 0.9.0 (numpy backend) CP-ALS / Tucker-HOOI /
# randomized-svd Tucker on the SAME synthetic tensors as bench_decomp.cpp
# (elementwise-exact formula => bit-identical f64 inputs). Matched 1-thread:
# run under `OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1
# taskset -c 4`. best-of-5 wall clock + final fit per row.
import os

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")

import time

import numpy as np
import tensorly as tl
from tensorly.decomposition import parafac, tucker


def bench_tensor(shape):
    mul = (31, 17, 7, 3)
    grids = np.meshgrid(*[np.arange(s) for s in shape], indexing="ij")
    m = sum(g * mul[d] for d, g in enumerate(grids))
    s = sum(grids)
    return (m % 101).astype(np.float64) / 50.5 - 1.0 + 2.0 / (1.0 + s.astype(np.float64))


def best_of(reps, fn):
    best = 1e300
    out = None
    for _ in range(reps):
        t0 = time.perf_counter()
        out = fn()
        dt = (time.perf_counter() - t0) * 1e3
        best = min(best, dt)
    return best, out


def fit_of(x, xhat, norm_x):
    return 1.0 - float(np.linalg.norm(x - xhat) / norm_x)


def main():
    print(f"tensorly {tl.__version__} numpy {np.__version__} threads=1")
    cases = [((64, 64, 64), 16, 16), ((32, 32, 32, 32), 8, 8), ((128, 128, 128), 32, 32)]
    for shape, cp_rank, tk_rank in cases:
        x = bench_tensor(shape)
        xt = tl.tensor(x)
        norm_x = np.linalg.norm(x)
        tag = "x".join(str(s) for s in shape)
        ms, cp = best_of(5, lambda: parafac(xt, rank=cp_rank, n_iter_max=10, init="svd",
                                            tol=1e-30, normalize_factors=False, random_state=0))
        fit = fit_of(x, np.asarray(tl.cp_to_tensor(cp)), norm_x)
        print(f"{tag:12s} parafac    rank {cp_rank:2d} iters 10 : {ms:10.2f} ms  fit {fit:.12f}")
        ranks = [tk_rank] * len(shape)
        ms, tk = best_of(5, lambda: tucker(xt, rank=ranks, n_iter_max=5, init="svd", tol=1e-30,
                                           random_state=0))
        fit = fit_of(x, np.asarray(tl.tucker_to_tensor(tk)), norm_x)
        print(f"{tag:12s} tucker     rank {tk_rank:2d} iters 5  : {ms:10.2f} ms  fit {fit:.12f}")
        try:
            ms, tr = best_of(5, lambda: tucker(xt, rank=ranks, n_iter_max=1, init="svd", tol=1e-30,
                                               svd="randomized_svd", random_state=0))
            fit = fit_of(x, np.asarray(tl.tucker_to_tensor(tr)), norm_x)
            print(f"{tag:12s} tucker-rnd rank {tk_rank:2d} iters 1  : {ms:10.2f} ms  fit {fit:.12f}")
        except Exception as e:  # noqa: BLE001 — peer capability probe
            print(f"{tag:12s} tucker-rnd unavailable: {e}")


if __name__ == "__main__":
    main()
