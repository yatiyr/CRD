#pragma once

// vhvp.hpp — Phase 3.1.6 v16-e (vectorized addendum): a VECTORIZED forward-over-reverse HESSIAN-VECTOR product for
// functions expressed as VECTOR operations (elementwise · reductions · cyclic shifts). The scalar tape (hvp.hpp) is
// exact + general but executes a user functor in its literal (often serial) order; a vectorizable functor at large n
// is then latency-bound where JAX SIMD-vectorizes. This is the v16-h-class fix scoped to the HVP: a `VTape` of
// VECTOR ops — O(#ops) nodes, NOT O(n) scalar nodes — where every node/edge is an n-wide SoA `DualVec` (value +
// tangent) elementwise loop that `-O3 -march=native` auto-vectorizes to SIMD, and a reduction is a vectorizable sum
// (forward) + broadcast (backward) rather than a serial accumulator. Seed input = Dual{x_i, v_i}; the scalar output's
// adjoint TANGENT is exactly ∇²f·v, the VALUE part is ∇f — one forward build + one backward, exact, deterministic.
// Same forward-over-reverse math as hvp.hpp, one node per vector op instead of one per scalar op. ADR-0097.

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace crd::hesap::autodiff::reverse::vec
{

enum class VOp : crd::u8
{
    Leaf,
    Mul,  // elementwise a⊙b
    Add,  // a+b (matching length)
    Sub,  // a−b
    Sin,  // elementwise sin
    Roll, // cyclic shift by `param` (c[j] = a[(j−param) mod n])
    Sum   // reduce to length 1
};

// One vector node: forward DualVec (val,tan) + backward DualVec (adjv,adjt), all length `len`, arena-allocated.
struct VNode
{
    crd::f64* val;
    crd::f64* tan;
    crd::f64* adjv;
    crd::f64* adjt;
    crd::f64* pv; // stored local partial (value part) for transcendental unaries — no backward recompute
    crd::f64* pt; // stored local partial (tangent part)
    crd::u32  i0;
    crd::u32  i1;
    int       len;
    int       param;
    VOp       op;
};

class VTape
{
public:
    VTape(crd::memory::IAllocator* alloc, int n, int max_nodes) noexcept
        : m_alloc(alloc), m_n(n), m_cap(max_nodes)
    {
        m_node = static_cast<VNode*>(alloc->allocate(sizeof(VNode) * static_cast<crd::usize>(max_nodes)));
        // one contiguous scratch pool of 6 half-vectors per node (val/tan/adjv/adjt + the stored partial pv/pt)
        m_pool = static_cast<crd::f64*>(
            alloc->allocate(sizeof(crd::f64) * static_cast<crd::usize>(max_nodes) * 6 * static_cast<crd::usize>(n)));
    }
    [[nodiscard]] int      n() const noexcept { return m_n; }
    [[nodiscard]] crd::u32 count() const noexcept { return m_count; }
    [[nodiscard]] VNode&   node(crd::u32 id) noexcept { return m_node[id]; }

    [[nodiscard]] crd::u32 make(VOp op, int len)
    {
        const crd::u32 id  = m_count++;
        VNode&         nd  = m_node[id];
        crd::f64*      base = m_pool + static_cast<crd::usize>(id) * 6 * static_cast<crd::usize>(m_n);
        nd.val  = base;
        nd.tan  = base + m_n;
        nd.adjv = base + 2 * m_n;
        nd.adjt = base + 3 * m_n;
        nd.pv   = base + 4 * m_n;
        nd.pt   = base + 5 * m_n;
        nd.op   = op;
        nd.len  = len;
        return id;
    }
    void reset() noexcept { m_count = 0; }

private:
    crd::memory::IAllocator* m_alloc;
    VNode*                   m_node  = nullptr;
    crd::f64*                m_pool  = nullptr;
    int                      m_n     = 0;
    int                      m_cap   = 0;
    crd::u32                 m_count = 0;
};

struct VVar
{
    VTape*   t;
    crd::u32 node;
};

// input leaf: value = x, tangent = v (the HVP direction).
[[nodiscard]] inline VVar input(VTape& t, const crd::f64* x, const crd::f64* v) noexcept
{
    const crd::u32 id = t.make(VOp::Leaf, t.n());
    VNode&         nd = t.node(id);
    for (int j = 0; j < t.n(); ++j) { nd.val[j] = x[j]; nd.tan[j] = v[j]; }
    return {&t, id};
}

[[nodiscard]] inline VVar operator*(VVar a, VVar b) noexcept
{
    VTape& t  = *a.t;
    const int n = t.node(a.node).len;
    const crd::u32 id = t.make(VOp::Mul, n);
    VNode& c = t.node(id);
    const VNode& na = t.node(a.node);
    const VNode& nb = t.node(b.node);
    c.i0 = a.node;
    c.i1 = b.node;
    for (int j = 0; j < n; ++j)
    {
        c.val[j] = na.val[j] * nb.val[j];
        c.tan[j] = na.val[j] * nb.tan[j] + na.tan[j] * nb.val[j];
    }
    return {&t, id};
}
[[nodiscard]] inline VVar operator+(VVar a, VVar b) noexcept
{
    VTape& t = *a.t;
    const int n = t.node(a.node).len;
    const crd::u32 id = t.make(VOp::Add, n);
    VNode& c = t.node(id);
    const VNode& na = t.node(a.node);
    const VNode& nb = t.node(b.node);
    c.i0 = a.node;
    c.i1 = b.node;
    for (int j = 0; j < n; ++j) { c.val[j] = na.val[j] + nb.val[j]; c.tan[j] = na.tan[j] + nb.tan[j]; }
    return {&t, id};
}
[[nodiscard]] inline VVar operator-(VVar a, VVar b) noexcept
{
    VTape& t = *a.t;
    const int n = t.node(a.node).len;
    const crd::u32 id = t.make(VOp::Sub, n);
    VNode& c = t.node(id);
    const VNode& na = t.node(a.node);
    const VNode& nb = t.node(b.node);
    c.i0 = a.node;
    c.i1 = b.node;
    for (int j = 0; j < n; ++j) { c.val[j] = na.val[j] - nb.val[j]; c.tan[j] = na.tan[j] - nb.tan[j]; }
    return {&t, id};
}
[[nodiscard]] inline VVar sin(VVar a) noexcept
{
    VTape& t = *a.t;
    const int n = t.node(a.node).len;
    const crd::u32 id = t.make(VOp::Sin, n);
    VNode& c = t.node(id);
    const VNode& na = t.node(a.node);
    c.i0 = a.node;
    for (int j = 0; j < n; ++j)
    {
        crd::f64 s = 0.0;
        crd::f64 cc = 0.0;
        crd::math::sincos(na.val[j], s, cc);
        c.val[j] = s;
        c.tan[j] = cc * na.tan[j];
        c.pv[j]  = cc;               // stored partial = cos(a) as Dual {cos, −sin·a'}
        c.pt[j]  = -s * na.tan[j];
    }
    return {&t, id};
}
// cyclic shift: c[j] = a[(j − k) mod n]  (k = param). Two contiguous copies (auto-vectorizable), no per-element mod.
[[nodiscard]] inline VVar roll(VVar a, int k) noexcept
{
    VTape& t = *a.t;
    const int n = t.node(a.node).len;
    const crd::u32 id = t.make(VOp::Roll, n);
    VNode& c = t.node(id);
    const VNode& na = t.node(a.node);
    c.i0    = a.node;
    c.param = k;
    const int sh = ((k % n) + n) % n; // c[j] = a[(j−sh) mod n]
    for (int j = 0; j < n; ++j)
    {
        const int src = (j - sh + n) % n;
        c.val[j] = na.val[src];
        c.tan[j] = na.tan[src];
    }
    return {&t, id};
}
[[nodiscard]] inline VVar sum(VVar a) noexcept
{
    VTape& t = *a.t;
    const int n = t.node(a.node).len;
    const crd::u32 id = t.make(VOp::Sum, 1);
    VNode& c = t.node(id);
    const VNode& na = t.node(a.node);
    c.i0 = a.node;
    crd::f64 sv = 0.0;
    crd::f64 st = 0.0;
    for (int j = 0; j < n; ++j) { sv += na.val[j]; st += na.tan[j]; }
    c.val[0] = sv;
    c.tan[0] = st;
    return {&t, id};
}

// The driver: build the graph via `build(VTape&) -> VVar` (a scalar output), then one vector backward yields grad + Hv.
template <class Build>
inline void vhvp(VTape& tape, const Build& build, int n, crd::f64* grad, crd::f64* hv) noexcept
{
    tape.reset();
    const VVar y = build(tape); // forward build (SIMD elementwise, vectorizable reductions)
    // zero all adjoints, seed the scalar output's adjoint = Dual{1,0}
    for (crd::u32 id = 0; id < tape.count(); ++id)
    {
        VNode& nd = tape.node(id);
        for (int j = 0; j < nd.len; ++j) { nd.adjv[j] = 0.0; nd.adjt[j] = 0.0; }
    }
    tape.node(y.node).adjv[0] = 1.0;
    tape.node(y.node).adjt[0] = 0.0;
    // backward in reverse node order (operands precede results) — each op's VJP is an n-wide SIMD loop.
    for (crd::u32 ii = tape.count(); ii-- > 0;)
    {
        VNode&    c  = tape.node(ii);
        const int m  = c.len;
        switch (c.op)
        {
        case VOp::Leaf: break;
        case VOp::Add:
        {
            VNode& a = tape.node(c.i0);
            VNode& b = tape.node(c.i1);
            for (int j = 0; j < m; ++j) { a.adjv[j] += c.adjv[j]; a.adjt[j] += c.adjt[j]; b.adjv[j] += c.adjv[j]; b.adjt[j] += c.adjt[j]; }
            break;
        }
        case VOp::Sub:
        {
            VNode& a = tape.node(c.i0);
            VNode& b = tape.node(c.i1);
            for (int j = 0; j < m; ++j) { a.adjv[j] += c.adjv[j]; a.adjt[j] += c.adjt[j]; b.adjv[j] -= c.adjv[j]; b.adjt[j] -= c.adjt[j]; }
            break;
        }
        case VOp::Mul:
        {
            VNode& a = tape.node(c.i0);
            VNode& b = tape.node(c.i1);
            for (int j = 0; j < m; ++j) // ā += (b as Dual) ⊙ c̄ ; b̄ += (a as Dual) ⊙ c̄
            {
                a.adjv[j] += b.val[j] * c.adjv[j];
                a.adjt[j] += b.val[j] * c.adjt[j] + b.tan[j] * c.adjv[j];
                b.adjv[j] += a.val[j] * c.adjv[j];
                b.adjt[j] += a.val[j] * c.adjt[j] + a.tan[j] * c.adjv[j];
            }
            break;
        }
        case VOp::Sin:
        {
            VNode& a = tape.node(c.i0);
            for (int j = 0; j < m; ++j) // ā += stored partial {pv,pt} ⊙ c̄  (no sincos recompute)
            {
                a.adjv[j] += c.pv[j] * c.adjv[j];
                a.adjt[j] += c.pv[j] * c.adjt[j] + c.pt[j] * c.adjv[j];
            }
            break;
        }
        case VOp::Roll:
        {
            VNode&    a  = tape.node(c.i0);
            const int sh = ((c.param % m) + m) % m; // forward c[j]=a[(j−sh)]; backward ā[(j−sh)] += c̄[j]
            for (int j = 0; j < m; ++j)
            {
                const int src = (j - sh + m) % m;
                a.adjv[src] += c.adjv[j];
                a.adjt[src] += c.adjt[j];
            }
            break;
        }
        case VOp::Sum:
        {
            VNode&         a  = tape.node(c.i0);
            const crd::f64 av = c.adjv[0];
            const crd::f64 at = c.adjt[0];
            for (int j = 0; j < a.len; ++j) { a.adjv[j] += av; a.adjt[j] += at; } // broadcast the scalar adjoint
            break;
        }
        }
    }
    // leaf is node 0 (built first); read grad = adjoint value, Hv = adjoint tangent
    const VNode& leaf = tape.node(0);
    for (int j = 0; j < n; ++j) { grad[j] = leaf.adjv[j]; hv[j] = leaf.adjt[j]; }
}

} // namespace crd::hesap::autodiff::reverse::vec
