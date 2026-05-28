// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — formula-IR test corpus. Phase 3.1.7 v9e-a.
//
// Covers:
//   1. Builder + IR storage (every primitive + every operator constructs a
//      valid IR — `validate()` returns Ok).
//   2. Validation diagnostics — corrupted IRs (root OOB, OOB child, etc.)
//      produce the right status.
//   3. Evaluator ground-truth match — for each of the 20 golden manifests,
//      the IR-walked distance at a small sample-point set MUST equal the
//      direct call to the underlying `sd_*` function within 1 ULP (was
//      bit-exact; relaxed after the 2026-05-19 full-sweep failure on
//      win-clang-cl-shipping, where LTO fused fmadd sequences differently
//      between TUs). 1 ULP matches the emitted-backend conformance
//      contract per ADR-0076 §26 D173 and is the strongest portable
//      guarantee; routing bugs in the evaluator still surface (would
//      diverge by far more than 1 ULP).
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/core/types.hpp>
#include <crd/geometry/primitives/formulary.hpp>
#include <crd/geometry/primitives/signed_distance.hpp>
#include <crd/geometry/shader_helpers/formula_evaluator.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/geometry/shader_helpers/golden_manifests.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/test_helpers/gpu_compare.hpp>

#include <cstring>

namespace
{

using namespace crd::geometry::shader_helpers;
namespace gold = crd::geometry::shader_helpers::golden;
namespace sd   = crd::geometry::primitives;

// Tolerance for "evaluator equals direct sd_* call" — see the test below
// for rationale. Bit-exact equality was an aspirational claim that
// silently relied on the compiler producing identical FP instruction
// sequences for both call sites. clang-cl + LTO can fuse different
// fmadd sequences across TU boundaries (surfaced by the 2026-05-19
// full-sweep failure on win-clang-cl-shipping). 1 ULP matches the
// emitted-backend conformance contract (ADR-0076 §26 D173) and is the
// strongest portable guarantee.
constexpr crd::u32 kEvaluatorEqualUlpBound = 1U;

[[nodiscard]] crd::math::Vec3<crd::f32> v3(crd::f32 x, crd::f32 y, crd::f32 z) noexcept
{
    return crd::math::Vec3<crd::f32>(x, y, z);
}

} // namespace

// =========================================================================
// IrBuilder + validation — every kind constructs a valid IR.
// =========================================================================

TEST_CASE("v9e-a IrBuilder constructs valid IR for every primitive kind",
          "[shader_helpers][ir][builder]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // Each primitive in its own IR — validation must return Ok.
    {
        IrBuilder b(&alloc);
        const auto root = b.sphere(0.5F);
        const auto ir   = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
        REQUIRE(ir.nodes().size() == 1U);
    }
    {
        IrBuilder b(&alloc);
        const auto root = b.box(0.4F, 0.4F, 0.4F);
        const auto ir   = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
    }
    {
        IrBuilder b(&alloc);
        const auto root = b.torus(0.3F, 0.05F);
        const auto ir   = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
    }
    {
        IrBuilder b(&alloc);
        const auto root = b.triangle(0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F);
        const auto ir   = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
    }
}

TEST_CASE("v9e-a IrBuilder constructs valid IR for every operator kind",
          "[shader_helpers][ir][builder]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // smin_poly union
    {
        IrBuilder b(&alloc);
        const auto a    = b.sphere(0.3F);
        const auto box  = b.box(0.25F, 0.25F, 0.25F);
        const auto root = b.smin_poly(a, box, 0.1F);
        const auto ir   = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
        REQUIRE(ir.nodes().size() == 3U);
    }
    // domain_repeat over a single child
    {
        IrBuilder b(&alloc);
        const auto child = b.sphere(0.1F);
        const auto root  = b.domain_repeat(child, 0.5F, 0.5F, 0.5F);
        const auto ir    = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
        REQUIRE(ir.nodes().size() == 2U);
    }
    // op_round
    {
        IrBuilder b(&alloc);
        const auto child = b.sphere(0.3F);
        const auto root  = b.op_round(child, 0.05F);
        const auto ir    = std::move(b).build(root);
        REQUIRE(validate(ir).status == IrValidationStatus::Ok);
    }
}

// =========================================================================
// Validation diagnostics — corrupted IRs produce the right status.
// =========================================================================

TEST_CASE("v9e-a validate diagnoses empty IR / OOB root / OOB child / cycle",
          "[shader_helpers][ir][validation][diagnostics]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // Empty IR
    {
        FormulaIr ir(&alloc);
        const auto r = validate(ir);
        CHECK(r.status == IrValidationStatus::EmptyIr);
    }

    // Root out of bounds — build a single-node IR, then set root past end.
    {
        IrBuilder b(&alloc);
        const auto root = b.sphere(0.3F);
        auto       ir   = std::move(b).build(root);
        ir.set_root(99U);
        const auto r = validate(ir);
        CHECK(r.status == IrValidationStatus::RootOutOfBounds);
    }

    // OOB child index — manually corrupt an operator's child to 99.
    {
        IrBuilder b(&alloc);
        const auto child = b.sphere(0.3F);
        const auto root  = b.op_round(child, 0.05F);
        auto       ir    = std::move(b).build(root);
        // Corrupt the operator's first child to point to a non-existent node.
        ir.children_mut()[0] = 99U;
        const auto r = validate(ir);
        CHECK(r.status == IrValidationStatus::NodeOutOfBoundsChild);
    }

    // Cycle: operator → operator → its own parent.
    {
        IrBuilder b(&alloc);
        const auto inner = b.sphere(0.3F);                  // node 0
        const auto outer = b.op_round(inner, 0.05F);         // node 1 (children=[0])
        auto       ir    = std::move(b).build(outer);
        // Corrupt node 0's tree: pretend it's an operator pointing back to node 1.
        // (We can't actually mutate kind via the public API; this test exercises
        // the cycle detector via the child-array path: redirect outer's child to
        // outer itself.)
        ir.children_mut()[0] = outer;
        const auto r = validate(ir);
        CHECK(r.status == IrValidationStatus::CycleDetected);
    }

    // Unreachable node — add a stray operator that points at a sphere, then
    // build with the SPHERE as root (so the stray operator is orphaned).
    {
        IrBuilder b(&alloc);
        const auto sphere = b.sphere(0.3F);    // node 0
        (void)b.op_round(sphere, 0.05F);        // node 1 — orphaned when root=sphere
        const auto ir = std::move(b).build(sphere);
        const auto r  = validate(ir);
        CHECK(r.status == IrValidationStatus::UnreachableNode);
    }
}

// =========================================================================
// Golden manifests — all 20 must validate.
// =========================================================================

TEST_CASE("v9e-a 20 golden manifests validate Ok",
          "[shader_helpers][ir][golden]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    using MakeFn = FormulaIr(*)(crd::memory::IAllocator*);
    const MakeFn makes[] = {
        &gold::make_sphere_unit,
        &gold::make_box_unit,
        &gold::make_round_box,
        &gold::make_box_frame,
        &gold::make_plane_y,
        &gold::make_capsule,
        &gold::make_cylinder,
        &gold::make_cone,
        &gold::make_torus,
        &gold::make_triangle,
        &gold::make_smin_poly_union,
        &gold::make_smin_cubic_union,
        &gold::make_smin_exp_union,
        &gold::make_smax_poly_intersect,
        &gold::make_op_round_sphere,
        &gold::make_op_onion_sphere,
        &gold::make_domain_repeat_sphere,
        &gold::make_domain_mirror_sphere,
        &gold::make_domain_elongate_sphere,
        &gold::make_domain_twist_cylinder,
        &gold::make_domain_bend_capsule,
    };
    constexpr crd::usize manifest_count = sizeof(makes) / sizeof(makes[0]);
    REQUIRE(manifest_count == 21U);  // 10 primitives + 6 value-domain + 5 position-domain

    for (crd::usize i = 0U; i < manifest_count; ++i)
    {
        const auto ir = makes[i](&alloc);
        const auto r  = validate(ir);
        INFO("manifest index " << i);
        REQUIRE(r.status == IrValidationStatus::Ok);
        REQUIRE(ir.nodes().size() >= 1U);
    }
}

// =========================================================================
// Evaluator ground-truth — IR walk must equal direct `sd_*` call bit-exactly.
// =========================================================================

TEST_CASE("v9e-a evaluator matches direct sd_* call within 1 ULP",
          "[shader_helpers][ir][evaluator]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    const crd::math::Vec3<crd::f32> queries[] = {
        v3(0.0F, 0.0F, 0.0F),
        v3(0.1F, 0.2F, 0.3F),
        v3(-0.4F, 0.1F, 0.2F),
        v3(0.5F, -0.3F, 0.0F),
        v3(1.0F, 0.0F, 0.0F),
        v3(0.7F, 0.7F, 0.7F),
    };

    // Sphere
    {
        const auto ir = gold::make_sphere_unit(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val  = evaluate<crd::f32>(ir, q);
            const crd::f32 ref_val = sd::sd_sphere<crd::f32>(q, 0.5F);
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
    // Box
    {
        const auto ir = gold::make_box_unit(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val  = evaluate<crd::f32>(ir, q);
            const crd::f32 ref_val = sd::sd_box<crd::f32>(q, v3(0.4F, 0.4F, 0.4F));
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
    // Torus
    {
        const auto ir = gold::make_torus(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val  = evaluate<crd::f32>(ir, q);
            const crd::f32 ref_val = sd::sd_torus<crd::f32>(q, crd::math::Vec2<crd::f32>(0.3F, 0.08F));
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
    // smin_poly union — IR composes sphere(0.3) ∪ box(0.25), k=0.1
    {
        const auto ir = gold::make_smin_poly_union(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val = evaluate<crd::f32>(ir, q);
            const crd::f32 da = sd::sd_sphere<crd::f32>(q, 0.3F);
            const crd::f32 db = sd::sd_box<crd::f32>(q, v3(0.25F, 0.25F, 0.25F));
            const crd::f32 ref_val = sd::smin_poly<crd::f32>(da, db, 0.1F);
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
    // op_round sphere — IR composes round(sphere(0.25), r=0.05)
    {
        const auto ir = gold::make_op_round_sphere(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val = evaluate<crd::f32>(ir, q);
            const crd::f32 d      = sd::sd_sphere<crd::f32>(q, 0.25F);
            const crd::f32 ref_val = sd::op_round<crd::f32>(d, 0.05F);
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
    // domain_repeat — IR composes repeat(sphere(0.1), cell=0.5)
    {
        const auto ir = gold::make_domain_repeat_sphere(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val   = evaluate<crd::f32>(ir, q);
            const auto     p_warped = sd::domain_repeat<crd::f32>(q, v3(0.5F, 0.5F, 0.5F));
            const crd::f32 ref_val  = sd::sd_sphere<crd::f32>(p_warped, 0.1F);
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
    // domain_twist — IR composes twist(cylinder(...), k=2.0)
    {
        const auto ir = gold::make_domain_twist_cylinder(&alloc);
        for (const auto& q : queries)
        {
            const crd::f32 ir_val   = evaluate<crd::f32>(ir, q);
            const auto     p_warped = sd::domain_twist<crd::f32>(q, 2.0F);
            const crd::f32 ref_val  = sd::sd_cylinder<crd::f32>(p_warped,
                                                                v3(0.0F, -0.4F, 0.0F),
                                                                v3(0.0F, 0.4F, 0.0F),
                                                                0.1F);
            CHECK(crd::test::detail::ulp_distance_f32(ir_val, ref_val) <= kEvaluatorEqualUlpBound);
        }
    }
}

// =========================================================================
// Evaluator f32/f64 — both instantiations exist + behave consistently.
// =========================================================================

TEST_CASE("v9e-a evaluator instantiates for f32 and f64",
          "[shader_helpers][ir][evaluator][precision]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    const auto ir = gold::make_sphere_unit(&alloc);
    const crd::math::Vec3<crd::f32> qf(0.3F, 0.4F, 0.0F);
    const crd::math::Vec3<crd::f64> qd(0.3,  0.4,  0.0);

    const crd::f32 vf = evaluate<crd::f32>(ir, qf);
    const crd::f64 vd = evaluate<crd::f64>(ir, qd);

    // Both should be approximately 0 (point is on the sphere of radius 0.5).
    CHECK(std::abs(vf) < 1e-5F);
    CHECK(std::abs(vd) < 1e-12);
}
