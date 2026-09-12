#pragma once

// ckir_module.hpp — GENERICS + MODULES (the CKIR authoring language layer). Today a reusable kernel component is a raw C++
// builder function — no registry, no call-site type checking, no first-class identity. `KModule` makes it a MODULE: a registry
// of named GENERIC graph functions (`KFn`). A `KFn` is a reusable subgraph builder, GENERIC over element dtype (bound from the
// arguments = monomorphization) and over shape (free — every CKIR op infers its shape from its operands). `KModule::call`
// INSTANTIATES a function by name into a graph, TYPE-CHECKED at the boundary (arity + uniform dtype). This turns CKIR from
// "build graphs in raw C++" into a composable, type-polymorphic authoring surface (Slang-class) — the foundation for a shared
// kernel standard library, the node editor's node types, and separate compilation of reusable components. Instantiations CSE
// through `optimize()`, so calling the same function with the same args costs nothing extra.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

// A generic graph function's body: build the subgraph into `g` from `args` (n_args input node ids), with element dtype `dtype`
// (monomorphized from the call). Returns the output node id, or -1 on an internal error. Shape is inferred by the ops it builds.
using KFnBody = int (*)(KGraph& g, const int* args, int n_args, DType dtype, void* user);

// A function's OUTPUT-SHAPE rule (GM-3): given the argument shapes + dtype, the output shape — so a first-class `call_node` knows
// its shape WITHOUT building the body (the body is inlined later by lower_calls). nullptr ⇒ shape-preserving (= arg[0]'s shape).
using KFnShapeRule = Shape (*)(const Shape* arg_shapes, int n_args, DType dtype);

// A registered generic function: its name, arity, whether the element dtype is generic (bound from arg[0]) or fixed, its body,
// and its output-shape rule (GM-3, for the first-class call-node).
struct KFn
{
    const char*  name          = nullptr;
    int          n_params      = 0;
    bool         generic_dtype = true; // true ⇒ element dtype T is bound from arg[0]'s dtype (monomorphization)
    KFnBody      body          = nullptr;
    KFnShapeRule shape_rule    = nullptr; // GM-3: output shape from arg shapes; nullptr = shape-preserving (arg[0])
    void*        user          = nullptr;
};

namespace module_detail
{
[[nodiscard]] inline bool name_eq(const char* a, const char* b) noexcept
{
    if (a == nullptr || b == nullptr) { return a == b; }
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}
} // namespace module_detail

// A module: a registry of named generic functions. `define` registers one; `call` instantiates one by name into a graph, TYPE-
// CHECKED (arity + uniform-dtype for generic functions). The whole point is that a consumer names a reusable component instead of
// re-authoring its subgraph, and the boundary is checked (a wrong arity / mixed dtype fails fast, returning -1, not miscompiling).
class KModule
{
public:
    explicit KModule(crd::memory::IAllocator* a) : m_fns(a) {}

    void define(const KFn& fn) { m_fns.push_back(fn); }

    [[nodiscard]] const KFn* find(const char* name) const
    {
        for (crd::usize i = 0; i < m_fns.size(); ++i)
        {
            if (module_detail::name_eq(m_fns[i].name, name)) { return &m_fns[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_fns.size()); }

    // Instantiate `name` into `g`. Boundary checks: the function exists, arity matches, every arg is a valid node, and (for a
    // generic function) every arg shares arg[0]'s element dtype — which is then the monomorphized `dtype` passed to the body.
    // Returns the output node id, or -1 on ANY failure (never a silently-wrong graph).
    [[nodiscard]] int call(KGraph& g, const char* name, const int* args, int n_args) const
    {
        const KFn* fn = find(name);
        if (fn == nullptr || fn->body == nullptr || n_args != fn->n_params) { return -1; }
        for (int i = 0; i < n_args; ++i) { if (args[i] < 0 || args[i] >= g.size()) { return -1; } }
        DType dtype = DType::F32;
        if (fn->generic_dtype && n_args > 0)
        {
            dtype = g.node(args[0]).dtype();
            for (int i = 1; i < n_args; ++i) { if (g.node(args[i]).dtype() != dtype) { return -1; } } // uniform-dtype constraint
        }
        return fn->body(g, args, n_args, dtype, fn->user);
    }

    // GM-3: build a FIRST-CLASS Call NODE for `name` (instead of inlining now). Output shape from the function's shape rule (or
    // arg[0] if none), dtype monomorphized — type-checked exactly like `call`. Returns the Call node id, or -1. The graph then
    // holds a serializable named-call (the node-editor node type); `lower_calls` inlines it before optimize/emit.
    [[nodiscard]] int call_node(KGraph& g, const char* name, const int* args, int n_args) const
    {
        const int idx = index_of(name);
        if (idx < 0 || m_fns[static_cast<crd::usize>(idx)].body == nullptr) { return -1; }
        const KFn& fn = m_fns[static_cast<crd::usize>(idx)];
        if (n_args != fn.n_params || n_args > 16) { return -1; }
        for (int i = 0; i < n_args; ++i) { if (args[i] < 0 || args[i] >= g.size()) { return -1; } }
        DType dtype = DType::F32;
        if (fn.generic_dtype && n_args > 0)
        {
            dtype = g.node(args[0]).dtype();
            for (int i = 1; i < n_args; ++i) { if (g.node(args[i]).dtype() != dtype) { return -1; } }
        }
        Shape ashapes[16];
        for (int i = 0; i < n_args; ++i) { ashapes[i] = g.node(args[i]).shape; }
        Shape osh; // default = empty; shape-preserving fallback uses arg[0] when there is no rule
        if (fn.shape_rule != nullptr) { osh = fn.shape_rule(ashapes, n_args, dtype); }
        else if (n_args > 0) { osh = ashapes[0]; }
        return g.call_node(idx, args, n_args, osh, dtype);
    }

    // GM-3: LOWER every Call node into its function's body (inline), splicing the body output in place of the Call. Run BEFORE
    // optimize/emit — the emitters + oracle never see a Call. Updates `roots` if a root itself was a Call. Returns the count.
    int lower_calls(KGraph& g, int* roots, int n_roots) const
    {
        int calls[512];
        int nc = 0;
        for (int i = 0; i < g.size() && nc < 512; ++i) { if (g.node(i).op == KOp::Call) { calls[nc++] = i; } }
        for (int c = 0; c < nc; ++c)
        {
            const int    cn  = calls[c];
            const KNode& node = g.node(cn);
            const int    idx = node.iidx;
            if (idx < 0 || idx >= count()) { continue; }
            int       args[16];
            const int na = static_cast<int>(node.n_ext) < 16 ? static_cast<int>(node.n_ext) : 16;
            for (int k = 0; k < na; ++k) { args[k] = g.ext_operand(node, k); }
            const int out = m_fns[static_cast<crd::usize>(idx)].body(g, args, na, node.dtype(), m_fns[static_cast<crd::usize>(idx)].user);
            if (out < 0) { continue; }
            g.redirect(cn, out); // splice the inlined body in place of the Call for all downstream consumers
            for (int r = 0; r < n_roots; ++r) { if (roots[r] == cn) { roots[r] = out; } }
        }
        return nc;
    }

private:
    [[nodiscard]] int index_of(const char* name) const
    {
        for (crd::usize i = 0; i < m_fns.size(); ++i) { if (module_detail::name_eq(m_fns[i].name, name)) { return static_cast<int>(i); } }
        return -1;
    }
    crd::containers::Array<KFn> m_fns;
};

// ── a small STANDARD LIBRARY of generic activation functions — the first module (proves reuse across shape + dtype) ───────────
namespace stdlib
{
// SiLU/swish: x·σ(x) = x / (1 + exp(-x)). Generic over shape (from x) + dtype.
[[nodiscard]] inline int fn_silu(KGraph& g, const int* args, int /*n*/, DType t, void* /*u*/)
{
    const int   x   = args[0];
    const Shape sh  = g.node(x).shape;
    const int   one = g.constant(1.0, sh, t);
    const int   ex  = g.unary(KOp::Exp, g.unary(KOp::Neg, x));
    const int   sig = g.unary(KOp::Recip, g.binary(KOp::Add, one, ex));
    return g.binary(KOp::Mul, x, sig);
}
// Softplus: log(1 + exp(x)).
[[nodiscard]] inline int fn_softplus(KGraph& g, const int* args, int /*n*/, DType t, void* /*u*/)
{
    const int   x   = args[0];
    const Shape sh  = g.node(x).shape;
    const int   one = g.constant(1.0, sh, t);
    return g.unary(KOp::Log, g.binary(KOp::Add, one, g.unary(KOp::Exp, x)));
}
// tanh-approx GELU: 0.5·x·(1 + tanh(0.7978845608·(x + 0.044715·x³))).
[[nodiscard]] inline int fn_gelu(KGraph& g, const int* args, int /*n*/, DType t, void* /*u*/)
{
    const int   x    = args[0];
    const Shape sh   = g.node(x).shape;
    const int   x3   = g.binary(KOp::Mul, x, g.binary(KOp::Mul, x, x));
    const int   c044 = g.constant(0.044715, sh, t);
    const int   inner = g.binary(KOp::Add, x, g.binary(KOp::Mul, c044, x3));
    const int   csq   = g.constant(0.7978845608028654, sh, t);
    const int   th    = g.unary(KOp::Tanh, g.binary(KOp::Mul, csq, inner));
    const int   one   = g.constant(1.0, sh, t);
    const int   half  = g.constant(0.5, sh, t);
    return g.binary(KOp::Mul, g.binary(KOp::Mul, half, x), g.binary(KOp::Add, one, th));
}

// Linear (dense) layer: x·W + b — a MULTI-ARGUMENT generic (3 params, uniform dtype checked at the call). x[M,K]·W[K,N] + b[N]
// broadcast to [M,N]. The contract IS the autotuner's GEMM (select_schedule tunes it) — so a module-authored layer inherits the
// vendor-crushing schedule for free. This is the payoff: real neural layers composed from NAMED, type-checked, reusable pieces.
[[nodiscard]] inline int fn_linear(KGraph& g, const int* args, int /*n*/, DType /*t*/, void* /*u*/)
{
    const int   c   = g.contract(args[0], args[1]);      // x · W  (the GEMM the AS autotuner schedules) → [M,N]
    const Shape cs  = g.node(c).shape;
    const int   ncol = static_cast<int>(cs.dims[cs.rank - 1]);
    // bias is per OUTPUT COLUMN: reshape [N]→[1,N] so the broadcast aligns the last axis (per-column), NOT [N]→[M] (per-row —
    // CKIR's 1-D broadcast aligns the FIRST axis, which for a square M==N would silently give a per-ROW bias).
    const int brow = g.reshape(args[2], make_shape({1, ncol}));
    const int bc   = g.broadcast(brow, cs);
    return g.binary(KOp::Add, c, bc);
}
// linear's output shape (GM-3, for the first-class call-node): x[M,K] · W[K,N] → [M,N].
[[nodiscard]] inline Shape linear_shape(const Shape* a, int /*n*/, DType /*t*/)
{
    return make_shape({static_cast<int>(a[0].dims[0]), static_cast<int>(a[1].dims[a[1].rank - 1])});
}

// Register the standard library into `m` (the first CKIR module): activations (shape-preserving) + the linear layer (shaped).
inline void register_activations(KModule& m)
{
    m.define(KFn{"silu", 1, true, &fn_silu, nullptr, nullptr});
    m.define(KFn{"softplus", 1, true, &fn_softplus, nullptr, nullptr});
    m.define(KFn{"gelu", 1, true, &fn_gelu, nullptr, nullptr});
    m.define(KFn{"linear", 3, true, &fn_linear, &linear_shape, nullptr});
}

// ── DTYPE-GENERIC NUMERIC stdlib (GM-4) — generics that hold over INTEGER + UNSIGNED + FLOAT element types, not just float ───────
// muladd(a,b,c) = a·b + c. Mul+Add are valid for every element dtype, and on a 32-bit INTEGER monomorphization they WRAP mod 2^32
// exactly as every GPU backend does (the oracle's apply_binary_typed wrap path). The SAME body builds a correct i32/u32/f32/f64
// subgraph — proving KModule generics extend beyond the float-only activations to true dtype polymorphism.
[[nodiscard]] inline int fn_muladd(KGraph& g, const int* args, int /*n*/, DType /*t*/, void* /*u*/)
{
    return g.binary(KOp::Add, g.binary(KOp::Mul, args[0], args[1]), args[2]);
}
// clamp(x,lo,hi) = min(max(x,lo),hi) — dtype-generic (KOp::Clamp; Min/Max only COMPARE, never overflow, so it is exact for
// integer AND float element types).
[[nodiscard]] inline int fn_clamp(KGraph& g, const int* args, int /*n*/, DType /*t*/, void* /*u*/)
{
    return g.ternary(KOp::Clamp, args[0], args[1], args[2]);
}

// Register the DTYPE-GENERIC numeric group (int + uint + float): muladd, clamp. Kept SEPARATE from register_activations (which is
// float-only transcendentals) so a consumer picks the group it needs — and the activations' registered count stays stable.
inline void register_numerics(KModule& m)
{
    m.define(KFn{"muladd", 3, true, &fn_muladd, nullptr, nullptr});
    m.define(KFn{"clamp", 3, true, &fn_clamp, nullptr, nullptr});
}

// ── SEPARATE COMPILATION / intra-module LINKAGE (GM-5) — a module function COMPOSED from OTHER module functions, resolved by NAME ─
// ffn(x,W,b) = gelu(linear(x,W,b)) — but authored as a module function that CALLS "linear" then "gelu" by name (late binding via
// the module handle in `user`), NOT by hand-inlining their subgraphs. The callees are resolved at INSTANTIATION, so `linear`/`gelu`
// can be defined/compiled independently of `ffn` (the essence of separate compilation) — and a consumer names `ffn` without seeing
// its internals. NOTE: the module is captured by POINTER (`user`), so a composite fn must not outlive nor be used after the module
// is moved; register composite fns with `&m` on the SAME module they will run against (see register_stdlib).
[[nodiscard]] inline int fn_ffn(KGraph& g, const int* args, int /*n*/, DType /*t*/, void* user)
{
    const KModule* m = static_cast<const KModule*>(user);
    if (m == nullptr) { return -1; }
    const int h = m->call(g, "linear", args, 3); // intra-module call — LINKED by name, not inlined by the author
    if (h < 0) { return -1; }
    const int hy[1] = {h};
    return m->call(g, "gelu", hy, 1); // compose the activation, again by name
}

// Register the WHOLE stdlib (activations + numerics + the composite `ffn` that LINKS them). `ffn` captures `&m` as its module
// handle, so it must be registered on — and run against — this same module object. Returns having defined 7 functions.
inline void register_stdlib(KModule& m)
{
    register_activations(m); // silu, softplus, gelu, linear
    register_numerics(m);    // muladd, clamp
    m.define(KFn{"ffn", 3, true, &fn_ffn, &linear_shape, &m}); // composite: gelu∘linear, output shaped like linear ([M,N])
}

// ── PRODUCTION NEURAL BLOCKS (GM-6) — real transformer components authored as composable module functions. No new KOps: every
// block is pure COMPOSITION of existing CKIR ops (reductions, broadcast, exp/sqrt/rsqrt, contract, permute) — so all five backends
// already emit them, and the CPU oracle already certifies them. This proves the authoring language expresses production neural nets.

// LayerNorm over the LAST axis: y = (x − μ)/√(σ²+ε)·γ + β, with μ,σ² per row. γ,β are [N] per-COLUMN ⇒ reshape [N]→[1,N] before
// broadcast (the 1-D-broadcast-is-per-row scar). Shape-preserving.
[[nodiscard]] inline int fn_layernorm(KGraph& g, const int* args, int /*n*/, DType t, void* /*u*/)
{
    const int      x    = args[0];
    const Shape    sh   = g.node(x).shape;                       // [..., N]
    const int      last = sh.rank - 1;
    const crd::u32 mask = 1U << static_cast<crd::u32>(last);     // reduce the last axis (keepdims ⇒ [..., 1])
    const int      ncol = static_cast<int>(sh.dims[last]);
    const int      sum  = g.reduce(KOp::ReduceSum, x, mask);     // Σx        → [..., 1]
    const Shape    rsh  = g.node(sum).shape;
    const int      invn = g.constant(1.0 / static_cast<crd::f64>(ncol), rsh, t);
    const int      mean = g.binary(KOp::Mul, sum, invn);         // μ         → [..., 1]
    const int      cen  = g.binary(KOp::Sub, x, g.broadcast(mean, sh)); // x − μ
    const int      vsum = g.reduce(KOp::ReduceSum, g.binary(KOp::Mul, cen, cen), mask); // Σ(x−μ)²
    const int      var  = g.binary(KOp::Mul, vsum, invn);        // σ²        → [..., 1]
    const int      eps  = g.constant(1e-5, rsh, t);
    const int      inv  = g.unary(KOp::Rsqrt, g.binary(KOp::Add, var, eps)); // 1/√(σ²+ε)
    const int      norm = g.binary(KOp::Mul, cen, g.broadcast(inv, sh));     // normalized
    const Shape    col  = make_shape({1, ncol});                 // [N] → [1,N] ⇒ per-column broadcast (last-axis aligned)
    const int      gcol = g.broadcast(g.reshape(args[1], col), sh);
    const int      bcol = g.broadcast(g.reshape(args[2], col), sh);
    return g.binary(KOp::Add, g.binary(KOp::Mul, norm, gcol), bcol);
}

// Softmax over the LAST axis: numerically-stable (subtract the row max) exp / Σexp. Shape-preserving.
[[nodiscard]] inline int fn_softmax(KGraph& g, const int* args, int /*n*/, DType /*t*/, void* /*u*/)
{
    const int      x    = args[0];
    const Shape    sh   = g.node(x).shape;
    const crd::u32 mask = 1U << static_cast<crd::u32>(sh.rank - 1);
    const int      mx   = g.reduce(KOp::ReduceMax, x, mask);                        // row max (keepdims)
    const int      e    = g.unary(KOp::Exp, g.binary(KOp::Sub, x, g.broadcast(mx, sh)));
    const int      s    = g.reduce(KOp::ReduceSum, e, mask);                        // Σexp
    return g.binary(KOp::Div, e, g.broadcast(s, sh));
}

// Scaled dot-product attention: softmax(q·kᵀ/√d)·v. q,k,v are [S,D]; scores [S,S]; output [S,D] (shape-preserving vs q). The
// softmax is LINKED by name (GM-5) — attention COMPOSES the softmax block, it does not re-author it.
[[nodiscard]] inline int fn_attention(KGraph& g, const int* args, int /*n*/, DType t, void* user)
{
    const KModule* mod = static_cast<const KModule*>(user);
    if (mod == nullptr) { return -1; }
    const int      q  = args[0];
    const int      k  = args[1];
    const int      v  = args[2];
    const Shape    qs = g.node(q).shape;                          // [S, D]
    const int      dim = static_cast<int>(qs.dims[qs.rank - 1]);
    const crd::u8  perm[2] = {1, 0};
    const int      kt   = g.permute(k, perm);                     // kᵀ → [D, S]
    const int      sc   = g.contract(q, kt);                      // q·kᵀ → [S, S]
    const int      scal = g.constant(1.0 / crd::math::sqrt(static_cast<crd::f64>(dim)), g.node(sc).shape, t);
    const int      sn   = g.binary(KOp::Mul, sc, scal);           // scaled scores
    const int      sm   = mod->call(g, "softmax", &sn, 1);        // LINKED softmax over the last axis → [S, S]
    if (sm < 0) { return -1; }
    return g.contract(sm, v);                                     // ·v → [S, D]
}

// A pre-norm TRANSFORMER BLOCK — the payoff: self-attention + FFN with two residuals, authored ENTIRELY as name-linked calls to
// layernorm / attention / linear / gelu. args = x, ln1_γ, ln1_β, ln2_γ, ln2_β, W1, b1, W2, b2 (9). Self-attention uses the normed
// input directly for q=k=v (separate Q/K/V/output projections are just more `linear` calls, already demonstrated). Shape-preserving
// (both residuals require the block to map [S,N] → [S,N], so W1 is [N,H], W2 is [H,N]).
[[nodiscard]] inline int fn_transformer_block(KGraph& g, const int* args, int /*n*/, DType /*t*/, void* user)
{
    const KModule* mod = static_cast<const KModule*>(user);
    if (mod == nullptr) { return -1; }
    const int x = args[0];
    // sub-block 1: h = x + attention(layernorm(x))   (pre-norm self-attention + residual)
    const int ln1_args[3] = {x, args[1], args[2]};
    const int a1 = mod->call(g, "layernorm", ln1_args, 3);
    if (a1 < 0) { return -1; }
    const int attn_args[3] = {a1, a1, a1};                        // self-attention: q = k = v = the normed input
    const int at = mod->call(g, "attention", attn_args, 3);
    if (at < 0) { return -1; }
    const int h = g.binary(KOp::Add, x, at);                      // residual 1
    // sub-block 2: out = h + linear(gelu(linear(layernorm(h))))   (pre-norm FFN + residual)
    const int ln2_args[3] = {h, args[3], args[4]};
    const int a2 = mod->call(g, "layernorm", ln2_args, 3);
    if (a2 < 0) { return -1; }
    const int lin1_args[3] = {a2, args[5], args[6]};
    const int f1 = mod->call(g, "linear", lin1_args, 3);          // [S,N]·[N,H] + [H] → [S,H]
    if (f1 < 0) { return -1; }
    const int gf = mod->call(g, "gelu", &f1, 1);
    if (gf < 0) { return -1; }
    const int lin2_args[3] = {gf, args[7], args[8]};
    const int f2 = mod->call(g, "linear", lin2_args, 3);          // [S,H]·[H,N] + [N] → [S,N]
    if (f2 < 0) { return -1; }
    return g.binary(KOp::Add, h, f2);                             // residual 2
}

// Register the PRODUCTION NEURAL group. layernorm/softmax are leaf blocks; attention LINKS softmax, transformer_block LINKS
// layernorm/attention/linear/gelu — so all capture `&m` and must run against this same module (which must ALSO hold the stdlib
// linear+gelu, i.e. call register_stdlib(m) first). Defines 4 functions.
inline void register_neural(KModule& m)
{
    m.define(KFn{"layernorm", 3, true, &fn_layernorm, nullptr, nullptr});
    m.define(KFn{"softmax", 1, true, &fn_softmax, nullptr, nullptr});
    m.define(KFn{"attention", 3, true, &fn_attention, nullptr, &m});
    m.define(KFn{"transformer_block", 9, true, &fn_transformer_block, nullptr, &m});
}
} // namespace stdlib

} // namespace crd::kir
