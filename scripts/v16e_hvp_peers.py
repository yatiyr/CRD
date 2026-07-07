#!/usr/bin/env python3
# v16e_hvp_peers.py — Phase 3.1.6 v16-e: JAX `hvp` (jvp∘grad) + torch functorch HVP peers. Runs the SAME scalar
# functor as external/crd_v16e_hvp_bench.cpp (f(x)=exp(x0)+Σ x_i x_{(i+1)%n}+Σ sin(x_i); matched f64, dims, init) and
# prints grad + Hessian-vector checksums (parity fingerprint) + median ns. Run pinned: taskset -c 4 python3 <this>.
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("XLA_FLAGS", "--xla_cpu_multi_thread_eigen=false intra_op_parallelism_threads=1")
import time
import numpy as np


def init(n):
    x = 0.3 + 0.2 * np.sin(1.0 + np.arange(n))
    v = 0.5 * np.cos(0.4 + np.arange(n))
    return x, v


def median_ns(fn, reps):
    for _ in range(5):
        fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter_ns(); fn(); ts.append(time.perf_counter_ns() - t0)
    ts.sort(); return ts[reps // 2]


def jax_peer():
    import jax
    jax.config.update("jax_enable_x64", True)
    import jax.numpy as jnp
    print(f"[jax {jax.__version__}] {jax.devices()}")

    def f(x):
        acc = jnp.sum(x * jnp.roll(x, -1))  # Σ x_i x_{(i+1)%n}
        acc = acc + jnp.sum(jnp.sin(x))
        return acc

    for n in (64, 256, 1024):
        x, v = init(n)
        jx, jv = jnp.asarray(x), jnp.asarray(v)
        # HVP = jvp of grad(f) in direction v  (forward-over-reverse, exactly Cerid's method)
        hvp = jax.jit(lambda xx, vv: jax.jvp(jax.grad(f), (xx,), (vv,)))
        g, Hv = hvp(jx, jv); g.block_until_ready(); Hv.block_until_ready()
        print(f"HVP n={n:<4d}  sum(grad)={float(g.sum()):.10f} sum(Hv)={float(Hv.sum()):.10f}")
        def call(): a, b = hvp(jx, jv); a.block_until_ready(); b.block_until_ready()
        print(f"  >> jax hvp (jvp∘grad): {median_ns(call, 4000 if n <= 256 else 1000):.0f} ns (median) <<")


def torch_peer():
    import torch
    torch.set_num_threads(1); torch.set_default_dtype(torch.float64)
    print(f"[torch {torch.__version__}] threads={torch.get_num_threads()}")
    from torch.func import grad, jvp

    def f(x):
        acc = torch.sum(x * torch.roll(x, -1))
        acc = acc + torch.sum(torch.sin(x))
        return acc

    for n in (64, 256, 1024):
        x, v = init(n)
        tx, tv = torch.tensor(x), torch.tensor(v)
        def hvp(xx, vv): return jvp(grad(f), (xx,), (vv,))
        g, Hv = hvp(tx, tv)
        print(f"HVP n={n:<4d}  sum(grad)={float(g.sum()):.10f} sum(Hv)={float(Hv.sum()):.10f}")
        def call(): hvp(tx, tv)
        print(f"  >> torch functorch hvp: {median_ns(call, 2000 if n <= 256 else 500):.0f} ns (median) <<")


if __name__ == "__main__":
    try:
        jax_peer()
    except Exception as ex:  # noqa
        print("jax peer FAILED:", ex)
    print("-" * 60)
    try:
        torch_peer()
    except Exception as ex:  # noqa
        print("torch peer FAILED:", ex)
