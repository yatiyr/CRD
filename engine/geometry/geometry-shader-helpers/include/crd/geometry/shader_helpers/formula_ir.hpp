#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — formula-IR. Phase 3.1.7 v9e-a (2026-05-18).
//
// A flat tree representation of SDF expressions composed from
// `signed_distance.hpp` primitives + `formulary.hpp` operators. The IR is
// walked by:
//   - `evaluate(ir, p)` (C++ ground-truth — drives the ULP-conformance test)
//   - `emit_glsl(ir)`   (v9e-b — emits GLSL `float sdf(vec3 p) { … }`)
//   - `emit_hlsl(ir)`   (v9e-c — same for HLSL)
//   - `cook(ir, out)`   (v9e-d — CMake-time cooker emits .glsl + .hlsl files)
//
// **Storage = three parallel `Array`s + a root index** (NOT pointers / NOT
// std::variant). Reasons:
//   1. Cache-friendly walk (contiguous nodes).
//   2. Bit-exact serialisation for the cooker (no pointer fix-up on load).
//   3. Easy to validate well-formedness (every reference is an array index;
//      bounds-check is O(1)).
//   4. Future GPU-side IR interp (running the IR on-device) is trivially
//      portable — same flat layout transfers to SSBO + GLSL walk function.
//
// `IrNode` is a discriminated union via `Kind` + an inner kind enum. Each
// node has slices into a shared `params` pool (`Array<f32>`) and a shared
// `children` pool (`Array<u32>` of node indices). Param count + child count
// are fixed by the primitive/operator kind (validated in `validate`).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::shader_helpers
{

// ===========================================================================
// Primitive kinds — one per `sd_*` function in `signed_distance.hpp`.
// ===========================================================================
enum class IrPrimKind : crd::u8
{
    // 3D primitives
    Sphere = 0,       // params: [radius]
    Box,              // params: [b.x, b.y, b.z] (half-extents)
    RoundBox,         // params: [b.x, b.y, b.z, r]
    BoxFrame,         // params: [b.x, b.y, b.z, e]
    Plane,            // params: [n.x, n.y, n.z, h] (unit normal + offset)
    Capsule,          // params: [a.x, a.y, a.z, b.x, b.y, b.z, r]
    Cylinder,         // params: [a.x, a.y, a.z, b.x, b.y, b.z, r]
    Cone,             // params: [c.x, c.y, h] — c = (sin θ, cos θ) half-angle
    Torus,            // params: [t.x, t.y] — (major, minor) radius
    Triangle3D,       // params: [a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z]

    Count_,           // sentinel
};

// ===========================================================================
// Operator kinds — value-domain (combine distances) and position-domain
// (warp the query point before evaluating a single child).
// ===========================================================================
enum class IrOpKind : crd::u8
{
    // Value-domain: act on children's distance values.
    // Binary smooth-min/max — children=[a, b], params=[k]
    SminPoly = 0,
    SminCubic,
    SminExp,
    SmaxPoly,
    // Unary value-warps — children=[a]
    OpRound,          // params: [r]   — inflate by r
    OpOnion,          // params: [t]   — shell of half-thickness t

    // Position-domain: act on the query point, child is evaluated at warped p.
    // children=[a]
    DomainRepeat,     // params: [c.x, c.y, c.z] — cell size
    DomainMirror,     // params: [c.x, c.y, c.z]
    DomainElongate,   // params: [h.x, h.y, h.z]
    DomainTwist,      // params: [k] — twist around +Y axis
    DomainBend,       // params: [k] — bend around +Z axis

    Count_,           // sentinel
};

// Compile-time spec table — number of (params, children) each kind requires.
// Single source of truth for validation + builder + evaluator.
//
// Centralised here so adding a new primitive / operator is a one-line change
// (enum entry + spec row + handler in evaluator/emit_glsl/emit_hlsl).
struct IrSpec
{
    crd::u8 param_count;
    crd::u8 child_count;
};

[[nodiscard]] constexpr IrSpec primitive_spec(IrPrimKind k) noexcept
{
    switch (k)
    {
        case IrPrimKind::Sphere:     return {1U, 0U};
        case IrPrimKind::Box:        return {3U, 0U};
        case IrPrimKind::RoundBox:   return {4U, 0U};
        case IrPrimKind::BoxFrame:   return {4U, 0U};
        case IrPrimKind::Plane:      return {4U, 0U};
        case IrPrimKind::Capsule:    return {7U, 0U};
        case IrPrimKind::Cylinder:   return {7U, 0U};
        case IrPrimKind::Cone:       return {3U, 0U};
        case IrPrimKind::Torus:      return {2U, 0U};
        case IrPrimKind::Triangle3D: return {9U, 0U};
        case IrPrimKind::Count_:     return {0U, 0U};
    }
    return {0U, 0U};
}

[[nodiscard]] constexpr IrSpec operator_spec(IrOpKind k) noexcept
{
    switch (k)
    {
        case IrOpKind::SminPoly:        return {1U, 2U};
        case IrOpKind::SminCubic:       return {1U, 2U};
        case IrOpKind::SminExp:         return {1U, 2U};
        case IrOpKind::SmaxPoly:        return {1U, 2U};
        case IrOpKind::OpRound:         return {1U, 1U};
        case IrOpKind::OpOnion:         return {1U, 1U};
        case IrOpKind::DomainRepeat:    return {3U, 1U};
        case IrOpKind::DomainMirror:    return {3U, 1U};
        case IrOpKind::DomainElongate:  return {3U, 1U};
        case IrOpKind::DomainTwist:     return {1U, 1U};
        case IrOpKind::DomainBend:      return {1U, 1U};
        case IrOpKind::Count_:          return {0U, 0U};
    }
    return {0U, 0U};
}

// ===========================================================================
// IrNode — flat node entry.
// ===========================================================================
struct IrNode
{
    enum class Kind : crd::u8 { Primitive = 0, Operator = 1 };

    Kind kind;
    // Discriminated union: read `prim` iff kind == Primitive; `op` otherwise.
    union
    {
        IrPrimKind prim;
        IrOpKind   op;
    };

    crd::u32 params_offset;    // slice into FormulaIr::params
    crd::u32 params_count;     // matches spec().param_count
    crd::u32 children_offset;  // slice into FormulaIr::children
    crd::u32 children_count;   // matches spec().child_count
};

// ===========================================================================
// FormulaIr — owning container; flat tree + shared params/children pools.
// ===========================================================================
class FormulaIr
{
public:
    explicit FormulaIr(crd::memory::IAllocator* alloc) noexcept
        : m_nodes(alloc), m_params(alloc), m_children(alloc)
    {
    }

    FormulaIr(const FormulaIr&)            = delete;
    FormulaIr& operator=(const FormulaIr&) = delete;
    FormulaIr(FormulaIr&&) noexcept        = default;
    FormulaIr& operator=(FormulaIr&&) noexcept = default;
    ~FormulaIr()                           = default;

    [[nodiscard]] crd::containers::ConstSpan<IrNode> nodes() const noexcept
    {
        return {m_nodes.data(), m_nodes.size()};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::f32> params() const noexcept
    {
        return {m_params.data(), m_params.size()};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> children() const noexcept
    {
        return {m_children.data(), m_children.size()};
    }
    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }
    [[nodiscard]] bool     is_empty() const noexcept { return m_nodes.size() == 0U; }

    [[nodiscard]] crd::containers::ConstSpan<crd::f32> params_of(const IrNode& n) const noexcept
    {
        return {m_params.data() + n.params_offset, n.params_count};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> children_of(const IrNode& n) const noexcept
    {
        return {m_children.data() + n.children_offset, n.children_count};
    }

    // Builder-side mutators.
    [[nodiscard]] crd::containers::Array<IrNode>&  nodes_mut() noexcept   { return m_nodes; }
    [[nodiscard]] crd::containers::Array<crd::f32>& params_mut() noexcept  { return m_params; }
    [[nodiscard]] crd::containers::Array<crd::u32>& children_mut() noexcept { return m_children; }
    void set_root(crd::u32 r) noexcept { m_root = r; }

private:
    crd::containers::Array<IrNode>  m_nodes;
    crd::containers::Array<crd::f32> m_params;
    crd::containers::Array<crd::u32> m_children;
    crd::u32                         m_root{0U};
};

// ===========================================================================
// IrBuilder — fluent construction. Returns node indices; chain together to
// compose. Final IR is obtained via `build(root_idx)`.
// ===========================================================================
class IrBuilder
{
public:
    explicit IrBuilder(crd::memory::IAllocator* alloc) noexcept
        : m_ir(alloc)
    {
    }

    // ---- 3D primitives -----------------------------------------------------
    [[nodiscard]] crd::u32 sphere(crd::f32 r) noexcept;
    [[nodiscard]] crd::u32 box(crd::f32 bx, crd::f32 by, crd::f32 bz) noexcept;
    [[nodiscard]] crd::u32 round_box(crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 r) noexcept;
    [[nodiscard]] crd::u32 box_frame(crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 e) noexcept;
    [[nodiscard]] crd::u32 plane(crd::f32 nx, crd::f32 ny, crd::f32 nz, crd::f32 h) noexcept;
    [[nodiscard]] crd::u32 capsule(crd::f32 ax, crd::f32 ay, crd::f32 az,
                                    crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 r) noexcept;
    [[nodiscard]] crd::u32 cylinder(crd::f32 ax, crd::f32 ay, crd::f32 az,
                                     crd::f32 bx, crd::f32 by, crd::f32 bz, crd::f32 r) noexcept;
    [[nodiscard]] crd::u32 cone(crd::f32 cx, crd::f32 cy, crd::f32 h) noexcept;
    [[nodiscard]] crd::u32 torus(crd::f32 tx, crd::f32 ty) noexcept;
    [[nodiscard]] crd::u32 triangle(crd::f32 ax, crd::f32 ay, crd::f32 az,
                                     crd::f32 bx, crd::f32 by, crd::f32 bz,
                                     crd::f32 cx, crd::f32 cy, crd::f32 cz) noexcept;

    // ---- Value-domain operators -------------------------------------------
    [[nodiscard]] crd::u32 smin_poly(crd::u32 a, crd::u32 b, crd::f32 k) noexcept;
    [[nodiscard]] crd::u32 smin_cubic(crd::u32 a, crd::u32 b, crd::f32 k) noexcept;
    [[nodiscard]] crd::u32 smin_exp(crd::u32 a, crd::u32 b, crd::f32 k) noexcept;
    [[nodiscard]] crd::u32 smax_poly(crd::u32 a, crd::u32 b, crd::f32 k) noexcept;
    [[nodiscard]] crd::u32 op_round(crd::u32 child, crd::f32 r) noexcept;
    [[nodiscard]] crd::u32 op_onion(crd::u32 child, crd::f32 t) noexcept;

    // ---- Position-domain operators ----------------------------------------
    [[nodiscard]] crd::u32 domain_repeat(crd::u32 child, crd::f32 cx, crd::f32 cy, crd::f32 cz) noexcept;
    [[nodiscard]] crd::u32 domain_mirror(crd::u32 child, crd::f32 cx, crd::f32 cy, crd::f32 cz) noexcept;
    [[nodiscard]] crd::u32 domain_elongate(crd::u32 child, crd::f32 hx, crd::f32 hy, crd::f32 hz) noexcept;
    [[nodiscard]] crd::u32 domain_twist(crd::u32 child, crd::f32 k) noexcept;
    [[nodiscard]] crd::u32 domain_bend(crd::u32 child, crd::f32 k) noexcept;

    // Finalise: returns the IR with `root` set. After this the builder is
    // moved-from and must not be reused.
    [[nodiscard]] FormulaIr build(crd::u32 root_idx) && noexcept;

private:
    FormulaIr m_ir;

    [[nodiscard]] crd::u32 push_primitive(IrPrimKind kind,
                                           std::initializer_list<crd::f32> params) noexcept;
    [[nodiscard]] crd::u32 push_operator(IrOpKind kind,
                                          std::initializer_list<crd::f32> params,
                                          std::initializer_list<crd::u32> children) noexcept;
};

// ===========================================================================
// Validation — every reference in bounds, every node's param + child count
// matches its spec. Tree must be a DAG rooted at `root` (no cycles, all
// reachable).
// ===========================================================================
enum class IrValidationStatus : crd::u8
{
    Ok = 0,
    EmptyIr,                    // 0 nodes
    RootOutOfBounds,            // root >= nodes.size()
    NodeOutOfBoundsChild,       // child index >= nodes.size()
    ParamCountMismatch,         // node's params_count != spec().param_count
    ChildCountMismatch,         // node's children_count != spec().child_count
    ParamOffsetOutOfBounds,     // params_offset + params_count > params.size()
    ChildOffsetOutOfBounds,     // children_offset + children_count > children.size()
    CycleDetected,              // reachability walk hit a visited node
    UnreachableNode,            // a node not reached from root (warning-class — caller
                                //     decides; we treat it as an error to keep IRs lean)
    InvalidKindEnum,            // primitive/operator kind out of range
};

struct IrValidationResult
{
    IrValidationStatus status = IrValidationStatus::Ok;
    crd::u32           node_index = 0U;  // first offending node (when applicable)
};

[[nodiscard]] IrValidationResult validate(const FormulaIr& ir) noexcept;

} // namespace crd::geometry::shader_helpers
