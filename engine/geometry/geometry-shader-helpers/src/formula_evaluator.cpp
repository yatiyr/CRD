// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — C++ ground-truth evaluator.
// Phase 3.1.7 v9e-a (2026-05-18).
//
// Recursive walk over the IR — each Primitive node calls the matching `sd_*`
// from `signed_distance.hpp`; each Operator node either combines child
// distances (smin / smax / op_round / op_onion) or warps the query point
// before recursing (domain_*). The same function tree the GLSL/HLSL emission
// in v9e-b/v9e-c will produce — this is its ULP-conformance reference.
// ---------------------------------------------------------------------------

#include <crd/geometry/shader_helpers/formula_evaluator.hpp>

#include <crd/core/assert.hpp>
#include <crd/geometry/primitives/formulary.hpp>
#include <crd/geometry/primitives/signed_distance.hpp>

namespace crd::geometry::shader_helpers
{

namespace
{

template <typename T>
[[nodiscard]] T evaluate_primitive(IrPrimKind kind,
                                    crd::containers::ConstSpan<crd::f32> params,
                                    const crd::math::Vec3<T>& p) noexcept
{
    namespace sd = crd::geometry::primitives;
    // Param indexing matches the order documented in formula_ir.hpp.
    switch (kind)
    {
        case IrPrimKind::Sphere:
            return sd::sd_sphere<T>(p, static_cast<T>(params[0]));

        case IrPrimKind::Box:
            return sd::sd_box<T>(p, crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                        static_cast<T>(params[1]),
                                                        static_cast<T>(params[2])));

        case IrPrimKind::RoundBox:
            return sd::sd_round_box<T>(p, crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                              static_cast<T>(params[1]),
                                                              static_cast<T>(params[2])),
                                       static_cast<T>(params[3]));

        case IrPrimKind::BoxFrame:
            return sd::sd_box_frame<T>(p, crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                              static_cast<T>(params[1]),
                                                              static_cast<T>(params[2])),
                                       static_cast<T>(params[3]));

        case IrPrimKind::Plane:
            return sd::sd_plane<T>(p, crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                          static_cast<T>(params[1]),
                                                          static_cast<T>(params[2])),
                                   static_cast<T>(params[3]));

        case IrPrimKind::Capsule:
            return sd::sd_capsule<T>(p,
                                      crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                          static_cast<T>(params[1]),
                                                          static_cast<T>(params[2])),
                                      crd::math::Vec3<T>(static_cast<T>(params[3]),
                                                          static_cast<T>(params[4]),
                                                          static_cast<T>(params[5])),
                                      static_cast<T>(params[6]));

        case IrPrimKind::Cylinder:
            return sd::sd_cylinder<T>(p,
                                       crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                           static_cast<T>(params[1]),
                                                           static_cast<T>(params[2])),
                                       crd::math::Vec3<T>(static_cast<T>(params[3]),
                                                           static_cast<T>(params[4]),
                                                           static_cast<T>(params[5])),
                                       static_cast<T>(params[6]));

        case IrPrimKind::Cone:
            return sd::sd_cone<T>(p,
                                   crd::math::Vec2<T>(static_cast<T>(params[0]),
                                                       static_cast<T>(params[1])),
                                   static_cast<T>(params[2]));

        case IrPrimKind::Torus:
            return sd::sd_torus<T>(p, crd::math::Vec2<T>(static_cast<T>(params[0]),
                                                          static_cast<T>(params[1])));

        case IrPrimKind::Triangle3D:
            return sd::sd_triangle<T>(p,
                                       crd::math::Vec3<T>(static_cast<T>(params[0]),
                                                           static_cast<T>(params[1]),
                                                           static_cast<T>(params[2])),
                                       crd::math::Vec3<T>(static_cast<T>(params[3]),
                                                           static_cast<T>(params[4]),
                                                           static_cast<T>(params[5])),
                                       crd::math::Vec3<T>(static_cast<T>(params[6]),
                                                           static_cast<T>(params[7]),
                                                           static_cast<T>(params[8])));

        case IrPrimKind::Count_:
            break;
    }
    CRD_ASSERT_MSG(false, "evaluate_primitive: invalid IrPrimKind");
    return static_cast<T>(0);
}

template <typename T>
T evaluate_node_impl(const FormulaIr& ir, crd::u32 idx, const crd::math::Vec3<T>& p) noexcept
{
    namespace sd = crd::geometry::primitives;
    const IrNode& node = ir.nodes()[idx];
    const auto    params   = ir.params_of(node);
    const auto    children = ir.children_of(node);

    if (node.kind == IrNode::Kind::Primitive)
    {
        return evaluate_primitive<T>(node.prim, params, p);
    }

    // Operator dispatch.
    switch (node.op)
    {
        // ---- Value-domain: evaluate both children, combine ---------------
        case IrOpKind::SminPoly:
        {
            const T da = evaluate_node_impl<T>(ir, children[0], p);
            const T db = evaluate_node_impl<T>(ir, children[1], p);
            return sd::smin_poly<T>(da, db, static_cast<T>(params[0]));
        }
        case IrOpKind::SminCubic:
        {
            const T da = evaluate_node_impl<T>(ir, children[0], p);
            const T db = evaluate_node_impl<T>(ir, children[1], p);
            return sd::smin_cubic<T>(da, db, static_cast<T>(params[0]));
        }
        case IrOpKind::SminExp:
        {
            const T da = evaluate_node_impl<T>(ir, children[0], p);
            const T db = evaluate_node_impl<T>(ir, children[1], p);
            return sd::smin_exp<T>(da, db, static_cast<T>(params[0]));
        }
        case IrOpKind::SmaxPoly:
        {
            const T da = evaluate_node_impl<T>(ir, children[0], p);
            const T db = evaluate_node_impl<T>(ir, children[1], p);
            return sd::smax_poly<T>(da, db, static_cast<T>(params[0]));
        }
        case IrOpKind::OpRound:
        {
            const T d = evaluate_node_impl<T>(ir, children[0], p);
            return sd::op_round<T>(d, static_cast<T>(params[0]));
        }
        case IrOpKind::OpOnion:
        {
            const T d = evaluate_node_impl<T>(ir, children[0], p);
            return sd::op_onion<T>(d, static_cast<T>(params[0]));
        }

        // ---- Position-domain: warp p, then evaluate the single child ----
        case IrOpKind::DomainRepeat:
        {
            const crd::math::Vec3<T> cell(static_cast<T>(params[0]),
                                           static_cast<T>(params[1]),
                                           static_cast<T>(params[2]));
            const auto p_warped = sd::domain_repeat<T>(p, cell);
            return evaluate_node_impl<T>(ir, children[0], p_warped);
        }
        case IrOpKind::DomainMirror:
        {
            const crd::math::Vec3<T> cell(static_cast<T>(params[0]),
                                           static_cast<T>(params[1]),
                                           static_cast<T>(params[2]));
            const auto p_warped = sd::domain_mirror<T>(p, cell);
            return evaluate_node_impl<T>(ir, children[0], p_warped);
        }
        case IrOpKind::DomainElongate:
        {
            const crd::math::Vec3<T> h_v(static_cast<T>(params[0]),
                                          static_cast<T>(params[1]),
                                          static_cast<T>(params[2]));
            const auto p_warped = sd::domain_elongate<T>(p, h_v);
            return evaluate_node_impl<T>(ir, children[0], p_warped);
        }
        case IrOpKind::DomainTwist:
        {
            const auto p_warped = sd::domain_twist<T>(p, static_cast<T>(params[0]));
            return evaluate_node_impl<T>(ir, children[0], p_warped);
        }
        case IrOpKind::DomainBend:
        {
            const auto p_warped = sd::domain_bend<T>(p, static_cast<T>(params[0]));
            return evaluate_node_impl<T>(ir, children[0], p_warped);
        }

        case IrOpKind::Count_:
            break;
    }
    CRD_ASSERT_MSG(false, "evaluate_node: invalid IrOpKind");
    return static_cast<T>(0);
}

} // namespace

template <typename T>
T evaluate(const FormulaIr& ir, const crd::math::Vec3<T>& p) noexcept
{
    CRD_ASSERT_MSG(!ir.is_empty(), "evaluate: empty IR");
    CRD_ASSERT_MSG(ir.root() < ir.nodes().size(), "evaluate: root out of bounds");
    return evaluate_node_impl<T>(ir, ir.root(), p);
}

template float  evaluate<float>(const FormulaIr&,  const crd::math::Vec3<float>&)  noexcept;
template double evaluate<double>(const FormulaIr&, const crd::math::Vec3<double>&) noexcept;

} // namespace crd::geometry::shader_helpers
