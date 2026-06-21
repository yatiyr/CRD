#!/usr/bin/env python3
"""scipy reference timing for the resample_poly benchmark (v11-k)."""
import time
import numpy as np
from scipy.signal import resample_poly


def run(n, up, down, reps):
    i = np.arange(n)
    x = np.sin(2 * np.pi * 0.05 * i) + 0.5 * np.sin(2 * np.pi * 0.13 * i)
    resample_poly(x, up, down)  # warm
    t0 = time.perf_counter()
    chk = 0.0
    for _ in range(reps):
        y = resample_poly(x, up, down)
        chk += y[y.size // 2]
    t1 = time.perf_counter()
    print(f"scipy resample_poly N={n} up={up} down={down}  {1e3*(t1-t0)/reps:.4f} ms/call (out={y.size} chk={chk:.5f})")


if __name__ == "__main__":
    run(1000000, 3, 2, 30)
    run(1000000, 2, 3, 30)
