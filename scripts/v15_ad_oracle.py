#!/usr/bin/env python3
"""v15 forward-AD reconstruct-verify ORACLE (template).

Peer oracle for Cerid v15 (forward mode). JAX at full f64 is the ground truth the
Cerid `Dual`/`Jet` rules are verified against BEFORE porting a single C++ line
(the reconstruct-verify-in-python-first doctrine). This file is the TEMPLATE:
v15-b extends it per crd::math rule (sin/cos/exp/log/pow/erf/...), v15-d for the
SIMD vector (multi-direction) mode, v15-g for Taylor-mode jets (jax.experimental.jet).

Run (WSL): ~/.venvs/crd-ad/bin/python /mnt/d/Dev/cerid/scripts/v15_ad_oracle.py

Everything prints to full f64 precision so the C++ gate can compare to 0/1 ULP.
"""

import jax

jax.config.update("jax_enable_x64", True)  # forward AD is verified at f64
import jax.numpy as jnp
from jax import jvp, grad, jacfwd


def f(x):
    # f(x) = sin(x^2) + 3 ln(x)   ->   f'(x) = 2x cos(x^2) + 3/x
    return jnp.sin(x * x) + 3.0 * jnp.log(x)


def g(v):
    # g(x,y) = x e^y + y sin(x)
    x, y = v[0], v[1]
    return x * jnp.exp(y) + y * jnp.sin(x)


def F(v):
    # F(x,y) = [x^2 y ; x + sin y]   ->   J = [[2xy, x^2],[1, cos y]]
    x, y = v[0], v[1]
    return jnp.stack([x * x * y, x + jnp.sin(y)])


def p(name, val):
    arr = jnp.asarray(val)
    flat = ", ".join(f"{c:.17g}" for c in arr.reshape(-1).tolist())
    print(f"  {name:<16} = [{flat}]")


def main():
    print(f"JAX {jax.__version__}  x64={jax.config.read('jax_enable_x64')}")

    # 1) scalar jvp: directional derivative of f at x=1.3 in direction 1.0 == f'(1.3)
    x0 = 1.3
    val, dval = jvp(f, (x0,), (1.0,))
    p("f(1.3)", val)
    p("f'(1.3) [jvp]", dval)

    # 2) gradient of g at (1.3, 0.7)
    v0 = jnp.array([1.3, 0.7])
    p("g(v0)", g(v0))
    p("grad g", grad(g)(v0))

    # 3) forward-mode Jacobian of F at (1.1, 0.9)
    w0 = jnp.array([1.1, 0.9])
    p("F(w0)", F(w0))
    p("jacfwd F", jacfwd(F)(w0))

    # 4) Taylor-mode jet (v15-g template): series coeffs of f at x=1.3, order 3.
    #    jet returns (primal, [d1, d2/1!*?, ...]) as normalized series terms.
    try:
        from jax.experimental import jet as jax_jet

        primals = (x0,)
        # series: first-order seed 1.0, higher-order seeds 0.0 -> pure Taylor of f(x0 + t)
        series = ([1.0, 0.0, 0.0],)
        y0, (y1, y2, y3) = jax_jet.jet(f, primals, series)
        p("jet f primal", y0)
        p("jet f d1..d3", [y1, y2, y3])
    except Exception as e:  # jax.experimental.jet is stable but guard anyway
        print(f"  jet: N/A ({type(e).__name__}: {e})")

    print("ORACLE OK")


if __name__ == "__main__":
    main()
