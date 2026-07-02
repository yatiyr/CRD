#!/usr/bin/env python3
"""gen_fft_crush.py — the genfft-style FFT codelet generator (the codegen campaign).

Campaign goal: generate scheduled straight-line FFT codelets that BEAT MKL above the
hand-construction ceiling (N>=64), and that the engine wires in for the whole small/mid band.
Hand war is won (N=8 2.39x, N=16 1.26x, N=32 1.10x, N=64 0.83x near-parity). This is the tool.

STEP 1 (this file): the symbolic DAG core.
  - Build the size-N DFT as a hash-consed DAG of complex ops (Input/Const/Add/Sub/CMul) with CSE.
  - Radix-2 + split-radix recursion (lowest known flop count).
  - Validate the DAG numerically vs numpy.fft (the codegen analog of the brute-DFT gate).
  - Report op counts (adds / mults) — the thing the scheduler later orders.
NEXT STEPS: (2) schedule the DAG (Belady/cache-oblivious register model, FFTW pldi99) ->
  (3) emit AoS/over-2 AVX2 intrinsics -> (4) brute-DFT gate + tight-interleaved bench vs MKL.
"""
import cmath
import numpy as np

# ---- hash-consed complex-expression DAG (CSE by construction) ----
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
    """multiply expression a by complex constant w (key trivial cases folded)."""
    if abs(w - 1) < 1e-15:
        return a
    if abs(w + 1) < 1e-15:
        return _node("neg", a)
    if abs(w - 1j) < 1e-15:
        return _node("muli", a)        # * (+i)
    if abs(w + 1j) < 1e-15:
        return _node("mulni", a)       # * (-i)
    return _node("cmul", a, None, (round(w.real, 17), round(w.imag, 17)))


# ---- recursive split-radix FFT building the DAG; returns list of N output Nodes ----
def fft_dag(x):
    n = len(x)
    if n == 1:
        return [x[0]]
    if n == 2:
        return [add(x[0], x[1]), sub(x[0], x[1])]
    # split-radix: U = FFT(evens), Z1 = FFT(x[1::4]), Z3 = FFT(x[3::4])
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
        di = _node("mulni", d)         # -i * d
        out[k] = add(U[k], s)
        out[k + n // 2] = sub(U[k], s)
        out[k + q] = add(U[k + q], di)
        out[k + 3 * q] = sub(U[k + q], di)
    return out


# ---- evaluate the DAG numerically (validation) ----
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


def op_counts(outputs):
    seen, adds, muls = set(), 0, 0
    stack = list(outputs)
    while stack:
        nd = stack.pop()
        if nd.id in seen:
            continue
        seen.add(nd.id)
        if nd.op in ("add", "sub"):
            adds += 1
            stack += [nd.a, nd.b]
        elif nd.op == "cmul":
            muls += 1
            stack.append(nd.a)
        elif nd.op in ("neg", "muli", "mulni"):
            stack.append(nd.a)
    return adds, muls, len(seen)


# ---- STEP 3: emit over-2 AVX2 codelet (each DAG op -> 1 SIMD op; 2 transforms in lanes, no lane-cross) ----
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
    return orde


def schedule(outputs):
    """STEP 2: greedy register-pressure-minimizing schedule (genfft lever).
    At each step emit the ready op that frees the most inputs (last-use -> dead -> register freed),
    minimizing peak-live so the compiler spills less. Deterministic (tie-break by id)."""
    nodes = _topo(outputs)
    ins_of = {}
    deps = {nd.id: [] for nd in nodes}
    for nd in nodes:
        ins = [c for c in (nd.a, nd.b) if isinstance(c, Node)]
        ins_of[nd.id] = ins
        for c in ins:
            deps[c.id].append(nd)
    uses = {nd.id: len(deps[nd.id]) + (1 if nd in outputs else 0) for nd in nodes}
    indeg = {nd.id: len(ins_of[nd.id]) for nd in nodes}
    ready = [nd for nd in nodes if indeg[nd.id] == 0]
    order = []
    while ready:
        def score(nd):
            freed = sum(1 for c in ins_of[nd.id] if uses[c.id] == 1)
            return (freed, -nd.id)  # max freed, then deterministic
        ready.sort(key=score, reverse=True)
        nd = ready.pop(0)
        order.append(nd)
        for c in ins_of[nd.id]:
            uses[c.id] -= 1
        for u in deps[nd.id]:
            indeg[u.id] -= 1
            if indeg[u.id] == 0:
                ready.append(u)
    return orde


def emit_codelet(N, outputs, name):
    order = schedule(outputs)
    L = [f"static inline void {name}(const __m256d* in, __m256d* out)", "{"]
    for nd in order:
        v = f"n{nd.id}"
        if nd.op == "input":
            L.append(f"    __m256d {v} = in[{nd.a}];")
        elif nd.op == "add":
            L.append(f"    __m256d {v} = _mm256_add_pd(n{nd.a.id}, n{nd.b.id});")
        elif nd.op == "sub":
            L.append(f"    __m256d {v} = _mm256_sub_pd(n{nd.a.id}, n{nd.b.id});")
        elif nd.op == "neg":
            L.append(f"    __m256d {v} = _mm256_sub_pd(_mm256_setzero_pd(), n{nd.a.id});")
        elif nd.op == "muli":
            L.append(f"    __m256d {v} = mul_pi(n{nd.a.id});")
        elif nd.op == "mulni":
            L.append(f"    __m256d {v} = mul_ni(n{nd.a.id});")
        elif nd.op == "cmul":
            wr, wi = nd.c
            L.append(f"    __m256d {v} = cmul(n{nd.a.id}, _mm256_set1_pd({wr!r}), _mm256_set1_pd({wi!r}));")
    for k, o in enumerate(outputs):
        L.append(f"    out[{k}] = n{o.id};")
    L.append("}")
    return "\n".join(L)


_HARNESS = r"""
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <immintrin.h>
#include <mkl_dfti.h>
using clk = std::chrono::steady_clock;
static const double PI = 3.141592653589793238;
static inline __m256d cmul(__m256d x, __m256d wr, __m256d wi) { return _mm256_fmaddsub_pd(wr, x, _mm256_mul_pd(wi, _mm256_permute_pd(x, 0x5))); }
static inline __m256d mul_pi(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(1.0, -1.0, 1.0, -1.0)); } // * (+i)
static inline __m256d mul_ni(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(-1.0, 1.0, -1.0, 1.0)); } // * (-i)
%CODELET%
int main()
{
    const int N = %N%, HM = 16, NP = HM / 2;
    alignas(64) static double in[HM * %N% * 2], out[HM * %N% * 2];
    std::uint64_t s = 3; for (int i = 0; i < HM * N * 2; ++i) { s = s * 6364136223846793005ULL + 1; in[i] = (double)(s >> 11) / (double)(1ULL << 53) * 2 - 1; }
    auto gather = [&](int p, __m256d* g) { const double* b0 = in + (2 * p) * N * 2, *b1 = in + (2 * p + 1) * N * 2; for (int j = 0; j < N; ++j) { __m128d lo = _mm_load_pd(b0 + j * 2), hi = _mm_load_pd(b1 + j * 2); g[j] = _mm256_set_m128d(hi, lo); } };
    __m256d ig[%N%], og[%N%]; gather(0, ig); codelet(ig, og);
    alignas(64) double o[%N% * 4]; for (int k = 0; k < N; ++k) _mm256_storeu_pd(o + 4 * k, og[k]);
    double me = 0, mr = 0;
    for (int tt = 0; tt < 2; ++tt) for (int k = 0; k < N; ++k) { double rr = 0, ri = 0; for (int n = 0; n < N; ++n) { double a = -2 * PI * ((k * n) % N) / (double)N, c = std::cos(a), sn = std::sin(a); double xr = in[(tt * N + n) * 2], xi = in[(tt * N + n) * 2 + 1]; rr += xr * c - xi * sn; ri += xr * sn + xi * c; } mr = std::fmax(mr, std::fabs(rr)); me = std::fmax(me, std::fabs(o[4 * k + tt * 2] - rr)); me = std::fmax(me, std::fabs(o[4 * k + tt * 2 + 1] - ri)); }
    std::printf("gen N=%d gate err=%.2e %s\n", N, me / (1 + mr), (me / (1 + mr) < 1e-12) ? "PASS" : "*** FAIL ***");
    DFTI_DESCRIPTOR_HANDLE h = nullptr; DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, (MKL_LONG)N); DftiSetValue(h, DFTI_NUMBER_OF_TRANSFORMS, (MKL_LONG)HM); DftiSetValue(h, DFTI_INPUT_DISTANCE, (MKL_LONG)N); DftiSetValue(h, DFTI_PLACEMENT, DFTI_NOT_INPLACE); DftiSetValue(h, DFTI_OUTPUT_DISTANCE, (MKL_LONG)N); DftiCommitDescriptor(h);
    const int burst = 30000, T = 140; double rat[T];
    auto cerb = [&]() { for (int p = 0; p < NP; ++p) { __m256d cig[%N%], cog[%N%]; const double* b0 = in + (2 * p) * N * 2, *b1 = in + (2 * p + 1) * N * 2; for (int j = 0; j < N; ++j) { __m128d lo = _mm_load_pd(b0 + j * 2), hi = _mm_load_pd(b1 + j * 2); cig[j] = _mm256_set_m128d(hi, lo); } codelet(cig, cog); double* d0 = out + (2 * p) * N * 2, *d1 = out + (2 * p + 1) * N * 2; for (int k = 0; k < N; ++k) { _mm_store_pd(d0 + k * 2, _mm256_castpd256_pd128(cog[k])); _mm_store_pd(d1 + k * 2, _mm256_extractf128_pd(cog[k], 1)); } } };
    for (int w = 0; w < 300; ++w) { cerb(); DftiComputeForward(h, in, out); }
    for (int t = 0; t < T; ++t) { auto c0 = clk::now(); for (int r = 0; r < burst; ++r) cerb(); auto c1 = clk::now(); auto m0 = clk::now(); for (int r = 0; r < burst; ++r) DftiComputeForward(h, in, out); auto m1 = clk::now(); rat[t] = std::chrono::duration<double, std::milli>(m1 - m0).count() / std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    std::sort(rat, rat + T);
    std::printf("gen N=%d  Cerid/MKL median %.3f IQR[%.3f,%.3f] => %s\n", N, rat[T / 2], rat[T / 4], rat[3 * T / 4], rat[T / 4] > 1.0 ? "CRUSH" : rat[3 * T / 4] < 1.0 ? "below" : "parity");
    return 0;
}
"""


# ---- STEP 2b: lane-split emission — N=2M, even/odd halves in the 2 ymm lanes (16 ymm not 32), lane-merge ----
_HARNESS_LS = r"""
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <immintrin.h>
#include <mkl_dfti.h>
using clk = std::chrono::steady_clock;
static const double PI = 3.141592653589793238;
static inline __m256d cmul(__m256d x, __m256d wr, __m256d wi) { return _mm256_fmaddsub_pd(wr, x, _mm256_mul_pd(wi, _mm256_permute_pd(x, 0x5))); }
static inline __m256d mul_pi(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(1.0, -1.0, 1.0, -1.0)); }
static inline __m256d mul_ni(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(-1.0, 1.0, -1.0, 1.0)); }
%SUB%
static __m256d Wp_r[%M%/2], Wp_i[%M%/2];
static void initw() { for (int j = 0; j < %M% / 2; ++j) { double c0 = std::cos(-2 * PI * (2 * j) / %N%), s0 = std::sin(-2 * PI * (2 * j) / %N%), c1 = std::cos(-2 * PI * (2 * j + 1) / %N%), s1 = std::sin(-2 * PI * (2 * j + 1) / %N%); Wp_r[j] = _mm256_set_pd(c1, c1, c0, c0); Wp_i[j] = _mm256_set_pd(s1, s1, s0, s0); } }
static inline void fftN(const double* xt, double* outt)
{
    __m256d ig[%M%], og[%M%];
    for (int j = 0; j < %M%; ++j) ig[j] = _mm256_load_pd(xt + (2 * j) * 2); // [x[2j],x[2j+1]] = [even_j, odd_j]
    sub(ig, og); // og[k] = [E[k], O[k]]
    for (int j = 0; j < %M% / 2; ++j)
    {
        __m256d a = og[2 * j], b = og[2 * j + 1];
        __m256d Ep = _mm256_permute2f128_pd(a, b, 0x20), Op = _mm256_permute2f128_pd(a, b, 0x31);
        __m256d WO = cmul(Op, Wp_r[j], Wp_i[j]);
        _mm256_store_pd(outt + (2 * j) * 2, _mm256_add_pd(Ep, WO));
        _mm256_store_pd(outt + (2 * j + %M%) * 2, _mm256_sub_pd(Ep, WO));
    }
}
int main()
{
    initw();
    const int N = %N%, HM = 16;
    alignas(64) static double in[HM * %N% * 2], out[HM * %N% * 2];
    std::uint64_t s = 3; for (int i = 0; i < HM * N * 2; ++i) { s = s * 6364136223846793005ULL + 1; in[i] = (double)(s >> 11) / (double)(1ULL << 53) * 2 - 1; }
    fftN(in, out);
    double me = 0, mr = 0;
    for (int k = 0; k < N; ++k) { double rr = 0, ri = 0; for (int n = 0; n < N; ++n) { double a = -2 * PI * ((k * n) % N) / (double)N, c = std::cos(a), sn = std::sin(a); rr += in[2 * n] * c - in[2 * n + 1] * sn; ri += in[2 * n] * sn + in[2 * n + 1] * c; } mr = std::fmax(mr, std::fabs(rr)); me = std::fmax(me, std::fabs(out[2 * k] - rr)); me = std::fmax(me, std::fabs(out[2 * k + 1] - ri)); }
    std::printf("gen-ls N=%d gate err=%.2e %s\n", N, me / (1 + mr), (me / (1 + mr) < 1e-12) ? "PASS" : "*** FAIL ***");
    DFTI_DESCRIPTOR_HANDLE h = nullptr; DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, (MKL_LONG)N); DftiSetValue(h, DFTI_NUMBER_OF_TRANSFORMS, (MKL_LONG)HM); DftiSetValue(h, DFTI_INPUT_DISTANCE, (MKL_LONG)N); DftiSetValue(h, DFTI_PLACEMENT, DFTI_NOT_INPLACE); DftiSetValue(h, DFTI_OUTPUT_DISTANCE, (MKL_LONG)N); DftiCommitDescriptor(h);
    const int burst = 20000, T = 140; double rat[T];
    auto cerb = [&]() { for (int t = 0; t < HM; ++t) fftN(in + t * N * 2, out + t * N * 2); };
    for (int w = 0; w < 300; ++w) { cerb(); DftiComputeForward(h, in, out); }
    for (int t = 0; t < T; ++t) { auto c0 = clk::now(); for (int r = 0; r < burst; ++r) cerb(); auto c1 = clk::now(); auto m0 = clk::now(); for (int r = 0; r < burst; ++r) DftiComputeForward(h, in, out); auto m1 = clk::now(); rat[t] = std::chrono::duration<double, std::milli>(m1 - m0).count() / std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    std::sort(rat, rat + T);
    std::printf("gen-ls N=%d  Cerid/MKL median %.3f IQR[%.3f,%.3f] => %s\n", N, rat[T / 2], rat[T / 4], rat[3 * T / 4], rat[T / 4] > 1.0 ? "CRUSH" : rat[3 * T / 4] < 1.0 ? "below" : "parity");
    return 0;
}
"""


def _emit_op(nd, load_input):
    v = f"n{nd.id}"
    if nd.op == "input":
        return f"    __m256d {v} = {load_input(nd.a)};"
    if nd.op == "add":
        return f"    __m256d {v} = _mm256_add_pd(n{nd.a.id}, n{nd.b.id});"
    if nd.op == "sub":
        return f"    __m256d {v} = _mm256_sub_pd(n{nd.a.id}, n{nd.b.id});"
    if nd.op == "neg":
        return f"    __m256d {v} = _mm256_sub_pd(_mm256_setzero_pd(), n{nd.a.id});"
    if nd.op == "muli":
        return f"    __m256d {v} = mul_pi(n{nd.a.id});"
    if nd.op == "mulni":
        return f"    __m256d {v} = mul_ni(n{nd.a.id});"
    if nd.op == "cmul":
        wr, wi = nd.c
        return f"    __m256d {v} = cmul(n{nd.a.id}, _mm256_set1_pd({wr!r}), _mm256_set1_pd({wi!r}));"
    raise ValueError(nd.op)


def emit_lanesplit_fused(N, M, outs):
    """merge-fusion: interleave the lane-merge into the scheduled sub-ops; emit each pair's
    merge the moment both its outputs are computed, freeing them mid-stream (kills the M-output boundary)."""
    order = schedule(outs)
    out_idx = {o.id: k for k, o in enumerate(outs)}
    var_of, done, merged = {}, set(), set()
    L = ["static inline void fftN(const double* xt, double* outt)", "{"]
    for nd in order:
        L.append(_emit_op(nd, lambda i: f"_mm256_load_pd(xt + (2 * {i}) * 2)"))
        if nd.id in out_idx:
            k = out_idx[nd.id]
            var_of[k] = f"n{nd.id}"
            done.add(k)
            j = k // 2
            if j not in merged and (2 * j) in done and (2 * j + 1) in done:
                a, b = var_of[2 * j], var_of[2 * j + 1]
                L.append(f"    {{ __m256d Ep = _mm256_permute2f128_pd({a}, {b}, 0x20), Op = _mm256_permute2f128_pd({a}, {b}, 0x31);")
                L.append(f"      __m256d WO = cmul(Op, Wp_r[{j}], Wp_i[{j}]);")
                L.append(f"      _mm256_store_pd(outt + (2 * {j}) * 2, _mm256_add_pd(Ep, WO)); _mm256_store_pd(outt + (2 * {j} + {M}) * 2, _mm256_sub_pd(Ep, WO)); }}")
                merged.add(j)
    L.append("}")
    return "\n".join(L)


_HARNESS_LSF = r"""
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <immintrin.h>
#include <mkl_dfti.h>
using clk = std::chrono::steady_clock;
static const double PI = 3.141592653589793238;
static inline __m256d cmul(__m256d x, __m256d wr, __m256d wi) { return _mm256_fmaddsub_pd(wr, x, _mm256_mul_pd(wi, _mm256_permute_pd(x, 0x5))); }
static inline __m256d mul_pi(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(1.0, -1.0, 1.0, -1.0)); }
static inline __m256d mul_ni(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(-1.0, 1.0, -1.0, 1.0)); }
static __m256d Wp_r[%M%/2], Wp_i[%M%/2];
static void initw() { for (int j = 0; j < %M% / 2; ++j) { double c0 = std::cos(-2 * PI * (2 * j) / %N%), s0 = std::sin(-2 * PI * (2 * j) / %N%), c1 = std::cos(-2 * PI * (2 * j + 1) / %N%), s1 = std::sin(-2 * PI * (2 * j + 1) / %N%); Wp_r[j] = _mm256_set_pd(c1, c1, c0, c0); Wp_i[j] = _mm256_set_pd(s1, s1, s0, s0); } }
%BODY%
int main()
{
    initw();
    const int N = %N%, HM = 16;
    alignas(64) static double in[HM * %N% * 2], out[HM * %N% * 2];
    std::uint64_t s = 3; for (int i = 0; i < HM * N * 2; ++i) { s = s * 6364136223846793005ULL + 1; in[i] = (double)(s >> 11) / (double)(1ULL << 53) * 2 - 1; }
    fftN(in, out);
    double me = 0, mr = 0;
    for (int k = 0; k < N; ++k) { double rr = 0, ri = 0; for (int n = 0; n < N; ++n) { double a = -2 * PI * ((k * n) % N) / (double)N, c = std::cos(a), sn = std::sin(a); rr += in[2 * n] * c - in[2 * n + 1] * sn; ri += in[2 * n] * sn + in[2 * n + 1] * c; } mr = std::fmax(mr, std::fabs(rr)); me = std::fmax(me, std::fabs(out[2 * k] - rr)); me = std::fmax(me, std::fabs(out[2 * k + 1] - ri)); }
    std::printf("gen-lsf N=%d gate err=%.2e %s\n", N, me / (1 + mr), (me / (1 + mr) < 1e-12) ? "PASS" : "*** FAIL ***");
    DFTI_DESCRIPTOR_HANDLE h = nullptr; DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, (MKL_LONG)N); DftiSetValue(h, DFTI_NUMBER_OF_TRANSFORMS, (MKL_LONG)HM); DftiSetValue(h, DFTI_INPUT_DISTANCE, (MKL_LONG)N); DftiSetValue(h, DFTI_PLACEMENT, DFTI_NOT_INPLACE); DftiSetValue(h, DFTI_OUTPUT_DISTANCE, (MKL_LONG)N); DftiCommitDescriptor(h);
    const int burst = 20000, T = 140; double rat[T];
    auto cerb = [&]() { for (int t = 0; t < HM; ++t) fftN(in + t * N * 2, out + t * N * 2); };
    for (int w = 0; w < 300; ++w) { cerb(); DftiComputeForward(h, in, out); }
    for (int t = 0; t < T; ++t) { auto c0 = clk::now(); for (int r = 0; r < burst; ++r) cerb(); auto c1 = clk::now(); auto m0 = clk::now(); for (int r = 0; r < burst; ++r) DftiComputeForward(h, in, out); auto m1 = clk::now(); rat[t] = std::chrono::duration<double, std::milli>(m1 - m0).count() / std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    std::sort(rat, rat + T);
    std::printf("gen-lsf N=%d  Cerid/MKL median %.3f IQR[%.3f,%.3f] => %s\n", N, rat[T / 2], rat[T / 4], rat[3 * T / 4], rat[T / 4] > 1.0 ? "CRUSH" : rat[3 * T / 4] < 1.0 ? "below" : "parity");
    return 0;
}
"""


_HARNESS_CMP = r"""
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <immintrin.h>
#include <mkl_dfti.h>
using clk = std::chrono::steady_clock;
static const double PI = 3.141592653589793238;
static inline __m256d cmul(__m256d x, __m256d wr, __m256d wi) { return _mm256_fmaddsub_pd(wr, x, _mm256_mul_pd(wi, _mm256_permute_pd(x, 0x5))); }
static inline __m256d mul_pi(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(1.0, -1.0, 1.0, -1.0)); }
static inline __m256d mul_ni(__m256d z) { return _mm256_mul_pd(_mm256_permute_pd(z, 0x5), _mm256_set_pd(-1.0, 1.0, -1.0, 1.0)); }
static __m256d WM_r[%S%/2], WM_i[%S%/2], WN_r[%M%/2], WN_i[%M%/2];
static void initw() {
    for (int j = 0; j < %S% / 2; ++j) { double c0=std::cos(-2*PI*(2*j)/%M%), s0=std::sin(-2*PI*(2*j)/%M%), c1=std::cos(-2*PI*(2*j+1)/%M%), s1=std::sin(-2*PI*(2*j+1)/%M%); WM_r[j]=_mm256_set_pd(c1,c1,c0,c0); WM_i[j]=_mm256_set_pd(s1,s1,c0*0+s0,s0); }
    for (int j = 0; j < %M% / 2; ++j) { double c0=std::cos(-2*PI*(2*j)/%N%), s0=std::sin(-2*PI*(2*j)/%N%), c1=std::cos(-2*PI*(2*j+1)/%N%), s1=std::sin(-2*PI*(2*j+1)/%N%); WN_r[j]=_mm256_set_pd(c1,c1,c0,c0); WN_i[j]=_mm256_set_pd(s1,s1,s0,s0); }
}
// fftHalf: size-M DFT of the M strided values v[m]=xt[base+2m]; lane-split sub-DFT size S=M/2; result -> Eo[M].
static inline void fftHalf(const double* xt, int base, double* Eo)
{
    __m256d og[%S%];
    {
%SUBBODY%
    }
    for (int j = 0; j < %S% / 2; ++j) {
        __m256d a = og[2 * j], b = og[2 * j + 1];
        __m256d Ep = _mm256_permute2f128_pd(a, b, 0x20), Op = _mm256_permute2f128_pd(a, b, 0x31);
        __m256d WO = cmul(Op, WM_r[j], WM_i[j]);
        _mm256_store_pd(Eo + (2 * j) * 2, _mm256_add_pd(Ep, WO));
        _mm256_store_pd(Eo + (2 * j + %S%) * 2, _mm256_sub_pd(Ep, WO));
    }
}
static inline void fftN(const double* xt, double* outt)
{
    alignas(64) double Eb[%M% * 2], Ob[%M% * 2];
    fftHalf(xt, 0, Eb); fftHalf(xt, 1, Ob);
    for (int j = 0; j < %M% / 2; ++j) {
        __m256d E = _mm256_load_pd(Eb + (2 * j) * 2), O = _mm256_load_pd(Ob + (2 * j) * 2);
        __m256d WO = cmul(O, WN_r[j], WN_i[j]);
        _mm256_store_pd(outt + (2 * j) * 2, _mm256_add_pd(E, WO));
        _mm256_store_pd(outt + (2 * j + %M%) * 2, _mm256_sub_pd(E, WO));
    }
}
int main()
{
    initw();
    const int N = %N%, HM = 16;
    alignas(64) static double in[HM * %N% * 2], out[HM * %N% * 2];
    std::uint64_t s = 3; for (int i = 0; i < HM * N * 2; ++i) { s = s * 6364136223846793005ULL + 1; in[i] = (double)(s >> 11) / (double)(1ULL << 53) * 2 - 1; }
    fftN(in, out);
    double me = 0, mr = 0;
    for (int k = 0; k < N; ++k) { double rr = 0, ri = 0; for (int n = 0; n < N; ++n) { double a = -2 * PI * ((k * n) % N) / (double)N, c = std::cos(a), sn = std::sin(a); rr += in[2 * n] * c - in[2 * n + 1] * sn; ri += in[2 * n] * sn + in[2 * n + 1] * c; } mr = std::fmax(mr, std::fabs(rr)); me = std::fmax(me, std::fabs(out[2 * k] - rr)); me = std::fmax(me, std::fabs(out[2 * k + 1] - ri)); }
    std::printf("gen-cmp N=%d gate err=%.2e %s\n", N, me / (1 + mr), (me / (1 + mr) < 1e-12) ? "PASS" : "*** FAIL ***");
    DFTI_DESCRIPTOR_HANDLE h = nullptr; DftiCreateDescriptor(&h, DFTI_DOUBLE, DFTI_COMPLEX, 1, (MKL_LONG)N); DftiSetValue(h, DFTI_NUMBER_OF_TRANSFORMS, (MKL_LONG)HM); DftiSetValue(h, DFTI_INPUT_DISTANCE, (MKL_LONG)N); DftiSetValue(h, DFTI_PLACEMENT, DFTI_NOT_INPLACE); DftiSetValue(h, DFTI_OUTPUT_DISTANCE, (MKL_LONG)N); DftiCommitDescriptor(h);
    const int burst = 16000, T = 140; double rat[T];
    auto cerb = [&]() { for (int t = 0; t < HM; ++t) fftN(in + t * N * 2, out + t * N * 2); };
    for (int w = 0; w < 300; ++w) { cerb(); DftiComputeForward(h, in, out); }
    for (int t = 0; t < T; ++t) { auto c0 = clk::now(); for (int r = 0; r < burst; ++r) cerb(); auto c1 = clk::now(); auto m0 = clk::now(); for (int r = 0; r < burst; ++r) DftiComputeForward(h, in, out); auto m1 = clk::now(); rat[t] = std::chrono::duration<double, std::milli>(m1 - m0).count() / std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    std::sort(rat, rat + T);
    std::printf("gen-cmp N=%d  Cerid/MKL median %.3f IQR[%.3f,%.3f] => %s\n", N, rat[T / 2], rat[T / 4], rat[3 * T / 4], rat[T / 4] > 1.0 ? "CRUSH" : rat[3 * T / 4] < 1.0 ? "below" : "parity");
    return 0;
}
"""


def emit_sub_body_strided(outs):
    """sub-DFT over-2 ops with STRIDED gather input (ig[i] = [xt[base+4i], xt[base+4i+2]]) -> og[k]."""
    order = schedule(outs)
    out_idx = {o.id: k for k, o in enumerate(outs)}
    L = []
    for nd in order:
        if nd.op == "input":
            i = nd.a
            L.append(f"        __m256d n{nd.id} = _mm256_set_m128d(_mm_load_pd(xt + (base + 4 * {i} + 2) * 2), _mm_load_pd(xt + (base + 4 * {i}) * 2));")
        else:
            L.append("    " + _emit_op(nd, None).strip())
    for k, o in enumerate(outs):
        L.append(f"        og[{k}] = n{o.id};")
    return "\n".join(L)


def write_cpp_compose(N, path):
    M = N // 2
    S = M // 2
    _TABLE.clear()
    _NODES.clear()
    xs = [inp(i) for i in range(S)]
    outs = fft_dag(xs)
    body = emit_sub_body_strided(outs)
    code = _HARNESS_CMP.replace("%SUBBODY%", body).replace("%S%", str(S)).replace("%M%", str(M)).replace("%N%", str(N))
    with open(path, "w") as f:
        f.write(code)
    print(f"wrote {path} (COMPOSE N={N} = 2x{M}, sub-DFT {S})")


def write_cpp_lsf(N, path):
    M = N // 2
    _TABLE.clear()
    _NODES.clear()
    xs = [inp(i) for i in range(M)]
    outs = fft_dag(xs)
    body = emit_lanesplit_fused(N, M, outs)
    code = _HARNESS_LSF.replace("%BODY%", body).replace("%M%", str(M)).replace("%N%", str(N))
    with open(path, "w") as f:
        f.write(code)
    print(f"wrote {path} (lane-split FUSED N={N})")


def write_cpp_ls(N, path):
    """lane-split: emit size-(N/2) over-2 codelet (lanes=E/O) + lane-merge."""
    M = N // 2
    _TABLE.clear()
    _NODES.clear()
    xs = [inp(i) for i in range(M)]
    outs = fft_dag(xs)
    sub = emit_codelet(M, outs, "sub")
    code = _HARNESS_LS.replace("%SUB%", sub).replace("%M%", str(M)).replace("%N%", str(N))
    with open(path, "w") as f:
        f.write(code)
    print(f"wrote {path} (lane-split N={N}, sub-DFT size {M})")


def write_cpp(N, path):
    _TABLE.clear()
    _NODES.clear()
    xs = [inp(i) for i in range(N)]
    outs = fft_dag(xs)
    code = _HARNESS.replace("%CODELET%", emit_codelet(N, outs, "codelet")).replace("%N%", str(N))
    with open(path, "w") as f:
        f.write(code)
    print(f"wrote {path} (N={N})")


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 3 and sys.argv[1] == "emit":
        write_cpp(int(sys.argv[2]), sys.argv[3])
        sys.exit(0)
    if len(sys.argv) >= 3 and sys.argv[1] == "emitls":
        write_cpp_ls(int(sys.argv[2]), sys.argv[3])
        sys.exit(0)
    if len(sys.argv) >= 3 and sys.argv[1] == "emitlsf":
        write_cpp_lsf(int(sys.argv[2]), sys.argv[3])
        sys.exit(0)
    if len(sys.argv) >= 3 and sys.argv[1] == "emitcmp":
        write_cpp_compose(int(sys.argv[2]), sys.argv[3])
        sys.exit(0)
    print("=== gen_fft_crush STEP 1: split-radix DAG, validated vs numpy ===")
    for N in (8, 16, 32, 64, 128, 256):
        _TABLE.clear()
        _NODES.clear()
        xs = [inp(i) for i in range(N)]
        outs = fft_dag(xs)
        rng = np.random.default_rng(1)
        xv = rng.standard_normal(N) + 1j * rng.standard_normal(N)
        got = np.array(evaluate(outs, xv))
        ref = np.fft.fft(xv)
        err = np.max(np.abs(got - ref)) / (1 + np.max(np.abs(ref)))
        adds, muls, nodes = op_counts(outs)
        status = "PASS" if err < 1e-12 else "*** FAIL ***"
        print(f"N={N:<4} err={err:.2e} {status}   adds={adds:<5} cmuls={muls:<4} dag_nodes={nodes}")
