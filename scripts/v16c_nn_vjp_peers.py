#!/usr/bin/env python3
# v16c_nn_vjp_peers.py — Phase 3.1.6 v16-c: the torch + JAX value+grad PARITY peers for the reverse-mode NN VJPs.
# Runs the SAME MLP and CNN as external/crd_v16c_nn_vjp_bench.cpp (matched f64, dims, deterministic init, layouts,
# mean cross-entropy) and prints loss + per-parameter gradient checksums (the parity fingerprint) + median ns for
# value+grad. Run pinned single-thread: taskset -c 4 python3 scripts/v16c_nn_vjp_peers.py
import os
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("XLA_FLAGS", "--xla_cpu_multi_thread_eigen=false intra_op_parallelism_threads=1")
import time
import numpy as np

# ---- shared init (must byte-match the C++ index formulas) ----
def mlp_init():
    B, D, H, C = 64, 64, 128, 10
    X  = (0.10 * np.sin(0.3 + 0.017 * np.arange(B * D))).reshape(B, D)
    W1 = (0.05 * np.cos(0.6 + 0.011 * np.arange(D * H))).reshape(D, H)
    b1 = (0.01 * ((np.arange(H) % 7) - 3))
    W2 = (0.04 * np.sin(0.2 + 0.013 * np.arange(H * C))).reshape(H, C)
    b2 = (0.005 * np.arange(C))
    labels = (np.arange(B) % C)
    return B, D, H, C, X, W1, b1, W2, b2, labels

def cnn_init():
    N, C, HH, WW, OC, kh, kw = 32, 3, 16, 16, 8, 3, 3
    oh2, ow2, Cls = 8, 8, 10
    flat = OC * oh2 * ow2
    X     = (0.10 * np.sin(0.4 + 0.007 * np.arange(N * C * HH * WW))).reshape(N, C, HH, WW)
    convW = (0.08 * np.cos(0.6 + 0.031 * np.arange(OC * C * kh * kw))).reshape(OC, C, kh, kw)
    convB = (0.02 * ((np.arange(OC) % 5) - 2))
    W2    = (0.03 * np.sin(0.2 + 0.005 * np.arange(flat * Cls))).reshape(flat, Cls)
    b2    = (0.005 * np.arange(Cls))
    labels = (np.arange(N) % Cls)
    return N, C, HH, WW, OC, flat, Cls, X, convW, convB, W2, b2, labels

def median_ns(fn, reps):
    for _ in range(5):
        fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter_ns()
        fn()
        ts.append(time.perf_counter_ns() - t0)
    ts.sort()
    return ts[reps // 2]

# ================================ PyTorch ================================
def torch_peer():
    import torch
    torch.set_num_threads(1)
    torch.set_default_dtype(torch.float64)
    print(f"[torch {torch.__version__}] threads={torch.get_num_threads()}")

    # ---- MLP ----
    B, D, H, C, X, W1, b1, W2, b2, labels = mlp_init()
    tX = torch.tensor(X); tlab = torch.tensor(labels, dtype=torch.long)
    p = {k: torch.tensor(v, requires_grad=True) for k, v in
         dict(W1=W1, b1=b1, W2=W2, b2=b2).items()}
    ce = torch.nn.functional.cross_entropy
    def mlp_loss():
        z1 = tX @ p["W1"] + p["b1"]
        a1 = torch.relu(z1)
        z2 = a1 @ p["W2"] + p["b2"]
        return ce(z2, tlab, reduction="mean")
    def mlp_vg():
        for t in p.values(): t.grad = None
        mlp_loss().backward()
    mlp_vg()
    L = mlp_loss().item()
    print(f"MLP  loss={L:.12f}  sum(gW1)={p['W1'].grad.sum():.10f} sum(gb1)={p['b1'].grad.sum():.10f} "
          f"sum(gW2)={p['W2'].grad.sum():.10f} sum(gb2)={p['b2'].grad.sum():.10f}")
    print(f"  >> torch value+grad: {median_ns(mlp_vg, 4000):.0f} ns (median) <<")

    # ---- CNN ----
    N, Cc, HH, WW, OC, flat, Cls, Xc, convW, convB, W2c, b2c, labc = cnn_init()
    tXc = torch.tensor(Xc); tlabc = torch.tensor(labc, dtype=torch.long)
    pc = {"convW": torch.tensor(convW, requires_grad=True),
          "convB": torch.tensor(convB, requires_grad=True),
          "W2": torch.tensor(W2c, requires_grad=True),
          "b2": torch.tensor(b2c, requires_grad=True)}
    def cnn_loss():
        y = torch.nn.functional.conv2d(tXc, pc["convW"], pc["convB"], stride=1, padding=1)
        y = torch.relu(y)
        y = torch.nn.functional.max_pool2d(y, 2, 2)
        y = y.reshape(N, flat)
        z = y @ pc["W2"] + pc["b2"]
        return ce(z, tlabc, reduction="mean")
    def cnn_vg():
        for t in pc.values(): t.grad = None
        cnn_loss().backward()
    cnn_vg()
    Lc = cnn_loss().item()
    print(f"CNN  loss={Lc:.12f}  sum(gConvW)={pc['convW'].grad.sum():.10f} sum(gConvB)={pc['convB'].grad.sum():.10f} "
          f"sum(gW2)={pc['W2'].grad.sum():.10f} sum(gb2)={pc['b2'].grad.sum():.10f}")
    print(f"  >> torch value+grad: {median_ns(cnn_vg, 400):.0f} ns (median) <<")

# ================================ JAX ================================
def jax_peer():
    import jax
    jax.config.update("jax_enable_x64", True)
    import jax.numpy as jnp
    print(f"[jax {jax.__version__}] devices={jax.devices()}")

    B, D, H, C, X, W1, b1, W2, b2, labels = mlp_init()
    jX = jnp.asarray(X); jlab = jnp.asarray(labels)
    params = (jnp.asarray(W1), jnp.asarray(b1), jnp.asarray(W2), jnp.asarray(b2))
    def loss_fn(prm):
        W1_, b1_, W2_, b2_ = prm
        a1 = jnp.maximum(jX @ W1_ + b1_, 0.0)
        z2 = a1 @ W2_ + b2_
        logp = z2 - jax.scipy.special.logsumexp(z2, axis=1, keepdims=True)
        return -jnp.mean(logp[jnp.arange(B), jlab])
    vg = jax.jit(jax.value_and_grad(loss_fn))
    L, g = vg(params); L.block_until_ready()
    print(f"MLP  loss={float(L):.12f}  sum(gW1)={float(g[0].sum()):.10f} sum(gb1)={float(g[1].sum()):.10f} "
          f"sum(gW2)={float(g[2].sum()):.10f} sum(gb2)={float(g[3].sum()):.10f}")
    def call():
        v, gg = vg(params); v.block_until_ready(); [x.block_until_ready() for x in gg]
    print(f"  >> jax value+grad: {median_ns(call, 4000):.0f} ns (median) <<")

    # ---- CNN ----
    from jax import lax
    N, Cc, HH, WW, OC, flat, Cls, Xc, convW, convB, W2c, b2c, labc = cnn_init()
    jXc = jnp.asarray(Xc); jlabc = jnp.asarray(labc)
    pcnn = (jnp.asarray(convW), jnp.asarray(convB), jnp.asarray(W2c), jnp.asarray(b2c))
    def cnn_loss(prm):
        cW, cB, W2_, b2_ = prm
        y = lax.conv_general_dilated(jXc, cW, (1, 1), "SAME",
                                     dimension_numbers=("NCHW", "OIHW", "NCHW"))
        y = y + cB[None, :, None, None]
        y = jnp.maximum(y, 0.0)
        y = lax.reduce_window(y, -jnp.inf, lax.max, (1, 1, 2, 2), (1, 1, 2, 2), "VALID")
        y = y.reshape(N, flat)
        z = y @ W2_ + b2_
        logp = z - jax.scipy.special.logsumexp(z, axis=1, keepdims=True)
        return -jnp.mean(logp[jnp.arange(N), jlabc])
    vgc = jax.jit(jax.value_and_grad(cnn_loss))
    Lc, gc = vgc(pcnn); Lc.block_until_ready()
    print(f"CNN  loss={float(Lc):.12f}  sum(gConvW)={float(gc[0].sum()):.10f} sum(gConvB)={float(gc[1].sum()):.10f} "
          f"sum(gW2)={float(gc[2].sum()):.10f} sum(gb2)={float(gc[3].sum()):.10f}")
    def callc():
        v, gg = vgc(pcnn); v.block_until_ready(); [x.block_until_ready() for x in gg]
    print(f"  >> jax value+grad: {median_ns(callc, 400):.0f} ns (median) <<")

if __name__ == "__main__":
    try:
        torch_peer()
    except Exception as e:  # noqa
        print("torch peer FAILED:", e)
    print("-" * 60)
    try:
        jax_peer()
    except Exception as e:  # noqa
        print("jax peer FAILED:", e)
