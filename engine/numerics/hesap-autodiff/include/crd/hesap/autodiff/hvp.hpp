#pragma once

// hvp.hpp — Phase 3.1.6 v16-e: HIGHER-ORDER — exact forward-over-reverse HESSIAN-VECTOR products + Hessian-free
// Newton-CG. A generic reverse-mode Wengert tape `RTape<T>` (the same transpose-of-JVP mechanism as tape.hpp, but
// templated on the scalar T): instantiate T = Dual<f64>, seed leaf i with Dual{x_i, v_i}. The forward build carries
// the directional tangent v; the backward accumulates Dual adjoints; so the output adjoint's TANGENT part is exactly
// (∇²f·v)_i and its VALUE part is (∇f)_i — the WHOLE gradient AND the Hessian-vector product from ONE forward build +
// ONE backward (forward-over-reverse, ~2 passes), exact, deterministic.
//
// The production f64 tape (tape.hpp) — with its batched-{1..16} deterministic-gradient moat — is DELIBERATELY left
// untouched; this small self-contained generic tape is the higher-order lane. Hessian-free Newton-CG (Newton step via
// CG, matrix-vector products = HVPs, never forms H) rides it. Cross-checked vs the v15-c hyper-dual (exact Hessian) +
// central FD of the gradient. Peers: JAX `hvp` (jvp∘grad), functorch. ADR-0097.

#include <crd/hesap/autodiff/dual.hpp> // Dual<f64> — the tangent carrier for forward-over-reverse

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::autodiff::reverse
{
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::hesap::autodiff::forward::Dual;

// Generic reverse-mode Wengert tape over the scalar T (T = f64 for a plain gradient, Dual<f64> for HVP). AoS: a node
// stores its value/adjoint + its ≤2 operands (index + local partial) INLINE — so ONE push per op (vs an SoA tape's
// 8), which is the build-time lever (the build, ~4n pushes, dominates the HVP at large n, not the backward). The
// backward propagates in reverse index order with NO zero-skip (a Dual adjoint may have value 0 yet tangent ≠ 0 —
// skipping on the value would drop the Hessian contribution).
template <class T>
struct RNode
{
    T        value; // read during the forward build only (dead in the backward)
    T        p0;
    T        p1;
    crd::u32 i0;
    crd::u32 i1;
    crd::u32 nops; // 0 = leaf, 1 = unary, 2 = binary
};

template <class T>
class RTape
{
public:
    explicit RTape(crd::memory::IAllocator* alloc) noexcept : m_node(alloc), m_adjoint(alloc) {}
    [[nodiscard]] crd::u32 leaf(T v)
    {
        RNode<T> nd;
        nd.value = v;
        nd.nops  = 0;
        return push(nd);
    }
    [[nodiscard]] crd::u32 unary(T v, crd::u32 a, T pa)
    {
        RNode<T> nd;
        nd.value = v;
        nd.i0    = a;
        nd.p0    = pa;
        nd.nops  = 1;
        return push(nd);
    }
    [[nodiscard]] crd::u32 binary(T v, crd::u32 a, T pa, crd::u32 b, T pb)
    {
        RNode<T> nd;
        nd.value = v;
        nd.i0    = a;
        nd.p0    = pa;
        nd.i1    = b;
        nd.p1    = pb;
        nd.nops  = 2;
        return push(nd);
    }
    // Add/sub fast nodes (unit partials): the backward is a plain += a / −= a — NO Dual multiply. The accumulator
    // pattern makes these ~half of all nodes; skipping their (identity) multiplies is the tape's biggest FLOP lever.
    [[nodiscard]] crd::u32 add_node(T v, crd::u32 a, crd::u32 b)
    {
        RNode<T> nd;
        nd.value = v;
        nd.i0    = a;
        nd.i1    = b;
        nd.nops  = 3;
        return push(nd);
    }
    [[nodiscard]] crd::u32 sub_node(T v, crd::u32 a, crd::u32 b)
    {
        RNode<T> nd;
        nd.value = v;
        nd.i0    = a;
        nd.i1    = b;
        nd.nops  = 4;
        return push(nd);
    }
    // Size the adjoint array (compact SoA) to the node count and zero it — one bulk pass instead of a per-node write.
    void finalize()
    {
        const crd::usize n = m_node.size();
        m_adjoint.resize(n);
        for (crd::usize i = 0; i < n; ++i) { m_adjoint[i] = static_cast<T>(0); }
    }
    [[nodiscard]] T    value(crd::u32 id) const noexcept { return m_node[id].value; }
    [[nodiscard]] T    grad(crd::u32 id) const noexcept { return m_adjoint[id]; }
    void               seed(crd::u32 id, T a) noexcept { m_adjoint[id] = a; }
    void               backward() noexcept
    {
        const crd::usize n = m_node.size();
        for (crd::usize ii = n; ii-- > 0;)
        {
            const T         a  = m_adjoint[ii];
            const RNode<T>& nd = m_node[ii];
            switch (nd.nops)
            {
            case 2: m_adjoint[nd.i0] += nd.p0 * a; m_adjoint[nd.i1] += nd.p1 * a; break; // general binary
            case 3: m_adjoint[nd.i0] += a; m_adjoint[nd.i1] += a; break;                 // add (unit partials)
            case 1: m_adjoint[nd.i0] += nd.p0 * a; break;                                // unary
            case 4: m_adjoint[nd.i0] += a; m_adjoint[nd.i1] -= a; break;                 // sub (unit partials)
            default: break;                                                             // 0 = leaf
            }
        }
    }
    void reset() noexcept
    {
        m_node.resize(0);
        m_adjoint.resize(0);
    }

private:
    crd::u32 push(const RNode<T>& nd)
    {
        const crd::u32 id = static_cast<crd::u32>(m_node.size());
        m_node.push_back(nd);
        return id;
    }
    crd::containers::Array<RNode<T>> m_node;
    crd::containers::Array<T>        m_adjoint;
};

template <class T>
struct RVar
{
    RTape<T>* tape = nullptr;
    crd::u32  node = 0;
    [[nodiscard]] T val() const noexcept { return tape->value(node); }
};

template <class T>
[[nodiscard]] inline RVar<T> make_leaf(RTape<T>& t, T v) noexcept { return RVar<T>{&t, t.leaf(v)}; }

// ---- arithmetic (local partials in T; the backward transposes them) -----------------------------------------
template <class T>
[[nodiscard]] inline RVar<T> operator+(RVar<T> a, RVar<T> b) noexcept { return {a.tape, a.tape->add_node(a.val() + b.val(), a.node, b.node)}; }
template <class T>
[[nodiscard]] inline RVar<T> operator-(RVar<T> a, RVar<T> b) noexcept { return {a.tape, a.tape->sub_node(a.val() - b.val(), a.node, b.node)}; }
template <class T>
[[nodiscard]] inline RVar<T> operator*(RVar<T> a, RVar<T> b) noexcept { return {a.tape, a.tape->binary(a.val() * b.val(), a.node, b.val(), b.node, a.val())}; }
template <class T>
[[nodiscard]] inline RVar<T> operator/(RVar<T> a, RVar<T> b) noexcept
{
    const T inv = static_cast<T>(1) / b.val();
    const T q   = a.val() * inv;
    return {a.tape, a.tape->binary(q, a.node, inv, b.node, -q * inv)};
}
template <class T>
[[nodiscard]] inline RVar<T> operator-(RVar<T> a) noexcept { return {a.tape, a.tape->unary(-a.val(), a.node, static_cast<T>(-1))}; }
// mixed scalar
template <class T>
[[nodiscard]] inline RVar<T> operator+(RVar<T> a, crd::f64 s) noexcept { return {a.tape, a.tape->unary(a.val() + static_cast<T>(s), a.node, static_cast<T>(1))}; }
template <class T>
[[nodiscard]] inline RVar<T> operator+(crd::f64 s, RVar<T> a) noexcept { return a + s; }
template <class T>
[[nodiscard]] inline RVar<T> operator-(RVar<T> a, crd::f64 s) noexcept { return {a.tape, a.tape->unary(a.val() - static_cast<T>(s), a.node, static_cast<T>(1))}; }
template <class T>
[[nodiscard]] inline RVar<T> operator-(crd::f64 s, RVar<T> a) noexcept { return {a.tape, a.tape->unary(static_cast<T>(s) - a.val(), a.node, static_cast<T>(-1))}; }
template <class T>
[[nodiscard]] inline RVar<T> operator*(RVar<T> a, crd::f64 s) noexcept { return {a.tape, a.tape->unary(a.val() * static_cast<T>(s), a.node, static_cast<T>(s))}; }
template <class T>
[[nodiscard]] inline RVar<T> operator*(crd::f64 s, RVar<T> a) noexcept { return a * s; }
template <class T>
[[nodiscard]] inline RVar<T> operator/(RVar<T> a, crd::f64 s) noexcept { const T inv = static_cast<T>(1) / static_cast<T>(s); return {a.tape, a.tape->unary(a.val() * inv, a.node, inv)}; }

// ---- transcendentals (value + local partial in T; ADL finds crd::math::FN for f64 and forward::FN for Dual) --
template <class T>
[[nodiscard]] inline RVar<T> exp(RVar<T> a) noexcept { using crd::math::exp; const T e = exp(a.val()); return {a.tape, a.tape->unary(e, a.node, e)}; }
template <class T>
[[nodiscard]] inline RVar<T> log(RVar<T> a) noexcept { using crd::math::log; return {a.tape, a.tape->unary(log(a.val()), a.node, static_cast<T>(1) / a.val())}; }
template <class T>
[[nodiscard]] inline RVar<T> sqrt(RVar<T> a) noexcept { using crd::math::sqrt; const T s = sqrt(a.val()); return {a.tape, a.tape->unary(s, a.node, static_cast<T>(1) / (static_cast<T>(2) * s))}; }
template <class T>
[[nodiscard]] inline RVar<T> tanh(RVar<T> a) noexcept { using crd::math::tanh; const T t = tanh(a.val()); return {a.tape, a.tape->unary(t, a.node, static_cast<T>(1) - t * t)}; }
// FUSED sin/cos: ONE range reduction yields BOTH sin(x) and cos(x) (as full T carrying the tangent) — a sin/cos node
// needs both (value + local partial), so this halves the transcendental cost of the tape's hot path.
namespace hvp_detail
{
inline void ad_sincos(crd::f64 x, crd::f64& s, crd::f64& c) noexcept { crd::math::sincos(x, s, c); }
inline void ad_sincos(Dual<crd::f64> x, Dual<crd::f64>& s, Dual<crd::f64>& c) noexcept
{
    crd::f64 sv = 0.0;
    crd::f64 cv = 0.0;
    crd::math::sincos(x.v, sv, cv);
    s = Dual<crd::f64>{sv, cv * x.d};   // sin: value sin, tangent cos·x'
    c = Dual<crd::f64>{cv, -sv * x.d};  // cos: value cos, tangent −sin·x'
}
} // namespace hvp_detail
template <class T>
[[nodiscard]] inline RVar<T> sin(RVar<T> a) noexcept { T s; T c; hvp_detail::ad_sincos(a.val(), s, c); return {a.tape, a.tape->unary(s, a.node, c)}; }
template <class T>
[[nodiscard]] inline RVar<T> cos(RVar<T> a) noexcept { T s; T c; hvp_detail::ad_sincos(a.val(), s, c); return {a.tape, a.tape->unary(c, a.node, -s)}; }
template <class T>
[[nodiscard]] inline RVar<T> pow(RVar<T> a, crd::f64 p) noexcept
{
    using crd::math::pow;
    const T val = pow(a.val(), p);
    return {a.tape, a.tape->unary(val, a.node, static_cast<T>(p) * pow(a.val(), p - 1.0))};
}

// ---- the forward-over-reverse HVP driver ---------------------------------------------------------------------
// f: scalar functor `RVar<Dual<f64>> f(const RVar<Dual<f64>>* x, int n)` (scalar-generic). Fills grad[0..n)=∇f and
// hv[0..n)=∇²f·v in ONE forward build + ONE backward. `scr` (≥ n) is caller leaf scratch; the tape is reset + reused.
template <class F>
inline void hvp(const F& f, ConstSpan<crd::f64> x, ConstSpan<crd::f64> v, Span<crd::f64> grad, Span<crd::f64> hv,
                RTape<Dual<crd::f64>>& tape, Span<RVar<Dual<crd::f64>>> scr) noexcept
{
    tape.reset();
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i) { scr[i] = make_leaf(tape, Dual<crd::f64>{x[i], v[i]}); } // value x_i, tangent v_i
    const RVar<Dual<crd::f64>> y = f(scr.data(), n);
    tape.finalize(); // size + zero the compact adjoint array (bulk)
    tape.seed(y.node, Dual<crd::f64>{1.0, 0.0});
    tape.backward();
    for (int i = 0; i < n; ++i)
    {
        const Dual<crd::f64> g = tape.grad(scr[i].node);
        grad[i]                = g.v; // (∇f)_i
        hv[i]                  = g.d; // (∇²f·v)_i
    }
}

// ---- Hessian-free Newton-CG (the opt consumer) ---------------------------------------------------------------
// One Newton step at x: solve H·p = −g by linear CG using HVPs (never forms H), then x += p. Returns f-gradient norm²
// after the step. All scratch caller-owned: g,hv,r,d,p (n each), scr (n leaf vars).
template <class F>
inline crd::f64 newton_cg_step(const F& f, Span<crd::f64> x, int cg_iters, crd::f64 cg_tol, RTape<Dual<crd::f64>>& tape,
                               Span<RVar<Dual<crd::f64>>> scr, Span<crd::f64> g, Span<crd::f64> hv, Span<crd::f64> r,
                               Span<crd::f64> d, Span<crd::f64> p) noexcept
{
    const int n = static_cast<int>(x.size());
    // g = ∇f(x) (HVP with v=0 gives the gradient in grad; hv unused)
    for (int i = 0; i < n; ++i) { d[i] = 0.0; }
    hvp(f, x, ConstSpan<crd::f64>(d.data(), n), g, hv, tape, scr);
    // CG solve H p = −g :  p=0, r=−g, d=r
    for (int i = 0; i < n; ++i) { p[i] = 0.0; r[i] = -g[i]; d[i] = r[i]; }
    crd::f64 rs = 0.0;
    for (int i = 0; i < n; ++i) { rs += r[i] * r[i]; }
    const crd::f64 rs0 = rs;
    for (int it = 0; it < cg_iters && rs > cg_tol * cg_tol * rs0; ++it)
    {
        hvp(f, x, ConstSpan<crd::f64>(d.data(), n), g, hv, tape, scr); // hv = H·d  (g reused = ∇f, ignored here)
        crd::f64 d_hd = 0.0;
        for (int i = 0; i < n; ++i) { d_hd += d[i] * hv[i]; }
        if (d_hd <= 0.0) { break; } // negative curvature — stop (trust-region would use the boundary)
        const crd::f64 alpha = rs / d_hd;
        for (int i = 0; i < n; ++i) { p[i] += alpha * d[i]; r[i] -= alpha * hv[i]; }
        crd::f64 rs_new = 0.0;
        for (int i = 0; i < n; ++i) { rs_new += r[i] * r[i]; }
        const crd::f64 beta = rs_new / rs;
        for (int i = 0; i < n; ++i) { d[i] = r[i] + beta * d[i]; }
        rs = rs_new;
    }
    for (int i = 0; i < n; ++i) { x[i] += p[i]; }
    // report the new gradient norm²
    for (int i = 0; i < n; ++i) { d[i] = 0.0; }
    hvp(f, x, ConstSpan<crd::f64>(d.data(), n), g, hv, tape, scr);
    crd::f64 gn = 0.0;
    for (int i = 0; i < n; ++i) { gn += g[i] * g[i]; }
    return gn;
}

} // namespace crd::hesap::autodiff::reverse
