// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — 20 golden formula-IR manifests (impl).
// Phase 3.1.7 v9e-a (2026-05-18).
//
// Each function constructs a fully-valid IR via IrBuilder, with parameters
// chosen to exercise the underlying primitive / operator without producing
// degenerate cases. Numerical values match the iq article defaults where
// the article publishes one.
// ---------------------------------------------------------------------------

#include <crd/geometry/shader_helpers/golden_manifests.hpp>

namespace crd::geometry::shader_helpers::golden
{

// ---- Primitives ---------------------------------------------------------

FormulaIr make_sphere_unit(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto root = b.sphere(0.5F);
    return std::move(b).build(root);
}

FormulaIr make_box_unit(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto root = b.box(0.4F, 0.4F, 0.4F);
    return std::move(b).build(root);
}

FormulaIr make_round_box(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto root = b.round_box(0.4F, 0.4F, 0.4F, 0.05F);
    return std::move(b).build(root);
}

FormulaIr make_box_frame(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto root = b.box_frame(0.4F, 0.4F, 0.4F, 0.02F);
    return std::move(b).build(root);
}

FormulaIr make_plane_y(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    // Plane y = 0 (normal +Y, offset 0).
    const auto root = b.plane(0.0F, 1.0F, 0.0F, 0.0F);
    return std::move(b).build(root);
}

FormulaIr make_capsule(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    // Capsule from (-0.3, 0, 0) to (0.3, 0, 0) radius 0.1.
    const auto root = b.capsule(-0.3F, 0.0F, 0.0F, 0.3F, 0.0F, 0.0F, 0.1F);
    return std::move(b).build(root);
}

FormulaIr make_cylinder(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    // Cylinder along Y axis, length 0.6, radius 0.15.
    const auto root = b.cylinder(0.0F, -0.3F, 0.0F, 0.0F, 0.3F, 0.0F, 0.15F);
    return std::move(b).build(root);
}

FormulaIr make_cone(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    // Cone with 30° half-angle (sin=0.5, cos≈0.866), height 0.5.
    const auto root = b.cone(0.5F, 0.86602540378F, 0.5F);
    return std::move(b).build(root);
}

FormulaIr make_torus(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    // Torus major=0.3, minor=0.08.
    const auto root = b.torus(0.3F, 0.08F);
    return std::move(b).build(root);
}

FormulaIr make_triangle(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    // Triangle in xy-plane at unit scale.
    const auto root = b.triangle(-0.3F,  0.0F, 0.0F,
                                  0.3F, -0.2F, 0.0F,
                                  0.0F,  0.3F, 0.0F);
    return std::move(b).build(root);
}

// ---- Value-domain operators --------------------------------------------

FormulaIr make_smin_poly_union(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.3F);
    const auto box    = b.box(0.25F, 0.25F, 0.25F);
    const auto root   = b.smin_poly(sphere, box, 0.1F);
    return std::move(b).build(root);
}

FormulaIr make_smin_cubic_union(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.3F);
    const auto box    = b.box(0.25F, 0.25F, 0.25F);
    const auto root   = b.smin_cubic(sphere, box, 0.1F);
    return std::move(b).build(root);
}

FormulaIr make_smin_exp_union(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.3F);
    const auto box    = b.box(0.25F, 0.25F, 0.25F);
    const auto root   = b.smin_exp(sphere, box, 0.1F);
    return std::move(b).build(root);
}

FormulaIr make_smax_poly_intersect(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.3F);
    const auto box    = b.box(0.25F, 0.25F, 0.25F);
    const auto root   = b.smax_poly(sphere, box, 0.1F);
    return std::move(b).build(root);
}

FormulaIr make_op_round_sphere(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.25F);
    const auto root   = b.op_round(sphere, 0.05F);
    return std::move(b).build(root);
}

FormulaIr make_op_onion_sphere(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.3F);
    const auto root   = b.op_onion(sphere, 0.05F);
    return std::move(b).build(root);
}

// ---- Position-domain operators -----------------------------------------

FormulaIr make_domain_repeat_sphere(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.1F);
    const auto root   = b.domain_repeat(sphere, 0.5F, 0.5F, 0.5F);
    return std::move(b).build(root);
}

FormulaIr make_domain_mirror_sphere(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.1F);
    const auto root   = b.domain_mirror(sphere, 0.5F, 0.5F, 0.5F);
    return std::move(b).build(root);
}

FormulaIr make_domain_elongate_sphere(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto sphere = b.sphere(0.15F);
    const auto root   = b.domain_elongate(sphere, 0.2F, 0.0F, 0.0F);
    return std::move(b).build(root);
}

FormulaIr make_domain_twist_cylinder(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto cyl  = b.cylinder(0.0F, -0.4F, 0.0F, 0.0F, 0.4F, 0.0F, 0.1F);
    const auto root = b.domain_twist(cyl, 2.0F);
    return std::move(b).build(root);
}

FormulaIr make_domain_bend_capsule(crd::memory::IAllocator* alloc) noexcept
{
    IrBuilder b(alloc);
    const auto cap  = b.capsule(-0.4F, 0.0F, 0.0F, 0.4F, 0.0F, 0.0F, 0.08F);
    const auto root = b.domain_bend(cap, 1.5F);
    return std::move(b).build(root);
}

} // namespace crd::geometry::shader_helpers::golden
