// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — formula-IR builder + validator implementation.
// Phase 3.1.7 v9e-a (2026-05-18).
// ---------------------------------------------------------------------------

#include <crd/geometry/shader_helpers/formula_ir.hpp>

#include <crd/core/assert.hpp>

namespace crd::geometry::shader_helpers
{

// ===========================================================================
// IrBuilder — internal helpers.
// ===========================================================================
crd::u32 IrBuilder::push_primitive(IrPrimKind kind, std::initializer_list<crd::f32> params) noexcept
{
    const IrSpec spec = primitive_spec(kind);
    CRD_ASSERT_MSG(static_cast<crd::usize>(params.size()) == spec.param_count,
                   "IrBuilder: primitive param count mismatch");

    const crd::u32 params_offset = static_cast<crd::u32>(m_ir.params_mut().size());
    for (crd::f32 v : params) { m_ir.params_mut().push_back(v); }

    IrNode node{};
    node.kind             = IrNode::Kind::Primitive;
    node.prim             = kind;
    node.params_offset    = params_offset;
    node.params_count     = static_cast<crd::u32>(spec.param_count);
    node.children_offset  = 0U;
    node.children_count   = 0U;

    const crd::u32 idx = static_cast<crd::u32>(m_ir.nodes_mut().size());
    m_ir.nodes_mut().push_back(node);
    return idx;
}

crd::u32 IrBuilder::push_operator(IrOpKind kind,
                                   std::initializer_list<crd::f32> params,
                                   std::initializer_list<crd::u32> children) noexcept
{
    const IrSpec spec = operator_spec(kind);
    CRD_ASSERT_MSG(static_cast<crd::usize>(params.size())   == spec.param_count,
                   "IrBuilder: operator param count mismatch");
    CRD_ASSERT_MSG(static_cast<crd::usize>(children.size()) == spec.child_count,
                   "IrBuilder: operator child count mismatch");

    const crd::u32 params_offset = static_cast<crd::u32>(m_ir.params_mut().size());
    for (crd::f32 v : params) { m_ir.params_mut().push_back(v); }

    const crd::u32 children_offset = static_cast<crd::u32>(m_ir.children_mut().size());
    for (crd::u32 c : children) { m_ir.children_mut().push_back(c); }

    IrNode node{};
    node.kind             = IrNode::Kind::Operator;
    node.op               = kind;
    node.params_offset    = params_offset;
    node.params_count     = static_cast<crd::u32>(spec.param_count);
    node.children_offset  = children_offset;
    node.children_count   = static_cast<crd::u32>(spec.child_count);

    const crd::u32 idx = static_cast<crd::u32>(m_ir.nodes_mut().size());
    m_ir.nodes_mut().push_back(node);
    return idx;
}

// ===========================================================================
// IrBuilder — primitive constructors.
// ===========================================================================
crd::u32 IrBuilder::sphere(crd::f32 r) noexcept
{
    return push_primitive(IrPrimKind::Sphere, {r});
}
crd::u32 IrBuilder::box(crd::f32 bx, crd::f32 by, crd::f32 bz) noexcept
{
    return push_primitive(IrPrimKind::Box, {bx, by, bz});
}
crd::u32 IrBuilder::round_box(crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 r) noexcept
{
    return push_primitive(IrPrimKind::RoundBox, {bx, by, bz, r});
}
crd::u32 IrBuilder::box_frame(crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 e) noexcept
{
    return push_primitive(IrPrimKind::BoxFrame, {bx, by, bz, e});
}
crd::u32 IrBuilder::plane(crd::f32 nx, crd::f32 ny, crd::f32 nz, crd::f32 h) noexcept
{
    return push_primitive(IrPrimKind::Plane, {nx, ny, nz, h});
}
crd::u32 IrBuilder::capsule(crd::f32 ax, crd::f32 ay, crd::f32 az,
                              crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 r) noexcept
{
    return push_primitive(IrPrimKind::Capsule, {ax, ay, az, bx, by, bz, r});
}
crd::u32 IrBuilder::cylinder(crd::f32 ax, crd::f32 ay, crd::f32 az,
                               crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 r) noexcept
{
    return push_primitive(IrPrimKind::Cylinder, {ax, ay, az, bx, by, bz, r});
}
crd::u32 IrBuilder::cone(crd::f32 cx, crd::f32 cy, crd::f32 h) noexcept
{
    return push_primitive(IrPrimKind::Cone, {cx, cy, h});
}
crd::u32 IrBuilder::torus(crd::f32 tx, crd::f32 ty) noexcept
{
    return push_primitive(IrPrimKind::Torus, {tx, ty});
}
crd::u32 IrBuilder::triangle(crd::f32 ax, crd::f32 ay, crd::f32 az,
                               crd::f32 bx, crd::f32 by, crd::f32 bz,
                               crd::f32 cx, crd::f32 cy, crd::f32 cz) noexcept
{
    return push_primitive(IrPrimKind::Triangle3D, {ax, ay, az, bx, by, bz, cx, cy, cz});
}

// ===========================================================================
// IrBuilder — operator constructors.
// ===========================================================================
crd::u32 IrBuilder::smin_poly(crd::u32 a, crd::u32 b, crd::f32 k) noexcept
{
    return push_operator(IrOpKind::SminPoly, {k}, {a, b});
}
crd::u32 IrBuilder::smin_cubic(crd::u32 a, crd::u32 b, crd::f32 k) noexcept
{
    return push_operator(IrOpKind::SminCubic, {k}, {a, b});
}
crd::u32 IrBuilder::smin_exp(crd::u32 a, crd::u32 b, crd::f32 k) noexcept
{
    return push_operator(IrOpKind::SminExp, {k}, {a, b});
}
crd::u32 IrBuilder::smax_poly(crd::u32 a, crd::u32 b, crd::f32 k) noexcept
{
    return push_operator(IrOpKind::SmaxPoly, {k}, {a, b});
}
crd::u32 IrBuilder::op_round(crd::u32 child, crd::f32 r) noexcept
{
    return push_operator(IrOpKind::OpRound, {r}, {child});
}
crd::u32 IrBuilder::op_onion(crd::u32 child, crd::f32 t) noexcept
{
    return push_operator(IrOpKind::OpOnion, {t}, {child});
}
crd::u32 IrBuilder::domain_repeat(crd::u32 child, crd::f32 cx, crd::f32 cy, crd::f32 cz) noexcept
{
    return push_operator(IrOpKind::DomainRepeat, {cx, cy, cz}, {child});
}
crd::u32 IrBuilder::domain_mirror(crd::u32 child, crd::f32 cx, crd::f32 cy, crd::f32 cz) noexcept
{
    return push_operator(IrOpKind::DomainMirror, {cx, cy, cz}, {child});
}
crd::u32 IrBuilder::domain_elongate(crd::u32 child, crd::f32 hx, crd::f32 hy, crd::f32 hz) noexcept
{
    return push_operator(IrOpKind::DomainElongate, {hx, hy, hz}, {child});
}
crd::u32 IrBuilder::domain_twist(crd::u32 child, crd::f32 k) noexcept
{
    return push_operator(IrOpKind::DomainTwist, {k}, {child});
}
crd::u32 IrBuilder::domain_bend(crd::u32 child, crd::f32 k) noexcept
{
    return push_operator(IrOpKind::DomainBend, {k}, {child});
}

FormulaIr IrBuilder::build(crd::u32 root_idx) && noexcept
{
    m_ir.set_root(root_idx);
    return std::move(m_ir);
}

// ===========================================================================
// validate() — depth-first reachability walk with bounds + cycle checks.
// ===========================================================================
namespace
{

[[nodiscard]] bool is_valid_prim_kind(IrPrimKind k) noexcept
{
    return static_cast<crd::u8>(k) < static_cast<crd::u8>(IrPrimKind::Count_);
}
[[nodiscard]] bool is_valid_op_kind(IrOpKind k) noexcept
{
    return static_cast<crd::u8>(k) < static_cast<crd::u8>(IrOpKind::Count_);
}

// Walk state: 0=unvisited, 1=in-progress (on the recursion stack), 2=visited.
// in-progress edges detect cycles.
constexpr crd::u8 kUnvisited  = 0U;
constexpr crd::u8 kInProgress = 1U;
constexpr crd::u8 kVisited    = 2U;

[[nodiscard]] IrValidationResult
visit_node(const FormulaIr& ir,
           crd::u32          idx,
           crd::containers::Array<crd::u8>& state) noexcept
{
    if (state[idx] == kInProgress)
    {
        return {IrValidationStatus::CycleDetected, idx};
    }
    if (state[idx] == kVisited) { return {IrValidationStatus::Ok, idx}; }

    state[idx] = kInProgress;
    const IrNode& n = ir.nodes()[idx];

    // Kind enum range.
    if (n.kind == IrNode::Kind::Primitive)
    {
        if (!is_valid_prim_kind(n.prim))
        {
            return {IrValidationStatus::InvalidKindEnum, idx};
        }
    }
    else
    {
        if (!is_valid_op_kind(n.op))
        {
            return {IrValidationStatus::InvalidKindEnum, idx};
        }
    }

    // Param + child count match spec.
    const IrSpec spec = (n.kind == IrNode::Kind::Primitive)
                        ? primitive_spec(n.prim)
                        : operator_spec(n.op);
    if (n.params_count != static_cast<crd::u32>(spec.param_count))
    {
        return {IrValidationStatus::ParamCountMismatch, idx};
    }
    if (n.children_count != static_cast<crd::u32>(spec.child_count))
    {
        return {IrValidationStatus::ChildCountMismatch, idx};
    }

    // Offsets in bounds.
    if (static_cast<crd::usize>(n.params_offset) + n.params_count > ir.params().size())
    {
        return {IrValidationStatus::ParamOffsetOutOfBounds, idx};
    }
    if (static_cast<crd::usize>(n.children_offset) + n.children_count > ir.children().size())
    {
        return {IrValidationStatus::ChildOffsetOutOfBounds, idx};
    }

    // Recurse into children.
    const auto children = ir.children_of(n);
    for (crd::u32 child_idx : children)
    {
        if (child_idx >= ir.nodes().size())
        {
            return {IrValidationStatus::NodeOutOfBoundsChild, idx};
        }
        const auto child_result = visit_node(ir, child_idx, state);
        if (child_result.status != IrValidationStatus::Ok) { return child_result; }
    }

    state[idx] = kVisited;
    return {IrValidationStatus::Ok, idx};
}

} // namespace

IrValidationResult validate(const FormulaIr& ir) noexcept
{
    if (ir.is_empty()) { return {IrValidationStatus::EmptyIr, 0U}; }
    if (ir.root() >= ir.nodes().size())
    {
        return {IrValidationStatus::RootOutOfBounds, ir.root()};
    }

    // Use the default allocator for the temporary state buffer. The IR's
    // allocator isn't exposed via the public API; this is a scratch buffer
    // (size = N, lifetime = this call) so the default is fine.
    crd::containers::Array<crd::u8> state;
    state.resize(ir.nodes().size(), kUnvisited);

    const auto walk_result = visit_node(ir, ir.root(), state);
    if (walk_result.status != IrValidationStatus::Ok) { return walk_result; }

    // Check all nodes are reachable from root (no orphans).
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(state.size()); ++i)
    {
        if (state[i] != kVisited)
        {
            return {IrValidationStatus::UnreachableNode, i};
        }
    }

    return {IrValidationStatus::Ok, 0U};
}

} // namespace crd::geometry::shader_helpers
