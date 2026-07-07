#!/usr/bin/env python3
# v16d_matrix_peers.py — Phase 3.1.6 v16-d: JAX (+ torch) value+grad PARITY peers for the matrix-calculus + suite VJPs,
# and the value-only DEGENERACY probe. Runs the SAME solve / logdet / svdvals / eigvals / fft value+grad as
# external/crd_v16d_matrix_bench.cpp (matched f64, dims, deterministic init) and prints loss + grad checksums + the
# degeneracy result (does JAX/torch NaN on repeated singular values?). Run pinned: taskset -c 4 python3 <this>.
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


def jax_peer():
    import jax
    jax.config.update("jax_enable_x64", True)
    import jax.numpy as jnp
    print(f"[jax {jax.__version__}] {jax.devices()}")

    # SOLVE  L = Σ solve(A,B)
    n, p = 32, 4
    A = (np.sin(0.3 + 1.7 * np.arange(n)[:, None] + 0.9 * np.arange(n)[None, :]) + 10.0 * np.eye(n))
    B = (0.4 * np.cos(0.5 + np.arange(n * p))).reshape(n, p)
    jA, jB = jnp.asarray(A), jnp.asarray(B)
    def solve_loss(a, b): return jnp.sum(jnp.linalg.solve(a, b))
    vg = jax.jit(jax.value_and_grad(solve_loss, argnums=(0, 1)))
    L, (gA, gB) = vg(jA, jB); L.block_until_ready()
    print(f"SOLVE n={n} p={p}  loss={float(L):.10f} sum(gA)={float(gA.sum()):.10f} sum(gB)={float(gB.sum()):.10f}")
    def call(): v, g = vg(jA, jB); v.block_until_ready(); [x.block_until_ready() for x in g]
    print(f"  >> jax value+grad: {median_ns(call, 4000):.0f} ns (median) <<")

    # LOGDET (SPD)  L = logdet(A)
    n = 24
    Bm = np.sin(0.6 + 1.3 * np.arange(n)[:, None] + 0.7 * np.arange(n)[None, :])
    Asp = Bm @ Bm.T + n * np.eye(n)
    def logdet_loss(a): return jnp.linalg.slogdet(a)[1]
    L, gA = jax.value_and_grad(logdet_loss)(jnp.asarray(Asp))
    print(f"LOGDET n={n}  loss={float(L):.10f} sum(gA)={float(gA.sum()):.10f}")

    # SVDVALS  L = Σ σ(A)
    m, n = 24, 16
    ii = np.arange(m)[:, None]; jj = np.arange(n)[None, :]
    Asv = np.sin(0.5 + 1.1 * ii + 0.6 * jj) + 0.3 * np.sin(0.17 + 0.13 * ii * jj)  # full rank (distinct σ)
    def svd_loss(a): return jnp.sum(jnp.linalg.svd(a, compute_uv=False))
    L, gA = jax.value_and_grad(svd_loss)(jnp.asarray(Asv))
    print(f"SVDVALS m={m} n={n}  loss={float(L):.10f} sum(gA)={float(gA.sum()):.10f}")
    # DEGENERACY: identity-columns -> repeated σ=1
    Ad = np.zeros((m, n)); Ad[:n, :n] = np.eye(n)
    Lg, gAd = jax.value_and_grad(svd_loss)(jnp.asarray(Ad))
    isnan = bool(np.any(np.isnan(np.asarray(gAd))))
    print(f"  DEGENERACY (repeated σ=1): jax svdvals-grad has NaN={isnan} sum(gA)={float(np.nansum(np.asarray(gAd))):.10f}")

    # EIGVALS (sym)  L = Σ λ²  (order-invariant)
    n = 20
    ii = np.arange(n)[:, None]; jj = np.arange(n)[None, :]
    Bm = np.cos(0.4 + 1.1 * ii + 0.5 * jj) + 0.3 * np.sin(0.11 * ii * jj)  # full rank (distinct λ)
    Ae = Bm @ Bm.T + n * np.eye(n)
    def eig_loss(a): return jnp.sum(jnp.linalg.eigvalsh(a) ** 2)
    L, gA = jax.value_and_grad(eig_loss)(jnp.asarray(Ae))
    print(f"EIGVALS n={n}  loss={float(L):.10f} sum(gA)={float(gA.sum()):.10f}")

    # FFT  L = Re Σ conj(w) fft(x)   (x real)
    n = 64
    x = np.sin(0.3 + 0.2 * np.arange(n))
    w = np.cos(0.5 + np.arange(n)) + 1j * np.sin(0.4 + 0.7 * np.arange(n))
    jw = jnp.asarray(w)
    def fft_loss(xx): return jnp.real(jnp.sum(jnp.conj(jw) * jnp.fft.fft(xx)))
    L, gx = jax.value_and_grad(fft_loss)(jnp.asarray(x))
    print(f"FFT n={n}  loss={float(L):.10f} sum(grad_x)={float(gx.sum()):.10f}")


def torch_peer():
    import torch
    torch.set_num_threads(1); torch.set_default_dtype(torch.float64)
    print(f"[torch {torch.__version__}] threads={torch.get_num_threads()}")
    # SVDVALS degeneracy — the headline: does torch svdvals grad NaN at repeated σ?
    m, n = 24, 16
    Ad = torch.zeros(m, n, dtype=torch.float64); Ad[:n, :n] = torch.eye(n, dtype=torch.float64)
    Ad.requires_grad_(True)
    torch.linalg.svdvals(Ad).sum().backward()
    isnan = bool(torch.isnan(Ad.grad).any())
    print(f"  DEGENERACY (repeated σ=1): torch svdvals-grad has NaN={isnan}")
    # full SVD grad at repeated σ (the F-matrix path) — expected to NaN
    Ad2 = torch.zeros(m, n, dtype=torch.float64); Ad2[:n, :n] = torch.eye(n, dtype=torch.float64)
    Ad2.requires_grad_(True)
    U, S, Vh = torch.linalg.svd(Ad2, full_matrices=False)
    (U.sum() + S.sum() + Vh.sum()).backward()
    isnan2 = bool(torch.isnan(Ad2.grad).any())
    print(f"  DEGENERACY (full SVD U+S+V grad): torch has NaN={isnan2}")


if __name__ == "__main__":
    try:
        jax_peer()
    except Exception as e:  # noqa
        print("jax peer FAILED:", e)
    print("-" * 60)
    try:
        torch_peer()
    except Exception as e:  # noqa
        print("torch peer FAILED:", e)
