// test_ckir_draw_shaders.cpp — RET-6 (ADR-0105) gate pt 1: the CKIR draw-shader suite (ckir_draw.hpp) — the faithful
// ports of line_aa / triangle_solid / infinite_grid — LOWERS through BOTH raster emitters (GLSL + HLSL, the wire-both
// scar) with the structural landmarks of the originals intact: the u32 storage pull (intBitsToFloat recovery), the
// VertexIndex-driven expansion, the AA smoothstep, the pristine-grid fwidth. The device draw gate (pixels through a
// real swapchain target) rides the gpu-context suite; THIS gate is pure IR — no GPU, no rhi.

#include <crd/draw/ckir_draw.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
[[nodiscard]] bool contains(const crd::containers::String& s, const char* needle)
{
    return std::strstr(s.c_str(), needle) != nullptr;
}
} // namespace

TEST_CASE("RET-6: the CKIR draw suite lowers to GLSL + HLSL on both raster stages", "[draw][ckir]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    namespace kir = crd::kir;
    namespace dck = crd::draw::ckir;

    struct BuilderPair
    {
        void (*vs)(kir::KGraph&, kir::KEntry&);
        void (*fs)(kir::KGraph&, kir::KEntry&);
        const char* name;
    };
    const BuilderPair pairs[3] = {{&dck::build_line_vs, &dck::build_line_fs, "line_aa"},
                                  {&dck::build_tri_vs, &dck::build_tri_fs, "triangle_solid"},
                                  {&dck::build_grid_vs, &dck::build_grid_fs, "infinite_grid"}};

    for (const BuilderPair& p : pairs)
    {
        INFO(p.name);
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        p.vs(vg, ve);
        REQUIRE(ve.stage == kir::KStage::Vertex);
        REQUIRE(ve.position >= 0);

        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        p.fs(fg, fe);
        REQUIRE(fe.stage == kir::KStage::Fragment);
        REQUIRE(fe.n_out == 1);

        kir::GlslKernel gv(&alloc);
        REQUIRE(kir::emit_stage_glsl(vg, ve, &alloc, gv));
        kir::GlslKernel gf(&alloc);
        REQUIRE(kir::emit_stage_glsl(fg, fe, &alloc, gf));

        kir::GlslKernel hv(&alloc);
        REQUIRE(kir::emit_stage_hlsl(vg, ve, &alloc, hv));
        kir::GlslKernel hf(&alloc);
        REQUIRE(kir::emit_stage_hlsl(fg, fe, &alloc, hf));

        // the vertex pull's landmarks: VertexIndex drives the expansion, floats recover via bit reinterpretation
        CHECK(contains(gv.source, "gl_VertexIndex"));
        CHECK(contains(gv.source, "intBitsToFloat"));
        CHECK(contains(hv.source, "asfloat"));
    }

    // per-shader landmarks of the original GLSL math
    {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        dck::build_line_fs(fg, fe);
        kir::GlslKernel gf(&alloc);
        REQUIRE(kir::emit_stage_glsl(fg, fe, &alloc, gf));
        CHECK(contains(gf.source, "smoothstep")); // the 1-pixel AA falloff
    }
    {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        dck::build_grid_fs(fg, fe);
        kir::GlslKernel gf(&alloc);
        REQUIRE(kir::emit_stage_glsl(fg, fe, &alloc, gf));
        CHECK(contains(gf.source, "fwidth")); // the pristine-grid derivative AA
        CHECK(contains(gf.source, "fract"));
    }
}
