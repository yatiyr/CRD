#!/usr/bin/env python3
"""scipy.special Bessel/Airy timing (vectorized — scipy's real strength), ns/element, vs Cerid's ns/call.

Run: python3 runtime/examples/bench_bessel_refs.py
"""
import numpy as np
import time
from scipy import special as sp


def bench(fn, *args, reps=30):
    fn(*args)  # warm
    t0 = time.perf_counter()
    for _ in range(reps):
        fn(*args)
    return (time.perf_counter() - t0) / reps


def main():
    n = 1_000_000
    rng = np.random.default_rng(1)
    nu = rng.choice([0.0, 1.0, 2.0, 5.0, 0.5, 2.5], n)
    x = rng.uniform(0.5, 40.0, n)
    print("# scipy.special Bessel/Airy — vectorized ns/element (peer for Cerid bench_bessel_vs_refs)")
    for name, fn in [("cyl_J", sp.jv), ("cyl_Y", sp.yv), ("cyl_I", sp.iv), ("cyl_K", sp.kv)]:
        ns = bench(fn, nu, x) / n * 1e9
        print(f"{name:10s} scipy {ns:8.2f} ns/elem")
    ax = rng.uniform(-10.0, 10.0, n)
    t_ai = bench(lambda a: sp.airy(a)[0], ax) / n * 1e9
    t_bi = bench(lambda a: sp.airy(a)[2], ax) / n * 1e9
    print(f"airy_Ai    scipy {t_ai:8.2f} ns/elem")
    print(f"airy_Bi    scipy {t_bi:8.2f} ns/elem")


if __name__ == "__main__":
    main()
