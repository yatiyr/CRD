#!/usr/bin/env python3
"""scipy reference timing for find_peaks + detrend (v11-s)."""
import time
import numpy as np
from scipy.signal import find_peaks, detrend


def main():
    n = 1000000
    rng = np.random.default_rng(0)
    i = np.arange(n)
    x = np.sin(0.05 * i) + 0.3 * (rng.random(n) * 2 - 1)
    xt = 0.5 + 1e-6 * i + np.sin(0.013 * i)
    reps = 20
    find_peaks(x)
    t0 = time.perf_counter()
    for _ in range(reps):
        p, _ = find_peaks(x)
    t1 = time.perf_counter()
    print(f"scipy find_peaks N={n}  {1e3*(t1-t0)/reps:.4f} ms/call (npeaks={p.size})")
    detrend(xt, type='linear')
    t2 = time.perf_counter()
    for _ in range(reps):
        d = detrend(xt, type='linear')
    t3 = time.perf_counter()
    print(f"scipy detrend    N={n}  {1e3*(t3-t2)/reps:.4f} ms/call (chk={d[n//2]:.5f})")


if __name__ == "__main__":
    main()
