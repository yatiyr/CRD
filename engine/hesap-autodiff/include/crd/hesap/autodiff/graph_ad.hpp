#pragma once

// graph_ad.hpp — Phase 3.1.6 v16-h: STRUCTURAL graph AD + tape→C++ CODEGEN. Trace a scalar-generic functor into an
// expression DAG (`Graph`), differentiate it SYMBOLICALLY (reverse-AD that emits gradient EXPRESSIONS as new graph
// nodes), run **const-fold / CSE / DCE** passes, then either INTERPRET the optimised graph or EMIT a straight-line C++
// kernel (`emit_cpp`) — the Enzyme/Tapenade source-transform lane, but as PORTABLE C++ (no LLVM-plugin lock-in). A
// codegen'd kernel drops the tape's per-node dispatch + dynamic allocation ⇒ a compiled hot loop; CSE/DCE shrink the
// op count. Deterministic (const-fold + emitted transcendentals both route through crd::math, so codegen is bit-
// identical to the interpreter). ADR-0081 (hot-reload cells) / ADR-0097.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace crd::hesap::autodiff::graph
{

enum class GOp : crd::u8 { Input, Const, Add, Sub, Mul, Div, Neg, Sin, Cos, Exp, Log, Sqrt, Tanh };

struct GNode
{
    GOp      op;
    int      a; // operand node id, or −1
    int      b; // operand node id, or −1
    crd::f64 c; // Const: value ; Input: input index ; else unused
};

// evaluate one op on already-computed operand values (shared by const-fold, eval, and the semantics codegen mirrors).
[[nodiscard]] inline crd::f64 g_apply(GOp op, crd::f64 av, crd::f64 bv) noexcept
{
    switch (op)
    {
    case GOp::Add: return av + bv;
    case GOp::Sub: return av - bv;
    case GOp::Mul: return av * bv;
    case GOp::Div: return av / bv;
    case GOp::Neg: return -av;
    case GOp::Sin: return crd::math::sin(av);
    case GOp::Cos: return crd::math::cos(av);
    case GOp::Exp: return crd::math::exp(av);
    case GOp::Log: return crd::math::log(av);
    case GOp::Sqrt: return crd::math::sqrt(av);
    case GOp::Tanh: return crd::math::tanh(av);
    default: return 0.0;
    }
}

class Graph
{
public:
    explicit Graph(crd::memory::IAllocator* alloc) : m_nodes(alloc) {}

    [[nodiscard]] int input() // a distinct input leaf; c = the input index (keeps CSE from merging distinct inputs)
    {
        m_nodes.push_back(GNode{GOp::Input, -1, -1, static_cast<crd::f64>(m_ninput++)});
        return static_cast<int>(m_nodes.size()) - 1;
    }
    [[nodiscard]] int constant(crd::f64 v)
    {
        m_nodes.push_back(GNode{GOp::Const, -1, -1, v});
        return static_cast<int>(m_nodes.size()) - 1;
    }
    [[nodiscard]] int un(GOp op, int a)
    {
        m_nodes.push_back(GNode{op, a, -1, 0.0});
        return static_cast<int>(m_nodes.size()) - 1;
    }
    [[nodiscard]] int bin(GOp op, int a, int b)
    {
        m_nodes.push_back(GNode{op, a, b, 0.0});
        return static_cast<int>(m_nodes.size()) - 1;
    }
    [[nodiscard]] int         size() const noexcept { return static_cast<int>(m_nodes.size()); }
    [[nodiscard]] int         n_inputs() const noexcept { return m_ninput; }
    [[nodiscard]] const GNode& node(int i) const noexcept { return m_nodes[static_cast<crd::usize>(i)]; }

    // interpret: fill vals[0..size) given the input vector `in` (indexed by each Input node's stored index).
    void eval(const crd::f64* in, crd::f64* vals) const noexcept
    {
        const int n = size();
        for (int i = 0; i < n; ++i)
        {
            const GNode& g = m_nodes[static_cast<crd::usize>(i)];
            if (g.op == GOp::Input) { vals[i] = in[static_cast<int>(g.c)]; }
            else if (g.op == GOp::Const) { vals[i] = g.c; }
            else { vals[i] = g_apply(g.op, vals[g.a], g.b >= 0 ? vals[g.b] : 0.0); }
        }
    }

    // symbolic reverse-AD of scalar `output` wrt the given input nodes ⇒ grad_nodes[k] = the node computing dOut/dInput_k.
    void reverse_ad(int output, const int* input_nodes, int n_in, int* grad_nodes)
    {
        const int         nfwd = size();
        crd::containers::Array<int> adj(m_nodes.allocator());
        adj.resize(static_cast<crd::usize>(nfwd), -1);
        adj[static_cast<crd::usize>(output)] = constant(1.0);
        for (int i = nfwd - 1; i >= 0; --i)
        {
            const int ci = adj[static_cast<crd::usize>(i)];
            if (ci < 0) { continue; }
            const GNode g = m_nodes[static_cast<crd::usize>(i)]; // COPY (pushes below can realloc m_nodes)
            switch (g.op)
            {
            case GOp::Add: accum(adj, g.a, ci); accum(adj, g.b, ci); break;
            case GOp::Sub: accum(adj, g.a, ci); accum(adj, g.b, un(GOp::Neg, ci)); break;
            case GOp::Mul: accum(adj, g.a, bin(GOp::Mul, ci, g.b)); accum(adj, g.b, bin(GOp::Mul, ci, g.a)); break;
            case GOp::Div:
            {
                accum(adj, g.a, bin(GOp::Div, ci, g.b));
                const int b2  = bin(GOp::Mul, g.b, g.b);
                const int num = bin(GOp::Mul, ci, g.a);
                accum(adj, g.b, un(GOp::Neg, bin(GOp::Div, num, b2))); // −ci·a/b²
                break;
            }
            case GOp::Neg: accum(adj, g.a, un(GOp::Neg, ci)); break;
            case GOp::Sin: accum(adj, g.a, bin(GOp::Mul, ci, un(GOp::Cos, g.a))); break;
            case GOp::Cos: accum(adj, g.a, un(GOp::Neg, bin(GOp::Mul, ci, un(GOp::Sin, g.a)))); break;
            case GOp::Exp: accum(adj, g.a, bin(GOp::Mul, ci, i)); break;             // d exp(a)=exp(a)=node i
            case GOp::Log: accum(adj, g.a, bin(GOp::Div, ci, g.a)); break;
            case GOp::Sqrt: accum(adj, g.a, bin(GOp::Div, ci, bin(GOp::Mul, constant(2.0), i))); break; // ci/(2·√a)
            case GOp::Tanh:
            {
                const int t2 = bin(GOp::Mul, i, i);
                const int om = bin(GOp::Sub, constant(1.0), t2); // 1−tanh²
                accum(adj, g.a, bin(GOp::Mul, ci, om));
                break;
            }
            default: break; // Input / Const are leaves
            }
        }
        for (int k = 0; k < n_in; ++k)
        {
            const int gi   = adj[static_cast<crd::usize>(input_nodes[k])];
            grad_nodes[k]  = (gi >= 0) ? gi : constant(0.0);
        }
    }

    // const-fold, then CSE (hash-cons), then DCE — keeping the `roots` (updated to their new ids). Idempotent.
    void optimize(int* roots, int n_roots)
    {
        const int                     n = size();
        crd::containers::Array<crd::f64> cval(m_nodes.allocator());
        crd::containers::Array<crd::u8>  isc(m_nodes.allocator());
        cval.resize(static_cast<crd::usize>(n), 0.0);
        isc.resize(static_cast<crd::usize>(n), 0);
        for (int i = 0; i < n; ++i) // const-fold in place
        {
            GNode& g = m_nodes[static_cast<crd::usize>(i)];
            if (g.op == GOp::Const) { isc[static_cast<crd::usize>(i)] = 1; cval[static_cast<crd::usize>(i)] = g.c; continue; }
            if (g.op == GOp::Input) { continue; }
            const bool ac = g.a < 0 || isc[static_cast<crd::usize>(g.a)];
            const bool bc = g.b < 0 || isc[static_cast<crd::usize>(g.b)];
            if (ac && bc)
            {
                const crd::f64 r = g_apply(g.op, g.a >= 0 ? cval[static_cast<crd::usize>(g.a)] : 0.0,
                                           g.b >= 0 ? cval[static_cast<crd::usize>(g.b)] : 0.0);
                g = GNode{GOp::Const, -1, -1, r};
                isc[static_cast<crd::usize>(i)]  = 1;
                cval[static_cast<crd::usize>(i)] = r;
            }
        }
        // mark reachable from roots
        crd::containers::Array<crd::u8> keep(m_nodes.allocator());
        crd::containers::Array<int>     stk(m_nodes.allocator());
        keep.resize(static_cast<crd::usize>(n), 0);
        for (int r = 0; r < n_roots; ++r) { stk.push_back(roots[r]); }
        while (stk.size() > 0)
        {
            const int i = stk[stk.size() - 1];
            stk.resize(stk.size() - 1);
            if (keep[static_cast<crd::usize>(i)]) { continue; }
            keep[static_cast<crd::usize>(i)] = 1;
            const GNode& g = m_nodes[static_cast<crd::usize>(i)];
            if (g.a >= 0) { stk.push_back(g.a); }
            if (g.b >= 0) { stk.push_back(g.b); }
        }
        // rebuild kept nodes in order with hash-cons CSE
        int cap = 1;
        while (cap < 2 * n + 4) { cap <<= 1; }
        crd::containers::Array<int> table(m_nodes.allocator());
        crd::containers::Array<int> newid(m_nodes.allocator());
        table.resize(static_cast<crd::usize>(cap), -1);
        newid.resize(static_cast<crd::usize>(n), -1);
        crd::containers::Array<GNode> nn(m_nodes.allocator());
        for (int i = 0; i < n; ++i)
        {
            if (!keep[static_cast<crd::usize>(i)]) { continue; }
            GNode g   = m_nodes[static_cast<crd::usize>(i)];
            if (g.a >= 0) { g.a = newid[static_cast<crd::usize>(g.a)]; }
            if (g.b >= 0) { g.b = newid[static_cast<crd::usize>(g.b)]; }
            const int found = intern(table, cap, nn, g);
            newid[static_cast<crd::usize>(i)] = found;
        }
        m_nodes = static_cast<crd::containers::Array<GNode>&&>(nn);
        for (int r = 0; r < n_roots; ++r) { roots[r] = newid[static_cast<crd::usize>(roots[r])]; }
    }

    [[nodiscard]] crd::memory::IAllocator* alloc() const noexcept { return m_nodes.allocator(); }

private:
    void accum(crd::containers::Array<int>& adj, int nodeidx, int contrib)
    {
        if (nodeidx < 0) { return; }
        int& slot = adj[static_cast<crd::usize>(nodeidx)];
        slot = (slot < 0) ? contrib : bin(GOp::Add, slot, contrib);
    }
    static crd::u64 key_hash(const GNode& g) noexcept
    {
        crd::u64 h = 1469598103934665603ULL;
        auto     mix = [&](crd::u64 v) { h ^= v; h *= 1099511628211ULL; };
        mix(static_cast<crd::u64>(g.op));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.a)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.b)));
        crd::u64 cb = 0;
        std::memcpy(&cb, &g.c, sizeof(cb));
        mix(cb);
        return h;
    }
    static bool same(const GNode& x, const GNode& y) noexcept
    {
        return x.op == y.op && x.a == y.a && x.b == y.b && x.c == y.c;
    }
    static int intern(crd::containers::Array<int>& table, int cap, crd::containers::Array<GNode>& nn, const GNode& g)
    {
        crd::u64 h = key_hash(g) & static_cast<crd::u64>(cap - 1);
        while (table[static_cast<crd::usize>(h)] >= 0)
        {
            const int id = table[static_cast<crd::usize>(h)];
            if (same(nn[static_cast<crd::usize>(id)], g)) { return id; }
            h = (h + 1) & static_cast<crd::u64>(cap - 1);
        }
        const int id = static_cast<int>(nn.size());
        nn.push_back(g);
        table[static_cast<crd::usize>(h)] = id;
        return id;
    }

    crd::containers::Array<GNode> m_nodes;
    int                           m_ninput = 0;
};

// tracing handle: build a Graph by running a scalar-generic functor on GExpr inputs.
struct GExpr
{
    Graph* g;
    int    id;
};
[[nodiscard]] inline GExpr gexpr_input(Graph& g) noexcept { return {&g, g.input()}; }

[[nodiscard]] inline GExpr operator+(GExpr a, GExpr b) noexcept { return {a.g, a.g->bin(GOp::Add, a.id, b.id)}; }
[[nodiscard]] inline GExpr operator-(GExpr a, GExpr b) noexcept { return {a.g, a.g->bin(GOp::Sub, a.id, b.id)}; }
[[nodiscard]] inline GExpr operator*(GExpr a, GExpr b) noexcept { return {a.g, a.g->bin(GOp::Mul, a.id, b.id)}; }
[[nodiscard]] inline GExpr operator/(GExpr a, GExpr b) noexcept { return {a.g, a.g->bin(GOp::Div, a.id, b.id)}; }
[[nodiscard]] inline GExpr operator-(GExpr a) noexcept { return {a.g, a.g->un(GOp::Neg, a.id)}; }
[[nodiscard]] inline GExpr operator+(GExpr a, crd::f64 c) noexcept { return {a.g, a.g->bin(GOp::Add, a.id, a.g->constant(c))}; }
[[nodiscard]] inline GExpr operator+(crd::f64 c, GExpr a) noexcept { return {a.g, a.g->bin(GOp::Add, a.g->constant(c), a.id)}; }
[[nodiscard]] inline GExpr operator-(GExpr a, crd::f64 c) noexcept { return {a.g, a.g->bin(GOp::Sub, a.id, a.g->constant(c))}; }
[[nodiscard]] inline GExpr operator-(crd::f64 c, GExpr a) noexcept { return {a.g, a.g->bin(GOp::Sub, a.g->constant(c), a.id)}; }
[[nodiscard]] inline GExpr operator*(GExpr a, crd::f64 c) noexcept { return {a.g, a.g->bin(GOp::Mul, a.id, a.g->constant(c))}; }
[[nodiscard]] inline GExpr operator*(crd::f64 c, GExpr a) noexcept { return {a.g, a.g->bin(GOp::Mul, a.g->constant(c), a.id)}; }
[[nodiscard]] inline GExpr operator/(GExpr a, crd::f64 c) noexcept { return {a.g, a.g->bin(GOp::Div, a.id, a.g->constant(c))}; }
[[nodiscard]] inline GExpr sin(GExpr a) noexcept { return {a.g, a.g->un(GOp::Sin, a.id)}; }
[[nodiscard]] inline GExpr cos(GExpr a) noexcept { return {a.g, a.g->un(GOp::Cos, a.id)}; }
[[nodiscard]] inline GExpr exp(GExpr a) noexcept { return {a.g, a.g->un(GOp::Exp, a.id)}; }
[[nodiscard]] inline GExpr log(GExpr a) noexcept { return {a.g, a.g->un(GOp::Log, a.id)}; }
[[nodiscard]] inline GExpr sqrt(GExpr a) noexcept { return {a.g, a.g->un(GOp::Sqrt, a.id)}; }
[[nodiscard]] inline GExpr tanh(GExpr a) noexcept { return {a.g, a.g->un(GOp::Tanh, a.id)}; }

// CODEGEN — emit a straight-line C++ kernel for the (optimised) graph: `crd_codegen_kernel(in, out, grad)`. The
// emitted transcendentals call crd::math (as the interpreter does) ⇒ the compiled kernel is BIT-IDENTICAL to eval().
// Constants print with %.17g (round-trips f64 exactly). Returns the number of chars the source needs (caller checks
// it is < cap; on overflow the buffer is truncated but the returned length still reflects the full size).
// append a printf-formatted chunk into buf at offset p (clamped), returning the new offset (variadic function, not a
// macro — tidy-clean).
inline int ga_append(char* buf, int cap, int p, const char* fmt, ...) noexcept
{
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(buf + (p < cap ? p : cap), static_cast<crd::usize>(p < cap ? cap - p : 0), fmt, ap);
    va_end(ap);
    return p + w;
}
inline int emit_cpp(const Graph& g, const int* out_nodes, int n_out, const int* grad_nodes, int n_grad, char* buf,
                    int cap) noexcept
{
    int p = 0;
    p     = ga_append(buf, cap, p, "#include <crd/math/cmath.hpp>\n");
    p     = ga_append(buf, cap, p, "extern \"C\" void crd_codegen_kernel(const double* in, double* out, double* grad) {\n");
    const int n = g.size();
    for (int i = 0; i < n; ++i)
    {
        const GNode& nd = g.node(i);
        switch (nd.op)
        {
        case GOp::Input: p = ga_append(buf, cap, p, "  const double n%d = in[%d];\n", i, static_cast<int>(nd.c)); break;
        case GOp::Const: p = ga_append(buf, cap, p, "  const double n%d = %.17g;\n", i, nd.c); break;
        case GOp::Add: p = ga_append(buf, cap, p, "  const double n%d = n%d + n%d;\n", i, nd.a, nd.b); break;
        case GOp::Sub: p = ga_append(buf, cap, p, "  const double n%d = n%d - n%d;\n", i, nd.a, nd.b); break;
        case GOp::Mul: p = ga_append(buf, cap, p, "  const double n%d = n%d * n%d;\n", i, nd.a, nd.b); break;
        case GOp::Div: p = ga_append(buf, cap, p, "  const double n%d = n%d / n%d;\n", i, nd.a, nd.b); break;
        case GOp::Neg: p = ga_append(buf, cap, p, "  const double n%d = -n%d;\n", i, nd.a); break;
        case GOp::Sin: p = ga_append(buf, cap, p, "  const double n%d = crd::math::sin(n%d);\n", i, nd.a); break;
        case GOp::Cos: p = ga_append(buf, cap, p, "  const double n%d = crd::math::cos(n%d);\n", i, nd.a); break;
        case GOp::Exp: p = ga_append(buf, cap, p, "  const double n%d = crd::math::exp(n%d);\n", i, nd.a); break;
        case GOp::Log: p = ga_append(buf, cap, p, "  const double n%d = crd::math::log(n%d);\n", i, nd.a); break;
        case GOp::Sqrt: p = ga_append(buf, cap, p, "  const double n%d = crd::math::sqrt(n%d);\n", i, nd.a); break;
        case GOp::Tanh: p = ga_append(buf, cap, p, "  const double n%d = crd::math::tanh(n%d);\n", i, nd.a); break;
        }
    }
    for (int k = 0; k < n_out; ++k) { p = ga_append(buf, cap, p, "  out[%d] = n%d;\n", k, out_nodes[k]); }
    for (int k = 0; k < n_grad; ++k) { p = ga_append(buf, cap, p, "  grad[%d] = n%d;\n", k, grad_nodes[k]); }
    p = ga_append(buf, cap, p, "}\n");
    return p;
}

} // namespace crd::hesap::autodiff::graph
