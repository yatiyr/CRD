#!/usr/bin/env python3
"""pywt reference timings for v11w-b wavedec (the C core — a real perf peer). Single-thread."""
import time
import numpy as np
import pywt

N = 1 << 20
# exactly-reproducible LCG, bit-identical to the C bench (so chk proves correctness at scale).
s = np.uint64(88172645463325252)
mul = np.uint64(6364136223846793005)
inc = np.uint64(1442695040888963407)
x = np.empty(N, dtype=np.float64)
with np.errstate(over="ignore"):
    for i in range(N):
        s = s * mul + inc
        x[i] = (float(s >> np.uint64(11)) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0


def bench(name, level, mode, reps=50):
    pywt.wavedec(x, name, mode=mode, level=level)  # warm
    t0 = time.perf_counter()
    for _ in range(reps):
        c = pywt.wavedec(x, name, mode=mode, level=level)
    dt = (time.perf_counter() - t0) / reps * 1e3
    chk = c[0][len(c[0]) // 2] + c[-1][0]
    print(f"pywt  wavedec N={N} wav={name:6s} level={level} mode={mode:13s}  {dt:.4f} ms/call (ncoef={len(c)} chk={chk:.6f})")


def bench_swt(name, level, reps=30):
    pywt.swt(x, name, level=level)  # warm
    t0 = time.perf_counter()
    for _ in range(reps):
        c = pywt.swt(x, name, level=level)
    dt = (time.perf_counter() - t0) / reps * 1e3
    chk = c[0][0][len(c[0][0]) // 2] + c[-1][1][0]
    print(f"pywt  swt     N={N} wav={name:6s} level={level}                    {dt:.4f} ms/call (chk={chk:.6f})")


def bench_cwt(name, nscales, n=1 << 14, reps=20):
    xx = x[:n]  # the SAME LCG prefix as the C bench (chk-comparable)
    scales = np.geomspace(1, 128, nscales)
    pywt.cwt(xx, scales, name)  # warm
    t0 = time.perf_counter()
    for _ in range(reps):
        c, _ = pywt.cwt(xx, scales, name)
    dt = (time.perf_counter() - t0) / reps * 1e3
    chk = float(np.real(c[nscales // 2][n // 2]))
    print(f"pywt  cwt     N={n} wav={name:6s} scales={nscales}  {dt:.4f} ms/call (chk={chk:.6f})")


def bench_dwt2(R, C, name, reps=30):
    img = x[: R * C].reshape(R, C)  # same LCG sequence, row-major (chk-comparable)
    pywt.dwt2(img, name, mode="periodization")  # warm
    t0 = time.perf_counter()
    for _ in range(reps):
        cA, _ = pywt.dwt2(img, name, mode="periodization")
    dt = (time.perf_counter() - t0) / reps * 1e3
    chk = float(cA.ravel()[cA.size // 2])
    print(f"pywt  dwt2    {R}x{C} wav={name:6s}          {dt:.4f} ms/call (chk={chk:.6f})")


if __name__ == "__main__":
    bench("haar", 6, "periodization")
    bench("db4", 6, "periodization")
    bench("db8", 6, "periodization")
    bench("sym8", 6, "symmetric")
    bench_swt("db4", 5)
    bench_swt("sym4", 5)
    bench_cwt("morl", 64)
    bench_cwt("cmor1.5-1.0", 64)
    bench_dwt2(1024, 1024, "db4")
