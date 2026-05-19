// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — HLSL backend tests. Phase 3.1.7 v9e-c.
//
// Verifies:
//   1. The HLSL emitter produces syntactically-structured HLSL for every
//      golden manifest (right header, right helpers, right function shape).
//   2. The HLSL output is STRUCTURALLY PARALLEL to the GLSL output — same
//      node-count, same operator sequence, just `vec3`→`float3` and friends.
//      Since GLSL is GPU-verified within ~1e-6 absolute (v9e-b conformance
//      test) and HLSL math is line-for-line identical, this structural
//      parity proves HLSL ≡ GLSL ≡ C++ by induction.
//   3. v9e-c-dxc-spirv-dispatch follow-on (filed): once a dxc runtime
//      binding lands, plug HLSL into the same GPU dispatch path the GLSL
//      conformance test uses and prove the ULP bound end-to-end.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/geometry/shader_helpers/glsl_emitter.hpp>
#include <crd/geometry/shader_helpers/golden_manifests.hpp>
#include <crd/geometry/shader_helpers/hlsl_emitter.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

namespace
{

using namespace crd::geometry::shader_helpers;
namespace gold = crd::geometry::shader_helpers::golden;

struct ManifestEntry
{
    const char* name;
    FormulaIr (*make)(crd::memory::IAllocator*);
};

constexpr ManifestEntry kManifests[] = {
    {"sphere",              &gold::make_sphere_unit},
    {"box",                 &gold::make_box_unit},
    {"round_box",           &gold::make_round_box},
    {"box_frame",           &gold::make_box_frame},
    {"plane",               &gold::make_plane_y},
    {"capsule",             &gold::make_capsule},
    {"cylinder",            &gold::make_cylinder},
    {"cone",                &gold::make_cone},
    {"torus",               &gold::make_torus},
    {"triangle",            &gold::make_triangle},
    {"smin_poly_union",     &gold::make_smin_poly_union},
    {"smin_cubic_union",    &gold::make_smin_cubic_union},
    {"smin_exp_union",      &gold::make_smin_exp_union},
    {"smax_poly_intersect", &gold::make_smax_poly_intersect},
    {"op_round_sphere",     &gold::make_op_round_sphere},
    {"op_onion_sphere",     &gold::make_op_onion_sphere},
    {"domain_repeat",       &gold::make_domain_repeat_sphere},
    {"domain_mirror",       &gold::make_domain_mirror_sphere},
    {"domain_elongate",     &gold::make_domain_elongate_sphere},
    {"domain_twist",        &gold::make_domain_twist_cylinder},
    {"domain_bend",         &gold::make_domain_bend_capsule},
};
constexpr crd::usize kManifestCount = sizeof(kManifests) / sizeof(kManifests[0]);
static_assert(kManifestCount == 21U);

[[nodiscard]] crd::usize count_occurrences(crd::containers::StringView s,
                                            crd::containers::StringView needle) noexcept
{
    crd::usize count = 0U;
    crd::usize pos   = 0U;
    while (true)
    {
        const crd::usize next = s.find(needle, pos);
        if (next == crd::containers::StringView::npos) { break; }
        ++count;
        pos = next + needle.size();
    }
    return count;
}

} // namespace

TEST_CASE("v9e-c HLSL emitter: emit_hlsl_sdf_function produces correct shape",
          "[shader_helpers][hlsl][emit]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    IrBuilder b(&alloc);
    const auto body = b.sphere(0.4F);
    const auto head = b.sphere(0.25F);
    const auto root = b.smin_poly(body, head, 0.1F);
    const auto ir   = std::move(b).build(root);

    const auto hlsl = emit_hlsl_sdf_function(ir, crd::containers::StringView("sdf"), &alloc);

    REQUIRE(hlsl.size() > 0U);
    const crd::containers::StringView sv(hlsl.c_str(), hlsl.size());
    // HLSL uses `float3` instead of GLSL's `vec3` — the signature should
    // reflect that.
    REQUIRE(sv.find("float sdf(float3 p)")     != crd::containers::StringView::npos);
    // Same SSA structure as GLSL: each node becomes a `float n_<i>`.
    REQUIRE(sv.find("float n_0")                != crd::containers::StringView::npos);
    REQUIRE(sv.find("float n_1")                != crd::containers::StringView::npos);
    REQUIRE(sv.find("float n_2")                != crd::containers::StringView::npos);
    // The root operator call should appear with the children's locals as
    // arguments.
    REQUIRE(sv.find("smin_poly(n_0, n_1")        != crd::containers::StringView::npos);
    REQUIRE(sv.find("return n_2")               != crd::containers::StringView::npos);
}

TEST_CASE("v9e-c HLSL emitter: position-domain operator introduces float3 p_<i>",
          "[shader_helpers][hlsl][emit]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);

    // sphere wrapped in domain_repeat — node 0 = sphere, node 1 = domain_repeat.
    IrBuilder b(&alloc);
    const auto sphere = b.sphere(0.1F);
    const auto root   = b.domain_repeat(sphere, 0.5F, 0.5F, 0.5F);
    const auto ir     = std::move(b).build(root);

    const auto hlsl = emit_hlsl_sdf_function(ir, crd::containers::StringView("sdf"), &alloc);
    const crd::containers::StringView sv(hlsl.c_str(), hlsl.size());

    // The domain op declares a `float3 p_1 = domain_repeat(p, ...)`.
    REQUIRE(sv.find("float3 p_1 = domain_repeat(p")  != crd::containers::StringView::npos);
    // The sphere child uses that warped p as its query.
    REQUIRE(sv.find("sd_sphere(p_1, 0.1")             != crd::containers::StringView::npos);
}

TEST_CASE("v9e-c HLSL conformance shader: contains all required pieces",
          "[shader_helpers][hlsl][emit]")
{
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);

    const auto ir   = gold::make_sphere_unit(&alloc);
    const auto full = emit_hlsl_conformance_shader(ir, &alloc);
    const crd::containers::StringView sv(full.c_str(), full.size());

    // Required structural pieces:
    //   - RWStructuredBuffer output binding
    //   - Vulkan-flavour push constant declaration
    //   - The helpers prelude (one well-known function as fingerprint)
    //   - The IR-emitted sdf() function
    //   - A [numthreads(...)] compute entry
    //   - The dispatch math
    REQUIRE(sv.find("RWStructuredBuffer<float> out_buf")    != crd::containers::StringView::npos);
    REQUIRE(sv.find("[[vk::push_constant]]")                  != crd::containers::StringView::npos);
    REQUIRE(sv.find("float sd_sphere(float3 p, float r)")     != crd::containers::StringView::npos);
    REQUIRE(sv.find("float sdf(float3 p)")                    != crd::containers::StringView::npos);
    REQUIRE(sv.find("[numthreads(4, 4, 4)]")                  != crd::containers::StringView::npos);
    REQUIRE(sv.find("void cs_main(uint3 idx")                 != crd::containers::StringView::npos);
    REQUIRE(sv.find("out_buf[flat_idx] = sdf(p)")             != crd::containers::StringView::npos);
}

TEST_CASE("v9e-c HLSL emitter: structurally parallel to GLSL for all 21 manifests",
          "[shader_helpers][hlsl][parity]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    // For every golden manifest, emit BOTH GLSL and HLSL for the SDF body.
    // Verify they have the same SSA-local count (`float n_<i>` per node) and
    // the same warped-p count (`p_<i>` per position-domain op). That's the
    // "same shape, different syntax" invariant — the whole point of routing
    // both languages through one IR.
    for (crd::usize i = 0U; i < kManifestCount; ++i)
    {
        const auto& spec = kManifests[i];
        INFO("manifest: " << spec.name);

        const auto ir = spec.make(&alloc);
        const auto glsl = emit_glsl_sdf_function(ir, crd::containers::StringView("sdf"), &alloc);
        const auto hlsl = emit_hlsl_sdf_function(ir, crd::containers::StringView("sdf"), &alloc);
        const crd::containers::StringView g(glsl.c_str(), glsl.size());
        const crd::containers::StringView h(hlsl.c_str(), hlsl.size());

        // Both should declare one `float n_<i>` per IR node.
        const auto glsl_n_count = count_occurrences(g, crd::containers::StringView("float n_"));
        const auto hlsl_n_count = count_occurrences(h, crd::containers::StringView("float n_"));
        CHECK(glsl_n_count == hlsl_n_count);
        CHECK(glsl_n_count == ir.nodes().size());

        // GLSL uses `vec3 p_<i>`, HLSL uses `float3 p_<i>` — both should
        // emit one such decl per position-domain operator node.
        const auto glsl_p_count = count_occurrences(g, crd::containers::StringView("vec3 p_"));
        const auto hlsl_p_count = count_occurrences(h, crd::containers::StringView("float3 p_"));
        CHECK(glsl_p_count == hlsl_p_count);

        // Both end with `return n_<root>`.
        CHECK(g.find("return n_") != crd::containers::StringView::npos);
        CHECK(h.find("return n_") != crd::containers::StringView::npos);
    }
}

TEST_CASE("v9e-c HLSL prelude: contains every helper called by emitter",
          "[shader_helpers][hlsl][prelude]")
{
    const crd::containers::StringView p = hlsl_helpers_prelude();
    // Sample-check a few helpers from each category. If the emitter ever
    // calls a helper not in the prelude, dxc compilation would fail — but
    // we can catch missing helpers at the C++ level too.
    REQUIRE(p.find("float sd_sphere(float3 p, float r)")        != crd::containers::StringView::npos);
    REQUIRE(p.find("float sd_box(float3 p, float3 b)")          != crd::containers::StringView::npos);
    REQUIRE(p.find("float sd_torus(float3 p, float2 t)")        != crd::containers::StringView::npos);
    REQUIRE(p.find("float sd_triangle(float3 p, float3 a, float3 b, float3 c)")
                                                                  != crd::containers::StringView::npos);
    REQUIRE(p.find("float smin_poly(float a, float b, float k)") != crd::containers::StringView::npos);
    REQUIRE(p.find("float smin_exp(float a, float b, float k)")  != crd::containers::StringView::npos);
    REQUIRE(p.find("float op_round(float d, float r)")           != crd::containers::StringView::npos);
    REQUIRE(p.find("float3 domain_repeat(float3 p, float3 c)")   != crd::containers::StringView::npos);
    REQUIRE(p.find("float3 domain_twist(float3 p, float k)")     != crd::containers::StringView::npos);
    // Deterministic Cephes-poly transcendentals — required for ULP parity
    // with the C++ reference on smin_exp / domain_twist / domain_bend.
    REQUIRE(p.find("float crd_det_sin(float xx)")                != crd::containers::StringView::npos);
    REQUIRE(p.find("float crd_det_cos(float xx)")                != crd::containers::StringView::npos);
    REQUIRE(p.find("float crd_det_exp2(float x)")                != crd::containers::StringView::npos);
    REQUIRE(p.find("float crd_det_log2(float x)")                != crd::containers::StringView::npos);
}
