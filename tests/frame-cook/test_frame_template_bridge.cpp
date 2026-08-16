// RAF-8 (ADR-0106): the FrameGraphDesc -> render-graph FrameGraphTemplate LOAD BRIDGE — device-free.
//
// Proves a cooked frame TOPOLOGY becomes a render-graph template the single live runtime compiles: a forward_csm-shaped
// graph (N shadow cascades via for_each, a depth prepass, a shadowed forward pass, a post pass, present) bridges to a
// template, and `crd::rendergraph::compile` SCHEDULES it correctly — every cascade (writes the atlas) before the
// forward pass (reads it), forward before post, post before present. Plus the two LOUD failure paths: an unmapped pass
// kind and an unresolved for_each are NAMED diagnostics, never silent.
//
// (Binding device resources + resolving the ECS draw lists is the host's job at record time — this gate is topology.)

#include <crd/ceir/ceir.hpp>
#include <crd/framecook/frame_asset.hpp>
#include <crd/framecook/frame_ceir.hpp>
#include <crd/framecook/frame_runtime.hpp> // CEIR-16-3c: FramePlans + build_frame_plans
#include <crd/framecook/frame_template_bridge.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
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

    // DECLARE the ECS draw lists (Fork A: a pass references a draw list by OPERAND, so it must be a declared frame.draw_list —
    // the canonical CEIR model; the bridge also resolves by name, so this is bridge-neutral but ceir-convertible).
    const u32 dl_shadow = b.add_draw_list(StringView("shadow_casters"));
    b.draw_list_all(dl_shadow, StringView("Transform"));
    b.draw_list_all(dl_shadow, StringView("Mesh"));
    const u32 dl_opaque = b.add_draw_list(StringView("opaque"));
    b.draw_list_all(dl_opaque, StringView("Transform"));
    b.draw_list_all(dl_opaque, StringView("Mesh"));

    // csm_cascade: a for_each depth-only pass, one instance per cascade, each writing its atlas slice.
    const u32 csm = b.add_pass(StringView("csm_cascade"), StringView("raster.depth_only"));
    b.pass_for_each(csm, fc::FrameForEach::LightCascades, 0U);
    b.pass_writes(csm, StringView("shadow_atlas"), /*indexed*/ true);
    b.pass_draw_list(csm, StringView("shadow_casters"));
    b.pass_clear_depth(csm, 1.0F);

    // depth_prepass: populate scene depth.
    const u32 prepass = b.add_pass(StringView("depth_prepass"), StringView("raster.depth_only"));
    b.pass_writes(prepass, StringView("scene_color")); // depth-only writes the target's depth companion
    b.pass_draw_list(prepass, StringView("opaque"));
    b.pass_clear_depth(prepass, 1.0F);

    // forward: shadowed forward shading — writes colour, READS the shadow atlas (the ordering edge under test).
    const u32 fwd = b.add_pass(StringView("forward"), StringView("raster.geometry"));
    b.pass_writes(fwd, StringView("scene_color"));
    b.pass_reads(fwd, StringView("shadow_atlas"));         // a sampled texture -> input0
    b.pass_reads(fwd, StringView("instances"));            // a buffer -> read_buffer0 (scheduling edge)
    b.pass_reads(fwd, StringView("cull_args"));            // a buffer -> read_buffer1
    b.pass_draw_list(fwd, StringView("opaque"));
    b.pass_clear_color(fwd, 0.0F, 0.0F, 0.0F, 1.0F);
    b.pass_clear_depth(fwd, 1.0F);

    // post: tonemap scene_color -> @output.
    const u32 post = b.add_pass(StringView("post"), StringView("raster.fullscreen"));
    b.pass_reads(post, StringView("scene_color"));
    b.pass_writes(post, StringView("@output"));
    b.pass_shader(post, StringView("engine://sh/tonemap"));

    // present the canvas.
    const u32 present = b.add_pass(StringView("present"), StringView("present"));
    b.pass_reads(present, StringView("@output"));
}

// A generic for_each resolver for the shipped-asset gate: every host count the built-in generators need.
u32 generic_for_each_count(fc::FrameForEach kind, u32 arg, void* /*user*/)
{
    switch (kind)
    {
    case fc::FrameForEach::None: return 1U;
    case fc::FrameForEach::StereoViews: return 2U;
    case fc::FrameForEach::CubeFaces: return 6U;
    case fc::FrameForEach::ShadowCastingLights: return 2U;
    case fc::FrameForEach::LightCascades: return arg > 0U ? arg : 4U;
    }
    return 1U;
}

// CEIR-15e: the A/B RUNTIME-FIDELITY gate for ONE desc — build_frame_graph_template(desc) must be BYTE-EQUIVALENT to
// build_frame_graph_template(desc round-tripped through ceir.frame). If the DIRECT desc does not bridge (a bridge gap, not
// a ceir divergence), we only assert the round-trip bridges identically. All comparisons via Catch CHECKs.
void ab_template_fidelity(const fc::FrameGraphDesc& desc, rp::ExecutorRegistry& schemas, crd::memory::IAllocator& alloc)
{
    crd::renderasset::DiagnosticList diags_a(&alloc);
    rg::FrameGraphTemplate           tmpl_a(&alloc);
    const bool ok_a = fc::build_frame_graph_template(desc, generic_for_each_count, nullptr, schemas, tmpl_a, diags_a);

    // round-trip through ceir.frame (Fork E) and lower the reconstruction.
    crd::ceir::Context       ctx(&alloc);
    crd::ceir::Module* const m = fc::to_ceir_frame(desc, ctx);
    REQUIRE(m != nullptr); // ⛔ the CEIR path must ACCEPT every shipped asset
    REQUIRE(ctx.find_frame_misuse(*m).kind == crd::ceir::FrameMisuseKind::None);
    fc::FrameGraphDesc desc_b(&alloc);
    REQUIRE(fc::from_ceir_frame(ctx, *m, &alloc, desc_b));
    crd::renderasset::DiagnosticList diags_b(&alloc);
    rg::FrameGraphTemplate           tmpl_b(&alloc);
    const bool ok_b = fc::build_frame_graph_template(desc_b, generic_for_each_count, nullptr, schemas, tmpl_b, diags_b);

    REQUIRE(ok_a == ok_b); // the ceir round-trip does not change bridgeability
    if (!ok_a) { return; } // a bridge gap on BOTH — not this gate's subject

    REQUIRE(tmpl_a.passes().size() == tmpl_b.passes().size());
    for (u32 i = 0; i < tmpl_a.passes().size(); ++i)
    {
        const rg::GraphPass& pa = tmpl_a.passes()[i];
        const rg::GraphPass& pb = tmpl_b.passes()[i];
        CHECK(pa.name_hash == pb.name_hash);
        CHECK(pa.payload.executor == pb.payload.executor);
        REQUIRE(pa.payload.resources.size() == pb.payload.resources.size());
        for (u32 r = 0; r < pa.payload.resources.size(); ++r)
        {
            const rp::ResourceRef& ra = pa.payload.resources[r];
            const rp::ResourceRef& rb = pb.payload.resources[r];
            CHECK(ra.slot_name_hash == rb.slot_name_hash);
            CHECK(ra.kind == rb.kind);
            CHECK(ra.access == rb.access);
            CHECK(ra.resource_id == rb.resource_id);
        }
    }

    rg::CompiledFrameGraph ca(&alloc);
    rg::CompiledFrameGraph cb(&alloc);
    const bool             cok_a = rg::compile(tmpl_a, schemas, 1920U, 1080U, ca, diags_a);
    const bool             cok_b = rg::compile(tmpl_b, schemas, 1920U, 1080U, cb, diags_b);
    REQUIRE(cok_a == cok_b);
    if (cok_a)
    {
        REQUIRE(ca.schedule().size() == cb.schedule().size());
        for (u32 s = 0; s < ca.schedule().size(); ++s) { CHECK(ca.schedule()[s] == cb.schedule()[s]); }
    }
}

// Load a shipped .frame.toml (from the CRD_FRAME_ASSETS_DIR the CMake pins) and run the A/B fidelity gate on it.
void gate_shipped_asset(const char* stem, crd::memory::IAllocator& alloc)
{
    INFO("shipped asset = " << stem);
    crd::containers::String path(&alloc);
    path.append(CRD_FRAME_ASSETS_DIR);
    path.append("/");
    path.append(stem);
    path.append(".frame.toml");
    crd::containers::String toml(&alloc);
    REQUIRE(crd::platform::fs::read_file_text(crd::platform::fs::Path{StringView(path.c_str(), path.size())}, toml));

    fc::FrameGraphDesc     desc(&alloc);
    crd::containers::String where(&alloc);
    REQUIRE(fc::parse_frame_toml(StringView(toml.c_str(), toml.size()), desc, &where) == fc::FrameCookError::Ok);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry             schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
    ab_template_fidelity(desc, schemas, alloc);
}

// CEIR-16-3c-5 PREREQ: parse a shipped .frame.toml and prove build_frame_plans builds a CEIR replay plan for EVERY fullscreen
// pass without error — the real-asset validation that must pass BEFORE the 3c-5 flip makes a null plan a record-time error.
void gate_frame_plans(const char* stem, crd::memory::IAllocator& alloc)
{
    INFO("shipped asset = " << stem);
    crd::containers::String path(&alloc);
    path.append(CRD_FRAME_ASSETS_DIR);
    path.append("/");
    path.append(stem);
    path.append(".frame.toml");
    crd::containers::String toml(&alloc);
    REQUIRE(crd::platform::fs::read_file_text(crd::platform::fs::Path{StringView(path.c_str(), path.size())}, toml));

    fc::FrameGraphDesc      desc(&alloc);
    crd::containers::String where(&alloc);
    REQUIRE(fc::parse_frame_toml(StringView(toml.c_str(), toml.size()), desc, &where) == fc::FrameCookError::Ok);

    fc::FramePlans                   plans(&alloc);
    crd::renderasset::DiagnosticList diags(&alloc);
    REQUIRE(fc::build_frame_plans(desc, plans, diags)); // ⭐ the extraction builds every real fullscreen composite w/o error
    REQUIRE_FALSE(diags.has_errors());
    for (u32 pi = 0; pi < desc.passes.size(); ++pi)
    {
        if (!fc::pass_is_fullscreen(desc.passes[pi])) { continue; }
        const rg::CeirPassPlan* const plan = plans.table.find(
            rp::pass_param_id(StringView(desc.passes[pi].name.c_str(), desc.passes[pi].name.size())));
        REQUIRE(plan != nullptr);  // every fullscreen pass got a bound plan (found by its authored name)
        CHECK(plan->count >= 3U);  // BeginRender + >=1 Draw + EndRender
    }
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
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);

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

// ── CEIR-15d-5: RUNTIME FIDELITY of the CEIR path (Fork E). The `ceir.frame` executes through the EXISTING runtime via the
// backward converter: `desc → to_ceir_frame → from_ceir_frame → desc' → build_frame_graph_template`. This proves the whole
// round-trip lowers to a BYTE-EQUIVALENT render-graph template — a STRONGER gate than the emit_frame_toml round-trip, because
// it catches any desc field the ceir round-trip lost that the BRIDGE consumes but the toml serializer does not. forward_csm
// exercises the hard cases together: a `for_each` depth-only pass, an INDEXED (`[$index]`) layered-atlas write, external
// buffers (imports), a sampled depth texture, MRT-adjacent depth/colour, and present. ──
TEST_CASE("CEIR-15d-5: the ceir round-trip lowers to an IDENTICAL render-graph template (Fork E runtime fidelity)",
          "[framecook][ceir][frame][bridge]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U, nullptr, "15d5-parity");
    fc::FrameGraphBuilder      builder(&alloc, StringView("forward_csm"));
    build_forward_csm(builder, alloc);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry             schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);

    // (A) lower the DIRECT desc.
    rg::FrameGraphTemplate tmpl_a(&alloc);
    REQUIRE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl_a, diags));
    REQUIRE_FALSE(diags.has_errors());

    // round-trip the desc THROUGH ceir.frame.
    crd::ceir::Context        ctx(&alloc);
    crd::ceir::Module* const  m = fc::to_ceir_frame(builder.desc(), ctx);
    REQUIRE(m != nullptr);
    fc::FrameGraphDesc        desc_b(&alloc);
    REQUIRE(fc::from_ceir_frame(ctx, *m, &alloc, desc_b));

    // (B) lower the ROUND-TRIPPED desc through the SAME bridge.
    rg::FrameGraphTemplate tmpl_b(&alloc);
    REQUIRE(fc::build_frame_graph_template(desc_b, four_cascades, nullptr, schemas, tmpl_b, diags));
    REQUIRE_FALSE(diags.has_errors());

    // ⭐ the two templates are IDENTICAL — pass-for-pass, executor-for-executor, slot-for-slot.
    REQUIRE(tmpl_a.passes().size() == tmpl_b.passes().size());
    for (u32 i = 0; i < tmpl_a.passes().size(); ++i)
    {
        const rg::GraphPass& pa = tmpl_a.passes()[i];
        const rg::GraphPass& pb = tmpl_b.passes()[i];
        CHECK(pa.name_hash == pb.name_hash);
        CHECK(pa.payload.executor == pb.payload.executor);
        REQUIRE(pa.payload.resources.size() == pb.payload.resources.size());
        for (u32 r = 0; r < pa.payload.resources.size(); ++r)
        {
            const rp::ResourceRef& ra = pa.payload.resources[r];
            const rp::ResourceRef& rb = pb.payload.resources[r];
            CHECK(ra.slot_name_hash == rb.slot_name_hash);
            CHECK(ra.kind == rb.kind);
            CHECK(ra.access == rb.access);
            CHECK(ra.resource_id == rb.resource_id);
        }
    }

    // …and they SCHEDULE identically (same passes, same order — the ordering the runtime executes).
    rg::CompiledFrameGraph ca(&alloc);
    rg::CompiledFrameGraph cb(&alloc);
    REQUIRE(rg::compile(tmpl_a, schemas, 1920U, 1080U, ca, diags));
    REQUIRE(rg::compile(tmpl_b, schemas, 1920U, 1080U, cb, diags));
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE(ca.schedule().size() == cb.schedule().size());
    for (u32 s = 0; s < ca.schedule().size(); ++s) { CHECK(ca.schedule()[s] == cb.schedule()[s]); }
}

// ── CEIR-15e: every SHIPPED frame asset lowers IDENTICALLY through the CEIR path (device-free per-asset fidelity). Each
// section reads the REAL .frame.toml, parses it, and asserts build_frame_graph_template(desc) is byte-equivalent to
// build_frame_graph_template(desc round-tripped through ceir.frame). A per-asset SECTION so a gap names the exact asset. ──
TEST_CASE("CEIR-15e: shipped frame assets lower identically through the CEIR path", "[framecook][ceir][frame][bridge][15e]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U, nullptr, "15e-assets");
    SECTION("forward_basic") { gate_shipped_asset("forward_basic", alloc); }
    SECTION("forward_srgb") { gate_shipped_asset("forward_srgb", alloc); }
    SECTION("forward_agx") { gate_shipped_asset("forward_agx", alloc); }
    SECTION("forward_csm") { gate_shipped_asset("forward_csm", alloc); }
    SECTION("forward_csm_srgb") { gate_shipped_asset("forward_csm_srgb", alloc); }
    SECTION("forward_csm_agx") { gate_shipped_asset("forward_csm_agx", alloc); }
    SECTION("forward_csm_moment") { gate_shipped_asset("forward_csm_moment", alloc); }
    SECTION("forward_csm_gpu") { gate_shipped_asset("forward_csm_gpu", alloc); }
    SECTION("forward_csm_gpu_srgb") { gate_shipped_asset("forward_csm_gpu_srgb", alloc); }
    SECTION("scene_cull") { gate_shipped_asset("scene_cull", alloc); }
    // (scene_gpu_cull was a compute-only GPU-cull FRAGMENT [no @output] — DELETED at CEIR-17z: it referenced the unregistered
    //  `engine://scene/cull_compact` [the superseded per-desc cull design] and had zero consumers. The live GPU cull is
    //  forward_csm_gpu's cooked per-view `cull_view0..4` variants, gated above.)
    SECTION("scene_mesh") { gate_shipped_asset("scene_mesh", alloc); }
    SECTION("scene_tess") { gate_shipped_asset("scene_tess", alloc); }
    SECTION("scene_visbuffer") { gate_shipped_asset("scene_visbuffer", alloc); }
    SECTION("scene_rt") { gate_shipped_asset("scene_rt", alloc); }
    SECTION("velocity_debug") { gate_shipped_asset("velocity_debug", alloc); }
    // ⭐⭐ CEIR-18z (band close): the Forward+ (tiled) + clustered 3D-froxel frame graphs had NEVER been through this A/B gate
    // (the SECTION list is hardcoded; 18a-2/18b added them). forward_plus = CPU-list (raster only); forward_plus_gpu +
    // forward_clustered_3d_gpu add a light_cull(_3d) COMPUTE pass + the `instances` external_buffer — the same compute-pass +
    // requires/fallback shape forward_csm_gpu already proves here. The exact latent-drop class 17z existed to catch.
    SECTION("forward_plus") { gate_shipped_asset("forward_plus", alloc); }
    SECTION("forward_plus_gpu") { gate_shipped_asset("forward_plus_gpu", alloc); }
    SECTION("forward_clustered_3d_gpu") { gate_shipped_asset("forward_clustered_3d_gpu", alloc); }
}

TEST_CASE("RAF-8: an amplification pass kind now MAPS (the RAF-8 tail closed the gap)", "[framecook][raf8][bridge]")
{
    // ⭐ RAF-8: `raster.mesh` (like tess / mesh-indirect / raytrace.pipeline / visbuffer) had NO executor and failed
    // the bridge LOUDLY; the RAF-8 tail gave each its own executor, so the same pass now BUILDS. The gap is closed.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-amplify");
    fc::FrameGraphBuilder builder(&alloc, StringView("mesh_frame"));
    builder.add_image(StringView("out"), crd::gpu::FgImageFormat::RGBA16F, 256U, 256U);
    const u32 mesh = builder.add_pass(StringView("amplify"), StringView("raster.mesh"));
    builder.pass_writes(mesh, StringView("out"));

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
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
    const u32 mi = builder.add_pass(StringView("draw"), StringView("raster.mesh.indirect"));
    builder.pass_writes(mi, StringView("out")); // a colour write but NO args buffer read

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
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
    const u32 cull = builder.add_pass(StringView("occlusion_cull"), StringView("compute"));
    builder.pass_kernel(cull, StringView("crd://scene/occlusion_cull"));
    builder.pass_reads(cull, StringView("hzb"));         // a TEXTURE -> sampled slot
    builder.pass_writes(cull, StringView("cull_args"));  // a buffer  -> storage slot

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
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
    const u32 pre = builder.add_pass(StringView("depth_prepass"), StringView("raster.mrt"));
    builder.pass_writes(pre, StringView("velocity"));    // colour (RG16F) -> color0
    builder.pass_writes(pre, StringView("scene_depth")); // depth  (D32Float) -> depth
    builder.pass_draw_list(pre, StringView("opaque"));
    builder.pass_clear_depth(pre, 0.0F);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
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

TEST_CASE("RAF-12.2: a TRUE multi-colour MRT emits per-attachment blend params (the WBOIT accumulate shape)",
          "[framecook][raf12][bridge]")
{
    // The WBOIT ACCUMULATE pass writes accum (RGBA16F) + revealage (R16F) — TWO colour attachments — with per-attachment
    // blend (additive into accum, revealage_multiply into reveal). The bridge must (a) route BOTH as colour writes
    // (`color` + `color1`) and (b) carry the per-attachment blend as `blend0` / `blend1` params so the scene.raster
    // executor reproduces it. Without the blend params the pass renders OPAQUE — the exact miss RAF-12.2 closes so the
    // inline `record_pass` MRT arm can retire. Device-free: this is TOPOLOGY + payload, no GPU.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf12-mrt-blend");
    fc::FrameGraphBuilder      builder(&alloc, StringView("wboit"));
    builder.add_image(StringView("accum"), crd::gpu::FgImageFormat::RGBA16F, 1920U, 1080U, /*sampled*/ true);
    builder.add_image(StringView("revealage"), crd::gpu::FgImageFormat::R16F, 1920U, 1080U, /*sampled*/ true);
    const u32 acc = builder.add_pass(StringView("accumulate"), StringView("raster.mrt"));
    builder.pass_writes(acc, StringView("accum"));     // colour 0 -> `color`
    builder.pass_writes(acc, StringView("revealage")); // colour 1 -> `color1`
    builder.pass_draw_list(acc, StringView("transparent"));
    builder.pass_clear_color(acc, 0.0F, 0.0F, 0.0F, 0.0F);
    // RAF-12.3 §7 fold: per-attachment blend is a param set now (blend_count + blend0..N).
    fc::set_pass_u32(builder.desc().passes[acc], StringView(fc::pp::kBlendCount), 2U);
    fc::set_pass_enum(builder.desc().passes[acc], StringView(fc::pp::kBlendSlot[0]), static_cast<crd::u32>(crd::gpu::BlendMode::Additive));          // accum
    fc::set_pass_enum(builder.desc().passes[acc], StringView(fc::pp::kBlendSlot[1]), static_cast<crd::u32>(crd::gpu::BlendMode::RevealageMultiply)); // revealage

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry             schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
    rg::FrameGraphTemplate tmpl(&alloc);
    REQUIRE(fc::build_frame_graph_template(builder.desc(), four_cascades, nullptr, schemas, tmpl, diags));
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE(tmpl.passes().size() == 1U);
    const rg::GraphPass& p = tmpl.passes()[0];

    // (a) BOTH colour writes present (no depth for the accumulate pass).
    u32 color_writes = 0U;
    for (u32 i = 0; i < p.payload.resources.size(); ++i)
    {
        if (p.payload.resources[i].kind == rp::SlotResourceKind::ColorTarget
            && p.payload.resources[i].access == rp::SlotAccess::Write)
        {
            ++color_writes;
        }
    }
    CHECK(color_writes == 2U);

    // (b) the per-attachment BLEND params, typed Enum, in colour-attachment order.
    const auto blend_of = [&](const char* name) -> int
    {
        const u64 id = rp::pass_param_id(StringView(name));
        for (u32 i = 0; i < p.payload.params.size(); ++i)
        {
            if (p.payload.params[i].name_hash == id && p.payload.params[i].value.type == rp::ExecutorParamType::Enum)
            {
                return static_cast<int>(p.payload.params[i].value.e);
            }
        }
        return -1;
    };
    CHECK(blend_of("blend0") == static_cast<int>(crd::gpu::BlendMode::Additive));
    CHECK(blend_of("blend1") == static_cast<int>(crd::gpu::BlendMode::RevealageMultiply));
}

TEST_CASE("RAF-8: an unresolved for_each fails the bridge LOUDLY", "[framecook][raf8][bridge]")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf8-foreach");
    fc::FrameGraphBuilder builder(&alloc, StringView("cascades"));
    builder.add_image(StringView("atlas"), crd::gpu::FgImageFormat::D32Float, 1024U, 1024U, true, 4U);
    const u32 csm = builder.add_pass(StringView("cascade"), StringView("raster.depth_only"));
    builder.pass_for_each(csm, fc::FrameForEach::LightCascades, 0U);
    builder.pass_writes(csm, StringView("atlas"), true);

    crd::renderasset::DiagnosticList diags(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, diags) == 13U);
    rg::FrameGraphTemplate tmpl(&alloc);
    // a NULL resolver ⇒ the host cannot answer the cascade count ⇒ UnresolvedForEach, not a silent skip.
    REQUIRE_FALSE(fc::build_frame_graph_template(builder.desc(), nullptr, nullptr, schemas, tmpl, diags));
    CHECK(diags.contains(crd::renderasset::DiagCode::UnresolvedForEach));
}

TEST_CASE("ceir 16-3c-3b: build_frame_plans builds a fullscreen pass's CEIR replay plan, found by name")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "ceir-16-3c-3b");
    fc::FrameGraphBuilder      b(&alloc, StringView("fs_plan_test"));
    b.add_image(StringView("scene_color"), crd::gpu::FgImageFormat::RGBA16F, 1920U, 1080U, /*sampled*/ true);
    const u32 post = b.add_pass(StringView("post"), StringView("raster.fullscreen"));
    b.pass_reads(post, StringView("scene_color")); // a sampled colour texture → input0 (the plain shape)
    b.pass_writes(post, StringView("@output"));
    b.pass_shader(post, StringView("engine://sh/tonemap"));

    fc::FramePlans                   plans(&alloc);
    crd::renderasset::DiagnosticList diags(&alloc);
    REQUIRE(fc::build_frame_plans(b.desc(), plans, diags));
    REQUIRE_FALSE(diags.has_errors());

    // ⭐ the fullscreen pass's replay plan is found by name hash and is a 3-command render scope (Begin/Draw/End).
    const rg::CeirPassPlan* const plan = plans.table.find(rp::pass_param_id(StringView("post")));
    REQUIRE(plan != nullptr);
    CHECK(plan->ctx != nullptr);
    CHECK(plan->commands != nullptr);
    CHECK(plan->count == 3U);
    // a name that is not a migrated pass resolves to no plan (its executor keeps its C++ record path).
    CHECK(plans.table.find(rp::pass_param_id(StringView("not_a_pass"))) == nullptr);
}

TEST_CASE("ceir 16-mesh-1: build_frame_plans builds a mesh.indirect pass's CEIR replay plan, found by name")
{
    // ⛔ CEIR-16-mesh-1: build_frame_plans now recognises mesh.indirect passes too (not just fullscreen) and builds a
    // build_mesh_indirect_ceir plan. No shipped asset uses mesh.indirect (like the shadow-atlas fullscreen shape), so this
    // hand-built pass is its device-free coverage.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "ceir-16-mesh-1");
    fc::FrameGraphBuilder      b(&alloc, StringView("mi_plan_test"));
    b.add_image(StringView("out"), crd::gpu::FgImageFormat::RGBA16F, 256U, 256U);
    {
        fc::FrameResourceDesc rb(&alloc);
        rb.name.append("mesh_args");
        rb.kind       = fc::FrameResourceKind::IndirectArgs; // the meshlet-count args buffer → the "args" slot at record
        rb.size_bytes = 256U;
        b.desc().resources.push_back(static_cast<fc::FrameResourceDesc&&>(rb));
    }
    const u32 mi = b.add_pass(StringView("meshi"), StringView("raster.mesh.indirect"));
    b.pass_reads(mi, StringView("mesh_args"));
    b.pass_writes(mi, StringView("out"));
    b.pass_shader(mi, StringView("engine://sh/meshlet"));

    fc::FramePlans                   plans(&alloc);
    crd::renderasset::DiagnosticList diags(&alloc);
    REQUIRE(fc::build_frame_plans(b.desc(), plans, diags));
    REQUIRE_FALSE(diags.has_errors());

    // ⭐ the mesh.indirect pass's replay plan is found by name hash: a 3-command scope (Begin/Draw(DispatchMeshIndirect)/End).
    const rg::CeirPassPlan* const plan = plans.table.find(rp::pass_param_id(StringView("meshi")));
    REQUIRE(plan != nullptr);
    CHECK(plan->ctx != nullptr);
    CHECK(plan->commands != nullptr);
    CHECK(plan->count == 3U);
}

TEST_CASE("ceir 16d-live-2: build_frame_plans builds scene.raster plans (forward + depth-only + mrt>=2 via 16d-live-4a-4)",
          "[framecook][ceir][frame]")
{
    using SV = crd::containers::StringView;
    SECTION("a forward scene pass (raster.geometry) gets a single-colour CEIR scene plan")
    {
        crd::memory::TlsfAllocator       alloc(2U << 20U, nullptr, "16d-live-2-fwd");
        fc::FrameGraphBuilder            b(&alloc, SV("scene_fwd"));
        b.add_image(SV("scene_color"), crd::gpu::FgImageFormat::RGBA16F, 1920U, 1080U, /*sampled*/ true);
        const u32 fwd = b.add_pass(SV("forward"), SV("raster.geometry"));
        b.pass_writes(fwd, SV("scene_color"));
        fc::FramePlans                   plans(&alloc);
        crd::renderasset::DiagnosticList diags(&alloc);
        REQUIRE(fc::build_frame_plans(b.desc(), plans, diags));
        REQUIRE_FALSE(diags.has_errors());
        const rg::CeirPassPlan* const plan = plans.table.find(rp::pass_param_id(SV("forward")));
        REQUIRE(plan != nullptr);
        CHECK(plan->count == 3U); // Begin(scope: colour + a depth TEMPLATE) / Draw(scene_draw_list) / End
    }
    SECTION("a depth-only cascade pass (raster.depth_only) gets a 0-colour depth-only scene plan")
    {
        crd::memory::TlsfAllocator       alloc(2U << 20U, nullptr, "16d-live-2-depth");
        fc::FrameGraphBuilder            b(&alloc, SV("scene_depth"));
        b.add_image(SV("shadow_atlas"), crd::gpu::FgImageFormat::D32Float, 1024U, 1024U, /*sampled*/ true, 4U);
        const u32 csm = b.add_pass(SV("cascade"), SV("raster.depth_only"));
        b.pass_writes(csm, SV("shadow_atlas"), /*indexed*/ true);
        fc::FramePlans                   plans(&alloc);
        crd::renderasset::DiagnosticList diags(&alloc);
        REQUIRE(fc::build_frame_plans(b.desc(), plans, diags));
        REQUIRE_FALSE(diags.has_errors());
        const rg::CeirPassPlan* const plan = plans.table.find(rp::pass_param_id(SV("cascade")));
        REQUIRE(plan != nullptr);
        CHECK(plan->count == 3U); // a 0-colour depth-only scope still lowers Begin / Draw / End
    }
    SECTION("an mrt>=2 scene pass (raster.mrt, 2 colour writes) BUILDS an N-colour CEIR plan (16d-live-4a-4 un-skip)")
    {
        // ⛔ gap iii: 16d-live-2 SKIPPED an mrt>=2 pass; now build_scene_ceir handles mrt>=2 (4a-2/3), so the skip is INVERTED
        // — the pass builds the N-colour scene plan (mrt_n=2, per-attachment blends) + records via record_ceir_render.
        crd::memory::TlsfAllocator       alloc(2U << 20U, nullptr, "16d-live-4a-4-mrt");
        fc::FrameGraphBuilder            b(&alloc, SV("scene_mrt"));
        b.add_image(SV("accum"), crd::gpu::FgImageFormat::RGBA16F, 1920U, 1080U, true);
        b.add_image(SV("revealage"), crd::gpu::FgImageFormat::R16F, 1920U, 1080U, true);
        const u32 acc = b.add_pass(SV("accumulate"), SV("raster.mrt"));
        b.pass_writes(acc, SV("accum"));     // colour 0
        b.pass_writes(acc, SV("revealage")); // colour 1 -> 2 colour attachments = mrt>=2
        fc::set_pass_enum(b.desc().passes[acc], SV(fc::pp::kBlendSlot[0]), static_cast<crd::u32>(crd::gpu::BlendMode::Additive));
        fc::set_pass_enum(b.desc().passes[acc], SV(fc::pp::kBlendSlot[1]),
                          static_cast<crd::u32>(crd::gpu::BlendMode::RevealageMultiply));
        fc::FramePlans                   plans(&alloc);
        crd::renderasset::DiagnosticList diags(&alloc);
        REQUIRE(fc::build_frame_plans(b.desc(), plans, diags));
        REQUIRE_FALSE(diags.has_errors());
        const rg::CeirPassPlan* const plan = plans.table.find(rp::pass_param_id(SV("accumulate")));
        REQUIRE(plan != nullptr); // ⭐ mrt>=2 now BUILDS a plan (was nullptr/skipped)
        CHECK(plan->count == 3U); // Begin(scope: 2 colour attachments) / Draw(scene_draw_list) / End
    }
    SECTION("a velocity-style raster.mrt (ONE colour + ONE depth) is NOT mrt>=2 — the depth write is excluded")
    {
        crd::memory::TlsfAllocator       alloc(2U << 20U, nullptr, "16d-live-2-vel");
        fc::FrameGraphBuilder            b(&alloc, SV("scene_vel"));
        b.add_image(SV("velocity"), crd::gpu::FgImageFormat::RGBA16F, 1920U, 1080U, true);
        b.add_image(SV("scene_depth"), crd::gpu::FgImageFormat::D32Float, 1920U, 1080U, true);
        const u32 vel = b.add_pass(SV("velocity_pass"), SV("raster.mrt"));
        b.pass_writes(vel, SV("velocity"));    // colour
        b.pass_writes(vel, SV("scene_depth")); // depth-format -> routes to depth_target, NOT a 2nd colour attachment
        fc::FramePlans                   plans(&alloc);
        crd::renderasset::DiagnosticList diags(&alloc);
        REQUIRE(fc::build_frame_plans(b.desc(), plans, diags));
        REQUIRE_FALSE(diags.has_errors());
        const rg::CeirPassPlan* const plan = plans.table.find(rp::pass_param_id(SV("velocity_pass")));
        REQUIRE(plan != nullptr); // 1 colour + 1 depth = mrt_n=1 -> a normal single-colour scene plan, never skipped
        CHECK(plan->count == 3U);
    }
}

TEST_CASE("ceir 16-3c-5 prereq: build_frame_plans succeeds on every shipped frame asset's fullscreen passes",
          "[framecook][ceir][frame]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U, nullptr, "3c5-prereq");
    // the shipped renderable frames — their fullscreen passes (tonemap `post`, moment convert/blur, HZB, TAA resolve) are the
    // real extraction targets. build_frame_plans must build a CEIR composite for each BEFORE 3c-5 makes a null plan an error.
    SECTION("forward_srgb") { gate_frame_plans("forward_srgb", alloc); }
    SECTION("forward_agx") { gate_frame_plans("forward_agx", alloc); }
    SECTION("forward_csm") { gate_frame_plans("forward_csm", alloc); }
    SECTION("forward_csm_srgb") { gate_frame_plans("forward_csm_srgb", alloc); }
    SECTION("forward_csm_agx") { gate_frame_plans("forward_csm_agx", alloc); }
    SECTION("forward_csm_moment") { gate_frame_plans("forward_csm_moment", alloc); }
    SECTION("forward_csm_gpu") { gate_frame_plans("forward_csm_gpu", alloc); }
    SECTION("forward_csm_gpu_srgb") { gate_frame_plans("forward_csm_gpu_srgb", alloc); }
    // ⭐⭐ CEIR-18z (band close): the Forward+ / clustered 3D-froxel frame graphs must build a CEIR replay plan for every
    // fullscreen pass too (18a-2/18b never ran here — the hardcoded list). The light_cull(_3d) compute pass + `instances`
    // external_buffer is the forward_csm_gpu compute shape.
    SECTION("forward_plus") { gate_frame_plans("forward_plus", alloc); }
    SECTION("forward_plus_gpu") { gate_frame_plans("forward_plus_gpu", alloc); }
    SECTION("forward_clustered_3d_gpu") { gate_frame_plans("forward_clustered_3d_gpu", alloc); }
}
