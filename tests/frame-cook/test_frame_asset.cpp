// test_frame_asset.cpp — REN-36.1 GATE (D-007 row 139): the authorable frame graph's schema + cooker.
//
// Two claims, both load-bearing for the whole "everything is an asset" direction:
//   1. A VALID graph parses, cooks, and ROUND-TRIPS BYTE-IDENTICALLY (the cooked form is canonical, packed and
//      padding-free, so a graph cooked under MSVC loads unchanged under gcc/clang — the ckir_serialize lesson).
//   2. EVERY way a graph can be malformed is REJECTED BY NAME AT COOK TIME. A shipped graph must already be
//      proven well-formed; a player's machine is not where a typo should surface.

#include <crd/framecook/frame_asset.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace fc = crd::framecook;

namespace
{
// The engine's default forward+shadow graph — the worked example from the REN-36 spec, and the shape that ships
// in the built-in pack. Exercises: layered depth atlas, for_each cascades, ECS-query draw lists, a compute-free
// raster chain, params, and @output.
constexpr const char* kValidGraph = R"(
schema = 1
name   = "forward_shadowed"
requires = ["bindless"]
fallback = "crd://frames/forward_basic"

[[resource]]
name = "shadow_atlas"
kind = "transient_image"
format = "D32Float"
width = 2048
height = 2048
layers = 4
sampled = true

[[resource]]
name = "hdr"
format = "RGBA16F"
scale = 1.0
sampled = true

[[draw_list]]
name = "shadow_casters"
all  = ["MeshRenderer", "Transform"]
none = ["NoShadowCast"]
cull = "frustum"
sort = "front_to_back"

[[draw_list]]
name = "visible_opaque"
all  = ["MeshRenderer", "Transform"]
none = ["Transparent"]
cull = "frustum"
sort = "material"

[[pass]]
name = "shadow"
kind = "raster.depth_only"
for_each = "light.0.cascades"
writes = ["shadow_atlas[$index]"]
draw_list = "shadow_casters"
view = "light.0.cascade[$index]"
clear_depth = 1.0
depth = "LessEqual"
material_pass = "Shadow"

[[pass]]
name = "forward"
kind = "raster.geometry"
reads = ["shadow_atlas"]
writes = ["hdr"]
draw_list = "visible_opaque"
view = "camera.main"
material_pass = "Forward"
clear_color = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name = "tonemap"
kind = "raster.fullscreen"
reads = ["hdr"]
writes = ["@output"]
shader = "crd://shaders/post/agx_tonemap"
[pass.params]
exposure_ev100 = 13.5
)";

// Parse `text` and return the error (the `where` string receives the offending name).
fc::FrameCookError parse_of(const char* text, crd::memory::IAllocator* a, crd::containers::String* where = nullptr)
{
    fc::FrameGraphDesc d(a);
    return fc::parse_frame_toml(crd::containers::StringView(text, std::strlen(text)), d, where);
}
} // namespace

TEST_CASE("REN-36.1: a valid .frame.toml parses, and every field survives", "[framecook][ren36]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    fc::FrameGraphDesc         d(&alloc);
    crd::containers::String    where(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kValidGraph, std::strlen(kValidGraph)), d, &where)
            == fc::FrameCookError::Ok);

    CHECK(d.schema == fc::kFrameSchemaVersion);
    CHECK(std::strcmp(d.name.c_str(), "forward_shadowed") == 0);
    CHECK(std::strcmp(d.fallback.c_str(), "crd://frames/forward_basic") == 0);
    REQUIRE(d.requires_caps.size() == 1U);
    CHECK(std::strcmp(d.requires_caps[0].c_str(), "bindless") == 0);

    REQUIRE(d.resources.size() == 2U);
    CHECK(d.resources[0].format == crd::gpu::FgImageFormat::D32Float);
    CHECK(d.resources[0].layers == 4U);
    CHECK(d.resources[0].sampled);
    CHECK(d.resources[1].scale == 1.0F);

    REQUIRE(d.draw_lists.size() == 2U);
    CHECK(d.draw_lists[0].all.size() == 2U);   // the ECS query the GRAPH declares (user-locked answer #1)
    CHECK(d.draw_lists[0].none.size() == 1U);
    CHECK(d.draw_lists[0].sort == fc::FrameSortMode::FrontToBack);
    CHECK(d.draw_lists[1].sort == fc::FrameSortMode::Material);

    REQUIRE(d.passes.size() == 3U);
    CHECK(d.passes[0].kind == fc::FramePassKind::RasterDepthOnly);
    CHECK(d.passes[0].for_each == fc::FrameForEach::LightCascades); // user-locked answer #2
    CHECK(d.passes[0].for_each_arg == 0U);                          // light.0
    REQUIRE(d.passes[0].writes.size() == 1U);
    CHECK(d.passes[0].writes[0].indexed);                           // shadow_atlas[$index]
    CHECK(d.passes[0].material_pass == fc::FrameMaterialPass::Shadow);
    CHECK(d.passes[0].has_clear_depth);
    CHECK(d.passes[1].has_clear_color);
    CHECK(d.passes[2].kind == fc::FramePassKind::RasterFullscreen);
    REQUIRE(d.passes[2].params.size() == 1U);
    CHECK(std::strcmp(d.passes[2].params[0].name.c_str(), "exposure_ev100") == 0);
    CHECK(d.passes[2].params[0].v[0] == 13.5);
}

TEST_CASE("REN-36.1: the cooked blob ROUND-TRIPS byte-identically (canonical, padding-free)", "[framecook][ren36]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    fc::FrameGraphDesc         d(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kValidGraph, std::strlen(kValidGraph)), d)
            == fc::FrameCookError::Ok);

    const auto blob = fc::cook_frame_graph(d, &alloc);
    CHECK(blob.size() > 64U);

    fc::FrameGraphDesc back(&alloc);
    REQUIRE(fc::read_frame_graph(crd::containers::ConstSpan<crd::u8>(blob.data(), blob.size()), back));
    CHECK(back.resources.size() == d.resources.size());
    CHECK(back.draw_lists.size() == d.draw_lists.size());
    CHECK(back.passes.size() == d.passes.size());
    CHECK(std::strcmp(back.name.c_str(), "forward_shadowed") == 0);
    CHECK(back.passes[0].for_each == fc::FrameForEach::LightCascades);
    CHECK(back.passes[0].writes[0].indexed);
    CHECK(back.passes[2].params[0].v[0] == 13.5);

    // re-cook the DESERIALIZED description: byte-identical ⇒ the encoding is a pure function of the content
    const auto blob2 = fc::cook_frame_graph(back, &alloc);
    REQUIRE(blob2.size() == blob.size());
    CHECK(std::memcmp(blob2.data(), blob.data(), blob.size()) == 0);
}

TEST_CASE("REN-36.1: a truncated or corrupt blob is REJECTED, never partially read", "[framecook][ren36]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    fc::FrameGraphDesc         d(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kValidGraph, std::strlen(kValidGraph)), d)
            == fc::FrameCookError::Ok);
    const auto blob = fc::cook_frame_graph(d, &alloc);

    fc::FrameGraphDesc t(&alloc);
    CHECK_FALSE(fc::read_frame_graph(crd::containers::ConstSpan<crd::u8>(blob.data(), blob.size() / 2U), t));

    crd::containers::Array<crd::u8> bad(&alloc);
    for (crd::usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
    bad[0] = static_cast<crd::u8>(bad[0] + 1U); // corrupt the FourCC
    fc::FrameGraphDesc t2(&alloc);
    CHECK_FALSE(fc::read_frame_graph(crd::containers::ConstSpan<crd::u8>(bad.data(), bad.size()), t2));
}

// ── EVERY named rejection fires. This is the "errors belong at COOK, not on a player's machine" contract. ──
TEST_CASE("REN-36.1: every malformed graph is rejected BY NAME at cook time", "[framecook][ren36]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("not valid TOML")
    {
        CHECK(parse_of("schema = = 1", &alloc) == fc::FrameCookError::ParseFailed);
    }
    SECTION("missing or wrong schema")
    {
        CHECK(parse_of("name = \"x\"", &alloc) == fc::FrameCookError::BadSchema);
        CHECK(parse_of("schema = 99\nname = \"x\"", &alloc) == fc::FrameCookError::BadSchema);
    }
    SECTION("missing name")
    {
        CHECK(parse_of("schema = 1", &alloc) == fc::FrameCookError::MissingName);
    }
    SECTION("unknown pass kind")
    {
        crd::containers::String w(&alloc);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.wat\"\n", &alloc, &w)
              == fc::FrameCookError::UnknownPassKind);
        CHECK(std::strcmp(w.c_str(), "raster.wat") == 0); // the OFFENDING name is reported, not just the class
    }
    SECTION("unknown format")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"r\"\nformat=\"BGR9\"\nwidth=4\nheight=4\n", &alloc)
              == fc::FrameCookError::UnknownFormat);
    }
    SECTION("unknown depth compare / sort / cull / material_pass / for_each")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.fullscreen\"\ndepth=\"Sideways\"\n", &alloc)
              == fc::FrameCookError::UnknownCompare);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[draw_list]]\nname=\"d\"\nsort=\"by_vibes\"\n", &alloc)
              == fc::FrameCookError::UnknownSort);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[draw_list]]\nname=\"d\"\ncull=\"psychic\"\n", &alloc)
              == fc::FrameCookError::UnknownCull);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.geometry\"\nmaterial_pass=\"Vibes\"\n", &alloc)
              == fc::FrameCookError::UnknownMaterialPass);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.geometry\"\nfor_each=\"light.x.cascades\"\n", &alloc)
              == fc::FrameCookError::UnknownForEach);
    }
    SECTION("duplicate names within a category")
    {
        crd::containers::String w(&alloc);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"r\"\nwidth=4\nheight=4\n"
                       "[[resource]]\nname=\"r\"\nwidth=4\nheight=4\n", &alloc, &w)
              == fc::FrameCookError::DuplicateName);
        CHECK(std::strcmp(w.c_str(), "r") == 0);
    }
    SECTION("a resource with neither size nor scale")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"r\"\nformat=\"RGBA16F\"\n", &alloc)
              == fc::FrameCookError::BadResourceSize);
    }
    SECTION("a pass reads a resource nobody declared")
    {
        crd::containers::String w(&alloc);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.fullscreen\"\n"
                       "shader=\"s\"\nreads=[\"ghost\"]\nwrites=[\"@output\"]\n", &alloc, &w)
              == fc::FrameCookError::UnknownResource);
        CHECK(std::strcmp(w.c_str(), "ghost") == 0);
    }
    SECTION("a declared resource nobody writes")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"orphan\"\nwidth=4\nheight=4\n"
                       "[[pass]]\nname=\"p\"\nkind=\"raster.fullscreen\"\nshader=\"s\"\nwrites=[\"@output\"]\n", &alloc)
              == fc::FrameCookError::ResourceNeverWritten);
    }
    SECTION("a dependency CYCLE")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n"
                       "[[resource]]\nname=\"a\"\nwidth=4\nheight=4\n"
                       "[[resource]]\nname=\"b\"\nwidth=4\nheight=4\n"
                       "[[pass]]\nname=\"p1\"\nkind=\"raster.fullscreen\"\nshader=\"s\"\nreads=[\"b\"]\nwrites=[\"a\"]\n"
                       "[[pass]]\nname=\"p2\"\nkind=\"raster.fullscreen\"\nshader=\"s\"\nreads=[\"a\"]\nwrites=[\"b\",\"@output\"]\n",
                       &alloc)
              == fc::FrameCookError::DependencyCycle);
    }
    SECTION("a fullscreen pass with no shader / a compute pass with no kernel")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.fullscreen\"\nwrites=[\"@output\"]\n", &alloc)
              == fc::FrameCookError::MissingShader);
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"compute\"\nwrites=[\"@output\"]\n", &alloc)
              == fc::FrameCookError::MissingShader);
    }
    SECTION("a geometry pass with no draw list")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[pass]]\nname=\"p\"\nkind=\"raster.geometry\"\nwrites=[\"@output\"]\n", &alloc)
              == fc::FrameCookError::MissingDrawList);
    }
    SECTION("[$index] on a non-layered resource")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"flat\"\nwidth=4\nheight=4\n"
                       "[[draw_list]]\nname=\"d\"\n"
                       "[[pass]]\nname=\"p\"\nkind=\"raster.depth_only\"\ndraw_list=\"d\"\n"
                       "for_each=\"cube.faces\"\nwrites=[\"flat[$index]\",\"@output\"]\n", &alloc)
              == fc::FrameCookError::SubscriptOnNonLayered);
    }
    SECTION("[$index] without a for_each")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"atlas\"\nwidth=4\nheight=4\nlayers=4\n"
                       "[[draw_list]]\nname=\"d\"\n"
                       "[[pass]]\nname=\"p\"\nkind=\"raster.depth_only\"\ndraw_list=\"d\"\n"
                       "writes=[\"atlas[$index]\",\"@output\"]\n", &alloc)
              == fc::FrameCookError::IndexWithoutForEach);
    }
    SECTION("nothing writes @output")
    {
        CHECK(parse_of("schema=1\nname=\"g\"\n[[resource]]\nname=\"r\"\nwidth=4\nheight=4\n"
                       "[[pass]]\nname=\"p\"\nkind=\"raster.fullscreen\"\nshader=\"s\"\nwrites=[\"r\"]\n", &alloc)
              == fc::FrameCookError::NoOutputPass);
    }
}

// ── THE EDITOR ROUND-TRIP. A node editor loads a graph, the user edits it, and it saves back to .frame.toml. ──
// The claim that makes that safe is LOSSLESSNESS, and it is gated rather than asserted: the emitted text must
// re-parse to a description that COOKS TO THE SAME BYTES. If emit ever dropped a field it did not understand,
// the cooked bytes would diverge and this fails — so an editor (or an agent) cannot silently damage a file a
// human hand-authored.
TEST_CASE("REN-36.2: emit -> parse -> cook is BYTE-IDENTICAL to the original cook (lossless editor save)",
          "[framecook][ren36]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);

    fc::FrameGraphDesc original(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kValidGraph, std::strlen(kValidGraph)), original)
            == fc::FrameCookError::Ok);
    const auto cooked_original = fc::cook_frame_graph(original, &alloc);

    // save
    const crd::containers::String text = fc::emit_frame_toml(original, &alloc);
    CHECK(text.size() > 64U);

    // load what we saved
    fc::FrameGraphDesc reloaded(&alloc);
    crd::containers::String where(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(text.c_str(), text.size()), reloaded, &where)
            == fc::FrameCookError::Ok);

    const auto cooked_reloaded = fc::cook_frame_graph(reloaded, &alloc);
    REQUIRE(cooked_reloaded.size() == cooked_original.size());
    CHECK(std::memcmp(cooked_reloaded.data(), cooked_original.data(), cooked_original.size()) == 0);

    // spot-check the fields most likely to be silently dropped by a naive emitter
    REQUIRE(reloaded.passes.size() == 3U);
    CHECK(reloaded.passes[0].for_each == fc::FrameForEach::LightCascades);
    CHECK(reloaded.passes[0].for_each_arg == 0U);
    CHECK(reloaded.passes[0].writes[0].indexed);            // the `[$index]` subscript survived
    CHECK(reloaded.passes[0].material_pass == fc::FrameMaterialPass::Shadow);
    CHECK(reloaded.resources[0].layers == 4U);
    CHECK(reloaded.resources[1].scale == 1.0F);
    CHECK(reloaded.requires_caps.size() == 1U);             // the capability tier survived
    CHECK(reloaded.passes[2].params[0].v[0] == 13.5);       // exact float round-trip (17 sig digits)
    CHECK(reloaded.draw_lists[0].sort == fc::FrameSortMode::FrontToBack);
}

TEST_CASE("REN-36.2: a PROGRAMMATICALLY built graph also emits and re-parses losslessly", "[framecook][ren36]")
{
    // the editor's real flow: build in memory (drag wires) -> save -> reload. Same guarantee.
    crd::memory::TlsfAllocator alloc(8U << 20U);
    fc::FrameGraphBuilder      b(&alloc, crd::containers::StringView("built", 5U));
    b.add_image(crd::containers::StringView("hdr", 3U), crd::gpu::FgImageFormat::RGBA16F, 256U, 256U, true);
    const crd::u32 p0 = b.add_pass(crd::containers::StringView("fill", 4U), fc::FramePassKind::RasterFullscreen);
    b.pass_shader(p0, crd::containers::StringView("app://sh/fill", 13U));
    b.pass_writes(p0, crd::containers::StringView("hdr", 3U));
    b.pass_clear_color(p0, 0.25F, 0.5F, 0.75F, 1.0F);
    const crd::u32 p1 = b.add_pass(crd::containers::StringView("post", 4U), fc::FramePassKind::RasterFullscreen);
    b.pass_shader(p1, crd::containers::StringView("app://sh/post", 13U));
    b.pass_reads(p1, crd::containers::StringView("hdr", 3U));
    b.pass_writes(p1, crd::containers::StringView("@output", 7U));
    b.pass_param(p1, crd::containers::StringView("exposure", 8U), 0.125);
    REQUIRE(b.validate() == fc::FrameCookError::Ok);

    const auto              cooked = fc::cook_frame_graph(b.desc(), &alloc);
    crd::containers::String text   = fc::emit_frame_toml(b.desc(), &alloc);

    fc::FrameGraphDesc back(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(text.c_str(), text.size()), back)
            == fc::FrameCookError::Ok);
    const auto cooked_back = fc::cook_frame_graph(back, &alloc);
    REQUIRE(cooked_back.size() == cooked.size());
    CHECK(std::memcmp(cooked_back.data(), cooked.data(), cooked.size()) == 0);
    CHECK(back.passes[0].clear_color[2] == 0.75F);
    CHECK(back.passes[1].params[0].v[0] == 0.125);
}

TEST_CASE("REN-36.1: every error code has a human-readable message", "[framecook][ren36]")
{
    // A rejection nobody can read is barely better than a silent one.
    for (crd::u32 i = 0; i <= static_cast<crd::u32>(fc::FrameCookError::BadResourceSize); ++i)
    {
        const char* t = fc::frame_cook_error_text(static_cast<fc::FrameCookError>(i));
        REQUIRE(t != nullptr);
        CHECK(std::strlen(t) > 0U);
        CHECK(std::strcmp(t, "unknown error") != 0);
    }
}
