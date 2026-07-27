#pragma once

// ckir_shape.hpp — REN-38 hygiene: the SHAPE CHECKER `nodes::detail::bin` had always deferred to — and which
// did not exist. The scar that forced it (38-E7): `lighting::pcf_shadow` takes a vec2 uv, every atlas is
// LAYERED, and passing a vec3 landed on `detail::bin`'s "two mismatched vectors — a caller error" arm, which
// BUILDS the invalid node anyway. The cook returned a valid node id and the SHADER failed to compile, with
// nothing pointing at the uv width. Fixing the caller fixed one caller; this closes the CLASS.
//
// ⛔ THE RULES MIRROR `ckir_eval.hpp`'s ORACLE EXACTLY, not GLSL. The oracle reads `a[e]` for e in
// [0, node.comps) of EVERY elementwise operand, so a narrower operand is an OUT-OF-BOUNDS READ there — while
// GLSL silently broadcasts a scalar. Two backends disagreeing about the same graph is precisely the
// portability bug class this engine exists to kill (`feedback_ckir_binary_vec_scalar_shape_mismatch`), which
// is why the elementwise rule is STRICT WIDTH EQUALITY: broadcast in CKIR is explicit (`splat`), never
// implied by an operand pair.
//
// Scope: the DEFINITE rules — the ones no valid graph can break. Ops whose shape semantics are richer than a
// width equation (tensor movement, reductions, Contract, aggregates) are deliberately not second-guessed
// here; they carry their own shape computation in the builders and their own oracle coverage.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp> // eval_detail::is_unary/is_binary/is_ternary — ONE classification, no drift

#include <crd/containers/array.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::kir
{

// The offending node and a static reason. `node` is the graph node id — the cookers surface it in `where`.
struct ShapeIssue
{
    int         node = -1;
    const char* why  = nullptr;
};

namespace shape_detail
{

// How many coordinate components a sample of this texture takes: spatial dims (cube = a vec3 direction) plus
// one array layer when the image is arrayed. This is the rule the layered-atlas scar violated.
[[nodiscard]] inline int coord_comps(const KType& tex) noexcept
{
    int c = 2;
    switch (tex.tex_dim())
    {
    case TexDim::Tex1D: c = 1; break;
    case TexDim::Tex2D: c = 2; break;
    case TexDim::Tex3D: c = 3; break;
    case TexDim::TexCube: c = 3; break;
    }
    return c + (tex.tex_arrayed() ? 1 : 0);
}

[[nodiscard]] inline bool value_form(const KType& t) noexcept
{
    return t.kind == TKind::Scalar || t.kind == TKind::Vec || t.kind == TKind::Mat;
}

// The per-node rule table. Returns true when `id`'s operands satisfy the op's definite shape contract.
[[nodiscard]] inline bool node_shape_ok(const KGraph& g, int id, const char** why) // NOLINT(readability-function-cognitive-complexity) — a rule table; splitting it hides the table
{
    const auto fail = [&](const char* reason) {
        if (why != nullptr) { *why = reason; }
        return false;
    };
    const KNode& n  = g.node(id);
    const int    sz = g.size();
    const auto   ok_id = [&](int x) { return x >= 0 && x < sz; };
    const auto   comps = [&](int x) { return g.node(x).type.comps(); };
    const auto   kind  = [&](int x) { return g.node(x).type.kind; };

    using eval_detail::is_binary;
    using eval_detail::is_ternary;
    using eval_detail::is_unary;

    if (is_unary(n.op))
    {
        if (!ok_id(n.a)) { return fail("unary op names no operand"); }
        if (!value_form(g.node(n.a).type)) { return fail("unary operand is not a value (a texture/sampler wired into arithmetic)"); }
        if (comps(n.a) != n.type.comps()) { return fail("unary operand width differs from the node's"); }
        return true;
    }
    if (is_binary(n.op))
    {
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("binary op names a missing operand"); }
        if (!value_form(g.node(n.a).type) || !value_form(g.node(n.b).type))
        {
            return fail("binary operand is not a value (a texture/sampler wired into arithmetic)");
        }
        // ⛔ STRICT equality — the `detail::bin` mismatched-vector arm. The GPU broadcasts, the oracle reads
        // out of bounds; broadcast in CKIR is explicit (`splat`), never implied.
        if (comps(n.a) != n.type.comps() || comps(n.b) != n.type.comps())
        {
            return fail("binary operand widths differ (broadcast is explicit in CKIR — splat first)");
        }
        return true;
    }
    if (is_ternary(n.op))
    {
        if (!ok_id(n.a) || !ok_id(n.b) || !ok_id(n.c)) { return fail("ternary op names a missing operand"); }
        if (!value_form(g.node(n.a).type) || !value_form(g.node(n.b).type) || !value_form(g.node(n.c).type))
        {
            return fail("ternary operand is not a value (a texture/sampler wired into arithmetic)");
        }
        if (comps(n.a) != n.type.comps() || comps(n.b) != n.type.comps() || comps(n.c) != n.type.comps())
        {
            return fail("ternary operand widths differ (broadcast is explicit in CKIR — splat first)");
        }
        return true;
    }

    switch (n.op)
    {
    case KOp::Select:
        if (!ok_id(n.a) || !ok_id(n.b) || !ok_id(n.c)) { return fail("select names a missing operand"); }
        // The oracle reads ONE cond per element and `comps` lanes of each arm — a vector cond or a narrower
        // arm reads memory the node does not own.
        if (comps(n.c) != 1) { return fail("select condition must be scalar (one cond per element)"); }
        if (comps(n.a) != n.type.comps() || comps(n.b) != n.type.comps())
        {
            return fail("select arms must match the node's width");
        }
        return true;
    case KOp::Vec2:
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("vec2 names a missing operand"); }
        if (comps(n.a) != 1 || comps(n.b) != 1) { return fail("vec2 operands must be scalars"); }
        return true;
    case KOp::Vec3:
        if (!ok_id(n.a) || !ok_id(n.b) || !ok_id(n.c)) { return fail("vec3 names a missing operand"); }
        if (comps(n.a) != 1 || comps(n.b) != 1 || comps(n.c) != 1) { return fail("vec3 operands must be scalars"); }
        return true;
    case KOp::VecComp:
        if (!ok_id(n.a)) { return fail("component extract names no operand"); }
        // The C2 attribute scar: a node id lands in a compile-time index slot, type-checks as `int`, and
        // "swizzles component 47". The bound is the operand's actual width.
        if (n.iidx < 0 || n.iidx >= comps(n.a)) { return fail("component index outside the operand's width"); }
        return true;
    case KOp::Swizzle:
    {
        if (!ok_id(n.a)) { return fail("swizzle names no operand"); }
        const int w = n.type.comps();
        for (int k = 0; k < w; ++k)
        {
            if (static_cast<int>(n.perm[k]) >= comps(n.a)) { return fail("swizzle lane outside the operand's width"); }
        }
        return true;
    }
    case KOp::VecConcat:
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("concat names a missing operand"); }
        if (comps(n.a) + comps(n.b) != n.type.comps()) { return fail("concat width is not the sum of its operands"); }
        if (n.type.comps() > 4) { return fail("concat exceeds four components"); }
        return true;
    case KOp::Dot:
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("dot names a missing operand"); }
        if (comps(n.a) != comps(n.b)) { return fail("dot operand widths differ"); }
        return true;
    case KOp::Cross:
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("cross names a missing operand"); }
        if (comps(n.a) != 3 || comps(n.b) != 3) { return fail("cross operands must be vec3"); }
        return true;
    case KOp::Reflect:
    case KOp::Faceforward:
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("geometric op names a missing operand"); }
        if (comps(n.a) != n.type.comps() || comps(n.b) != n.type.comps())
        {
            return fail("geometric operand widths differ");
        }
        return true;
    case KOp::Refract:
        if (!ok_id(n.a) || !ok_id(n.b) || !ok_id(n.c)) { return fail("refract names a missing operand"); }
        if (comps(n.a) != n.type.comps() || comps(n.b) != n.type.comps()) { return fail("refract vector widths differ"); }
        if (comps(n.c) != 1) { return fail("refract eta must be scalar"); }
        return true;
    case KOp::MatVecMul:
        if (!ok_id(n.a) || !ok_id(n.b)) { return fail("mat*vec names a missing operand"); }
        if (kind(n.a) != TKind::Mat) { return fail("mat*vec left operand is not a matrix"); }
        if (comps(n.b) != static_cast<int>(g.node(n.a).type.cols)) { return fail("mat*vec vector width differs from the matrix columns"); }
        return true;
    case KOp::Splat:
        if (!ok_id(n.a)) { return fail("splat names no operand"); }
        if (comps(n.a) != 1) { return fail("splat takes a scalar"); }
        return true;
    // ── The sample family. `a` = texture, `b` = sampler, `c` = uv. The uv width rule is the layered-atlas
    // scar; the sampler-pairing rule is the `Hard`-filter scar (`tex_sample` where `tex_sample_cmp` belonged
    // compiles and renders a comparison against nothing an author wrote).
    case KOp::TexSample:
    case KOp::SampleLod:
    case KOp::SampleGrad:
    case KOp::TexGather:
        if (!ok_id(n.a) || !ok_id(n.b) || !ok_id(n.c)) { return fail("texture sample names a missing operand"); }
        if (!g.node(n.a).type.is_texture()) { return fail("sample source is not a texture"); }
        if (!g.node(n.b).type.is_sampler()) { return fail("sample sampler operand is not a sampler"); }
        if (g.node(n.b).type.tex_shadow()) { return fail("a comparison sampler requires tex_sample_cmp"); }
        if (comps(n.c) != coord_comps(g.node(n.a).type)) { return fail("sample uv width differs from the texture's coordinate count"); }
        return true;
    case KOp::SampleCmp:
        if (!ok_id(n.a) || !ok_id(n.b) || !ok_id(n.c) || !ok_id(n.d)) { return fail("compare sample names a missing operand"); }
        if (!g.node(n.a).type.is_texture()) { return fail("compare-sample source is not a texture"); }
        if (!g.node(n.b).type.is_sampler()) { return fail("compare-sample sampler operand is not a sampler"); }
        if (!g.node(n.b).type.tex_shadow()) { return fail("tex_sample_cmp requires a comparison sampler"); }
        if (comps(n.c) != coord_comps(g.node(n.a).type)) { return fail("compare-sample uv width differs from the texture's coordinate count"); }
        if (comps(n.d) != 1) { return fail("compare-sample reference must be scalar"); }
        return true;
    default:
        return true; // ops with richer shape semantics own their rules in their builders + oracle coverage
    }
}

} // namespace shape_detail

// Check every node REACHABLE from `root`. Reachability is the point: a dead mis-built node is DCE'd by
// lowering and harms nothing (38-D3's unwired-control gate depends on exactly that), so only what the result
// actually computes is judged.
[[nodiscard]] inline bool graph_shapes_valid(const KGraph& g, int root, crd::memory::IAllocator* alloc,
                                             ShapeIssue* issue = nullptr)
{
    if (root < 0 || root >= g.size())
    {
        if (issue != nullptr) { issue->node = root; issue->why = "root names no node"; }
        return false;
    }
    crd::containers::Array<crd::u8> seen(alloc);
    seen.resize(static_cast<crd::usize>(g.size()));
    for (crd::usize i = 0; i < seen.size(); ++i) { seen[i] = 0U; }
    crd::containers::Array<int> stack(alloc);
    stack.push_back(root);
    while (!stack.empty())
    {
        const int id = stack[stack.size() - 1U];
        stack.pop_back();
        if (seen[static_cast<crd::usize>(id)] != 0U) { continue; }
        seen[static_cast<crd::usize>(id)] = 1U;

        const char* why = nullptr;
        if (!shape_detail::node_shape_ok(g, id, &why))
        {
            if (issue != nullptr) { issue->node = id; issue->why = why; }
            return false;
        }
        const KNode& n = g.node(id);
        const int    ops[4] = {n.a, n.b, n.c, n.d};
        for (const int o : ops)
        {
            if (o >= 0 && o < g.size()) { stack.push_back(o); }
        }
        for (int k = 0; k < static_cast<int>(n.n_ext); ++k)
        {
            const int o = g.ext_operand(n, k);
            if (o >= 0 && o < g.size()) { stack.push_back(o); }
        }
    }
    return true;
}

// Check every node an ENTRY can reach: the stage outputs, the position/depth/discard/rate roots, and the
// statement body (stores, conditions, loop counts, ray scalars) — the exact set the emitters lower.
[[nodiscard]] inline bool entry_shapes_valid(const KGraph& g, const KEntry& e, crd::memory::IAllocator* alloc,
                                             ShapeIssue* issue = nullptr)
{
    const auto check_root = [&](int root) { return root < 0 || graph_shapes_valid(g, root, alloc, issue); };

    if (!check_root(e.position) || !check_root(e.frag_depth) || !check_root(e.discard_cond)
        || !check_root(e.shading_rate) || !check_root(e.storage_write_index) || !check_root(e.storage_write_value)
        || !check_root(e.mesh_prim) || !check_root(e.task_emit) || !check_root(e.tess_inner)
        || !check_root(e.tess_outer))
    {
        return false;
    }
    for (int i = 0; i < e.n_out; ++i)
    {
        if (!check_root(e.out[i].node)) { return false; }
    }
    for (crd::u32 i = 0; i < e.n_task_payload; ++i)
    {
        if (!check_root(e.task_payload[i])) { return false; }
    }
    // The statement body. Nested For/If bodies are ranges into the same pool, so a worklist of ranges walks
    // the whole tree without recursion.
    if (e.kernel_body_count > 0)
    {
        crd::containers::Array<int> ranges(alloc); // pairs pushed as (begin, count)
        ranges.push_back(e.kernel_body_begin);
        ranges.push_back(e.kernel_body_count);
        while (!ranges.empty())
        {
            const int count = ranges[ranges.size() - 1U];
            ranges.pop_back();
            const int begin = ranges[ranges.size() - 1U];
            ranges.pop_back();
            for (int s = begin; s < begin + count && s < g.stmt_count(); ++s)
            {
                const KStmt& st = g.stmt(s);
                if (!check_root(st.target) || !check_root(st.index) || !check_root(st.value)
                    || !check_root(st.result))
                {
                    return false;
                }
                for (int k = 0; k < static_cast<int>(st.n_ext); ++k)
                {
                    if (!check_root(g.stmt_ext_operand(st, k))) { return false; }
                }
                if (st.body_count > 0)
                {
                    ranges.push_back(st.body_begin);
                    ranges.push_back(st.body_count);
                }
            }
        }
    }
    return true;
}

} // namespace crd::kir
