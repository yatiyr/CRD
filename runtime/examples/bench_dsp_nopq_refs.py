#!/usr/bin/env python3
"""scipy reference timing for the v11-q CZT benchmark."""
import time
import numpy as np
from scipy.signal import czt, CZT


def main():
    n = 4096
    i = np.arange(n)
    x = np.sin(0.013 * i) + 0.4 * np.cos(0.071 * i)
    # one-shot.
    czt(x, n)
    t0 = time.perf_counter()
    chk = 0.0
    for _ in range(100):
        X = czt(x, n)
        chk += X[2].imag
    t1 = time.perf_counter()
    print(f"scipy czt one-shot  N={n} M={n}  {1e3*(t1-t0)/100:.4f} ms/call (chk={chk:.4f})")
    # plan-cached CZT object (apples-to-apples with Cerid CztPlan).
    transform = CZT(n, n)
    transform(x)
    t2 = time.perf_counter()
    chk1 = 0.0
    for _ in range(100):
        X = transform(x)
        chk1 += X[2].imag
    t3 = time.perf_counter()
    print(f"scipy czt cached    N={n} M={n}  {1e3*(t3-t2)/100:.4f} ms/call (chk={chk1:.4f})")


if __name__ == "__main__":
    main()
