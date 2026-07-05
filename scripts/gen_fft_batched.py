#!/usr/bin/env python3
"""gen_fft_batched.py — the rebuilt batched-codelet generator (2026-07-03 FFT campaign).

Rebuilds the lost build/gen_subfft_m3.py capability for the BATCHED SoA form: emits
codelet{N}_batched(const Complex<f64>*, Complex<f64>*, usize b) — split-radix DAG + CSE +
register-pressure schedule, Vec4d over the batch axis, in the EXACT style of the surviving
generated kernels in hier_codelets.hpp (loads first, plain V(const) mul/add pairs, stores
last => in-place-safe by construction). Numerically validated vs numpy IN this script; the
engine's four-step oracle gates it end-to-end.

Run (WSL): python3 scripts/gen_fft_batched.py > engine/hesap-fft/include/crd/hesap/fft/detail/batched_codelets_gen.hpp
(optional argv[1] = scheduler mode: hybrid2 [tracked default] | hybrid | greedy | belady — see SCHED_VARIANT)
"""
import cmath

import numpy as np

# ---- hash-consed DAG core (from gen_fft_crush.py, return typos fixed) ----
_TABLE = {}
_NODES = []


class Node:
    __slots__ = ("op", "a", "b", "c", "id")

    def __init__(self, op, a=None, b=None, c=None):
        self.op, self.a, self.b, self.c, self.id = op, a, b, c, len(_NODES)


def _node(op, a=None, b=None, c=None):
    key = (op, a.id if isinstance(a, Node) else a, b.id if isinstance(b, Node) else b, c)
    h = _TABLE.get(key)
    if h is not None:
        return h
    n = Node(op, a, b, c)
    _TABLE[key] = n
    _NODES.append(n)
    return n


def inp(i):
    return _node("input", i)


def add(a, b):
    return _node("add", a, b)


def sub(a, b):
    return _node("sub", a, b)


def cmul(a, w):
    if abs(w - 1) < 1e-15:
        return a
    if abs(w + 1) < 1e-15:
        return _node("neg", a)
    if abs(w - 1j) < 1e-15:
        return _node("muli", a)
    if abs(w + 1j) < 1e-15:
        return _node("mulni", a)
    return _node("cmul", a, None, (round(w.real, 17), round(w.imag, 17)))


def fft_dag(x):
    n = len(x)
    if n == 1:
        return [x[0]]
    if n == 2:
        return [add(x[0], x[1]), sub(x[0], x[1])]
    U = fft_dag(x[0::2])
    Z1 = fft_dag(x[1::4])
    Z3 = fft_dag(x[3::4])
    out = [None] * n
    q = n // 4
    for k in range(q):
        w1 = cmath.exp(-2j * cmath.pi * k / n)
        w3 = cmath.exp(-2j * cmath.pi * 3 * k / n)
        t1 = cmul(Z1[k], w1)
        t3 = cmul(Z3[k], w3)
        s = add(t1, t3)
        d = sub(t1, t3)
        di = _node("mulni", d)
        out[k] = add(U[k], s)
        out[k + n // 2] = sub(U[k], s)
        out[k + q] = add(U[k + q], di)
        out[k + 3 * q] = sub(U[k + q], di)
    return out


def evaluate(outputs, xvals):
    memo = {}

    def ev(node):
        if node.id in memo:
            return memo[node.id]
        op = node.op
        if op == "input":
            v = xvals[node.a]
        elif op == "add":
            v = ev(node.a) + ev(node.b)
        elif op == "sub":
            v = ev(node.a) - ev(node.b)
        elif op == "neg":
            v = -ev(node.a)
        elif op == "muli":
            v = 1j * ev(node.a)
        elif op == "mulni":
            v = -1j * ev(node.a)
        elif op == "cmul":
            v = complex(node.c[0], node.c[1]) * ev(node.a)
        else:
            raise ValueError(op)
        memo[node.id] = v
        return v

    return [ev(o) for o in outputs]


def _topo(outputs):
    order, seen = [], set()

    def visit(nd):
        if nd.id in seen:
            return
        seen.add(nd.id)
        for ch in (nd.a, nd.b):
            if isinstance(ch, Node):
                visit(ch)
        order.append(nd)

    for o in outputs:
        visit(o)
    return order


# Scheduler selection (A/B'd 2026-07-03): "greedy" = freed-count; "belady" adds a shortest-live-range
# tiebreak. Measured: belady wins broadly on the Vec8f/f32 kernels (+8..13% on the f32 sh band) and
# LOSES badly on the f64 deep-split kernels (256K −16%, 512K −68% — spill regime differs at 4 lanes).
# ⇒ the tracked default is "hybrid": f32 emitters schedule belady, f64 emitters greedy.
SCHED_VARIANT = "greedy"


def schedule(outputs):
    nodes = _topo(outputs)
    ins_of = {}
    deps = {nd.id: [] for nd in nodes}
    for nd in nodes:
        ins = [c for c in (nd.a, nd.b) if isinstance(c, Node)]
        ins_of[nd.id] = ins
        for c in ins:
            deps[c.id].append(nd)
    outset = {o.id for o in outputs}
    uses = {nd.id: len(deps[nd.id]) + (1 if nd.id in outset else 0) for nd in nodes}
    indeg = {nd.id: len(ins_of[nd.id]) for nd in nodes}
    ready = [nd for nd in nodes if indeg[nd.id] == 0]
    order = []
    while ready:
        if SCHED_VARIANT == "belady":
            # Belady-window tiebreak: among equal freed-counts prefer the node whose value will be
            # consumed SOONEST (min remaining-indegree over its consumers) — shortest live range first.
            def score(nd):
                freed = sum(1 for c in ins_of[nd.id] if uses[c.id] == 1)
                near = min((indeg[u.id] - 1 for u in deps[nd.id]), default=0)
                return (freed, -near, -nd.id)
        else:
            def score(nd):
                freed = sum(1 for c in ins_of[nd.id] if uses[c.id] == 1)
                return (freed, -nd.id)

        ready.sort(key=score, reverse=True)
        nd = ready.pop(0)
        order.append(nd)
        for c in ins_of[nd.id]:
            uses[c.id] -= 1
        for u in deps[nd.id]:
            indeg[u.id] -= 1
            if indeg[u.id] == 0:
                ready.append(u)
    return order


# ---- the batched-SoA Vec4d emitter (the hier_codelets.hpp house style) ----
def emit_batched_f64(N, name):
    global _TABLE, _NODES
    _TABLE, _NODES = {}, []
    xs = [inp(i) for i in range(N)]
    outs = fft_dag(xs)

    # numpy gate: 8 random vectors, rel err < 1e-12
    rng = np.random.default_rng(1407 + N)
    for _ in range(8):
        xv = rng.standard_normal(N) + 1j * rng.standard_normal(N)
        got = np.array(evaluate(outs, list(xv)))
        ref = np.fft.fft(xv)
        rel = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
        assert rel < 1e-12, f"N={N} DAG validation FAILED rel={rel}"

    order = schedule(outs)
    outset = {o.id for o in outs}
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::f64>* in,",
        "    crd::hesap::Complex<crd::f64>* out, crd::usize b) noexcept",
    ]
    L = []
    L.append("    using V = crd::math::simd::Vec4d;")
    L.append("    namespace simd = crd::math::simd;")
    L.append("    for (crd::usize t = 0; t + 4 <= b; t += 4)")
    L.append("    {")
    for nd in order:
        v = f"n{nd.id}"
        if nd.op == "input":
            L.append(
                f"        V {v}r, {v}i; simd::load_complex_deinterleaved("
                f"reinterpret_cast<const crd::f64*>(in + {nd.a} * b + t), {v}r, {v}i);"
            )
        elif nd.op == "add":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r + {c}r, {v}i = {a}i + {c}i;")
        elif nd.op == "sub":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r - {c}r, {v}i = {a}i - {c}i;")
        elif nd.op == "neg":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0) - {a}r, {v}i = V(0.0) - {a}i;")
        elif nd.op == "muli":  # +i*z = (-zi, zr)
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0) - {a}i, {v}i = {a}r;")
        elif nd.op == "mulni":  # -i*z = (zi, -zr)
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = {a}i, {v}i = V(0.0) - {a}r;")
        elif nd.op == "cmul":
            a = f"n{nd.a.id}"
            wr, wi = nd.c
            L.append(
                f"        const V {v}r = simd::fnmadd(V({wi!r}), {a}i, V({wr!r}) * {a}r), "
                f"{v}i = simd::fma(V({wi!r}), {a}r, V({wr!r}) * {a}i);"
            )
    for k, o in enumerate(outs):
        L.append(
            f"        simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(out + {k} * b + t), "
            f"n{o.id}r, n{o.id}i);"
        )
    L.append("    }")
    return _assemble(sig, L, _scalar_batched(order, outs, "f64"))


def emit_stage1_fused_f64(N1, name):
    """Stage-1 for the standalone-hier: N1-point leaf over an n2-wide column batch (runtime n2),
    FUSED inter-stage twiddle (runtime table, [k*n2+i2] layout) + 4x4-transposed store into the
    stage-2 element-major layout out[i2*N1 + k]. One pass replaces leaf + twiddle + transpose."""
    global _TABLE, _NODES
    _TABLE, _NODES = {}, []
    xs = [inp(i) for i in range(N1)]
    outs = fft_dag(xs)
    rng = np.random.default_rng(2707 + N1)
    for _ in range(4):
        xv = rng.standard_normal(N1) + 1j * rng.standard_normal(N1)
        got = np.array(evaluate(outs, list(xv)))
        ref = np.fft.fft(xv)
        rel = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
        assert rel < 1e-12, f"N1={N1} stage1 DAG validation FAILED rel={rel}"
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::f64>* in,",
        "    crd::hesap::Complex<crd::f64>* out, crd::usize n2, const crd::f64* twr,",
        "    const crd::f64* twi) noexcept",
    ]
    L = []
    L.append("    using V = crd::math::simd::Vec4d;")
    L.append("    namespace simd = crd::math::simd;")
    L.append("    for (crd::usize t = 0; t + 4 <= n2; t += 4)")
    L.append("    {")
    for nd in order:
        v = f"n{nd.id}"
        if nd.op == "input":
            L.append(
                f"        V {v}r, {v}i; simd::load_complex_deinterleaved("
                f"reinterpret_cast<const crd::f64*>(in + {nd.a} * n2 + t), {v}r, {v}i);"
            )
        elif nd.op == "add":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r + {c}r, {v}i = {a}i + {c}i;")
        elif nd.op == "sub":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r - {c}r, {v}i = {a}i - {c}i;")
        elif nd.op == "neg":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0) - {a}r, {v}i = V(0.0) - {a}i;")
        elif nd.op == "muli":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0) - {a}i, {v}i = {a}r;")
        elif nd.op == "mulni":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = {a}i, {v}i = V(0.0) - {a}r;")
        elif nd.op == "cmul":
            a = f"n{nd.a.id}"
            wr, wi = nd.c
            L.append(
                f"        const V {v}r = simd::fnmadd(V({wi!r}), {a}i, V({wr!r}) * {a}r), "
                f"{v}i = simd::fma(V({wi!r}), {a}r, V({wr!r}) * {a}i);"
            )
    # fused twiddle + 4x4-transposed store, k in groups of 4
    for k0 in range(0, N1, 4):
        ids = [outs[k0 + l].id for l in range(4)]
        L.append("        {")
        for l in range(4):
            L.append(
                f"            const V w{l}r = V::load(twr + {k0 + l} * n2 + t), "
                f"w{l}i = V::load(twi + {k0 + l} * n2 + t);"
            )
        for l in range(4):
            L.append(
                f"            V x{l}r = simd::fnmadd(w{l}i, n{ids[l]}i, n{ids[l]}r * w{l}r), "
                f"x{l}i = simd::fma(w{l}i, n{ids[l]}r, n{ids[l]}i * w{l}r);"
            )
        L.append("            simd::transpose4x4(x0r, x1r, x2r, x3r);")
        L.append("            simd::transpose4x4(x0i, x1i, x2i, x3i);")
        for l in range(4):
            L.append(
                f"            simd::store_complex_interleaved("
                f"reinterpret_cast<crd::f64*>(out + (t + {l}) * {N1} + {k0}), x{l}r, x{l}i);"
            )
        L.append("        }")
    L.append("    }")
    return _assemble(sig, L, _scalar_fused_sh(order, outs, "f64", N1))


def emit_batched_f32(N, name):
    """Vec8f edition of emit_batched_f64 (the f32 house style of hier_codelets.hpp): lane width 8,
    Complex<f32>, f32-cast constants. Overloads the f64 names on Complex<f32>."""
    global _TABLE, _NODES
    _TABLE, _NODES = {}, []
    xs = [inp(i) for i in range(N)]
    outs = fft_dag(xs)

    # numpy gate: 8 random vectors, rel err < 1e-12 (the DAG itself is exact; f32 rounding happens in C++)
    rng = np.random.default_rng(1407 + N)
    for _ in range(8):
        xv = rng.standard_normal(N) + 1j * rng.standard_normal(N)
        got = np.array(evaluate(outs, list(xv)))
        ref = np.fft.fft(xv)
        rel = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
        assert rel < 1e-12, f"N={N} DAG validation FAILED rel={rel}"

    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::f32>* in,",
        "    crd::hesap::Complex<crd::f32>* out, crd::usize b) noexcept",
    ]
    L = []
    L.append("    using V = crd::math::simd::Vec8f;")
    L.append("    namespace simd = crd::math::simd;")
    L.append("    for (crd::usize t = 0; t + 8 <= b; t += 8)")
    L.append("    {")
    for nd in order:
        v = f"n{nd.id}"
        if nd.op == "input":
            L.append(
                f"        V {v}r, {v}i; simd::load_complex_deinterleaved("
                f"reinterpret_cast<const crd::f32*>(in + {nd.a} * b + t), {v}r, {v}i);"
            )
        elif nd.op == "add":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r + {c}r, {v}i = {a}i + {c}i;")
        elif nd.op == "sub":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r - {c}r, {v}i = {a}i - {c}i;")
        elif nd.op == "neg":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0F) - {a}r, {v}i = V(0.0F) - {a}i;")
        elif nd.op == "muli":  # +i*z = (-zi, zr)
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0F) - {a}i, {v}i = {a}r;")
        elif nd.op == "mulni":  # -i*z = (zi, -zr)
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = {a}i, {v}i = V(0.0F) - {a}r;")
        elif nd.op == "cmul":
            a = f"n{nd.a.id}"
            wr, wi = nd.c
            L.append(
                f"        const V {v}r = simd::fnmadd(V(static_cast<crd::f32>({wi!r})), {a}i, "
                f"V(static_cast<crd::f32>({wr!r})) * {a}r), "
                f"{v}i = simd::fma(V(static_cast<crd::f32>({wi!r})), {a}r, "
                f"V(static_cast<crd::f32>({wr!r})) * {a}i);"
            )
    for k, o in enumerate(outs):
        L.append(
            f"        simd::store_complex_interleaved(reinterpret_cast<crd::f32*>(out + {k} * b + t), "
            f"n{o.id}r, n{o.id}i);"
        )
    L.append("    }")
    return _assemble(sig, L, _scalar_batched(order, outs, "f32"))


def emit_stage1_fused_f32(N1, name):
    """Vec8f edition of emit_stage1_fused_f64: N1-point leaf over a runtime-n2 column batch, fused
    inter-stage twiddle ([k*n2+i2] table) + 8x8-transposed store into out[i2*N1 + k]. One pass."""
    global _TABLE, _NODES
    _TABLE, _NODES = {}, []
    xs = [inp(i) for i in range(N1)]
    outs = fft_dag(xs)
    rng = np.random.default_rng(2707 + N1)
    for _ in range(4):
        xv = rng.standard_normal(N1) + 1j * rng.standard_normal(N1)
        got = np.array(evaluate(outs, list(xv)))
        ref = np.fft.fft(xv)
        rel = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
        assert rel < 1e-12, f"N1={N1} stage1 DAG validation FAILED rel={rel}"
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::f32>* in,",
        "    crd::hesap::Complex<crd::f32>* out, crd::usize n2, const crd::f32* twr,",
        "    const crd::f32* twi) noexcept",
    ]
    L = []
    L.append("    using V = crd::math::simd::Vec8f;")
    L.append("    namespace simd = crd::math::simd;")
    L.append("    for (crd::usize t = 0; t + 8 <= n2; t += 8)")
    L.append("    {")
    for nd in order:
        v = f"n{nd.id}"
        if nd.op == "input":
            L.append(
                f"        V {v}r, {v}i; simd::load_complex_deinterleaved("
                f"reinterpret_cast<const crd::f32*>(in + {nd.a} * n2 + t), {v}r, {v}i);"
            )
        elif nd.op == "add":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r + {c}r, {v}i = {a}i + {c}i;")
        elif nd.op == "sub":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"        const V {v}r = {a}r - {c}r, {v}i = {a}i - {c}i;")
        elif nd.op == "neg":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0F) - {a}r, {v}i = V(0.0F) - {a}i;")
        elif nd.op == "muli":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = V(0.0F) - {a}i, {v}i = {a}r;")
        elif nd.op == "mulni":
            a = f"n{nd.a.id}"
            L.append(f"        const V {v}r = {a}i, {v}i = V(0.0F) - {a}r;")
        elif nd.op == "cmul":
            a = f"n{nd.a.id}"
            wr, wi = nd.c
            L.append(
                f"        const V {v}r = simd::fnmadd(V(static_cast<crd::f32>({wi!r})), {a}i, "
                f"V(static_cast<crd::f32>({wr!r})) * {a}r), "
                f"{v}i = simd::fma(V(static_cast<crd::f32>({wi!r})), {a}r, "
                f"V(static_cast<crd::f32>({wr!r})) * {a}i);"
            )
    # fused twiddle + 8x8-transposed store, k in groups of 8
    for k0 in range(0, N1, 8):
        ids = [outs[k0 + l].id for l in range(8)]
        L.append("        {")
        for l in range(8):
            L.append(
                f"            const V w{l}r = V::load(twr + {k0 + l} * n2 + t), "
                f"w{l}i = V::load(twi + {k0 + l} * n2 + t);"
            )
        for l in range(8):
            L.append(
                f"            V x{l}r = simd::fnmadd(w{l}i, n{ids[l]}i, n{ids[l]}r * w{l}r), "
                f"x{l}i = simd::fma(w{l}i, n{ids[l]}r, n{ids[l]}i * w{l}r);"
            )
        L.append("            simd::transpose8x8(x0r, x1r, x2r, x3r, x4r, x5r, x6r, x7r);")
        L.append("            simd::transpose8x8(x0i, x1i, x2i, x3i, x4i, x5i, x6i, x7i);")
        for l in range(8):
            L.append(
                f"            simd::store_complex_interleaved("
                f"reinterpret_cast<crd::f32*>(out + (t + {l}) * {N1} + {k0}), x{l}r, x{l}i);"
            )
        L.append("        }")
    L.append("    }")
    return _assemble(sig, L, _scalar_fused_sh(order, outs, "f32", N1))


def _emit_dag_body_scalar(order, tname, batch_var, indent="            ", soa_in=0):
    """Lane-scalar edition of the DAG body (unoptimized builds): the SAME schedule, plain T scalars per
    column `tl` — bit-identical to the SIMD tiles (Vec ops are lane-wise IEEE ops) with ~8x smaller
    /Od frames (the straight-line SIMD bodies need 1.2-1.4 MB when every temporary gets its own slot).
    soa_in (0=off, else lane count L): read AoSoA block-interleaved rows via the emitter-provided
    `ta` (in-row block address of column tl) and `b2` (row stride = 2*batch)."""
    L = []
    for nd in order:
        L += _render_node_scalar(nd, tname, batch_var, indent, soa_in)
    return L


def _render_node_scalar(nd, tname, batch_var, indent="            ", soa_in=0):
    cst = (lambda x: f"{x!r}") if tname == "f64" else (lambda x: f"static_cast<crd::f32>({x!r})")
    L = []
    if True:
        v = f"n{nd.id}"
        if nd.op == "input":
            if soa_in:
                L.append(f"{indent}const T {v}r = inb[{nd.a} * b2 + ta], {v}i = inb[{nd.a} * b2 + ta + {soa_in}];")
            else:
                L.append(f"{indent}const T {v}r = in[{nd.a} * {batch_var} + tl].re, {v}i = in[{nd.a} * {batch_var} + tl].im;")
        elif nd.op == "add":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"{indent}const T {v}r = {a}r + {c}r, {v}i = {a}i + {c}i;")
        elif nd.op == "sub":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"{indent}const T {v}r = {a}r - {c}r, {v}i = {a}i - {c}i;")
        elif nd.op == "neg":
            a = f"n{nd.a.id}"
            # `0 - x` (NOT unary minus): matches the SIMD `V(0.0) - x` on signed zeros bit-for-bit.
            L.append(f"{indent}const T {v}r = static_cast<T>(0.0) - {a}r, {v}i = static_cast<T>(0.0) - {a}i;")
        elif nd.op == "muli":
            a = f"n{nd.a.id}"
            L.append(f"{indent}const T {v}r = static_cast<T>(0.0) - {a}i, {v}i = {a}r;")
        elif nd.op == "mulni":
            a = f"n{nd.a.id}"
            L.append(f"{indent}const T {v}r = {a}i, {v}i = static_cast<T>(0.0) - {a}r;")
        elif nd.op == "cmul":
            a = f"n{nd.a.id}"
            wr, wi = nd.c
            # FMA edition (2026-07-04 crush): scalar std::fma mirrors the simd fma/fnmadd fallbacks
            # bit-for-bit (single-rounded IEEE; std::fma is deterministic like std::sqrt). The negated
            # constant is parenthesized — `-` + a negative literal would lex as `--` (C2105).
            L.append(
                f"{indent}const T {v}r = std::fma(-({cst(wi)}), {a}i, {cst(wr)} * {a}r), "
                f"{v}i = std::fma({cst(wi)}, {a}r, {cst(wr)} * {a}i);"
            )
    return L


def _interleave_stores(order, outs, store_of, group_lanes=0):
    """STORE-EARLY scheduling (2026-07-04 spill crush): the AoSoA pipeline kernels have DISJOINT
    in/out buffers, so each output store can be emitted the moment its value completes — freeing the
    register instead of holding ALL N outputs live to the end (the measured 44K-spill mechanism).
    order: the scheduled DAG; outs: output nodes (terminal); store_of(k) -> lines for output k;
    group_lanes: if nonzero, outputs are stored in transpose groups of that size — a group is
    emitted once its last member is computed. Returns the interleaved body lines."""
    from collections import defaultdict

    outs_of_node = defaultdict(list)
    for k, o in enumerate(outs):
        outs_of_node[o.id].append(k)
    L = []
    if group_lanes:
        ngroups = len(outs) // group_lanes
        remaining = {}
        for g in range(ngroups):
            members = {outs[g * group_lanes + l].id for l in range(group_lanes)}
            remaining[g] = set(members)
        done = set()
        for nd in order:
            L.append(nd)  # placeholder: caller renders nodes; here we return (node|('store', k)) stream
            done.add(nd.id)
            for g in range(ngroups):
                if remaining[g] and remaining[g] <= done:
                    L.append(("store_group", g))
                    remaining[g] = set()
    else:
        for nd in order:
            L.append(nd)
            for k in outs_of_node.get(nd.id, ()):
                L.append(("store", k))
    return L


def _assemble(sig_lines, simd_inner, scalar_inner):
    """Dual-body kernel: SIMD tiles under CRD_FFT_GEN_SIMD_BODY (NDEBUG), lane-scalar otherwise."""
    out = list(sig_lines)
    out.append("{")
    out.append("#ifdef CRD_FFT_GEN_SIMD_BODY")
    out += simd_inner
    out.append("#else // lane-scalar edition (unoptimized builds): same schedule per column => bit-identical")
    out += scalar_inner
    out.append("#endif")
    out.append("}")
    return "\n".join(out)


def _scalar_batched(order, outs, tname, bvar="b"):
    S = [f"    using T = crd::{tname};", f"    for (crd::usize tl = 0; tl < {bvar}; ++tl)", "    {"]
    S += _emit_dag_body_scalar(order, tname, bvar, "        ")
    for k, o in enumerate(outs):
        S.append(f"        out[{k} * {bvar} + tl] = crd::hesap::Complex<T>{{n{o.id}r, n{o.id}i}};")
    S.append("    }")
    return S


def _scalar_fused_sh(order, outs, tname, N1):
    S = [f"    using T = crd::{tname};", "    for (crd::usize tl = 0; tl < n2; ++tl)", "    {"]
    S += _emit_dag_body_scalar(order, tname, "n2", "        ")
    for k, o in enumerate(outs):
        S.append(f"        {{ const T wr = twr[{k} * n2 + tl], wi = twi[{k} * n2 + tl];")
        S.append(
            f"          out[tl * {N1} + {k}] = crd::hesap::Complex<T>{{std::fma(-wi, n{o.id}i, n{o.id}r * wr), "
            f"std::fma(wi, n{o.id}r, n{o.id}i * wr)}}; }}"
        )
    S.append("    }")
    return S


def _scalar_fused_notr(order, outs, tname):
    S = [
        f"    using T = crd::{tname};",
        "    const crd::usize vc = b >> ash;",
        "    for (crd::usize tl = 0; tl < b; ++tl)",
        "    {",
        "        const crd::usize v = tl >> ash;",
    ]
    S += _emit_dag_body_scalar(order, tname, "b", "        ")
    for k, o in enumerate(outs):
        S.append(f"        {{ const T wr = twr[{k} * vc + v], wi = twi[{k} * vc + v];")
        S.append(
            f"          out[{k} * b + tl] = crd::hesap::Complex<T>{{std::fma(-wi, n{o.id}i, n{o.id}r * wr), "
            f"std::fma(wi, n{o.id}r, n{o.id}i * wr)}}; }}"
        )
    S.append("    }")
    return S


def _scalar_batched_strided(order, outs, tname):
    S = [f"    using T = crd::{tname};", "    for (crd::usize tl = 0; tl < bs; ++tl)", "    {"]
    S += _emit_dag_body_scalar(order, tname, "bs", "        ")
    for k, o in enumerate(outs):
        S.append(f"        out[{k} * os + tl] = crd::hesap::Complex<T>{{n{o.id}r, n{o.id}i}};")
    S.append("    }")
    return S


def _emit_dag_body(order, prec, batch_var, indent="        ", soa_in=0):
    """Shared DAG-body emitter: loads + arithmetic (no stores). prec = ('f64', 'V(0.0)', cstfmt) etc.
    soa_in (0=off, else lane count L): plain V::load from AoSoA block-interleaved rows (no deinterleave
    shuffles, ONE stream per row — [L×re | L×im] vector-width blocks; row j of a b-wide batch occupies
    2b T at inb + j*2b, block t at offset 2t (re) / 2t+L (im); emitters provide b2 = 2*batch, t2 = 2*t)."""
    L = []
    for nd in order:
        L += _render_node_simd(nd, prec, batch_var, indent, soa_in)
    return L


def _render_node_simd(nd, prec, batch_var, indent="        ", soa_in=0):
    tname, zero, cst = prec
    L = []
    if True:
        v = f"n{nd.id}"
        if nd.op == "input":
            if soa_in:
                L.append(
                    f"{indent}const V {v}r = V::load(inb + {nd.a} * b2 + t2); "
                    f"const V {v}i = V::load(inb + {nd.a} * b2 + t2 + {soa_in});"
                )
            else:
                L.append(
                    f"{indent}V {v}r, {v}i; simd::load_complex_deinterleaved("
                    f"reinterpret_cast<const crd::{tname}*>(in + {nd.a} * {batch_var} + t), {v}r, {v}i);"
                )
        elif nd.op == "add":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"{indent}const V {v}r = {a}r + {c}r, {v}i = {a}i + {c}i;")
        elif nd.op == "sub":
            a, c = f"n{nd.a.id}", f"n{nd.b.id}"
            L.append(f"{indent}const V {v}r = {a}r - {c}r, {v}i = {a}i - {c}i;")
        elif nd.op == "neg":
            a = f"n{nd.a.id}"
            L.append(f"{indent}const V {v}r = {zero} - {a}r, {v}i = {zero} - {a}i;")
        elif nd.op == "muli":
            a = f"n{nd.a.id}"
            L.append(f"{indent}const V {v}r = {zero} - {a}i, {v}i = {a}r;")
        elif nd.op == "mulni":
            a = f"n{nd.a.id}"
            L.append(f"{indent}const V {v}r = {a}i, {v}i = {zero} - {a}r;")
        elif nd.op == "cmul":
            a = f"n{nd.a.id}"
            wr, wi = nd.c
            # FMA edition: re = wr*ar - wi*ai (fnmadd), im = wi*ar + wr*ai (fma) — 2 mul + 2 fma
            # replacing 4 mul + add + sub. Single-rounded, deterministic (crush pass 2026-07-04).
            L.append(
                f"{indent}const V {v}r = simd::fnmadd({cst(wi)}, {a}i, {cst(wr)} * {a}r), "
                f"{v}i = simd::fma({cst(wi)}, {a}r, {cst(wr)} * {a}i);"
            )
    return L


_PREC = {
    "f64": ("f64", "V(0.0)", lambda x: f"V({x!r})", "crd::math::simd::Vec4d", 4),
    "f32": ("f32", "V(0.0F)", lambda x: f"V(static_cast<crd::f32>({x!r}))", "crd::math::simd::Vec8f", 8),
}


def _build_dag(N, seed):
    global _TABLE, _NODES
    _TABLE, _NODES = {}, []
    xs = [inp(i) for i in range(N)]
    outs = fft_dag(xs)
    rng = np.random.default_rng(seed + N)
    for _ in range(4):
        xv = rng.standard_normal(N) + 1j * rng.standard_normal(N)
        got = np.array(evaluate(outs, list(xv)))
        ref = np.fft.fft(xv)
        rel = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
        assert rel < 1e-12, f"N={N} DAG validation FAILED rel={rel}"
    return outs


def emit_fused_notr(N, name, prec_key):
    """Deep-split S2: batched N-point leaf + BROADCAST runtime twiddle + NATURAL (untransposed) store.
    Twiddle table is COMPACT: twr[k*(b>>ash) + (t>>ash)] — the W_{BC}^{k·v} value, constant across the
    A=1<<ash contiguous lanes of each v. Store: out[k*b + t] (in-place-unsafe: out != in required)."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N, 5407)
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::{tname}>* in,",
        f"    crd::hesap::Complex<crd::{tname}>* out, crd::usize b, crd::u32 ash,",
        f"    const crd::{tname}* twr, const crd::{tname}* twi) noexcept",
    ]
    L = []
    L.append(f"    using V = {vtype};")
    L.append("    namespace simd = crd::math::simd;")
    L.append("    const crd::usize vc = b >> ash;")
    L.append(f"    for (crd::usize t = 0; t + {lanes} <= b; t += {lanes})")
    L.append("    {")
    L.append("        const crd::usize v = t >> ash;")
    L += _emit_dag_body(order, (tname, zero, cst), "b")
    for k, o in enumerate(outs):
        L.append("        {")
        L.append(f"            const V wr = V(twr[{k} * vc + v]), wi = V(twi[{k} * vc + v]);")
        L.append(
            f"            simd::store_complex_interleaved(reinterpret_cast<crd::{tname}*>(out + {k} * b + t), "
            f"simd::fnmadd(wi, n{o.id}i, n{o.id}r * wr), simd::fma(wi, n{o.id}r, n{o.id}i * wr));"
        )
        L.append("        }")
    L.append("    }")
    return _assemble(sig, L, _scalar_fused_notr(order, outs, tname))


def emit_batched_strided(N, name, prec_key):
    """Deep-split S3: batched N-point leaf with SEPARATE in row stride (= lane count bs) and out row
    stride os — reads in[j*bs + t], writes out[k*os + t]. out != in (strided rows would alias)."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N, 6607)
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::{tname}>* in,",
        f"    crd::hesap::Complex<crd::{tname}>* out, crd::usize bs, crd::usize os) noexcept",
    ]
    L = []
    L.append(f"    using V = {vtype};")
    L.append("    namespace simd = crd::math::simd;")
    L.append(f"    for (crd::usize t = 0; t + {lanes} <= bs; t += {lanes})")
    L.append("    {")
    L += _emit_dag_body(order, (tname, zero, cst), "bs")
    for k, o in enumerate(outs):
        L.append(
            f"        simd::store_complex_interleaved(reinterpret_cast<crd::{tname}*>(out + {k} * os + t), "
            f"n{o.id}r, n{o.id}i);"
        )
    L.append("    }")
    return _assemble(sig, L, _scalar_batched_strided(order, outs, tname))


def emit_stage1_fused_cs(N1, name, prec_key):
    """AoSoA stage-1 (crush pass 2026-07-04): interleaved Complex in (the ONE deinterleave), leaf +
    FMA twiddle + per-plane transpose + PLAIN block stores into the AoSoA row layout ([L×re | L×im]
    blocks) — no interleave shuffles on the store side, ONE stream per row downstream."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N1, 2707)
    order = schedule(outs)
    tp = "transpose4x4" if lanes == 4 else "transpose8x8"
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::{tname}>* in,",
        f"    crd::{tname}* outb, crd::usize n2, const crd::{tname}* twr,",
        f"    const crd::{tname}* twi) noexcept",
    ]
    def group_block(g):
        k0 = g * lanes
        ids = [outs[k0 + l].id for l in range(lanes)]
        B = ["        {"]
        for l in range(lanes):
            B.append(
                f"            const V w{l}r = V::load(twr + {k0 + l} * n2 + t), "
                f"w{l}i = V::load(twi + {k0 + l} * n2 + t);"
            )
        for l in range(lanes):
            B.append(
                f"            V x{l}r = simd::fnmadd(w{l}i, n{ids[l]}i, n{ids[l]}r * w{l}r), "
                f"x{l}i = simd::fma(w{l}i, n{ids[l]}r, n{ids[l]}i * w{l}r);"
            )
        args_r = ", ".join(f"x{l}r" for l in range(lanes))
        args_i = ", ".join(f"x{l}i" for l in range(lanes))
        B.append(f"            simd::{tp}({args_r});")
        B.append(f"            simd::{tp}({args_i});")
        for l in range(lanes):
            B.append(f"            x{l}r.store(outb + (t + {l}) * {2 * N1} + {2 * k0});")
            B.append(f"            x{l}i.store(outb + (t + {l}) * {2 * N1} + {2 * k0 + lanes});")
        B.append("        }")
        return B

    L = [
        f"    using V = {vtype};",
        "    namespace simd = crd::math::simd;",
        f"    for (crd::usize t = 0; t + {lanes} <= n2; t += {lanes})",
        "    {",
    ]
    for item in _interleave_stores(order, outs, None, group_lanes=lanes):
        if isinstance(item, tuple):
            L += group_block(item[1])
        else:
            L += _render_node_simd(item, (tname, zero, cst), "n2")
    L.append("    }")
    S = [f"    using T = crd::{tname};", "    for (crd::usize tl = 0; tl < n2; ++tl)", "    {"]
    S += _emit_dag_body_scalar(order, tname, "n2", "        ")
    for k, o in enumerate(outs):
        off = (k // lanes) * 2 * lanes + (k % lanes)
        S.append(f"        {{ const T wr = twr[{k} * n2 + tl], wi = twi[{k} * n2 + tl];")
        S.append(f"          outb[tl * {2 * N1} + {off}] = std::fma(-wi, n{o.id}i, n{o.id}r * wr);")
        S.append(f"          outb[tl * {2 * N1} + {off + lanes}] = std::fma(wi, n{o.id}r, n{o.id}i * wr); }}")
    S.append("    }")
    return _assemble(sig, L, S)


def emit_stage1_fused_csf(N1, name, prec_key):
    """FACTORED-TWIDDLE AoSoA stage-1 (crush pass 2026-07-04): same as _cs but the per-output twiddle
    comes from two L1-RESIDENT tables — W_n^{k·u} = hi[k·UH + u>>msh] · lo[(k<<msh) + (u&(M-1))],
    M = 1<<msh ≥ lanes, UH = n2>>msh — instead of streaming the full n-entry table (as many bytes as
    the data) through L2/DRAM every call. Costs 2 extra FMAs per output; exact index split (no mod)."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N1, 2707)
    order = schedule(outs)
    tp = "transpose4x4" if lanes == 4 else "transpose8x8"
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::hesap::Complex<crd::{tname}>* in,",
        f"    crd::{tname}* outb, crd::usize n2, const crd::{tname}* lor, const crd::{tname}* loi,",
        f"    const crd::{tname}* hir, const crd::{tname}* hii, crd::u32 msh) noexcept",
    ]
    def group_block(g):
        k0 = g * lanes
        ids = [outs[k0 + l].id for l in range(lanes)]
        B = ["        {"]
        for l in range(lanes):
            B.append(
                f"            const V h{l}r = V(hir[{k0 + l} * uhc + uh]), h{l}i = V(hii[{k0 + l} * uhc + uh]);"
            )
            B.append(
                f"            const V g{l}r = V::load(lor + ((crd::usize{{{k0 + l}}} << msh) + ul)), "
                f"g{l}i = V::load(loi + ((crd::usize{{{k0 + l}}} << msh) + ul));"
            )
            B.append(
                f"            const V w{l}r = simd::fnmadd(h{l}i, g{l}i, h{l}r * g{l}r), "
                f"w{l}i = simd::fma(h{l}i, g{l}r, h{l}r * g{l}i);"
            )
        for l in range(lanes):
            B.append(
                f"            V x{l}r = simd::fnmadd(w{l}i, n{ids[l]}i, n{ids[l]}r * w{l}r), "
                f"x{l}i = simd::fma(w{l}i, n{ids[l]}r, n{ids[l]}i * w{l}r);"
            )
        args_r = ", ".join(f"x{l}r" for l in range(lanes))
        args_i = ", ".join(f"x{l}i" for l in range(lanes))
        B.append(f"            simd::{tp}({args_r});")
        B.append(f"            simd::{tp}({args_i});")
        for l in range(lanes):
            B.append(f"            x{l}r.store(outb + (t + {l}) * {2 * N1} + {2 * k0});")
            B.append(f"            x{l}i.store(outb + (t + {l}) * {2 * N1} + {2 * k0 + lanes});")
        B.append("        }")
        return B

    L = [
        f"    using V = {vtype};",
        "    namespace simd = crd::math::simd;",
        "    const crd::usize uhc = n2 >> msh;",
        f"    for (crd::usize t = 0; t + {lanes} <= n2; t += {lanes})",
        "    {",
        "        const crd::usize uh = t >> msh;",
        "        const crd::usize ul = t & ((crd::usize{1} << msh) - 1U);",
    ]
    for item in _interleave_stores(order, outs, None, group_lanes=lanes):
        if isinstance(item, tuple):
            L += group_block(item[1])
        else:
            L += _render_node_simd(item, (tname, zero, cst), "n2")
    L.append("    }")
    S = [
        f"    using T = crd::{tname};",
        "    const crd::usize uhc = n2 >> msh;",
        "    for (crd::usize tl = 0; tl < n2; ++tl)",
        "    {",
        "        const crd::usize uh = tl >> msh;",
        "        const crd::usize ul = tl & ((crd::usize{1} << msh) - 1U);",
    ]
    S += _emit_dag_body_scalar(order, tname, "n2", "        ")
    for k, o in enumerate(outs):
        off = (k // lanes) * 2 * lanes + (k % lanes)
        S.append(f"        {{ const T hr = hir[{k} * uhc + uh], hi_ = hii[{k} * uhc + uh];")
        S.append(f"          const T gr = lor[(crd::usize{{{k}}} << msh) + ul], gi = loi[(crd::usize{{{k}}} << msh) + ul];")
        S.append(f"          const T wr = std::fma(-hi_, gi, hr * gr), wi = std::fma(hi_, gr, hr * gi);")
        S.append(f"          outb[tl * {2 * N1} + {off}] = std::fma(-wi, n{o.id}i, n{o.id}r * wr);")
        S.append(f"          outb[tl * {2 * N1} + {off + lanes}] = std::fma(wi, n{o.id}r, n{o.id}i * wr); }}")
    S.append("    }")
    return _assemble(sig, L, S)


def _scalar_aosoa_prelude(tname, bound_var, lanes):
    ls = lanes.bit_length() - 1
    return [
        f"    using T = crd::{tname};",
        f"    for (crd::usize tl = 0; tl < {bound_var}; ++tl)",
        "    {",
        f"        const crd::usize ta = ((tl >> {ls}) << {ls + 1}) + (tl & {lanes - 1}U);",
    ]


def emit_batched_sc(N, name, prec_key):
    """AoSoA final leaf: plain block loads (no shuffles, one stream/row), DAG, interleaved store (the
    ONE reinterleave). NOT in-place (AoSoA in vs Complex out — always disjoint in the sh/ds pipelines)."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N, 1407)
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::{tname}* inb,",
        f"    crd::hesap::Complex<crd::{tname}>* out, crd::usize b) noexcept",
    ]
    L = [
        f"    using V = {vtype};",
        "    namespace simd = crd::math::simd;",
        "    const crd::usize b2 = 2 * b;",
        f"    for (crd::usize t = 0; t + {lanes} <= b; t += {lanes})",
        "    {",
        "        const crd::usize t2 = 2 * t;",
    ]
    for item in _interleave_stores(order, outs, None):
        if isinstance(item, tuple):
            k = item[1]
            o = outs[k]
            L.append(
                f"        simd::store_complex_interleaved(reinterpret_cast<crd::{tname}*>(out + {k} * b + t), "
                f"n{o.id}r, n{o.id}i);"
            )
        else:
            L += _render_node_simd(item, (tname, zero, cst), "b", soa_in=lanes)
    L.append("    }")
    S = _scalar_aosoa_prelude(tname, "b", lanes)
    S.insert(1, "    const crd::usize b2 = 2 * b;")
    S += _emit_dag_body_scalar(order, tname, "b", "        ", soa_in=lanes)
    for k, o in enumerate(outs):
        S.append(f"        out[{k} * b + tl] = crd::hesap::Complex<T>{{n{o.id}r, n{o.id}i}};")
    S.append("    }")
    return _assemble(sig, L, S)


def emit_fused_notr_ss(N, name, prec_key):
    """AoSoA deep-split S2: block loads + broadcast twiddle + block stores — ZERO shuffles, one
    stream per row on both sides."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N, 5407)
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::{tname}* inb,",
        f"    crd::{tname}* outb, crd::usize b, crd::u32 ash,",
        f"    const crd::{tname}* twr, const crd::{tname}* twi) noexcept",
    ]
    L = [
        f"    using V = {vtype};",
        "    namespace simd = crd::math::simd;",
        "    const crd::usize vc = b >> ash;",
        "    const crd::usize b2 = 2 * b;",
        f"    for (crd::usize t = 0; t + {lanes} <= b; t += {lanes})",
        "    {",
        "        const crd::usize v = t >> ash;",
        "        const crd::usize t2 = 2 * t;",
    ]
    for item in _interleave_stores(order, outs, None):
        if isinstance(item, tuple):
            k = item[1]
            o = outs[k]
            L.append("        {")
            L.append(f"            const V wr = V(twr[{k} * vc + v]), wi = V(twi[{k} * vc + v]);")
            L.append(f"            const V xr = simd::fnmadd(wi, n{o.id}i, n{o.id}r * wr);")
            L.append(f"            const V xi = simd::fma(wi, n{o.id}r, n{o.id}i * wr);")
            L.append(f"            xr.store(outb + {k} * b2 + t2);")
            L.append(f"            xi.store(outb + {k} * b2 + t2 + {lanes});")
            L.append("        }")
        else:
            L += _render_node_simd(item, (tname, zero, cst), "b", soa_in=lanes)
    L.append("    }")
    S = _scalar_aosoa_prelude(tname, "b", lanes)
    S.insert(1, "    const crd::usize vc = b >> ash;")
    S.insert(2, "    const crd::usize b2 = 2 * b;")
    S.append("        const crd::usize v = tl >> ash;")
    S += _emit_dag_body_scalar(order, tname, "b", "        ", soa_in=lanes)
    for k, o in enumerate(outs):
        S.append(f"        {{ const T wr = twr[{k} * vc + v], wi = twi[{k} * vc + v];")
        S.append(f"          outb[{k} * b2 + ta] = std::fma(-wi, n{o.id}i, n{o.id}r * wr);")
        S.append(f"          outb[{k} * b2 + ta + {lanes}] = std::fma(wi, n{o.id}r, n{o.id}i * wr); }}")
    S.append("    }")
    return _assemble(sig, L, S)


def emit_batched_strided_sc(N, name, prec_key):
    """AoSoA deep-split S3: block loads (row stride 2*bs), interleaved strided stores (out-stride os)."""
    tname, zero, cst, vtype, lanes = _PREC[prec_key]
    outs = _build_dag(N, 6607)
    order = schedule(outs)
    sig = [
        f"CRD_FFT_GEN_INLINE void {name}(const crd::{tname}* inb,",
        f"    crd::hesap::Complex<crd::{tname}>* out, crd::usize bs, crd::usize os) noexcept",
    ]
    L = [
        f"    using V = {vtype};",
        "    namespace simd = crd::math::simd;",
        "    const crd::usize b2 = 2 * bs;",
        f"    for (crd::usize t = 0; t + {lanes} <= bs; t += {lanes})",
        "    {",
        "        const crd::usize t2 = 2 * t;",
    ]
    for item in _interleave_stores(order, outs, None):
        if isinstance(item, tuple):
            k = item[1]
            o = outs[k]
            L.append(
                f"        simd::store_complex_interleaved(reinterpret_cast<crd::{tname}*>(out + {k} * os + t), "
                f"n{o.id}r, n{o.id}i);"
            )
        else:
            L += _render_node_simd(item, (tname, zero, cst), "bs", soa_in=lanes)
    L.append("    }")
    S = _scalar_aosoa_prelude(tname, "bs", lanes)
    S.insert(1, "    const crd::usize b2 = 2 * bs;")
    S += _emit_dag_body_scalar(order, tname, "bs", "        ", soa_in=lanes)
    for k, o in enumerate(outs):
        S.append(f"        out[{k} * os + tl] = crd::hesap::Complex<T>{{n{o.id}r, n{o.id}i}};")
    S.append("    }")
    return _assemble(sig, L, S)


def main(mode="hybrid"):
    global SCHED_VARIANT
    sched_of = {
        "greedy": {"f64": "greedy", "f32": "greedy"},
        "belady": {"f64": "belady", "f32": "belady"},
        "hybrid": {"f64": "greedy", "f32": "belady"},
        # hybrid2: additionally belady for the three f64 kernels the row-level A/B credited
        # (codelet128_batched + codelet128/256_stage1_fused_sh — the 8K/16K/32K rows); everything
        # else f64 stays greedy (256_batched + notr/strided regressed under belady).
        "hybrid2": {"f64": "greedy", "f32": "belady"},
    }[mode]
    print("#pragma once")
    print("// GENERATED by scripts/gen_fft_batched.py — the rebuilt batched-codelet generator")
    print("// (split-radix DAG + CSE + register-pressure schedule, Vec4d/Vec8f over the batch axis,")
    print("//  numpy-validated in-script; in-place-safe: all loads precede all stores per tile).")
    print(f"// Scheduler mode: {mode} (f64 = {sched_of['f64']}, f32 = {sched_of['f32']}).")
    print("#include <crd/core/platform.hpp>")
    print("#include <crd/core/types.hpp>")
    print("#include <crd/hesap/complex.hpp>")
    print("#include <crd/math/simd/simd.hpp>")
    print("#include <cmath> // std::fma in the lane-scalar bodies (single-rounded, deterministic like std::sqrt)")
    print("// Unoptimized builds (/Od-class) give every expression temporary its OWN stack slot: the")
    print("// straight-line SIMD bodies then need 1.2-1.4 MB frames (MEASURED, MSVC /Od /RTC1) — past the")
    print("// 1 MB Windows default stack (C00000FD in win-debug, found 2026-07-03). Debug builds therefore")
    print("// take the LANE-SCALAR edition of each kernel: the SAME schedule per column, so results are")
    print("// bit-identical (Vec ops are lane-wise IEEE ops; shuffles are data movement), frames ~8x smaller.")
    print("// __OPTIMIZE__ covers optimized gcc/clang builds that don't define NDEBUG (ad-hoc -O3 bench")
    print("// builds); MSVC optimized configs all carry NDEBUG in this project.")
    print("#if defined(NDEBUG) || defined(__OPTIMIZE__)")
    print("#define CRD_FFT_GEN_SIMD_BODY 1")
    print("#endif")
    print("// MSVC honors __forceinline even under LTCG, so every codelet body below would be inlined into")
    print("// the ONE dispatch function that calls it; with multi-thousand-line codelets that exhausts the")
    print("// compiler heap in link-time pass 2 (fatal C1002 / LNK1257, hit 2026-07-05 in FftPlan::execute).")
    print("// MSVC therefore gets plain `inline` — its /O2 cost model declines bodies this size on its own,")
    print("// and one call into a straight-line kernel of thousands of instructions is noise. gcc/clang keep")
    print("// always_inline: they compile it fine, and the recorded bench baselines were built with them.")
    print("#if defined(_MSC_VER) && !defined(__clang__)")
    print("#define CRD_FFT_GEN_INLINE inline")
    print("#else")
    print("#define CRD_FFT_GEN_INLINE CRD_FORCEINLINE")
    print("#endif")
    print("namespace crd::hesap::fft::gen {")
    for n in (128, 256):
        SCHED_VARIANT = "belady" if (mode == "hybrid2" and n == 128) else sched_of["f64"]
        print(f"// N={n} batched split-radix codelet (Vec4d over batch). GENERATED.")
        print(emit_batched_f64(n, f"codelet{n}_batched"))
    for n1 in (32, 64, 128, 256):
        SCHED_VARIANT = "belady" if (mode == "hybrid2" and n1 >= 128) else sched_of["f64"]
        print(f"// N1={n1} stage-1 FUSED (leaf + runtime twiddle + 4x4-transposed store). GENERATED.")
        print(emit_stage1_fused_f64(n1, f"codelet{n1}_stage1_fused_sh"))
    SCHED_VARIANT = sched_of["f32"]
    for n in (128, 256):
        print(f"// N={n} batched split-radix codelet (f32, Vec8f over batch). GENERATED.")
        print(emit_batched_f32(n, f"codelet{n}_batched"))
    # (The INTERLEAVED stage1_fused_sh / fused_notr / batched_strided editions were pruned 2026-07-04:
    #  the sh + deep-split pipelines run the AoSoA editions below; the emitters remain in this script.)

    # AoSoA (block-interleaved) pipeline editions (crush pass 2026-07-04): deinterleave once (stage-1),
    # compute in [L×re|L×im] block rows (zero shuffles, one stream per row), reinterleave once.
    def sched_for(prec, kind, n):
        if mode == "hybrid2" and prec == "f64":
            if kind == "batched" and n == 128:
                return "belady"
            if kind == "fused_sh" and n >= 128:
                return "belady"
            return "greedy"
        return sched_of[prec]

    for prec in ("f64", "f32"):
        for n1 in (16, 32, 64, 128, 256):
            SCHED_VARIANT = sched_for(prec, "fused_sh", n1)
            print(f"// N1={n1} SPLIT-LAYOUT stage-1 (complex in, plane out) ({prec}). GENERATED.")
            print(emit_stage1_fused_cs(n1, f"codelet{n1}_stage1_fused_sh_cs", prec))
            print(f"// N1={n1} FACTORED-TWIDDLE stage-1 (L1-resident hi/lo tables) ({prec}). GENERATED.")
            print(emit_stage1_fused_csf(n1, f"codelet{n1}_stage1_fused_sh_csf", prec))
        for n in (32, 64, 128, 256):
            SCHED_VARIANT = sched_for(prec, "batched", n)
            print(f"// N={n} SPLIT-LAYOUT final leaf (plane in, complex out) ({prec}). GENERATED.")
            print(emit_batched_sc(n, f"codelet{n}_batched_sc", prec))
        SCHED_VARIANT = sched_of[prec]
        for n in (8, 16, 32, 64, 128):
            print(f"// N={n} SPLIT-LAYOUT deep-split S2, zero shuffles ({prec}). GENERATED.")
            print(emit_fused_notr_ss(n, f"codelet{n}_fused_notr_ss", prec))
            print(f"// N={n} SPLIT-LAYOUT deep-split S3 (plane in, strided complex out) ({prec}). GENERATED.")
            print(emit_batched_strided_sc(n, f"codelet{n}_batched_strided_sc", prec))
    print("} // namespace crd::hesap::fft::gen")


if __name__ == "__main__":
    import sys

    main(sys.argv[1] if len(sys.argv) > 1 else "hybrid2")  # "hybrid2" (tracked) | "hybrid" | "greedy" | "belady"
