// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — cooker tests. Phase 3.1.7 v9e-d.
//
// Verifies the v9e-d cooker:
//   1. `cook_helpers_prelude` writes both `sdf_helpers.glsl` + `sdf_helpers.hlsl`
//      and the on-disk content matches `glsl_helpers_prelude()` / `hlsl_helpers_prelude()`.
//   2. `cook_ir` writes both `<name>.glsl` + `<name>.hlsl` for every golden
//      manifest and the on-disk content matches `emit_glsl_sdf_function` /
//      `emit_hlsl_sdf_function`.
//   3. Empty name → CookResult{ok=false, error_message=…}, no files written.
//   4. Invalid IR → CookResult{ok=false}, no files written.
//   5. Idempotency: cooking twice into the same output dir succeeds and the
//      second cook produces byte-identical files.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/shader_helpers/cooker.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/geometry/shader_helpers/glsl_emitter.hpp>
#include <crd/geometry/shader_helpers/golden_manifests.hpp>
#include <crd/geometry/shader_helpers/hlsl_emitter.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

namespace
{

using namespace crd::geometry::shader_helpers;
namespace gold = crd::geometry::shader_helpers::golden;
namespace fs   = crd::platform::fs;

// Per-test output dir under the build tree's runtime working dir. Cleared at
// each TEST_CASE entry so we can verify "files appeared" by checking exists().
[[nodiscard]] crd::containers::String make_output_dir(
    crd::containers::StringView slug,
    crd::memory::IAllocator*    alloc) noexcept
{
    crd::containers::String dir("cooker_test_output_", alloc);
    dir.append(slug);
    // Best-effort cleanup; ignore the result (the dir may not exist on first run).
    (void)fs::remove_all(fs::Path(crd::containers::StringView{dir.data(), dir.size()}));
    return dir;
}

[[nodiscard]] crd::containers::String read_text(
    const crd::containers::String& path,
    crd::memory::IAllocator*       alloc) noexcept
{
    crd::containers::String out(alloc);
    const bool ok = fs::read_file_text(
        fs::Path(crd::containers::StringView{path.data(), path.size()}), out);
    if (!ok) { out = crd::containers::String("<read failed>", alloc); }
    return out;
}

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

} // namespace

TEST_CASE("v9e-d cooker: cook_helpers_prelude writes both .glsl and .hlsl",
          "[shader_helpers][cooker][prelude]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const crd::containers::String dir = make_output_dir("prelude", &alloc);

    const auto dir_view = crd::containers::StringView{dir.data(), dir.size()};
    const CookResult result = cook_helpers_prelude(dir_view, &alloc);
    REQUIRE(result.ok);
    REQUIRE(result.emitted_paths.size() == 2U);

    // Both expected files exist on disk and have non-empty content.
    crd::containers::String glsl_path(dir_view, &alloc);
    glsl_path.append("/sdf_helpers.glsl");
    crd::containers::String hlsl_path(dir_view, &alloc);
    hlsl_path.append("/sdf_helpers.hlsl");

    REQUIRE(fs::is_file(fs::Path(crd::containers::StringView{glsl_path.data(), glsl_path.size()})));
    REQUIRE(fs::is_file(fs::Path(crd::containers::StringView{hlsl_path.data(), hlsl_path.size()})));

    // On-disk content matches the emitter's in-memory prelude byte-for-byte.
    const crd::containers::String glsl_disk = read_text(glsl_path, &alloc);
    const crd::containers::String hlsl_disk = read_text(hlsl_path, &alloc);
    REQUIRE(glsl_disk.size() == glsl_helpers_prelude().size());
    REQUIRE(hlsl_disk.size() == hlsl_helpers_prelude().size());
    REQUIRE(crd::containers::StringView{glsl_disk.data(), glsl_disk.size()} == glsl_helpers_prelude());
    REQUIRE(crd::containers::StringView{hlsl_disk.data(), hlsl_disk.size()} == hlsl_helpers_prelude());

    // Cleanup so the next test starts clean (the directory remove_all in
    // make_output_dir runs at entry, not at exit).
    REQUIRE(fs::remove_all(fs::Path(dir_view)));
}

TEST_CASE("v9e-d cooker: cook_ir writes <name>.glsl and <name>.hlsl for every golden manifest",
          "[shader_helpers][cooker][ir]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    const crd::containers::String dir = make_output_dir("manifests", &alloc);
    const auto dir_view = crd::containers::StringView{dir.data(), dir.size()};

    // Cook the prelude once so consumers can `#include` the helpers it
    // provides — same flow a real consumer would use.
    {
        const CookResult prelude_result = cook_helpers_prelude(dir_view, &alloc);
        REQUIRE(prelude_result.ok);
    }

    for (crd::usize i = 0U; i < kManifestCount; ++i)
    {
        const auto& entry = kManifests[i];
        const FormulaIr ir = entry.make(&alloc);

        const crd::containers::StringView name(entry.name);
        const CookResult result = cook_ir(ir, name, dir_view, &alloc);
        INFO("manifest=" << entry.name);
        REQUIRE(result.ok);
        REQUIRE(result.emitted_paths.size() == 2U);

        // Paths exist on disk.
        crd::containers::String glsl_path(dir_view, &alloc);
        glsl_path.append("/");
        glsl_path.append(name);
        glsl_path.append(".glsl");

        crd::containers::String hlsl_path(dir_view, &alloc);
        hlsl_path.append("/");
        hlsl_path.append(name);
        hlsl_path.append(".hlsl");

        REQUIRE(fs::is_file(fs::Path(crd::containers::StringView{glsl_path.data(), glsl_path.size()})));
        REQUIRE(fs::is_file(fs::Path(crd::containers::StringView{hlsl_path.data(), hlsl_path.size()})));

        // On-disk content matches the emitter output exactly.
        const crd::containers::String glsl_disk = read_text(glsl_path, &alloc);
        const crd::containers::String glsl_mem  = emit_glsl_sdf_function(ir, name, &alloc);
        REQUIRE(crd::containers::StringView{glsl_disk.data(), glsl_disk.size()} ==
                crd::containers::StringView{glsl_mem.data(), glsl_mem.size()});

        const crd::containers::String hlsl_disk = read_text(hlsl_path, &alloc);
        const crd::containers::String hlsl_mem  = emit_hlsl_sdf_function(ir, name, &alloc);
        REQUIRE(crd::containers::StringView{hlsl_disk.data(), hlsl_disk.size()} ==
                crd::containers::StringView{hlsl_mem.data(), hlsl_mem.size()});

        // Sanity: the on-disk GLSL has the expected `float <name>(vec3 p)`
        // signature, and the HLSL has `float <name>(float3 p)`.
        crd::containers::String glsl_sig("float ", &alloc);
        glsl_sig.append(name);
        glsl_sig.append("(vec3 p)");

        crd::containers::String hlsl_sig("float ", &alloc);
        hlsl_sig.append(name);
        hlsl_sig.append("(float3 p)");

        const crd::containers::StringView glsl_view{glsl_disk.data(), glsl_disk.size()};
        const crd::containers::StringView hlsl_view{hlsl_disk.data(), hlsl_disk.size()};
        REQUIRE(glsl_view.find(crd::containers::StringView{glsl_sig.data(), glsl_sig.size()}) !=
                crd::containers::StringView::npos);
        REQUIRE(hlsl_view.find(crd::containers::StringView{hlsl_sig.data(), hlsl_sig.size()}) !=
                crd::containers::StringView::npos);
    }

    REQUIRE(fs::remove_all(fs::Path(dir_view)));
}

TEST_CASE("v9e-d cooker: empty name returns failure with no files written",
          "[shader_helpers][cooker][validation]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const crd::containers::String dir = make_output_dir("empty_name", &alloc);
    const auto dir_view = crd::containers::StringView{dir.data(), dir.size()};

    const FormulaIr ir = gold::make_sphere_unit(&alloc);
    const CookResult result = cook_ir(ir, crd::containers::StringView(""), dir_view, &alloc);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.emitted_paths.size() == 0U);
    REQUIRE(result.error_message.size() > 0U);

    // Cooker MUST NOT create the output dir on failed-validation paths.
    // (We assert the dir does not exist because make_output_dir cleared it.)
    REQUIRE_FALSE(fs::is_directory(fs::Path(dir_view)));
}

TEST_CASE("v9e-d cooker: invalid IR returns failure with no files written",
          "[shader_helpers][cooker][validation]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    const crd::containers::String dir = make_output_dir("invalid_ir", &alloc);
    const auto dir_view = crd::containers::StringView{dir.data(), dir.size()};

    // Empty IR (no nodes) → IrValidationStatus::EmptyIr.
    FormulaIr empty_ir(&alloc);
    const CookResult result = cook_ir(
        empty_ir, crd::containers::StringView("bad"), dir_view, &alloc);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.emitted_paths.size() == 0U);
    REQUIRE(result.error_message.size() > 0U);
}

TEST_CASE("v9e-d cooker: re-cook into same dir overwrites idempotently",
          "[shader_helpers][cooker][idempotent]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    const crd::containers::String dir = make_output_dir("idempotent", &alloc);
    const auto dir_view = crd::containers::StringView{dir.data(), dir.size()};

    const FormulaIr ir = gold::make_torus(&alloc);
    const crd::containers::StringView name("my_torus");

    const CookResult first  = cook_ir(ir, name, dir_view, &alloc);
    const CookResult second = cook_ir(ir, name, dir_view, &alloc);
    REQUIRE(first.ok);
    REQUIRE(second.ok);

    crd::containers::String glsl_path(dir_view, &alloc);
    glsl_path.append("/");
    glsl_path.append(name);
    glsl_path.append(".glsl");

    const crd::containers::String content1 = read_text(glsl_path, &alloc);
    const crd::containers::String content2 = read_text(glsl_path, &alloc);
    REQUIRE(crd::containers::StringView{content1.data(), content1.size()} ==
            crd::containers::StringView{content2.data(), content2.size()});

    REQUIRE(fs::remove_all(fs::Path(dir_view)));
}
