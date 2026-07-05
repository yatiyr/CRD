#!/usr/bin/env python3
# v14-h batched-LA peer harness: torch-CPU (bmm/linalg on stacks — MKL-backed)
# + NumPy, timed pinned, over the EKF/skinning regime (small matrices, big
# batches). MATLAB pagemtimes/pagemldivide rows are measured at board time in
# ONE -batch call (the 44.5 s startup rule); the native MKL cblas_?gemm_batch
# harness rides the C++ A/B script. Values double as reference tolerances for
# the C++ gates (our batched == our loop-of-single is the BIT gate; peers gate
# accuracy + speed).
# Run (WSL): taskset -c 4 python3 -u scripts/v14h_peers.py
import time

import numpy as np
import torch

torch.set_num_threads(1)  # matched threading: our serial vs their serial (MT board separate)

SIZES = [4, 6, 8, 16]
BATCHES = [10_000, 100_000]
REPS = 5


def bench(fn, *args):
    best = 1e300
    for _ in range(REPS):
        t0 = time.perf_counter()
        fn(*args)
        t1 = time.perf_counter()
        best = min(best, t1 - t0)
    return best * 1e3


def main():
    rng = np.random.default_rng(1407)
    for n in SIZES:
        for b in BATCHES:
            a = rng.standard_normal((b, n, n))
            bb = rng.standard_normal((b, n, n))
            # SPD for cholesky/solve
            spd = a @ a.transpose(0, 2, 1) + n * np.eye(n)[None, :, :]
            rhs = rng.standard_normal((b, n, 1))
            ta = torch.from_numpy(a)
            tb = torch.from_numpy(bb)
            tspd = torch.from_numpy(spd)
            trhs = torch.from_numpy(rhs)
            r = {}
            r["np_matmul"] = bench(lambda: a @ bb)
            r["t_bmm"] = bench(lambda: torch.bmm(ta, tb))
            r["t_chol"] = bench(lambda: torch.linalg.cholesky(tspd))
            r["t_cholsolve"] = bench(lambda: torch.cholesky_solve(trhs, torch.linalg.cholesky(tspd)))
            r["t_lu"] = bench(lambda: torch.linalg.lu_factor(ta))
            r["t_svd"] = bench(lambda: torch.linalg.svd(ta, full_matrices=False))
            row = " ".join(f"{k}={v:9.2f}ms" for k, v in r.items())
            print(f"[n={n:2d} b={b:6d}] {row}", flush=True)


if __name__ == "__main__":
    main()
