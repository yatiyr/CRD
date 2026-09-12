// test_ckir_draw_shaders.cpp — REN-38-F7 gate: the debug-draw suite is AUTHORED (.crdv + .crdm). The 339-line
// hand-written CKIR builder (`ckir_draw.hpp`) is DELETED — THIS gate cooks the same three programs (line_aa /
// triangle_solid / infinite_grid) from the declarations `crd-draw`'s init embeds, proves each pair against the
// 38-D4 varying contract, and lowers through BOTH raster emitters (GLSL + HLSL, the wire-both scar) with the
// structural landmarks of the originals intact: the record pull's bit reinterpretation, the VertexIndex-driven
// expansion, the AA smoothstep, the pristine-grid fwidth. The device pixel gate rides the gpu-context suite.

#include <crd/draw/draw_assets.hpp>
#include <crd/kir/ckir_cook.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_material.hpp>
#include <crd/kir/ckir_technique.hpp>
#include <crd/matcook/material_asset.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/vertexcook/vertex_asset.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>

namespace
{
[[nodiscard]] bool contains(const crd::containers::String& s, const char* needle)
{
    return std::strstr(s.c_str(), needle) != nullptr;
}

// The FS composition crd-draw's init runs: the authored material through `unlit` with CONSTANT surface inputs.
[[nodiscard]] bool cook_draw_fs(crd::memory::IAllocator* alloc, const char* toml, crd::kir::KGraph& g,
                                crd::kir::KEntry& e)
{
    namespace ck = crd::kir::cook;
    namespace tq = crd::kir::technique;
    crd::matcook::MaterialDesc d(alloc);
    crd::containers::String    where(alloc);
    if (crd::matcook::parse_material_toml(crd::containers::StringView(toml), d, &where)
        != crd::matcook::MaterialCookError::Ok)
    {
        return false;
    }
    const auto k = [&](double v) { return g.constant(v, crd::kir::make_shape({1}), crd::kir::DType::F32); };
    ck::SurfaceInputs in;
    in.world_normal = g.vec3(k(0.0), k(0.0), k(1.0));
    in.world_pos    = g.vec3(k(0.0), k(0.0), k(0.0));
    in.view_dir     = g.vec3(k(0.0), k(0.0), k(1.0));
    const auto surface_thunk = [](crd::kir::KGraph& gg, int sid, const ck::SurfaceInputs&, void* user) {
        return crd::matcook::cook_material(*static_cast<const crd::matcook::MaterialDesc*>(user), gg, sid);
    };
    const ck::MaterialTemplate tmpl{surface_thunk, &d};
    const ck::VariantOptions   opts{crd::kir::material::AlphaMode::Opaque, 0.5};
    const tq::Technique        un = tq::unlit();
    return tq::build_fs_for_pass(tmpl, un, ck::PassType::Forward, opts, in, g, e,
                                 g.vec3(k(0.0), k(0.0), k(1.0)), g.vec3(k(1.0), k(1.0), k(1.0)), nullptr, 0,
                                 nullptr, 0);
}
} // namespace

TEST_CASE("REN-38-F7: the AUTHORED draw suite cooks, honours the varying contract, lowers to GLSL + HLSL",
          "[draw][ckir][ren38]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    namespace kir = crd::kir;
    namespace vc  = crd::vertcook;

    struct AssetPair
    {
        const char* vs;
        const char* fs;
        const char* name;
    };
    const AssetPair pairs[3] = {{crd::draw::kDrawLineVs, crd::draw::kDrawLineMat, "line_aa"},
                                {crd::draw::kDrawTriVs, crd::draw::kDrawTriMat, "triangle_solid"},
                                {crd::draw::kDrawGridVs, crd::draw::kDrawGridMat, "infinite_grid"}};

    for (const AssetPair& p : pairs)
    {
        INFO(p.name);
        vc::VertexProgramDesc   vd(&alloc);
        crd::containers::String where(&alloc);
        REQUIRE(vc::parse_vertex_toml(crd::containers::StringView(p.vs), vd, &where)
                == vc::VertexCookError::Ok);
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        REQUIRE(vc::cook_vertex_program(vd, vg, ve));
        REQUIRE(ve.stage == kir::KStage::Vertex);
        REQUIRE(ve.position >= 0);
        {
            const char* why      = nullptr;
            const bool  valid_vs = kir::entry_valid(vg, ve, &why);
            INFO((why == nullptr ? "" : why));
            CHECK(valid_vs);
        }

        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        REQUIRE(cook_draw_fs(&alloc, p.fs, fg, fe));
        REQUIRE(fe.stage == kir::KStage::Fragment);
        REQUIRE(fe.n_out == 1);

        // ⛔ the 38-D4 LIVE contract, per pair: every varying the cooked FS reads — location, width,
        // interpolation — must be one the authored VS declares. The mismatch class no validation layer sees.
        vc::VaryingRequirement reqs[vc::kMaxVaryings];
        crd::u32               n_reqs = 0U;
        REQUIRE(vc::fs_varying_requirements(fg, fe, reqs, vc::kMaxVaryings, &n_reqs, &alloc));
        CHECK(vc::verify_varying_contract(vd, reqs, n_reqs, &where) == vc::VertexCookError::Ok);

        kir::GlslKernel gv(&alloc);
        REQUIRE(kir::emit_stage_glsl(vg, ve, &alloc, gv));
        kir::GlslKernel gf(&alloc);
        REQUIRE(kir::emit_stage_glsl(fg, fe, &alloc, gf));
        kir::GlslKernel hv(&alloc);
        REQUIRE(kir::emit_stage_hlsl(vg, ve, &alloc, hv));
        kir::GlslKernel hf(&alloc);
        REQUIRE(kir::emit_stage_hlsl(fg, fe, &alloc, hf));

        // the expansion's landmarks: VertexIndex drives it; record/header floats recover via bit reinterpretation
        CHECK(contains(gv.source, "gl_VertexIndex"));
        CHECK(contains(gv.source, "intBitsToFloat"));
        CHECK(contains(hv.source, "asfloat"));
    }

    // per-shader landmarks of the original math, surviving the authored form
    {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        REQUIRE(cook_draw_fs(&alloc, crd::draw::kDrawLineMat, fg, fe));
        kir::GlslKernel gf(&alloc);
        REQUIRE(kir::emit_stage_glsl(fg, fe, &alloc, gf));
        CHECK(contains(gf.source, "smoothstep")); // the 1-pixel AA falloff

        // ⛔⛔ PRESENCE IS NOT CORRECTNESS — and this exact `contains("smoothstep")` check is what let the bug
        // below ship. `smoothstep`'s arguments are MaterialX-ordered in a `.crdm` (`in, low, high`), GLSL-ordered
        // in the emitted shader (`low, high, in`); the material declared them GLSL-style, so the cooked call came
        // out as `smoothstep(1.0, d, edge0)` — a REVERSED range whose value depends on the very quantity it is
        // meant to bound. Live: every debug line rendered narrow and translucent, and fragments whose interpolated
        // |cy| landed just past 1 clamped the other way to FULL opacity — the 1-pixel, intermittent "dotted line
        // beside the gizmo". So gate the OPERAND ROLES on the graph, not the spelling:
        //   x    (KNode::c) must be the ABS of the quad coordinate — the value being tested
        //   low  (KNode::a) must be the SUB (1 - fade)            — the ramp's start
        // A rotation of the three inputs moves the abs out of `c` and this fails immediately.
        {
            int n_ss = 0;
            for (crd::usize i = 0; i < fg.serial_nodes().size(); ++i)
            {
                const kir::KNode& nd = fg.serial_nodes()[i];
                if (nd.op != kir::KOp::Smoothstep) { continue; }
                ++n_ss;
                REQUIRE(nd.a >= 0);
                REQUIRE(nd.c >= 0);
                CHECK(fg.node(nd.c).op == kir::KOp::Abs); // the VALUE rides the third operand
                CHECK(fg.node(nd.a).op == kir::KOp::Sub); // the RANGE START rides the first
            }
            CHECK(n_ss == 1); // exactly one AA falloff in the line material
        }
    }
    {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        REQUIRE(cook_draw_fs(&alloc, crd::draw::kDrawGridMat, fg, fe));
        kir::GlslKernel gf(&alloc);
        REQUIRE(kir::emit_stage_glsl(fg, fe, &alloc, gf));
        CHECK(contains(gf.source, "fwidth")); // the pristine-grid derivative AA (the new registry node)
    }
}

// ⛔ Two copies of one declaration drift — the embedded pack and the shipped `assets/` files are PINNED to one
// canonical form, exactly like the scene renderer's pack. `CRD_ASSETS_DIR` comes from ctest; without it the
// gate SKIPS rather than passing on nothing.
TEST_CASE("REN-38-F7 DRIFT GATE: the shipped draw assets match the embedded pack, canonically",
          "[draw][ren38]")
{
    const char* root = std::getenv("CRD_ASSETS_DIR");
    if (root == nullptr || root[0] == '\0') { SKIP("CRD_ASSETS_DIR not set (run through ctest)"); }
    crd::memory::TlsfAllocator alloc(32U << 20U);
    namespace vc = crd::vertcook;
    namespace mc = crd::matcook;

    const auto read_shipped = [&](const char* rel, crd::containers::String& out) {
        crd::containers::String p(&alloc);
        p.append(root);
        p.append("/");
        p.append(rel);
        return crd::platform::fs::read_file_text(
            crd::platform::fs::Path(crd::containers::StringView(p.c_str(), p.size())), out);
    };
    const auto same = [&](const crd::containers::String& a, const crd::containers::String& b) {
        return a.size() == b.size() && std::memcmp(a.c_str(), b.c_str(), a.size()) == 0;
    };

    const auto check_vertex = [&](const char* rel) {
        INFO(rel);
        crd::containers::String shipped(&alloc);
        crd::containers::String embedded(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        REQUIRE(crd::draw::builtin_draw_asset_text(rel, embedded));
        vc::VertexProgramDesc   a(&alloc);
        vc::VertexProgramDesc   b(&alloc);
        crd::containers::String where(&alloc);
        REQUIRE(vc::parse_vertex_toml(crd::containers::StringView(shipped.c_str(), shipped.size()), a, &where)
                == vc::VertexCookError::Ok);
        REQUIRE(vc::parse_vertex_toml(crd::containers::StringView(embedded.c_str(), embedded.size()), b, &where)
                == vc::VertexCookError::Ok);
        CHECK(same(vc::emit_vertex_toml(a, &alloc), vc::emit_vertex_toml(b, &alloc)));
    };
    const auto check_material = [&](const char* rel) {
        INFO(rel);
        crd::containers::String shipped(&alloc);
        crd::containers::String embedded(&alloc);
        REQUIRE(read_shipped(rel, shipped));
        REQUIRE(crd::draw::builtin_draw_asset_text(rel, embedded));
        mc::MaterialDesc        a(&alloc);
        mc::MaterialDesc        b(&alloc);
        crd::containers::String where(&alloc);
        REQUIRE(mc::parse_material_toml(crd::containers::StringView(shipped.c_str(), shipped.size()), a, &where)
                == mc::MaterialCookError::Ok);
        REQUIRE(mc::parse_material_toml(crd::containers::StringView(embedded.c_str(), embedded.size()), b, &where)
                == mc::MaterialCookError::Ok);
        CHECK(same(mc::emit_material_toml(a, &alloc), mc::emit_material_toml(b, &alloc)));
    };
    check_vertex("vertex/draw_line.crdv");
    check_vertex("vertex/draw_tri.crdv");
    check_vertex("vertex/draw_grid.crdv");
    check_material("material/draw_line.crdm");
    check_material("material/draw_tri.crdm");
    check_material("material/draw_grid.crdm");
}
