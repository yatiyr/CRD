#!/usr/bin/env python3
# v16h_codegen_peers.py — Phase 3.1.6 v16-h: JAX jit (the XLA-codegen peer) for the same value+grad function as
# external/crd_v16h_codegen_bench.cpp. Both codegen a differentiated kernel; Cerid emits portable C++ (no LLVM/XLA
# runtime), JAX jits to XLA. Run: taskset -c 4 python3 scripts/v16h_codegen_peers.py
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("XLA_FLAGS", "--xla_cpu_multi_thread_eigen=false intra_op_parallelism_threads=1")
import time
import numpy as np


def median_ns(fn, reps):
    for _ in range(5):
        fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter_ns(); fn(); ts.append(time.perf_counter_ns() - t0)
    ts.sort(); return ts[reps // 2]


def main():
    import jax
    import jax.numpy as jnp
    jax.config.update("jax_enable_x64", True)
    n = 32
    x = jnp.array([0.3 + 0.2 * np.sin(1.0 + i) for i in range(n)])

    def f(x):  # Σ sin(x_i)·cos(x_{i+1}) + exp(0.1 x_i)   (matches the Cerid functor)
        acc = x[0] * x[0]
        acc = acc + jnp.sum(jnp.sin(x[:-1]) * jnp.cos(x[1:]) + jnp.exp(0.1 * x[:-1]))
        return acc

    val_grad = jax.jit(jax.value_and_grad(f))
    v, g = val_grad(x)
    v.block_until_ready(); g.block_until_ready()
    print(f"[jax {jax.__version__} jit/XLA]  value={float(v):.14f}  Sum_grad={float(g.sum()):.14f}")
    t = median_ns(lambda: val_grad(x)[1].block_until_ready(), 5000)
    print(f"  >> jax jit value+grad (XLA-codegen): {t:.0f} ns (median) <<")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa
        print("jax peer FAILED:", repr(e)[:200])
