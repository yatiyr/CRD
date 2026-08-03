// RAF-8 (ADR-0106): the FrameGraphDesc -> render-graph FrameGraphTemplate LOAD BRIDGE — device-free.
//
// Proves a cooked frame TOPOLOGY becomes a render-graph template the single live runtime compiles: a forward_csm-shaped
// graph (N shadow cascades via for_each, a depth prepass, a shadowed forward pass, a post pass, present) bridges to a
// template, and `crd::rendergraph::compile` SCHEDULES it correctly — every cascade (writes the atlas) before the
// forward pass (reads it), forward before post, post before present. Plus the two LOUD failure paths: an unmapped pass
// kind and an unresolved for_each are NAMED diagnostics, never silent.
//
// (Binding device resources + resolving the ECS draw lists is the host's job at record time — this gate is topology.)

#include <crd/framecook/frame_asset.hpp>
#include <crd/framecook/frame_template_bridge.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/rendergraph/frame_graph.hpp>
#include <crd/renderpass/executor_registry.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace fc = crd::framecook;
namespace rp = crd::renderpass;
namespace rg = crd::rendergraph;
using crd::u32;
using crd::u64;
using crd::containers::StringView;

// A `for_each` resolver: the CSM cascade count this frame (the host's answer). 4 cascades.
u32 four_cascades(fc::FrameForEach kind, u32 /*arg*/, void* /*user*/)
{
    return kind == fc::FrameForEach::LightCascades ? 4U : 1U;
}

// The bridge's per-instance pass naming (white-box: base-name hash mixed with the instance). Lets the test find a
// pass's schedule position by its authored name.
u64 bridged_name(const char* base, u32 inst)
{
    return rp::pass_param_id(StringView(base)) ^ (static_cast<u64>(inst + 1U) * 0x9E3779B97F4A7C15ULL);
}

// The position of a pass (by its bridged name_hash) within the compiled schedule; -1 if absent.
int sched_pos(const rg::CompiledFrameGraph& c, const rg::FrameGraphTemplate& t, u64 name_hash)
{
    u32 pass_index = 0xFFFFFFFFU;
    for (u32 i = 0; i < t.passes().size(); ++i)
    {
        if (t.passes()[i].name_hash == name_hash)
        {
            pass_index = i;
            break;
        }
    }
    if (pass_index == 0xFFFFFFFFU)
    {
        return -1;
    }
    for (u32 s = 0; s < c.schedule().size(); ++s)
    {
        if (c.schedule()[s] == pass_index)
        {
            return static_cast<int>(s);
        }
    }
    return -1;
}

// Build a forward_csm-shaped cooked graph.
void build_forward_csm(fc::FrameGraphBuilder& b, crd::memory::IAllocator& alloc)
{
    // resources: a layered shadow atlas (sampled depth), a scene colour+depth buffer.
    b.add_image(StringView("shadow_atlas"), crd::gpu::FgImageFormat::D32Float, 2048U, 2048U, /*sampled*/ true,
                /*layers*/ 4U);
    b.add_image(StringView("scene_color"), crd::gpu::FgImageFormat::RGBA16F, 1920U, 1080U, /*sampled*/ true);
    // GPU-cull command buffers the forward pass reads for SCHEDULING (the real frame's `instances` + `cull_args`).
    const auto add_buffer = [&](const char* name)
    {
        fc::FrameResourceDesc rb(&alloc);
        rb.name.append(name);
        rb.kind = fc::FrameResourceKind::ExternalBuffer;
        rb.size_bytes = 4096U;
        b.desc().resources.push_back(static_cast<fc::FrameResourceDesc&&>(rb));
    };
    add_buffer("instances");
    add_buffer("cull_args");

    // csm_cascade: a for_each depth-only pass, one instance per cascade, each writing its atlas slice.
    const u32 csm = b.add_pass(StringView("csm_cascade"), fc::FramePassKind::RasterDepthOnly);
    b.pass_for_each(csm, fc::FrameForEach::LightCascades, 0U);
    b.pass_writes(csm, StringView("shadow_atlas"), /*indexed*/ true);
    b.pass_draw_list(csm, StringView("shadow_casters"));
    b.pass_clear_depth(csm, 1.0F);

    // depth_prepass: populate scene depth.
    const u32 prepass = b.add_pass(StringView("depth_prepass"), fc::FramePassKind::RasterDepthOnly);
    b.pass_writes(prepass, StringView("scene_color")); // depth-only writes the target's depth companion
    b.pass_draw_list(prepass, StringView("opaque"));
    b.pass_clear_depth(prepass, 1.0F);

    // forward: shadowed forward shading — writes colour, READS the shadow atlas (the ordering edge under test).
    const u32 fwd = b.add_pass(StringView("forward"), fc::FramePassKind::RasterGeometry);
    b.pass_writes(fwd, StringView("scene_color"));
    b.pass_reads(fwd, StringView("shadow_atlas"));         // a sampled texture -> input0
    b.pass_reads(fwd, StringView("instances"));            // a buffer -> read_buffer0 (scheduling edge)
    b.pass_reads(fwd, StringView("cull_args"));            // a buffer -> read_buffer1
    b.pass_draw_list(fwd, StringView("opaque"));
    b.pass_clear_color(fwd, 0.0F, 0.0F, 0.0F, 1.0F);
    b.pass_clear_depth(fwd, 1.0F);

    // post: tonemap scene_color -> @output.
    const u32 post = b.add_pass(StringView("post"), fc::FramePassKind::RasterFullscreen);
    b.pass_reads(post, StringView("scene_color"));
    b.pass_writes(post, StringView("@output"));
    b.pass_shader(post, StringView("engine://sh/tonemap"));

    // present the canvas.
    const u32 present = b.add_pass(StringView("present"), fc::FramePassKind::Present);
    b.pass_reads(present, StringView("@output"));
}

} // namespace

TEST_CASE("RAF-8: a forward_csm frame bridges to a render-graph template and compiles/schedules correctly",
          "[framecook][raf8][bridge]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U, nullptr, "raf8-bridge");
    fc::FrameGraphBuilder builder(&alloc, StringView("forward_csm"));
    build_forward_csm(builder, alloc);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 14U);

    rg::FrameGraphTemplate tmpl(&alloc);
    REQUIRE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl, diags));
    REQUIRE_FALSE(diags.has_errors());

    // 4 cascades (for_each expanded) + prepass + forward + post + present = 8 passes.
    CHECK(tmpl.passes().size() == 8U);
    // shadow_atlas + scene_color declared; @output auto-declared as an external persistent target.
    CHECK(tmpl.find_resource(rp::pass_param_id(StringView("shadow_atlas"))) != nullptr);
    CHECK(tmpl.find_resource(rp::pass_param_id(StringView("scene_color"))) != nullptr);
    const rg::GraphResource* out_res = tmpl.find_resource(rp::pass_param_id(StringView("@output")));
    REQUIRE(out_res != nullptr);
    CHECK(out_res->lifetime == rg::ResourceLifetime::Persistent);

    // ⭐ RAF-8: the forward pass reads a TEXTURE (shadow_atlas) + two BUFFERS (instances, cull_args). The bridge must
    // route buffer reads to StorageBuffer scheduling slots, NOT texture inputs — validate accepts either kind against
    // its slot, so only this structural check catches a buffer bound to a sampler.
    {
        const rg::GraphPass* fwd = nullptr;
        for (u32 i = 0; i < tmpl.passes().size(); ++i)
        {
            if (tmpl.passes()[i].name_hash == bridged_name("forward", 0U)) { fwd = &tmpl.passes()[i]; }
        }
        REQUIRE(fwd != nullptr);
        u32 tex_reads = 0U;
        u32 buf_reads = 0U;
        for (u32 i = 0; i < fwd->payload.resources.size(); ++i)
        {
            const rp::ResourceRef& rr = fwd->payload.resources[i];
            if (rr.access != rp::SlotAccess::Read) { continue; }
            if (rr.kind == rp::SlotResourceKind::Texture) { ++tex_reads; }
            else if (rr.kind == rp::SlotResourceKind::StorageBuffer) { ++buf_reads; }
        }
        CHECK(tex_reads == 1U); // shadow_atlas
        CHECK(buf_reads == 2U); // instances + cull_args on read_buffer0/1
    }

    // Compile the bridged template — the single live runtime's scheduler.
    rg::CompiledFrameGraph compiled(&alloc);
    REQUIRE(rg::compile(tmpl, schemas, 1920U, 1080U, compiled, diags));
    REQUIRE_FALSE(diags.has_errors());
    CHECK(compiled.schedule().size() == 8U);

    // ⛔ THE ORDERING PROOF: every cascade (writes the atlas) is scheduled BEFORE the forward pass (reads it).
    const int fwd_pos = sched_pos(compiled, tmpl, bridged_name("forward", 0U));
    const int post_pos = sched_pos(compiled, tmpl, bridged_name("post", 0U));
    const int present_pos = sched_pos(compiled, tmpl, bridged_name("present", 0U));
    REQUIRE(fwd_pos >= 0);
    REQUIRE(post_pos >= 0);
    REQUIRE(present_pos >= 0);
    for (u32 c = 0; c < 4U; ++c)
    {
        const int cascade_pos = sched_pos(compiled, tmpl, bridged_name("csm_cascade", c));
        REQUIRE(cascade_pos >= 0);
        CHECK(cascade_pos < fwd_pos); // atlas write before the shadowed forward read
    }
    CHECK(fwd_pos < post_pos);     // forward writes scene_color; post reads it
    CHECK(post_pos < present_pos); // post writes @output; present reads it
}

TEST_CASE("RAF-8: an amplification pass kind now MAPS (the RAF-8 tail closed the gap)", "[framecook][raf8][bridge]")
{
    // ⭐ RAF-8: `raster.mesh` (like tess / mesh-indirect / raytrace.pipeline / visbuffer) had NO executor and failed
    // the bridge LOUDLY; the RAF-8 tail gave each its own executor, so the same pass now BUILDS. The gap is closed.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-amplify");
    fc::FrameGraphBuilder builder(&alloc, StringView("mesh_frame"));
    builder.add_image(StringView("out"), crd::gpu::FgImageFormat::RGBA16F, 256U, 256U);
    const u32 mesh = builder.add_pass(StringView("amplify"), fc::FramePassKind::RasterMesh);
    builder.pass_writes(mesh, StringView("out"));

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 14U);
    rg::FrameGraphTemplate tmpl(&alloc);
    REQUIRE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl, diags));
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE(tmpl.passes().size() == 1U);
    CHECK(tmpl.passes()[0].payload.executor == rp::executor_type_id(StringView("mesh.raster")));
}

TEST_CASE("RAF-8: a malformed mapped pass still fails the bridge LOUDLY", "[framecook][raf8][bridge]")
{
    // A mesh-INDIRECT pass takes its meshlet count from an args buffer READ; declaring none is not a silent skip but a
    // NAMED diagnostic — the loud-failure contract survives the gap closure (it just moved from kind to structure).
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-malformed");
    fc::FrameGraphBuilder builder(&alloc, StringView("meshind_frame"));
    builder.add_image(StringView("out"), crd::gpu::FgImageFormat::RGBA16F, 256U, 256U);
    const u32 mi = builder.add_pass(StringView("draw"), fc::FramePassKind::RasterMeshIndirect);
    builder.pass_writes(mi, StringView("out")); // a colour write but NO args buffer read

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 14U);
    rg::FrameGraphTemplate tmpl(&alloc);
    REQUIRE_FALSE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl, diags));
    CHECK(diags.contains(crd::renderasset::DiagCode::InvalidSlot));
}

TEST_CASE("RAF-8: a compute pass that reads a TEXTURE routes it to the sampled slot", "[framecook][raf8][bridge]")
{
    // gap (b): occlusion_cull reads the HZB (an image) + writes a buffer. The bridge must route the image read to the
    // `sampled` texture slot (dispatch_kernel_sampled), NOT a storage slot — validate accepts either against its slot,
    // so only this structural check catches a texture bound as a buffer.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-compute-tex");
    fc::FrameGraphBuilder builder(&alloc, StringView("cull"));
    builder.add_image(StringView("hzb"), crd::gpu::FgImageFormat::R32F, 512U, 512U, /*sampled*/ true);
    {
        fc::FrameResourceDesc rb(&alloc);
        rb.name.append("cull_args");
        rb.kind = fc::FrameResourceKind::ExternalBuffer;
        rb.size_bytes = 4096U;
        builder.desc().resources.push_back(static_cast<fc::FrameResourceDesc&&>(rb));
    }
    const u32 cull = builder.add_pass(StringView("occlusion_cull"), fc::FramePassKind::Compute);
    builder.pass_kernel(cull, StringView("crd://scene/occlusion_cull"));
    builder.pass_reads(cull, StringView("hzb"));         // a TEXTURE -> sampled slot
    builder.pass_writes(cull, StringView("cull_args"));  // a buffer  -> storage slot

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 14U);
    rg::FrameGraphTemplate tmpl(&alloc);
    REQUIRE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl, diags));
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE(tmpl.passes().size() == 1U);
    u32 tex_reads = 0U;
    u32 buf_writes = 0U;
    for (u32 i = 0; i < tmpl.passes()[0].payload.resources.size(); ++i)
    {
        const rp::ResourceRef& rr = tmpl.passes()[0].payload.resources[i];
        if (rr.kind == rp::SlotResourceKind::Texture && rr.access == rp::SlotAccess::Read) { ++tex_reads; }
        if (rr.kind == rp::SlotResourceKind::StorageBuffer && rr.access == rp::SlotAccess::ReadWrite) { ++buf_writes; }
    }
    CHECK(tex_reads == 1U);  // hzb on the `sampled` slot
    CHECK(buf_writes == 1U); // cull_args on `storage`
}

TEST_CASE("RAF-8: an MRT depth-prepass routes the depth write to depth and the colour write to color",
          "[framecook][raf8][bridge]")
{
    // gap (d): depth_prepass (raster.mrt) writes velocity (RG16F colour) + scene_depth (D32Float depth). The bridge
    // must route by RESOURCE FORMAT — the depth-format write to `depth`, the colour write to `color` — not both to
    // colour slots (which would leave the pass with no depth attachment and a broken occlusion prepass).
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-mrt-prepass");
    fc::FrameGraphBuilder builder(&alloc, StringView("prepass"));
    builder.add_image(StringView("velocity"), crd::gpu::FgImageFormat::RG16F, 1920U, 1080U, /*sampled*/ true);
    builder.add_image(StringView("scene_depth"), crd::gpu::FgImageFormat::D32Float, 1920U, 1080U, /*sampled*/ true);
    const u32 pre = builder.add_pass(StringView("depth_prepass"), fc::FramePassKind::RasterMrt);
    builder.pass_writes(pre, StringView("velocity"));    // colour (RG16F) -> color0
    builder.pass_writes(pre, StringView("scene_depth")); // depth  (D32Float) -> depth
    builder.pass_draw_list(pre, StringView("opaque"));
    builder.pass_clear_depth(pre, 0.0F);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 14U);
    rg::FrameGraphTemplate tmpl(&alloc);
    REQUIRE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl, diags));
    REQUIRE_FALSE(diags.has_errors());
    // scene_depth is a DepthTarget resource (depth format); velocity is a ColorTarget resource.
    const rg::GraphResource* sd = tmpl.find_resource(rp::pass_param_id(StringView("scene_depth")));
    REQUIRE(sd != nullptr);
    CHECK(sd->kind == rp::SlotResourceKind::DepthTarget);
    REQUIRE(tmpl.passes().size() == 1U);
    u32 color_writes = 0U;
    u32 depth_writes = 0U;
    for (u32 i = 0; i < tmpl.passes()[0].payload.resources.size(); ++i)
    {
        const rp::ResourceRef& rr = tmpl.passes()[0].payload.resources[i];
        if (rr.kind == rp::SlotResourceKind::ColorTarget && rr.access == rp::SlotAccess::Write) { ++color_writes; }
        if (rr.kind == rp::SlotResourceKind::DepthTarget) { ++depth_writes; }
    }
    CHECK(color_writes == 1U); // velocity
    CHECK(depth_writes == 1U); // scene_depth
}

TEST_CASE("RAF-8: an unresolved for_each fails the bridge LOUDLY", "[framecook][raf8][bridge]")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-foreach");
    fc::FrameGraphBuilder builder(&alloc, StringView("cascades"));
    builder.add_image(StringView("atlas"), crd::gpu::FgImageFormat::D32Float, 1024U, 1024U, true, 4U);
    const u32 csm = builder.add_pass(StringView("cascade"), fc::FramePassKind::RasterDepthOnly);
    builder.pass_for_each(csm, fc::FrameForEach::LightCascades, 0U);
    builder.pass_writes(csm, StringView("atlas"), true);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 14U);
    rg::FrameGraphTemplate tmpl(&alloc);
    // a NULL resolver ⇒ the host cannot answer the cascade count ⇒ UnresolvedForEach, not a silent skip.
    REQUIRE_FALSE(fc::build_frame_graph_template(builder.desc(), nullptr, nullptr, schemas, tmpl, diags));
    CHECK(diags.contains(crd::renderasset::DiagCode::UnresolvedForEach));
}
