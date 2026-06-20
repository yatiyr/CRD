#!/usr/bin/env python3
"""v11-b window-generation throughput — scipy reference (companion to bench_dsp_windows_vs_refs.cpp).

Run at N=2^20 (compute-bound — fair vs Cerid's AVX2 C++; realistic sizes 256-8192 are Python-overhead-bound
for scipy and NOT a fair fight). Run: python3 runtime/examples/bench_dsp_windows_refs.py
"""
import time
from scipy.signal import windows

N = 1 << 20
reps = 50


def bench(name, fn):
    fn()
    t0 = time.perf_counter()
    for _ in range(reps):
        fn()
    print(f"{name:16s} {(time.perf_counter()-t0)/reps*1e3:7.3f} ms/call")


print("=== SCIPY window generation (N=2^20, numpy-C) ===")
bench("hann", lambda: windows.hann(N, sym=True))
bench("hamming", lambda: windows.hamming(N, sym=True))
bench("blackmanharris", lambda: windows.blackmanharris(N, sym=True))
bench("kaiser_b14", lambda: windows.kaiser(N, 14.0, sym=True))
bench("gaussian", lambda: windows.gaussian(N, N / 6.0, sym=True))
