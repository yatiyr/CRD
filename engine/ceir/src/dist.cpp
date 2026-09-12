#include <crd/ceir/dist.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::dist
{
namespace
{
using containers::StringView;

// A type is Tensor-kinded (the 3d Tensor TypeKind).
[[nodiscard]] bool is_tensor_type(const Context& ctx, TypeId t) noexcept { return ctx.type_of(t).kind == TypeKind::Tensor; }

// A Tensor's RANK = the member count of its shape type (members[1]); 0 if malformed (a Tensor is [element, shape]).
[[nodiscard]] usize tensor_rank(const Context& ctx, TypeId tensor_type) noexcept
{
    const Type tt = ctx.type_of(tensor_type);
    if (tt.members.size() < 2U) { return 0U; }
    return ctx.type_of(tt.members[1]).members.size();
}

// Parse the mesh `shape`: comma-separated POSITIVE ints. Fills `count` (the mesh RANK); false on a bad token, a non-positive
// dim, or an empty list (a mesh has >=1 axis). The tensor.cpp parse_int_list precedent, tightened to > 0. `count` is 0 on false.
[[nodiscard]] bool parse_mesh_shape(StringView s, u32& count) noexcept
{
    count = 0U;
    if (s.size() == 0U) { return false; } // a mesh needs >=1 axis
    usize start = 0U;
    for (usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            if (i == start) { count = 0U; return false; } // empty field (leading/trailing/double comma)
            i64 v = 0;
            for (usize j = start; j < i; ++j)
            {
                const char c = s[j];
                if (c < '0' || c > '9') { count = 0U; return false; }
                v = v * 10 + (c - '0');
            }
            if (v <= 0) { count = 0U; return false; } // a mesh dim is POSITIVE
            ++count;
            start = i + 1U;
        }
    }
    return count >= 1U;
}

// The collective `fn` vocab: tensor.reduce's {sum,prod,max,min} MINUS mean (mean needs a materialization post-scale — the
// 30a-3 named-forward; a deliberate subset, NOT tensor.cpp's anon-namespace fn_in which is unreachable from here).
[[nodiscard]] bool collective_fn_in(StringView s) noexcept
{
    return s == StringView("sum") || s == StringView("prod") || s == StringView("max") || s == StringView("min");
}

// A Symbol attr's name; {} (empty) if the attr is absent or not a SymbolRef.
[[nodiscard]] StringView symbol_name(const Context& ctx, AttrId a) noexcept
{
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::SymbolRef ? v.s : StringView();
}

// The FIRST dist.mesh (pre-order, region-recursive) whose `name` == `name`; nullptr if none. ⛔ I6 — op NAME.
const Operation* find_mesh(const Context& ctx, const Region* r, StringView name) // NOLINT(misc-no-recursion)
{
    if (r == nullptr || name.size() == 0U) { return nullptr; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == StringView("dist.mesh") && symbol_name(ctx, op->attr("name")) == name)
            {
                return op;
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const Operation* const m = find_mesh(ctx, op->region(i), name);
                if (m != nullptr) { return m; }
            }
        }
    }
    return nullptr;
}

// Per-op semantic checks (NOT recursive). `root` = the module body — mesh resolution scans the WHOLE module (forward refs legal).
DistMisuse check_op(const Context& ctx, const Region* root, const Operation* op)
{
    const StringView nm = ctx.op_name(op->kind());
    if (nm == StringView("dist.mesh"))
    {
        const AttrValue shp = ctx.attr_value(op->attr("shape"));
        u32             rank = 0U;
        if (shp.kind != AttrKind::String || !parse_mesh_shape(shp.s, rank))
        {
            return {nullptr, op, DistMisuseKind::MeshShapeInvalid};
        }
        const StringView nml = symbol_name(ctx, op->attr("name"));
        if (nml.size() != 0U && find_mesh(ctx, root, nml) != op) // an EARLIER mesh owns this name ⇒ this is the duplicate
        {
            return {nullptr, op, DistMisuseKind::DuplicateMesh};
        }
        return {};
    }
    const bool is_shard = nm == StringView("dist.shard");
    const bool is_ar    = nm == StringView("dist.all_reduce");
    if (!is_shard && !is_ar) { return {}; }
    if (op->num_operands() < 1U || op->num_results() < 1U) { return {}; } // structural — the generated verifier owns it
    const Value* const in  = op->operand(0U);
    const Value* const res = op->result(0U);
    if (!is_tensor_type(ctx, in->type())) { return {in, op, DistMisuseKind::OperandNotTensor}; }
    if (!is_tensor_type(ctx, res->type())) { return {res, op, DistMisuseKind::OperandNotTensor}; }
    if (res->type() != in->type()) { return {res, op, DistMisuseKind::ResultTypeMismatch}; } // placement PRESERVES the tensor
    const Operation* const mesh = find_mesh(ctx, root, symbol_name(ctx, op->attr("mesh")));
    if (mesh == nullptr) { return {nullptr, op, DistMisuseKind::UnknownMesh}; }
    if (is_shard)
    {
        const AttrValue ax   = ctx.attr_value(op->attr("axis"));
        const i64       axis = (ax.kind == AttrKind::Int) ? ax.i : -1;
        if (axis < 0 || axis >= static_cast<i64>(tensor_rank(ctx, in->type())))
        {
            return {nullptr, op, DistMisuseKind::ShardAxisInvalid};
        }
        const AttrValue mshp      = ctx.attr_value(mesh->attr("shape"));
        u32             mesh_rank = 0U;
        const bool      mesh_ok   = mshp.kind == AttrKind::String && parse_mesh_shape(mshp.s, mesh_rank);
        if (!mesh_ok) { mesh_rank = 0U; } // an invalid mesh shape is the mesh's own MeshShapeInvalid; no valid axis here
        const AttrValue ma        = ctx.attr_value(op->attr("mesh_axis"));
        const i64       mesh_axis = (ma.kind == AttrKind::Int) ? ma.i : -1;
        if (mesh_axis < 0 || mesh_axis >= static_cast<i64>(mesh_rank))
        {
            return {nullptr, op, DistMisuseKind::MeshAxisInvalid};
        }
        return {};
    }
    const AttrValue fn = ctx.attr_value(op->attr("fn")); // all_reduce
    if (fn.kind != AttrKind::String || !collective_fn_in(fn.s)) { return {nullptr, op, DistMisuseKind::FnInvalid}; }
    return {};
}

DistMisuse scan_dist_region(const Context& ctx, const Region* root, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const DistMisuse e = check_op(ctx, root, op);
            if (e.kind != DistMisuseKind::None) { return e; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const DistMisuse ce = scan_dist_region(ctx, root, op->region(i));
                if (ce.kind != DistMisuseKind::None) { return ce; }
            }
        }
    }
    return {};
}
} // namespace

DistMisuse find_dist_misuse(const Context& ctx, const Module& m) { return scan_dist_region(ctx, m.body(), m.body()); }

StringView dist_misuse_kind_name(DistMisuseKind k) noexcept
{
    switch (k)
    {
    case DistMisuseKind::None: return StringView("none");
    case DistMisuseKind::MeshShapeInvalid: return StringView("mesh-shape-invalid");
    case DistMisuseKind::DuplicateMesh: return StringView("duplicate-mesh");
    case DistMisuseKind::UnknownMesh: return StringView("unknown-mesh");
    case DistMisuseKind::OperandNotTensor: return StringView("operand-not-tensor");
    case DistMisuseKind::ResultTypeMismatch: return StringView("result-type-mismatch");
    case DistMisuseKind::ShardAxisInvalid: return StringView("shard-axis-invalid");
    case DistMisuseKind::MeshAxisInvalid: return StringView("mesh-axis-invalid");
    case DistMisuseKind::FnInvalid: return StringView("fn-invalid");
    }
    return StringView("?");
}
} // namespace crd::ceir::dist
