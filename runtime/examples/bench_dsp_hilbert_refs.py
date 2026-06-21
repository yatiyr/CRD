#!/usr/bin/env python3
"""scipy reference timing for the Hilbert / analytic-signal benchmark (v11-l)."""
import time
import numpy as np
from scipy.signal import hilbert


def run(n, reps):
    i = np.arange(n)
    x = np.sin(0.01 * i) + 0.3 * np.cos(0.023 * i)
    hilbert(x)  # warm
    t0 = time.perf_counter()
    chk = 0.0
    for _ in range(reps):
        h = hilbert(x)
        chk += h[n // 2].imag
    t1 = time.perf_counter()
    print(f"scipy hilbert N={n:<8d}  {1e3*(t1-t0)/reps:.4f} ms/call (chk={chk:.4f})")


if __name__ == "__main__":
    run(1 << 16, 500)
    run(1 << 20, 50)
