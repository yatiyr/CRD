#!/usr/bin/env python3
"""Generate straight-line FFT codelets (genfft-lite) — v10-b1 of the codelet campaign (beat MKL).

Run (WSL): python3 scripts/gen_fft_codelets.py > engine/hesap-fft/include/crd/hesap/fft/detail/codelets.hpp

genfft discipline (Frigo-Johnson): for each leaf size N, build the N-point DFT as an operation DAG via
recursive Cooley-Tukey, apply common-subexpression elimination, NUMERICALLY SELF-CHECK the DAG against numpy
(a fast wrong codelet is worthless), then emit straight-line C++ with all twiddles as compile-time constants
and zero loop overhead. These are the cache-resident leaf transforms where MKL's hand-tuned codelets win;
the Stockham/four-step driver combines them (v10-b2). SoA (split re/im) so the batch axis vectorizes.

This first cut emits SCALAR straight-line codelets (validated). SIMD-batched emission (process W complex
transforms per AVX2 register over the batch axis) is the v10-b2 refinement — the DAG + CSE are identical, only
the emitted load/store/op types change (T -> Vec4d), so this generator is the foundation for both.
"""

import cmath
import sys

import numpy as np

LEAF_SIZES = [2, 4, 8, 16, 32]
# radix-L Stockham combine codelets — now REGISTER-PRESSURE-SCHEDULED (loads late / stores early), the genfft
# engine retry of radix-8-over-k: the naive unscheduled radix-8 lost (−17%) by holding all 16 ymm live; the
# scheduler collapses peak live so it should fit. Re-testing radix-8 first; radix-16 follows if it wins.
TWIDDLE_SIZES = [8, 16, 32]


class Gen:
    """Builds an operation DAG for an N-point DFT with CSE + a numeric value per node, then emits C++."""

    def __init__(self):
        self.lines = []          # emitted "const T tK = ...;" statements
        self.memo = {}           # CSE: expr-key -> (varname, complex value)
        self.counter = 0
        self.vals = {}           # varname -> complex value (for the self-check)

    def _new(self, expr, value):
        key = expr
        if key in self.memo:
            return self.memo[key]
        name = f"t{self.counter}"
        self.counter += 1
        self.lines.append(f"        const T {name}_r = {expr[0]};")
        self.lines.append(f"        const T {name}_i = {expr[1]};")
        self.memo[key] = name
        self.vals[name] = value
        return name

    # Each "value" is a varname; .r/.i give its real/imag C++ refs and a complex python value.
    def add(self, a, b):
        return self._new((f"{a}_r + {b}_r", f"{a}_i + {b}_i"), self.vals[a] + self.vals[b])

    def sub(self, a, b):
        return self._new((f"{a}_r - {b}_r", f"{a}_i - {b}_i"), self.vals[a] - self.vals[b])

    def mul_tw(self, a, w):
        """a * w, w a compile-time complex constant; special-case 1, -1, i, -i to avoid real muls."""
        wr, wi = w.real, w.imag
        if abs(wi) < 1e-15 and abs(wr - 1.0) < 1e-15:
            return a
        if abs(wi) < 1e-15 and abs(wr + 1.0) < 1e-15:
            return self._new((f"-{a}_r", f"-{a}_i"), -self.vals[a])
        if abs(wr) < 1e-15 and abs(wi + 1.0) < 1e-15:  # w = -i : a*(-i) = (a_i, -a_r)
            return self._new((f"{a}_i", f"-{a}_r"), self.vals[a] * w)
        if abs(wr) < 1e-15 and abs(wi - 1.0) < 1e-15:  # w = +i : a*i = (-a_i, a_r)
            return self._new((f"-{a}_i", f"{a}_r"), self.vals[a] * w)
        wrs = repr(wr)
        wis = repr(wi)
        return self._new(
            (f"static_cast<T>({wrs}) * {a}_r - static_cast<T>({wis}) * {a}_i",
             f"static_cast<T>({wrs}) * {a}_i + static_cast<T>({wis}) * {a}_r"),
            self.vals[a] * w,
        )

    def fft(self, xs, sign):
        """Recursive radix-2 Cooley-Tukey on the list of value-names xs (len = power of 2)."""
        n = len(xs)
        if n == 1:
            return xs
        even = self.fft(xs[0::2], sign)
        odd = self.fft(xs[1::2], sign)
        out = [None] * n
        for k in range(n // 2):
            w = cmath.exp(sign * 2j * cmath.pi * k / n)
            t = self.mul_tw(odd[k], w)
            out[k] = self.add(even[k], t)
            out[k + n // 2] = self.sub(even[k], t)
        return out


# ---------------------------------------------------------------------------------------------------------
# Scheduled twiddle-codelet generator (the genfft engine core). Builds the radix-L combine codelet as a
# dependency DAG (CSE'd, numpy-self-checked), then a REGISTER-PRESSURE LIST SCHEDULER emits operations so
# loads land late, stores fire as soon as their output is ready, and live values die ASAP — collapsing the
# peak live-register count so a radix-8/16 over-k codelet fits the 16-ymm file instead of spilling+stalling
# (gcc does not re-schedule large straight-line FP DAGs well, so source emission order is the lever).
# ---------------------------------------------------------------------------------------------------------


def _build_twiddle_dag(L, inverse):
    """Return (nodes, outs): nodes is a list of structured ops; outs[m] is the node index of output m.
    Each node = (kind, *operands). Self-checked against numpy."""
    nodes = []
    vals = []
    memo = {}

    def add(key, value):
        if key in memo:
            return memo[key]
        idx = len(nodes)
        nodes.append(key)
        vals.append(value)
        memo[key] = idx
        return idx

    rng = np.random.default_rng(99000 + L + (1 if inverse else 0))
    xin = rng.standard_normal(L) + 1j * rng.standard_normal(L)
    wv = rng.standard_normal(L) + 1j * rng.standard_normal(L)
    wv[0] = 1.0 + 0j  # w_0 == 1 (point 0 is never twiddled)
    loads = [add(("load", m), complex(xin[m])) for m in range(L)]
    a = [loads[0]] + [add(("tw_rt", loads[m], m), vals[loads[m]] * complex(wv[m])) for m in range(1, L)]
    sign = 1.0 if inverse else -1.0

    def mul_tw(src, w):  # compile-time internal DFT twiddle, with ±1/±i special-cased to 0 real muls
        wr, wi = w.real, w.imag
        if abs(wi) < 1e-15 and abs(wr - 1.0) < 1e-15:
            return src
        if abs(wi) < 1e-15 and abs(wr + 1.0) < 1e-15:
            return add(("neg", src), -vals[src])
        if abs(wr) < 1e-15 and abs(wi - 1.0) < 1e-15:
            return add(("mulj", src), vals[src] * w)  # × +i
        if abs(wr) < 1e-15 and abs(wi + 1.0) < 1e-15:
            return add(("mulnj", src), vals[src] * w)  # × -i
        return add(("mulc", src, repr(wr), repr(wi)), vals[src] * w)

    def fft(xs):
        nn = len(xs)
        if nn == 1:
            return xs
        ev = fft(xs[0::2])
        od = fft(xs[1::2])
        out = [None] * nn
        for k in range(nn // 2):
            t = mul_tw(od[k], cmath.exp(sign * 2j * cmath.pi * k / nn))
            out[k] = add(("add", ev[k], t), vals[ev[k]] + vals[t])
            out[k + nn // 2] = add(("sub", ev[k], t), vals[ev[k]] - vals[t])
        return out

    outs = fft(a)
    got = np.array([vals[o] for o in outs])
    aval = xin * wv
    ref = (np.fft.ifft(aval) * L) if inverse else np.fft.fft(aval)
    err = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
    if err > 1e-11:
        raise SystemExit(f"twiddle codelet L={L} inverse={inverse} SELF-CHECK FAILED: {err:.2e}")
    return nodes, outs


def _deps(nd):
    k = nd[0]
    if k == "load":
        return []
    if k in ("add", "sub"):
        return [nd[1], nd[2]]
    return [nd[1]]  # tw_rt / mulc / neg / mulj / mulnj


def _schedule(nodes, outs):
    """List-schedule compute nodes + per-output stores to minimize peak live registers. Returns an emission
    sequence of ('c', node_idx) | ('s', out_m). Greedy key (delta_live, is_load, -idx): stores & operations
    that kill their inputs go first; loads are deferred to the latest point they are still needed."""
    n = len(nodes)
    out_n = len(outs)
    remaining = [0] * n  # # of not-yet-scheduled consumers (compute consumers + 1 per store using this node)
    for nd in nodes:
        for d in _deps(nd):
            remaining[d] += 1
    for o in outs:
        remaining[o] += 1
    fanout = remaining[:]  # static total consumer count (result lifetime proxy: fewer ⇒ dies sooner)
    scheduled = [False] * n
    stored = [False] * out_n
    order = []
    while len(order) < n + out_n:
        best = None
        best_key = None
        for i in range(n):
            if scheduled[i] or any(not scheduled[d] for d in _deps(nodes[i])):
                continue
            dying = sum(1 for d in _deps(nodes[i]) if remaining[d] == 1)
            # key (genfft-aligned register-pressure minimization): kill the most inputs first; defer loads;
            # then prefer SHORT-LIVED results (low fanout ⇒ frees its register sooner); then later-created.
            key = (1 - dying, 1 if nodes[i][0] == "load" else 0, fanout[i], -i)
            if best_key is None or key < best_key:
                best_key, best = key, ("c", i)
        for m in range(out_n):
            if stored[m] or not scheduled[outs[m]]:
                continue
            key = (-1 if remaining[outs[m]] == 1 else 0, -1, -1)  # stores win ties over computes
            if best_key is None or key < best_key:
                best_key, best = key, ("s", m)
        if best[0] == "c":
            i = best[1]
            scheduled[i] = True
            for d in _deps(nodes[i]):
                remaining[d] -= 1
        else:
            m = best[1]
            stored[m] = True
            remaining[outs[m]] -= 1
        order.append(best)
    return order


def _emit_node(idx, nodes, simd):
    nd = nodes[idx]
    k = nd[0]
    ty = "V" if simd else "T"

    def bw(ref):  # broadcast runtime scalar
        return f"V({ref})" if simd else ref

    def cw(s):  # compile-time const
        return f"V(static_cast<T>({s}))" if simd else f"static_cast<T>({s})"

    if k == "load":
        m = nd[1]
        re = f"V::load(xr + ibase + {m} * r + k)" if simd else f"xr[ibase + {m} * r + k]"
        im = f"V::load(xi + ibase + {m} * r + k)" if simd else f"xi[ibase + {m} * r + k]"
    elif k == "tw_rt":
        s, m = f"t{nd[1]}", nd[2]
        wr, wi = bw(f"w_re[{m}]"), bw(f"w_im[{m}]")
        if simd:  # FMA-fuse the complex twiddle mul: re = wr·sr − wi·si, im = wi·sr + wr·si (cuts p0/p1 ops)
            re = f"crd::math::simd::fnmadd({wi}, {s}_i, {wr} * {s}_r)"
            im = f"crd::math::simd::fma({wi}, {s}_r, {wr} * {s}_i)"
        else:
            re, im = f"{wr} * {s}_r - {wi} * {s}_i", f"{wr} * {s}_i + {wi} * {s}_r"
    elif k == "mulc":
        s = f"t{nd[1]}"
        cwr, cwi = cw(nd[2]), cw(nd[3])
        if simd:
            re = f"crd::math::simd::fnmadd({cwi}, {s}_i, {cwr} * {s}_r)"
            im = f"crd::math::simd::fma({cwi}, {s}_r, {cwr} * {s}_i)"
        else:
            re, im = f"{cwr} * {s}_r - {cwi} * {s}_i", f"{cwr} * {s}_i + {cwi} * {s}_r"
    elif k == "neg":
        s = f"t{nd[1]}"
        re, im = f"-{s}_r", f"-{s}_i"
    elif k == "mulj":  # × +i : (a+bi)·i = -b + ai
        s = f"t{nd[1]}"
        re, im = f"-{s}_i", f"{s}_r"
    elif k == "mulnj":  # × -i : (a+bi)·(-i) = b - ai
        s = f"t{nd[1]}"
        re, im = f"{s}_i", f"-{s}_r"
    elif k == "add":
        a, b = f"t{nd[1]}", f"t{nd[2]}"
        re, im = f"{a}_r + {b}_r", f"{a}_i + {b}_i"
    else:  # sub
        a, b = f"t{nd[1]}", f"t{nd[2]}"
        re, im = f"{a}_r - {b}_r", f"{a}_i - {b}_i"
    return [f"                const {ty} t{idx}_r = {re};", f"                const {ty} t{idx}_i = {im};"]


def _emit_store(m, outs, simd):
    s = f"t{outs[m]}"
    if simd:
        return [f"                ({s}_r).store(yr + obase + {m} * q + k);",
                f"                ({s}_i).store(yi + obase + {m} * q + k);"]
    return [f"                yr[obase + {m} * q + k] = {s}_r;",
            f"                yi[obase + {m} * q + k] = {s}_i;"]


def gen_twiddle_codelet(L, inverse):
    """A radix-L Stockham COMBINE pass over one j-group, register-pressure-scheduled (see header). SIMD over k
    (Vec4d/Vec8f) + scalar tail on DISJOINT k ranges (moat-safe). Inputs xr/xi[ibase + m*r + k], combine-
    twiddled by (w_re[m], w_im[m]), L-point DFT'd, stored to yr/yi[obase + m*q + k]."""
    fn = "fwd" if not inverse else "inv"
    nodes, outs = _build_twiddle_dag(L, inverse)
    order = _schedule(nodes, outs)

    def body(simd):
        lines = []
        for kind, x in order:
            lines += _emit_node(x, nodes, simd) if kind == "c" else _emit_store(x, outs, simd)
        return "\n".join(lines)

    vbody, sbody = body(True), body(False)
    out = (
        f"    template <typename T> CRD_FORCEINLINE void twiddle{L}_{fn}(const T* xr, const T* xi, T* yr,\n"
        f"        T* yi, crd::usize ibase, crd::usize r, crd::usize obase, crd::usize q, const T* w_re,\n"
        f"        const T* w_im) noexcept\n    {{\n"
    )
    out += "        crd::usize k = 0;\n"
    out += "        if constexpr (std::is_same_v<T, crd::f64>)\n        {\n"
    out += "            using V = crd::math::simd::Vec4d;\n"
    out += "            for (; k + 4 <= r; k += 4)\n            {\n" + vbody + "\n            }\n        }\n"
    out += "        else if constexpr (std::is_same_v<T, crd::f32>)\n        {\n"
    out += "            using V = crd::math::simd::Vec8f;\n"
    out += "            for (; k + 8 <= r; k += 8)\n            {\n" + vbody + "\n            }\n        }\n"
    sbody_dedent = "\n".join(line[4:] for line in sbody.split("\n"))  # tail loop is one level shallower
    out += "        for (; k < r; ++k)\n        {\n" + sbody_dedent + "\n        }\n"
    out += "    }\n"
    return out


def gen_codelet(n, inverse):
    g = Gen()
    # input value-names: x0..x(n-1), each with a random complex value for the self-check.
    rng = np.random.default_rng(12345 + n + (1 if inverse else 0))
    xin = rng.standard_normal(n) + 1j * rng.standard_normal(n)
    names = []
    for i in range(n):
        nm = f"x{i}"
        g.vals[nm] = complex(xin[i])
        g.lines.append(f"        const T {nm}_r = inre[{i} * istride];")
        g.lines.append(f"        const T {nm}_i = inim[{i} * istride];")
        names.append(nm)
    sign = 1.0 if inverse else -1.0
    outs = g.fft(names, sign)
    # SELF-CHECK: DAG numeric value vs numpy.
    got = np.array([g.vals[o] for o in outs])
    ref = np.fft.ifft(xin) * n if inverse else np.fft.fft(xin)
    err = np.max(np.abs(got - ref)) / (1.0 + np.max(np.abs(ref)))
    if err > 1e-12:
        raise SystemExit(f"codelet N={n} inverse={inverse} SELF-CHECK FAILED: rel err {err:.2e}")
    body = []
    body += g.lines
    for k in range(n):
        body.append(f"        outre[{k} * ostride] = {outs[k]}_r;")
        body.append(f"        outim[{k} * ostride] = {outs[k]}_i;")
    fn = "fwd" if not inverse else "inv"
    sig = (f"    template <typename T> CRD_FORCEINLINE void codelet_{n}_{fn}("
           f"const T* inre, const T* inim, crd::usize istride, T* outre, T* outim, crd::usize ostride) noexcept")
    return sig + "\n    {\n" + "\n".join(body) + "\n    }\n"


def main():
    print("#pragma once")
    print()
    print("// codelets.hpp -- GENERATED by scripts/gen_fft_codelets.py. DO NOT EDIT BY HAND.")
    print("// Straight-line, CSE'd, constant-twiddle DFT leaf codelets (genfft-lite, v10-b1). Each was")
    print("// numerically self-checked against numpy before emission. The Stockham/four-step driver combines")
    print("// them as cache-resident leaves (v10-b2). SoA split re/im; istride/ostride index complex elements.")
    print()
    print("#include <crd/core/platform.hpp>")
    print("#include <crd/core/types.hpp>")
    print("#include <crd/math/simd/simd.hpp>")
    print()
    print("#include <type_traits>")
    print()
    print("namespace crd::hesap::fft::detail")
    print("{")
    for n in LEAF_SIZES:
        print(gen_codelet(n, inverse=False))
        print(gen_codelet(n, inverse=True))
    # radix-L Stockham COMBINE twiddle-codelets (SIMD over k). The driver picks log_L(N) passes.
    for n in TWIDDLE_SIZES:
        print(gen_twiddle_codelet(n, inverse=False))
        print(gen_twiddle_codelet(n, inverse=True))
    print("} // namespace crd::hesap::fft::detail")


if __name__ == "__main__":
    sys.exit(main())
