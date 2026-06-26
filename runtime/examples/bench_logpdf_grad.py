# v12-l crush peers — gradient of Sum_i logpdf(x_i | theta) w.r.t. theta (the HMC/MLE gradient), the same quantity
# bench_logpdf_grad.cpp times analytically. Compares Cerid against the AUTODIFF peers (NumPy/scipy/Boost have NO
# logpdf-gradient API, so they are not applicable to this task; MATLAB only does it inside hmcSampler). All f64,
# 1-thread, ns per data point. Run: python3 runtime/examples/bench_logpdf_grad.py
import time

import torch

import jax

jax.config.update("jax_enable_x64", True)  # f64, to match Cerid + PyTorch (JAX defaults to f32)
import jax.numpy as jnp
from jax import grad, jit
from jax.scipy.special import gammaln

torch.set_num_threads(1)
N = 1 << 20
REPS = 50

xs_t = 0.5 + (torch.arange(N) % 997).double() * 0.001
xs_j = jnp.asarray(0.5 + (jnp.arange(N) % 997).astype(jnp.float64) * 0.001)
LN2PI = 1.8378770664093453


def bench_torch(name, dist_fn, params):
    ps = [torch.tensor(p, dtype=torch.float64, requires_grad=True) for p in params]
    t0 = time.perf_counter()
    for _ in range(REPS):
        for p in ps:
            p.grad = None
        dist_fn(*ps).log_prob(xs_t).sum().backward()
    return (time.perf_counter() - t0) / REPS * 1e9  # ns per gradient call (= per leapfrog step)


def bench_jax(name, logpdf_sum, params):
    g = jit(grad(logpdf_sum, argnums=tuple(range(len(params)))))
    a = [jnp.float64(p) for p in params]
    jax.block_until_ready(g(*a))  # warmup: JIT-compile
    t0 = time.perf_counter()
    for _ in range(REPS):
        jax.block_until_ready(g(*a))
    return (time.perf_counter() - t0) / REPS * 1e9  # ns per gradient call (= per leapfrog step)


CASES = [
    ("Normal",
     lambda mu, sig: torch.distributions.Normal(mu, sig),
     lambda mu, sig: (-0.5 * ((xs_j - mu) / sig) ** 2 - jnp.log(sig) - 0.5 * LN2PI).sum(),
     [1.0, 2.0]),
    ("Gamma",  # torch uses (concentration, rate); jax/Cerid (shape, scale) — same 2-param cost
     lambda a, rate: torch.distributions.Gamma(a, rate),
     lambda a, s: ((a - 1.0) * jnp.log(xs_j) - xs_j / s - a * jnp.log(s) - gammaln(a)).sum(),
     [2.5, 1.0 / 1.3]),
    ("StudentT",
     lambda nu: torch.distributions.StudentT(nu),
     lambda nu: (gammaln((nu + 1) / 2) - gammaln(nu / 2) - 0.5 * jnp.log(nu * jnp.pi)
                 - (nu + 1) / 2 * jnp.log1p(xs_j ** 2 / nu)).sum(),
     [5.0]),
]

print(f"Peer per-leapfrog gradient ({N}-point dataset x {REPS} calls, 1-thread, f64), ns/call:")
print(f"{'dist':10s} {'PyTorch':>14s} {'JAX(jit)':>14s}")
for name, tfn, jfn, tparams in CASES:
    jparams = tparams[:1] if name == "StudentT" else ([tparams[0], 1.3] if name == "Gamma" else tparams)
    nt = bench_torch(name, tfn, tparams)
    nj = bench_jax(name, jfn, jparams)
    print(f"{name:10s} {nt:10.3f} {nj:10.3f}")
