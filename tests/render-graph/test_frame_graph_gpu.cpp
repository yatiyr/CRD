// RAF-7 live-GPU wiring — the unified frame graph executing on a REAL device in ONE SUBMISSION.
//
// A two-pass graph — scene.raster (storage-pulled triangle into a transient colour target) then transfer.copy (the
// transient into an output target) — is compiled (scheduled: scene before copy) and executed on Vulkan AND D3D12
// via `execute_frame`: it builds a gpu-context frame graph, imports the resolved resources, and records each pass's
// RECORD function through a FRAME-RECORDING command encoder, so the whole graph is ONE command buffer / one
// submission (mission Gate 7). The output pixels prove the whole path:
//   FrameGraphTemplate → compile → record functions → canonical commands → encoder → backend → GPU.
//
// The FOUR frame-graph-shaped kinds (MRT · indexed-indirect · comparison-sampler/shadow · bindless) are gated here
// through that one-submission path with DISTINGUISHABLE per-slot output. ⛔ CORRECTED (RAF-7): these verbs are NOT
// blocked by "coherent transient allocation" (the RAF-2-era misdiagnosis) — they are simply frame-recording verbs,
// which is exactly the mode `execute_frame` runs them in. No synchronous verb scaffolding exists or is needed.
//
// GPU tests skip cleanly when no device / no shader-object capability is present.

#include <crd/rendergraph/frame_graph.hpp>

#include <crd/ceir/context.hpp>                     // CEIR-16-3d: the plan's Context for the device A/B render
#include <crd/ceir/gpu/render_fullscreen_build.hpp> // CEIR-16-3d: build_fullscreen_ceir — the CEIR-replay plan
#include <crd/gpu/context.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/renderpass/executor_registry.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#if defined(_WIN32)
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#endif

#include <catch2/catch_test_macros.hpp>
#include <ckir_raster_triangle.hpp>
#include <ckir_vertex_pull.hpp>

#include <memory>

namespace rp = crd::renderpass;
using crd::f32;
using crd::u32;
using crd::u64;

namespace
{
rp::TypedValue tv_vec4(f32 r, f32 g, f32 b, f32 a)
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::Vec4;
    t.v4[0] = r;
    t.v4[1] = g;
    t.v4[2] = b;
    t.v4[3] = a;
    return t;
}
rp::TypedValue tv_f32(f32 v)
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::F32;
    t.f = v;
    return t;
}
rp::TypedValue tv_enum(u32 e)
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::Enum;
    t.e = e;
    return t;
}
rp::TypedValue tv_bool(bool b)
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::Bool;
    t.b = b;
    return t;
}

// ⭐⭐ RAF-8 gap (a): a fullscreen pass reads a DEPTH texture as RAW FLOAT (`depth_as_float`) — the HZB / TAA shape.
// The executor must NOT pair a comparison sampler (that is a shadow lookup); the encoder must route the depth texture
// (no comparison sampler) to draw_textured, not to a procedural no-op. Bind a depth texture of a known value 0.5 and
// assert the sampled RED ≈ 128 — a comparison sampler would test/fail (≈0 or 255), a dropped read leaves the clear (0).
void run_depth_float_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;
    constexpr u32 dim = 32U;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_pull_textured_fs(fg, fe); // samples texture(0,1) at uv → outputs the sampled value
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the depth_as_float run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 tw = 16U;
    float depth[tw * tw];
    for (u32 i = 0; i < tw * tw; ++i) { depth[i] = 0.5F; }
    auto dtex = raster.create_depth_texture(tw, tw, depth);
    auto color = raster.create_color_target(dim, dim);
    REQUIRE(dtex != nullptr);
    REQUIRE(color != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);

    const u64 r_col = rp::pass_param_id("col");
    const u64 r_in0 = rp::pass_param_id("dep");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_col, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_in0, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("fullscreen.raster");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("depth_as_float"), tv_bool(true)});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget,
                                                      rp::SlotAccess::Write, r_col});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("input0"), rp::SlotResourceKind::Texture,
                                                      rp::SlotAccess::Read, r_in0});
        tmpl.add_pass(p);
    }
    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
    // ⭐ CEIR-16-3d-3: record the depth_as_float fullscreen pass through the GENERIC CEIR replay (record_ceir_render,
    // driven by a build_fullscreen_ceir PLAIN plan — depth_as_float ⇒ a plain sampler, never the shadow atlas). This is the
    // SOLE fullscreen record path now (record_fullscreen_raster deleted); the value check brackets the RAW-float read.
    crd::ceir::Context                  cctx(&alloc);
    crd::ceir::gpu::FullscreenBuildDesc bd;
    bd.num_inputs             = 1U;
    bd.inputs[0].source_param = rp::pass_param_id("input0");
    bd.inputs[0].is_depth     = true;
    bd.depth_as_float         = true;
    crd::containers::Array<crd::ceir::gpu::LoweredCommand> cmds(&alloc);
    REQUIRE(crd::ceir::gpu::build_fullscreen_ceir(cctx, bd, cmds));
    CeirPlanTable plans(&alloc);
    plans.bind(1U, CeirPassPlan{&cctx, cmds.data(), static_cast<u32>(cmds.size())}); // keyed by the pass name_hash (1U)
    GraphExecutorTable records(&alloc);
    REQUIRE(records.register_record(rp::executor_type_id("fullscreen.raster"), record_ceir_render, d));
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_col, rp::SlotResourceKind::ColorTarget, color.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_in0, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, dtex.get()});
    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count, nullptr, nullptr,
                          &plans));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);
    const u32 px = color->read_pixel(dim / 2U, dim / 2U);
    CHECK((px & 0xFFU) >= 100U); // R ≈ 128 (raw depth 0.5 sampled)
    CHECK((px & 0xFFU) <= 160U); // brackets the RAW-float read; a comparison sampler would not land near mid-grey
}

// Build + run the two-pass graph on a raster context; assert the copied output holds the rendered triangle.
void run_graph_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the frame-graph GPU run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 dim = 32U;
    float verts[36] = {0.0F};
    const auto set = [&](int i, f32 x, f32 y, f32 z)
    {
        verts[i * 12 + 0] = x;
        verts[i * 12 + 1] = y;
        verts[i * 12 + 2] = z;
    };
    set(0, 0.0F, -0.8F, 0.0F);
    set(1, 0.8F, 0.8F, 0.0F);
    set(2, -0.8F, 0.8F, 0.0F);
    auto geo_buf = raster.create_storage_buffer(static_cast<u32>(sizeof(verts)));
    REQUIRE(geo_buf != nullptr);
    REQUIRE(raster.upload_storage(*geo_buf, 0U, verts, static_cast<u32>(sizeof(verts))));

    auto scene_target = raster.create_color_target(dim, dim);
    auto copy_target = raster.create_color_target(dim, dim);
    REQUIRE(scene_target != nullptr);
    REQUIRE(copy_target != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);

    const u64 r_scene = rp::pass_param_id("scene");
    const u64 r_geo = rp::pass_param_id("geo");
    const u64 r_copy = rp::pass_param_id("copy");

    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_scene, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Transient, 1U});
    tmpl.add_resource(GraphResource{r_geo, rp::SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_copy, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});

    // Pass 1 — scene.raster: draw the storage-pulled triangle into `scene`.
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("scene.raster");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("clear_color"), tv_vec4(0.0F, 0.0F, 1.0F, 1.0F)});
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("clear_depth"), tv_f32(1.0F)});
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("depth_compare"), tv_enum(3U)}); // LessEqual
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_scene});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("geometry"), rp::SlotResourceKind::StorageBuffer,
                                                      rp::SlotAccess::Read, r_geo});
        tmpl.add_pass(p);
    }
    // Pass 2 — transfer.copy: copy `scene` → `copy`.
    {
        GraphPass p;
        p.name_hash = 2U;
        p.payload.executor = rp::executor_type_id("transfer.copy");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Transfer;
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("src"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Read, r_scene});
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("dst"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_copy});
        tmpl.add_pass(p);
    }

    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
    REQUIRE(compiled.schedule().size() == 2U);
    REQUIRE(compiled.schedule()[0] == 0U); // scene before copy (data dependency)
    REQUIRE(compiled.schedule()[1] == 1U);

    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_scene, rp::SlotResourceKind::ColorTarget, scene_target.get(), nullptr, nullptr});
    table.bind(ResolvedResource{r_geo, rp::SlotResourceKind::StorageBuffer, nullptr, geo_buf.get(), nullptr});
    table.bind(ResolvedResource{r_copy, rp::SlotResourceKind::ColorTarget, copy_target.get(), nullptr, nullptr});

    PassPrograms programs;
    programs.raster = program.get();
    // RAF-7 (one-submission): execute_frame runs the compiled graph as a real frame via a gpu-context frame graph —
    // ONE command buffer, cross-pass barriers + readback owned by it. Gate 7's "one submission where expected".
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);

    // The scene pass rendered the triangle into `scene`; the copy pass reproduced it in `copy`. In ONE-submission
    // mode `scene` is an INTERMEDIATE the copy consumes (it ends in TRANSFER_SRC and the frame graph does not
    // re-copy it to its host readback), so the checkable proof is the FINAL output `copy` — which reflects `scene`'s
    // render exactly (the whole chain scene.raster → transfer.copy in one submission).
    CHECK((copy_target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle centre, copied
    CHECK(((copy_target->read_pixel(0U, 0U) >> 16U) & 0xFFU) >= 250U);    // blue clear corner, copied
}

// ── the 4 FRAME-GRAPH-SHAPED command kinds through the ONE-SUBMISSION graph ──────────────────────────────────────
// MRT / indexed-indirect / comparison-sampler(shadow) / bindless: mapped by the RAF-2 encoder but only ever run in
// frame-recording mode. Driven here through execute_frame (the gpu-context frame graph in one submission) via
// TEST executors registered WITHOUT an engine-enum edit (the RAF-6 precedent). Each proves DISTINGUISHABLE per-slot
// output, so a silently-dropped attachment/binding fails loudly.

// MRT record: two colour attachments + a storage-pulled draw (build_gbuffer_two_output_fs writes RED to 0, GREEN to 1).
void record_test_mrt(const crd::rendergraph::PassPayload& /*payload*/, crd::rendergraph::RecordContext& ctx,
                     crd::gpu::ICommandEncoder& enc)
{
    namespace gpu = crd::gpu;
    gpu::IRasterTarget* c0 = ctx.color_target(rp::pass_param_id("color0"));
    gpu::IRasterTarget* c1 = ctx.color_target(rp::pass_param_id("color1"));
    gpu::IStorageBuffer* geo = ctx.storage(rp::pass_param_id("geometry"));
    if (!ctx.ok() || c0 == nullptr || c1 == nullptr || geo == nullptr)
    {
        return;
    }
    gpu::RenderingDesc rd;
    rd.width = c0->width();
    rd.height = c0->height();
    // CEIR-14z-4b: DISTINCT per-attachment clears — c0 clears BLUE, c1 clears RED. A verb that broadcast ONE clear to
    // every attachment (the pre-14z-4b shared-clear bug) would leave both corners the SAME colour; distinct corners prove
    // each attachment carried its OWN clear.
    const gpu::ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};
    const gpu::ClearColor red{1.0F, 0.0F, 0.0F, 1.0F};
    rd.color.push_back(gpu::ColorAttachmentDesc{c0, gpu::LoadOp::Clear, gpu::StoreOp::Store, blue, gpu::BlendMode::Opaque});
    rd.color.push_back(gpu::ColorAttachmentDesc{c1, gpu::LoadOp::Clear, gpu::StoreOp::Store, red, gpu::BlendMode::Opaque});
    gpu::RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = gpu::RasterCommandKind::Draw;
    p.geometry.kind = gpu::GeometryKind::StoragePull;
    p.geometry.vertex_or_index_count = 3U;
    p.bindings.push_back(gpu::ResourceBinding{gpu::BindingFrequency::Object, gpu::BindingKind::StorageBuffer, 0U, geo});
    enc.begin_rendering(rd);
    enc.draw(p);
    enc.end_rendering();
}

void run_mrt_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_gbuffer_two_output_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the frame-graph MRT run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 dim = 32U;
    float verts[36] = {0.0F};
    const auto set = [&](int i, f32 x, f32 y, f32 z)
    {
        verts[i * 12 + 0] = x;
        verts[i * 12 + 1] = y;
        verts[i * 12 + 2] = z;
    };
    set(0, 0.0F, -0.8F, 0.0F);
    set(1, 0.8F, 0.8F, 0.0F);
    set(2, -0.8F, 0.8F, 0.0F);
    auto geo_buf = raster.create_storage_buffer(static_cast<u32>(sizeof(verts)));
    REQUIRE(geo_buf != nullptr);
    REQUIRE(raster.upload_storage(*geo_buf, 0U, verts, static_cast<u32>(sizeof(verts))));

    auto c0 = raster.create_color_target(dim, dim);
    auto c1 = raster.create_color_target(dim, dim);
    REQUIRE(c0 != nullptr);
    REQUIRE(c1 != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    rp::PassExecutorDesc mrt{};
    mrt.id = rp::executor_type_id("test.mrt");
    mrt.name = "test.mrt";
    mrt.schema.version = 1U;
    mrt.schema.queue = rp::QueueKind::Graphics;
    mrt.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("color0"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, true});
    mrt.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("color1"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, true});
    mrt.schema.slots.push_back(rp::ResourceSlotSpec{rp::pass_param_id("geometry"), rp::SlotResourceKind::StorageBuffer,
                                                    rp::SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(mrt, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(rp::executor_type_id("test.mrt"), record_test_mrt, d));

    const u64 r_c0 = rp::pass_param_id("c0");
    const u64 r_c1 = rp::pass_param_id("c1");
    const u64 r_geo = rp::pass_param_id("geo");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_c0, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_c1, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_geo, rp::SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("test.mrt");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("color0"), rp::SlotResourceKind::ColorTarget,
                                                      rp::SlotAccess::Write, r_c0});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("color1"), rp::SlotResourceKind::ColorTarget,
                                                      rp::SlotAccess::Write, r_c1});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("geometry"), rp::SlotResourceKind::StorageBuffer,
                                                      rp::SlotAccess::Read, r_geo});
        tmpl.add_pass(p);
    }

    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));

    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_c0, rp::SlotResourceKind::ColorTarget, c0.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_c1, rp::SlotResourceKind::ColorTarget, c1.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_geo, rp::SlotResourceKind::StorageBuffer, nullptr, geo_buf.get(), nullptr, nullptr});

    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);

    // ⛔ attachment 0 = RED, attachment 1 = GREEN — the DISTINGUISHABLE per-attachment proof. A dropped attachment 1
    // (the historical "attachment 1 black" scar) would leave c1 green at 0 and FAIL loudly.
    const u32 p0 = c0->read_pixel(dim / 2U, dim / 2U);
    const u32 p1 = c1->read_pixel(dim / 2U, dim / 2U);
    CHECK((p0 & 0xFFU) >= 250U);         // c0 RED high
    CHECK(((p0 >> 8U) & 0xFFU) < 8U);    // c0 green low
    CHECK(((p1 >> 8U) & 0xFFU) >= 250U); // c1 GREEN high — attachment 1 was written
    CHECK((p1 & 0xFFU) < 8U);            // c1 red low

    // ⭐ CEIR-14z-4b: the DISTINCT per-attachment CLEAR proof — c0's corner is BLUE, c1's corner is RED (each attachment's
    // OWN clear). A verb that broadcast one shared clear to all attachments (the pre-14z-4b bug) would leave both corners
    // the SAME colour; here they differ, so each attachment's clear was threaded independently. The screen corner (0,0)
    // lies outside the ±0.8 triangle on BOTH backends (no y-flip ambiguity — the triangle reaches no screen corner).
    const u32 k0 = c0->read_pixel(0U, 0U);
    const u32 k1 = c1->read_pixel(0U, 0U);
    CHECK(((k0 >> 16U) & 0xFFU) >= 250U); // c0 corner BLUE high
    CHECK((k0 & 0xFFU) < 8U);             // c0 corner red low
    CHECK((k1 & 0xFFU) >= 250U);          // c1 corner RED high
    CHECK(((k1 >> 16U) & 0xFFU) < 8U);    // c1 corner blue low — DISTINCT from c0's blue clear
}

// BINDLESS record: a fullscreen draw sampling a 2-element texture array by a per-fragment index (uv.x<0.5 ? 0 : 1).
void record_test_bindless(const crd::rendergraph::PassPayload& /*payload*/, crd::rendergraph::RecordContext& ctx,
                          crd::gpu::ICommandEncoder& enc)
{
    namespace gpu = crd::gpu;
    gpu::IRasterTarget* color = ctx.color_target(rp::pass_param_id("color"));
    gpu::ITexture* t0 = ctx.texture(rp::pass_param_id("tex0"));
    gpu::ITexture* t1 = ctx.texture(rp::pass_param_id("tex1"));
    if (!ctx.ok() || color == nullptr || t0 == nullptr || t1 == nullptr)
    {
        return;
    }
    gpu::ITexture* texs[2] = {t0, t1}; // consumed synchronously by enc.draw() (frame-recording draw_bindless)
    gpu::RenderingDesc rd;
    rd.width = color->width();
    rd.height = color->height();
    rd.color.push_back(
        gpu::ColorAttachmentDesc{color, gpu::LoadOp::Clear, gpu::StoreOp::Store, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::BlendMode::Opaque});
    gpu::RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = gpu::RasterCommandKind::Draw;
    p.geometry.kind = gpu::GeometryKind::None; // build_textured_vs is attributeless (position + uv from VertexIndex)
    p.geometry.vertex_or_index_count = 3U;
    gpu::ResourceBinding b{};
    b.frequency = gpu::BindingFrequency::Material;
    b.kind = gpu::BindingKind::BindlessTextureArray;
    b.slot = 0U;
    b.texture_array = static_cast<gpu::ITexture* const*>(texs);
    b.array_count = 2U;
    p.bindings.push_back(b);
    enc.begin_rendering(rd);
    enc.draw(p);
    enc.end_rendering();
}

void run_bindless_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    if (!raster.supports_bindless())
    {
        WARN("adapter has no non-uniform bindless indexing; skipping the frame-graph bindless run");
        return;
    }
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_bindless_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the frame-graph bindless run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 dim = 32U;
    crd::u8 red[4U * 4U * 4U];
    crd::u8 green[4U * 4U * 4U];
    for (u32 i = 0; i < 16U; ++i)
    {
        red[i * 4U] = 255U;   red[i * 4U + 1U] = 0U;   red[i * 4U + 2U] = 0U;   red[i * 4U + 3U] = 255U;
        green[i * 4U] = 0U;   green[i * 4U + 1U] = 255U; green[i * 4U + 2U] = 0U; green[i * 4U + 3U] = 255U;
    }
    auto tex0 = raster.create_texture(4U, 4U, red);
    auto tex1 = raster.create_texture(4U, 4U, green);
    REQUIRE(tex0 != nullptr);
    REQUIRE(tex1 != nullptr);
    auto color = raster.create_color_target(dim, dim);
    REQUIRE(color != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    rp::PassExecutorDesc bl{};
    bl.id = rp::executor_type_id("test.bindless");
    bl.name = "test.bindless";
    bl.schema.version = 1U;
    bl.schema.queue = rp::QueueKind::Graphics;
    bl.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, true});
    bl.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("tex0"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, true});
    bl.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("tex1"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(bl, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(rp::executor_type_id("test.bindless"), record_test_bindless, d));

    const u64 r_col = rp::pass_param_id("col");
    const u64 r_t0 = rp::pass_param_id("t0");
    const u64 r_t1 = rp::pass_param_id("t1");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_col, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_t0, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_t1, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("test.bindless");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_col});
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("tex0"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, r_t0});
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("tex1"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, r_t1});
        tmpl.add_pass(p);
    }

    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));

    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_col, rp::SlotResourceKind::ColorTarget, color.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_t0, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, tex0.get()});
    table.bind(ResolvedResource{r_t1, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, tex1.get()});

    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);

    // ⛔ left half samples element 0 (RED), right half element 1 (GREEN) — the DISTINGUISHABLE proof: binding only the
    // first leaves green at 0, binding them SWAPPED leaves red at 0.
    const u32 left = color->read_pixel(dim / 4U, dim / 2U);
    const u32 right = color->read_pixel(3U * dim / 4U, dim / 2U);
    CHECK((left & 0xFFU) >= 250U);          // left RED
    CHECK(((right >> 8U) & 0xFFU) >= 250U); // right GREEN
}

// A SCENE-SHADOW fragment shader: samples the atlas at bindings 4/5 (SAMPLED_IMAGE + comparison SAMPLER — the
// draw_storage_shadowed_depth layout, NOT the simple draw_shadow 1/2). ref = uv.x vs a 0.5 depth, LessEqual ⇒
// screen-left (uv.x ≤ 0.5) lit (white), screen-right shadowed (black).
void build_scene_shadow_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const int uv = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    const int tex = g.texture(0, 4, kir::DType::F32, kir::TexDim::Tex2D, false, false, /*shadow=*/true);
    const int samp = g.sampler(0, 5, /*shadow=*/true);
    const int ref = g.swizzle(uv, 0);
    const int r = g.tex_sample_cmp(tex, samp, uv, ref);
    const int one = g.constant(1.0, sh, kir::DType::F32);
    fe.stage = kir::KStage::Fragment;
    fe.n_out = 1;
    fe.out[0] = {g.vec4(r, r, r, one), 0};
}

// SHADOW record: a storage-pulled DEPTH-TESTED draw with a depth atlas (SampledTexture, is_depth) + a comparison
// sampler ⇒ the encoder routes to draw_storage_shadowed_depth.
void record_test_shadow(const crd::rendergraph::PassPayload& /*payload*/, crd::rendergraph::RecordContext& ctx,
                        crd::gpu::ICommandEncoder& enc)
{
    namespace gpu = crd::gpu;
    gpu::IRasterTarget* cd = ctx.color_target(rp::pass_param_id("color")); // colour+depth target
    gpu::IStorageBuffer* geo = ctx.storage(rp::pass_param_id("geometry"));
    gpu::ITexture* atlas = ctx.texture(rp::pass_param_id("atlas"));
    if (!ctx.ok() || cd == nullptr || geo == nullptr || atlas == nullptr)
    {
        return;
    }
    gpu::RenderingDesc rd;
    rd.width = cd->width();
    rd.height = cd->height();
    rd.color.push_back(gpu::ColorAttachmentDesc{cd, gpu::LoadOp::Clear, gpu::StoreOp::Store,
                                                gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::BlendMode::Opaque});
    rd.depth = gpu::DepthStencilAttachmentDesc{cd,   true, gpu::LoadOp::Clear, gpu::StoreOp::Store, 1.0F, true,
                                               gpu::DepthCompare::LessEqual};
    gpu::RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = gpu::RasterCommandKind::Draw;
    p.geometry.kind = gpu::GeometryKind::StoragePull;
    p.geometry.vertex_or_index_count = 3U;
    p.bindings.push_back(gpu::ResourceBinding{gpu::BindingFrequency::Object, gpu::BindingKind::StorageBuffer, 0U, geo});
    gpu::ResourceBinding tb{};
    tb.frequency = gpu::BindingFrequency::Material;
    tb.kind = gpu::BindingKind::SampledTexture;
    tb.slot = 4U;
    tb.texture = atlas;
    p.bindings.push_back(tb);
    gpu::ResourceBinding sb{};
    sb.frequency = gpu::BindingFrequency::Material;
    sb.kind = gpu::BindingKind::ComparisonSampler;
    sb.slot = 5U;
    p.bindings.push_back(sb);
    enc.begin_rendering(rd);
    enc.draw(p);
    enc.end_rendering();
}

void run_shadow_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_pull_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    build_scene_shadow_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the frame-graph shadow run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 dim = 32U;
    // a fullscreen triangle: {x,y,z,u,v}, UV [0,2] over NDC [-1,3] ⇒ visible screen maps UV [0,1].
    const float verts[15] = {-1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 3.0F,  -1.0F, 0.0F,
                             2.0F,  0.0F,  -1.0F, 3.0F, 0.0F, 0.0F,  2.0F};
    auto geo_buf = raster.create_storage_buffer(static_cast<u32>(sizeof(verts)));
    REQUIRE(geo_buf != nullptr);
    REQUIRE(raster.upload_storage(*geo_buf, 0U, verts, static_cast<u32>(sizeof(verts))));

    constexpr u32 tw = 16U;
    float depth[tw * tw];
    for (u32 i = 0; i < tw * tw; ++i)
    {
        depth[i] = 0.5F;
    }
    auto atlas = raster.create_depth_texture(tw, tw, depth);
    REQUIRE(atlas != nullptr);
    auto cd = raster.create_color_depth_target(dim, dim);
    REQUIRE(cd != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    rp::PassExecutorDesc sh{};
    sh.id = rp::executor_type_id("test.shadow");
    sh.name = "test.shadow";
    sh.schema.version = 1U;
    sh.schema.queue = rp::QueueKind::Graphics;
    sh.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, true});
    sh.schema.slots.push_back(rp::ResourceSlotSpec{rp::pass_param_id("geometry"), rp::SlotResourceKind::StorageBuffer,
                                                   rp::SlotAccess::Read, true});
    sh.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("atlas"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(sh, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(rp::executor_type_id("test.shadow"), record_test_shadow, d));

    const u64 r_cd = rp::pass_param_id("cd");
    const u64 r_geo = rp::pass_param_id("geo");
    const u64 r_atlas = rp::pass_param_id("atlas_res");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_cd, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_geo, rp::SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_atlas, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("test.shadow");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_cd});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("geometry"),
                                                      rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, r_geo});
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("atlas"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, r_atlas});
        tmpl.add_pass(p);
    }

    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));

    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_cd, rp::SlotResourceKind::ColorTarget, cd.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_geo, rp::SlotResourceKind::StorageBuffer, nullptr, geo_buf.get(), nullptr, nullptr});
    table.bind(ResolvedResource{r_atlas, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, atlas.get()});

    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);

    // ⛔ left lit (comparison-sampler PASS ⇒ white), right shadowed (FAIL ⇒ black) — the comparison-sampler proof.
    const u32 left = cd->read_pixel(dim / 4U, dim / 2U);
    const u32 right = cd->read_pixel(3U * dim / 4U, dim / 2U);
    CHECK((left & 0xFFU) >= 200U);  // left WHITE (lit)
    CHECK((right & 0xFFU) < 64U);   // right BLACK (shadowed)
}

// INDEXED-INDIRECT record: an ExecuteIndirect / vkCmdDrawIndexedIndirect over a device args buffer (max_draws = 1).
void record_test_indirect(const crd::rendergraph::PassPayload& /*payload*/, crd::rendergraph::RecordContext& ctx,
                          crd::gpu::ICommandEncoder& enc)
{
    namespace gpu = crd::gpu;
    gpu::IRasterTarget* cd = ctx.color_target(rp::pass_param_id("color"));
    gpu::IStorageBuffer* idx = ctx.storage(rp::pass_param_id("indices"));
    gpu::IStorageBuffer* args = ctx.storage(rp::pass_param_id("args"));
    if (!ctx.ok() || cd == nullptr || idx == nullptr || args == nullptr)
    {
        return;
    }
    gpu::RenderingDesc rd;
    rd.width = cd->width();
    rd.height = cd->height();
    rd.color.push_back(gpu::ColorAttachmentDesc{cd, gpu::LoadOp::Clear, gpu::StoreOp::Store,
                                                gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::BlendMode::Opaque});
    rd.depth = gpu::DepthStencilAttachmentDesc{cd,   true, gpu::LoadOp::Clear, gpu::StoreOp::Store, 0.0F, true,
                                               gpu::DepthCompare::Always};
    gpu::RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = gpu::RasterCommandKind::DrawIndexedIndirect;
    p.geometry.kind = gpu::GeometryKind::Indirect;
    p.geometry.vertex_or_index_count = 3U;
    p.geometry.index_buffer = idx;
    p.geometry.index_offset = 0U;
    p.geometry.args_buffer = args;
    p.geometry.args_offset = 0U;
    p.geometry.max_draws = 1U;
    p.bindings.push_back(gpu::ResourceBinding{gpu::BindingFrequency::Object, gpu::BindingKind::StorageBuffer, 0U, idx});
    enc.begin_rendering(rd);
    enc.draw(p);
    enc.end_rendering();
}

// `is_dx12` selects the args byte layout: D3D12's command signature prepends a DrawIndex root constant (6 u32 / 24 B),
// Vulkan reads a bare VkDrawIndexedIndirectCommand (5 u32 / 20 B). Same verb, same encoder — only the device args differ.
void run_indirect_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc,
                      bool is_dx12)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_indexed_probe_vs(vg, ve); // positions by index VALUE {4,5,6}; no storage read (DX12-SRV-safe)
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the frame-graph indirect run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 dim = 32U;
    const u32 indices[3] = {4U, 5U, 6U}; // build_indexed_probe_vs positions a centred triangle from these
    auto idx_buf = raster.create_storage_buffer(static_cast<u32>(sizeof(indices)));
    REQUIRE(idx_buf != nullptr);
    REQUIRE(raster.upload_storage(*idx_buf, 0U, indices, static_cast<u32>(sizeof(indices))));

    // one indexed-indirect command; the DrawIndex slot (DX12 only) is 0.
    u32 args_words[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    u32 args_bytes = 0U;
    if (is_dx12)
    {
        args_words[0] = 0U; // DrawIndex root constant
        args_words[1] = 3U; // IndexCountPerInstance
        args_words[2] = 1U; // InstanceCount
        args_words[3] = 0U; // StartIndexLocation
        args_words[4] = 0U; // BaseVertexLocation
        args_words[5] = 0U; // StartInstanceLocation
        args_bytes = 24U;
    }
    else
    {
        args_words[0] = 3U; // indexCount
        args_words[1] = 1U; // instanceCount
        args_words[2] = 0U; // firstIndex
        args_words[3] = 0U; // vertexOffset
        args_words[4] = 0U; // firstInstance
        args_bytes = 20U;
    }
    auto args_buf = raster.create_storage_buffer(32U); // ≥ 24 for either layout
    REQUIRE(args_buf != nullptr);
    REQUIRE(raster.upload_storage(*args_buf, 0U, args_words, args_bytes));

    auto cd = raster.create_color_depth_target(dim, dim);
    REQUIRE(cd != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    rp::PassExecutorDesc in{};
    in.id = rp::executor_type_id("test.indirect");
    in.name = "test.indirect";
    in.schema.version = 1U;
    in.schema.queue = rp::QueueKind::Graphics;
    in.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, true});
    in.schema.slots.push_back(rp::ResourceSlotSpec{rp::pass_param_id("indices"), rp::SlotResourceKind::StorageBuffer,
                                                   rp::SlotAccess::Read, true});
    in.schema.slots.push_back(rp::ResourceSlotSpec{rp::pass_param_id("args"), rp::SlotResourceKind::StorageBuffer,
                                                   rp::SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(in, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(rp::executor_type_id("test.indirect"), record_test_indirect, d));

    const u64 r_cd = rp::pass_param_id("cd");
    const u64 r_idx = rp::pass_param_id("idx");
    const u64 r_args = rp::pass_param_id("args_res");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_cd, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_idx, rp::SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_args, rp::SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("test.indirect");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_cd});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("indices"),
                                                      rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, r_idx});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("args"), rp::SlotResourceKind::StorageBuffer,
                                                      rp::SlotAccess::Read, r_args});
        tmpl.add_pass(p);
    }

    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));

    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_cd, rp::SlotResourceKind::ColorTarget, cd.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_idx, rp::SlotResourceKind::StorageBuffer, nullptr, idx_buf.get(), nullptr, nullptr});
    table.bind(ResolvedResource{r_args, rp::SlotResourceKind::StorageBuffer, nullptr, args_buf.get(), nullptr, nullptr});

    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);

    // ⛔ the GPU-driven indexed-indirect draw produced the centred triangle — a dropped/misread args buffer leaves 0.
    CHECK((cd->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle centre
}

// RAF-8a increment 1: the BUILT-IN `fullscreen.raster` executor at full RasterFullscreen parity — a 1-input textured
// pass (draw_textured) and an N-input bindless pass (draw_bindless) through the one-submission graph, both backends.
void run_fullscreen_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;
    constexpr u32 dim = 32U;

    // ---- sub-case A: ONE input → draw_textured (single sampled texture at binding 1) ----
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_textured_vs(vg, ve); // attributeless fullscreen + UV
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        crd::gputest::build_pull_textured_fs(fg, fe); // samples texture(0,1) at uv
        auto vs = gctx.create_program(vg, ve);
        if (vs == nullptr)
        {
            WARN("shader compilation unavailable; skipping the fullscreen.raster run");
            return;
        }
        auto fs = gctx.create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster.create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);

        crd::u8 magenta[4U * 4U * 4U];
        for (u32 i = 0; i < 16U; ++i)
        {
            magenta[i * 4U] = 200U; magenta[i * 4U + 1U] = 50U; magenta[i * 4U + 2U] = 150U; magenta[i * 4U + 3U] = 255U;
        }
        auto in0 = raster.create_texture(4U, 4U, magenta);
        auto color = raster.create_color_target(dim, dim);
        REQUIRE(in0 != nullptr);
        REQUIRE(color != nullptr);

        DiagnosticList d(&alloc);
        rp::ExecutorRegistry schemas(&alloc);
        REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
        GraphExecutorTable records(&alloc);
        REQUIRE(records.register_record(rp::executor_type_id("fullscreen.raster"), record_ceir_render, d));

        const u64 r_col = rp::pass_param_id("col");
        const u64 r_in0 = rp::pass_param_id("in0");
        FrameGraphTemplate tmpl(&alloc);
        tmpl.add_resource(GraphResource{r_col, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
        tmpl.add_resource(GraphResource{r_in0, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
        {
            GraphPass p;
            p.name_hash = 1U;
            p.payload.executor = rp::executor_type_id("fullscreen.raster");
            p.payload.schema_version = 1U;
            p.payload.queue = rp::QueueKind::Graphics;
            p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget,
                                                          rp::SlotAccess::Write, r_col});
            p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("input0"), rp::SlotResourceKind::Texture,
                                                          rp::SlotAccess::Read, r_in0});
            tmpl.add_pass(p);
        }
        CompiledFrameGraph compiled(&alloc);
        REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
        // CEIR-16-3d-3: the SOLE fullscreen record path — a build_fullscreen_ceir PLAIN plan (1 input) replayed by
        // record_ceir_render. The value check proves draw_textured sampled the magenta through the generic path.
        crd::ceir::Context                  cctx(&alloc);
        crd::ceir::gpu::FullscreenBuildDesc bd;
        bd.num_inputs             = 1U;
        bd.inputs[0].source_param = rp::pass_param_id("input0");
        crd::containers::Array<crd::ceir::gpu::LoweredCommand> cmds(&alloc);
        REQUIRE(crd::ceir::gpu::build_fullscreen_ceir(cctx, bd, cmds));
        CeirPlanTable plans(&alloc);
        plans.bind(1U, CeirPassPlan{&cctx, cmds.data(), static_cast<u32>(cmds.size())});
        ResourceTable table(&alloc);
        table.bind(ResolvedResource{r_col, rp::SlotResourceKind::ColorTarget, color.get(), nullptr, nullptr, nullptr});
        table.bind(ResolvedResource{r_in0, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, in0.get()});
        PassPrograms programs;
        programs.raster = program.get();
        u32 submit_count = 0U;
        REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count, nullptr, nullptr,
                              &plans));
        REQUIRE_FALSE(d.has_errors());
        CHECK(submit_count == 1U);
        const u32 px = color->read_pixel(dim / 2U, dim / 2U);
        CHECK((px & 0xFFU) >= 180U);          // R ≈ 200 (the sampled magenta) — proves draw_textured ran
        CHECK(((px >> 16U) & 0xFFU) >= 120U); // B ≈ 150
    }

    // ---- sub-case B: TWO inputs → draw_bindless (array in input0/input1 order) ----
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_textured_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        crd::gputest::build_two_texture_composite_fs(fg, fe); // bindless[0].r, bindless[1].g at binding 16
        auto vs = gctx.create_program(vg, ve);
        REQUIRE(vs != nullptr);
        auto fs = gctx.create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster.create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        if (!raster.supports_bindless())
        {
            WARN("no bindless; skipping the fullscreen bindless sub-case");
            return;
        }

        crd::u8 red[4U * 4U * 4U];
        crd::u8 green[4U * 4U * 4U];
        for (u32 i = 0; i < 16U; ++i)
        {
            red[i * 4U] = 255U; red[i * 4U + 1U] = 0U; red[i * 4U + 2U] = 0U; red[i * 4U + 3U] = 255U;
            green[i * 4U] = 0U; green[i * 4U + 1U] = 255U; green[i * 4U + 2U] = 0U; green[i * 4U + 3U] = 255U;
        }
        auto in0 = raster.create_texture(4U, 4U, red);
        auto in1 = raster.create_texture(4U, 4U, green);
        auto color = raster.create_color_target(dim, dim);
        REQUIRE(in0 != nullptr);
        REQUIRE(in1 != nullptr);
        REQUIRE(color != nullptr);

        DiagnosticList d(&alloc);
        rp::ExecutorRegistry schemas(&alloc);
        REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
        GraphExecutorTable records(&alloc);
        REQUIRE(records.register_record(rp::executor_type_id("fullscreen.raster"), record_ceir_render, d));

        const u64 r_col = rp::pass_param_id("col");
        const u64 r_in0 = rp::pass_param_id("in0");
        const u64 r_in1 = rp::pass_param_id("in1");
        FrameGraphTemplate tmpl(&alloc);
        tmpl.add_resource(GraphResource{r_col, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
        tmpl.add_resource(GraphResource{r_in0, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
        tmpl.add_resource(GraphResource{r_in1, rp::SlotResourceKind::Texture, ResourceLifetime::Persistent, 1U});
        {
            GraphPass p;
            p.name_hash = 1U;
            p.payload.executor = rp::executor_type_id("fullscreen.raster");
            p.payload.schema_version = 1U;
            p.payload.queue = rp::QueueKind::Graphics;
            p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget,
                                                          rp::SlotAccess::Write, r_col});
            p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("input0"), rp::SlotResourceKind::Texture,
                                                          rp::SlotAccess::Read, r_in0});
            p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("input1"), rp::SlotResourceKind::Texture,
                                                          rp::SlotAccess::Read, r_in1});
            tmpl.add_pass(p);
        }
        CompiledFrameGraph compiled(&alloc);
        REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
        // CEIR-16-3d-3: the SOLE fullscreen record path — a build_fullscreen_ceir BINDLESS plan (2 inputs ⇒ one
        // resource_table(image)) replayed by record_ceir_render. The value check proves both slots bound in order.
        crd::ceir::Context                  cctx(&alloc);
        crd::ceir::gpu::FullscreenBuildDesc bd;
        bd.num_inputs             = 2U;
        bd.inputs[0].source_param = rp::pass_param_id("input0");
        bd.inputs[1].source_param = rp::pass_param_id("input1");
        crd::containers::Array<crd::ceir::gpu::LoweredCommand> cmds(&alloc);
        REQUIRE(crd::ceir::gpu::build_fullscreen_ceir(cctx, bd, cmds));
        CeirPlanTable plans(&alloc);
        plans.bind(1U, CeirPassPlan{&cctx, cmds.data(), static_cast<u32>(cmds.size())});
        ResourceTable table(&alloc);
        table.bind(ResolvedResource{r_col, rp::SlotResourceKind::ColorTarget, color.get(), nullptr, nullptr, nullptr});
        table.bind(ResolvedResource{r_in0, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, in0.get()});
        table.bind(ResolvedResource{r_in1, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, in1.get()});
        PassPrograms programs;
        programs.raster = program.get();
        u32 submit_count = 0U;
        REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count, nullptr, nullptr,
                              &plans));
        REQUIRE_FALSE(d.has_errors());
        CHECK(submit_count == 1U);
        // build_two_texture_composite_fs → out.r = input0.r, out.g = input1.g (proves both bindless slots bound in order)
        const u32 px = color->read_pixel(dim / 2U, dim / 2U);
        CHECK((px & 0xFFU) >= 250U);          // R from input0 (red)
        CHECK(((px >> 8U) & 0xFFU) >= 250U);  // G from input1 (green)
    }
}

// RAF-8: the canonical CPU MULTI-DRAW batch (GeometryKind::MultiStoragePull → draw_storage_multi_depth) — the scene
// batching perf contract, exercised through the encoder. A dropped MultiStoragePull mapping leaves the target clear.
void record_test_multidraw(const crd::rendergraph::PassPayload& /*payload*/, crd::rendergraph::RecordContext& ctx,
                           crd::gpu::ICommandEncoder& enc)
{
    namespace gpu = crd::gpu;
    gpu::IRasterTarget* cd = ctx.color_target(rp::pass_param_id("color"));
    gpu::IStorageBuffer* geo = ctx.storage(rp::pass_param_id("geometry"));
    if (!ctx.ok() || cd == nullptr || geo == nullptr)
    {
        return;
    }
    gpu::RenderingDesc rd;
    rd.width = cd->width();
    rd.height = cd->height();
    rd.color.push_back(gpu::ColorAttachmentDesc{cd, gpu::LoadOp::Clear, gpu::StoreOp::Store,
                                                gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::BlendMode::Opaque});
    rd.depth = gpu::DepthStencilAttachmentDesc{cd,   true, gpu::LoadOp::Clear, gpu::StoreOp::Store, 1.0F, true,
                                               gpu::DepthCompare::Always};
    static const u32 kCounts[2] = {3U, 3U}; // static ⇒ the host-owned array trivially outlives the draw
    gpu::RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = gpu::RasterCommandKind::DrawMulti;
    p.geometry.kind = gpu::GeometryKind::MultiStoragePull;
    p.geometry.multi_counts = static_cast<const u32*>(kCounts);
    p.geometry.draw_count = 2U;
    p.geometry.first_draw_index = 0U;
    p.bindings.push_back(gpu::ResourceBinding{gpu::BindingFrequency::Object, gpu::BindingKind::StorageBuffer, 0U, geo});
    enc.begin_rendering(rd);
    enc.draw(p);
    enc.end_rendering();
}

void run_multidraw_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the multi-draw run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr u32 dim = 32U;
    float verts[36] = {0.0F};
    const auto set = [&](int i, f32 x, f32 y, f32 z)
    {
        verts[i * 12 + 0] = x;
        verts[i * 12 + 1] = y;
        verts[i * 12 + 2] = z;
    };
    set(0, 0.0F, -0.8F, 0.0F);
    set(1, 0.8F, 0.8F, 0.0F);
    set(2, -0.8F, 0.8F, 0.0F);
    auto geo_buf = raster.create_storage_buffer(static_cast<u32>(sizeof(verts)));
    REQUIRE(geo_buf != nullptr);
    REQUIRE(raster.upload_storage(*geo_buf, 0U, verts, static_cast<u32>(sizeof(verts))));
    auto cd = raster.create_color_depth_target(dim, dim);
    REQUIRE(cd != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    rp::PassExecutorDesc md{};
    md.id = rp::executor_type_id("test.multidraw");
    md.name = "test.multidraw";
    md.schema.version = 1U;
    md.schema.queue = rp::QueueKind::Graphics;
    md.schema.slots.push_back(
        rp::ResourceSlotSpec{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, true});
    md.schema.slots.push_back(rp::ResourceSlotSpec{rp::pass_param_id("geometry"), rp::SlotResourceKind::StorageBuffer,
                                                   rp::SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(md, d));
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(rp::executor_type_id("test.multidraw"), record_test_multidraw, d));

    const u64 r_cd = rp::pass_param_id("cd");
    const u64 r_geo = rp::pass_param_id("geo");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_cd, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_geo, rp::SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    {
        GraphPass p;
        p.name_hash = 1U;
        p.payload.executor = rp::executor_type_id("test.multidraw");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_cd});
        p.payload.resources.push_back(rp::ResourceRef{rp::pass_param_id("geometry"),
                                                      rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, r_geo});
        tmpl.add_pass(p);
    }
    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_cd, rp::SlotResourceKind::ColorTarget, cd.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_geo, rp::SlotResourceKind::StorageBuffer, nullptr, geo_buf.get(), nullptr, nullptr});
    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);
    // ⛔ the batched multi-draw produced the triangle — a dropped MultiStoragePull mapping leaves the centre clear.
    CHECK((cd->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle centre, drawn via draw_storage_multi_depth
}

// ⭐⭐ RAF-8: the SCENE.RASTER executor driven by a RESOLVED DRAW LIST — the real live path. Two items, each carrying
// its OWN storage buffer (a triangle in a distinct screen third), are handed to `record_scene_raster` through a
// DrawListTable. This proves the executor ITERATES the draw list and binds EACH item's geometry (not one shared slot):
// both thirds render, and the CENTRE stays background — the two draws are SEPARATE, and the second does not clear the
// first (the encoder's clear-once). A dropped iteration leaves a third clear; a lost per-item bind draws one triangle
// twice; a missing load-after-first wipes the left triangle. This is the scene half of the "encoder == verb" contract.
void run_scene_drawlist_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster,
                            crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the scene draw-list run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    // Two triangles, one per screen third — non-overlapping, with a background gap down the centre column.
    const auto make_tri = [&](f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy)
    {
        float v[36] = {0.0F};
        const auto set = [&](int i, f32 x, f32 y) { v[i * 12 + 0] = x; v[i * 12 + 1] = y; };
        set(0, ax, ay);
        set(1, bx, by);
        set(2, cx, cy);
        auto buf = raster.create_storage_buffer(static_cast<u32>(sizeof(v)));
        REQUIRE(buf != nullptr);
        REQUIRE(raster.upload_storage(*buf, 0U, v, static_cast<u32>(sizeof(v))));
        return buf;
    };
    auto geo_left = make_tri(-0.5F, -0.8F, -0.1F, 0.8F, -0.9F, 0.8F);
    auto geo_right = make_tri(0.5F, -0.8F, 0.9F, 0.8F, 0.1F, 0.8F);

    constexpr u32 dim = 32U;
    // A color-DEPTH target: the executor uses its bundled depth, so the second draw LOADS (preserves the first) via
    // the depth-aware storage verbs — exactly the live scene shape (a plain colour target would clear each draw).
    auto color = raster.create_color_depth_target(dim, dim);
    REQUIRE(color != nullptr);

    DiagnosticList d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);

    const u64 r_col = rp::pass_param_id("col");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_col, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    constexpr u64 pass_hash = 1U;
    {
        GraphPass p;
        p.name_hash = pass_hash;
        p.payload.executor = rp::executor_type_id("scene.raster");
        p.payload.schema_version = 1U;
        p.payload.queue = rp::QueueKind::Graphics;
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("clear_color"), tv_vec4(0.0F, 0.0F, 0.0F, 1.0F)});
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("clear_depth"), tv_f32(1.0F)});
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("depth_compare"), tv_enum(3U)}); // LessEqual
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_col});
        tmpl.add_pass(p);
    }
    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_col, rp::SlotResourceKind::ColorTarget, color.get(), nullptr, nullptr, nullptr});

    // The RESOLVED draw list — two plain items, each its own storage buffer (distinct storage ⇒ the run never
    // coalesces, so this exercises the per-item single-draw arm and the clear-once across two draws).
    const RenderDrawItem items[2] = {
        RenderDrawItem{geo_left.get(), nullptr, nullptr, 3U, false, 0U, 0U, 0U, nullptr, 0U},
        RenderDrawItem{geo_right.get(), nullptr, nullptr, 3U, false, 0U, 0U, 0U, nullptr, 0U},
    };
    DrawList list;
    list.items = static_cast<const RenderDrawItem*>(items);
    list.count = 2U;
    DrawListTable draw_lists(&alloc);
    draw_lists.bind(pass_hash, list);

    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count, &draw_lists));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);
    // both thirds rendered by SEPARATE items, and the centre gap stayed background (two draws, not one).
    CHECK((color->read_pixel(dim / 4U, dim / 2U) & 0xFFU) >= 250U);       // left third: item 0 drew
    CHECK((color->read_pixel((dim * 3U) / 4U, dim / 2U) & 0xFFU) >= 250U); // right third: item 1 drew
    CHECK((color->read_pixel(dim / 2U, dim / 2U) & 0xFFU) < 32U);          // centre column: background (a real gap)
}
// ⭐⭐ RAF-12.2 prereq-0: a sub-1 two-output FS for the MRT BLEND gate. out0 (accum source) and out1 (revealage source)
// are BELOW 1 so an ADDITIVE accumulation of two overlapping draws (2× = 0.2 / 0.1 / 0.05) is DISTINGUISHABLE from an
// opaque single write (0.1 / 0.05 / 0.025) — a saturating full-white output would hide the blend.
void build_mrt_blend_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const auto kf = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    fe.stage      = kir::KStage::Fragment;
    fe.n_out      = 2;
    fe.out[0]     = {g.vec4(kf(0.1), kf(0.05), kf(0.025), kf(1.0)), 0}; // ADDITIVE accum source  (Ci·ai·wi analogue)
    fe.out[1]     = {g.vec4(kf(0.5), kf(0.5), kf(0.5), kf(0.5)), 1};    // REVEALAGE source: dst·(1-src) each draw
}

// ⭐⭐ RAF-12.2 prereq-0: the PRODUCTION `scene.raster` executor records a TRUE MULTI-COLOUR MRT (n_writes>1) with
// PER-ATTACHMENT BLEND — the one live shape `record_scene_raster` historically dropped (it bound only color0, so a real
// deferred G-buffer / WBOIT ACCUMULATE pass had to keep the inline `record_pass` arm). This drives the REAL scene.raster
// record fn (not a bespoke test executor) through execute_frame with a resolved DrawList: attachment 0 ADDITIVE (accum,
// cleared to 0), attachment 1 REVEALAGE_MULTIPLY (reveal, cleared to the multiplicative identity 1 by draw_storage_mrt).
// TWO overlapping full-screen triangles (one storage buffer, 6 verts) make each blend OBSERVABLE:
//   · Additive accum  = 0 + s + s          = (0.2, 0.1, 0.05) → RGBA8 ≈ (51, 26, 13); opaque/no-blend would leave (26,13,6)
//   · Revealage reveal = 1·(1-0.5)·(1-0.5) = 0.25             → R ≈ 64; a ZERO-clear stays 0, an opaque write leaves 128.
// So a dropped attachment, a missing blend, or a wrong reveal-clear each FAIL loudly — this is the coverage the 12.2 swap
// depends on before the inline MRT arm can be deleted.
void run_mrt_blend_gpu(crd::gpu::IGpuContext& gctx, crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc)
{
    namespace kir = crd::kir;
    using namespace crd::rendergraph;
    using namespace crd::gpu;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    build_mrt_blend_fs(fg, fe);
    auto vs = gctx.create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("shader compilation unavailable; skipping the MRT blend run");
        return;
    }
    auto fs = gctx.create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster.create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    // TWO full-screen triangles (same 3 clip verts twice) in ONE storage buffer ⇒ every pixel covered twice ⇒ the
    // per-attachment blend accumulates observably. Stride 12 floats/vertex (the GEO-1 pull), position in [0..2].
    float verts[72] = {0.0F};
    const auto set   = [&](int i, f32 x, f32 y) { verts[i * 12 + 0] = x; verts[i * 12 + 1] = y; };
    for (int t = 0; t < 2; ++t)
    {
        set(t * 3 + 0, -1.0F, -1.0F);
        set(t * 3 + 1, 3.0F, -1.0F);
        set(t * 3 + 2, -1.0F, 3.0F);
    }
    auto geo = raster.create_storage_buffer(static_cast<u32>(sizeof(verts)));
    REQUIRE(geo != nullptr);
    REQUIRE(raster.upload_storage(*geo, 0U, verts, static_cast<u32>(sizeof(verts))));

    constexpr u32 dim = 32U;
    auto          c0 = raster.create_color_target(dim, dim); // accum   (Additive, clear 0)
    auto          c1 = raster.create_color_target(dim, dim); // reveal  (RevealageMultiply, clear-to-1)
    REQUIRE(c0 != nullptr);
    REQUIRE(c1 != nullptr);

    DiagnosticList       d(&alloc);
    rp::ExecutorRegistry schemas(&alloc);
    REQUIRE(rp::register_builtin_executors(schemas, d) == 14U);
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);

    const u64 r_c0 = rp::pass_param_id("c0");
    const u64 r_c1 = rp::pass_param_id("c1");
    FrameGraphTemplate tmpl(&alloc);
    tmpl.add_resource(GraphResource{r_c0, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    tmpl.add_resource(GraphResource{r_c1, rp::SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    constexpr u64 pass_hash = 1U;
    {
        GraphPass p;
        p.name_hash              = pass_hash;
        p.payload.executor       = rp::executor_type_id("scene.raster");
        p.payload.schema_version = 1U;
        p.payload.queue          = rp::QueueKind::Graphics;
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("clear_color"), tv_vec4(0.0F, 0.0F, 0.0F, 0.0F)});
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("clear_depth"), tv_f32(1.0F)});   // required by schema
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("depth_compare"), tv_enum(3U)});  // LessEqual (required)
        p.payload.params.push_back(
            rp::ParamValue{rp::pass_param_id("blend0"), tv_enum(static_cast<u32>(BlendMode::Additive))});
        p.payload.params.push_back(
            rp::ParamValue{rp::pass_param_id("blend1"), tv_enum(static_cast<u32>(BlendMode::RevealageMultiply))});
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_c0});
        p.payload.resources.push_back(
            rp::ResourceRef{rp::pass_param_id("color1"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, r_c1});
        tmpl.add_pass(p);
    }
    CompiledFrameGraph compiled(&alloc);
    REQUIRE(compile(tmpl, schemas, dim, dim, compiled, d));
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_c0, rp::SlotResourceKind::ColorTarget, c0.get(), nullptr, nullptr, nullptr});
    table.bind(ResolvedResource{r_c1, rp::SlotResourceKind::ColorTarget, c1.get(), nullptr, nullptr, nullptr});

    const RenderDrawItem items[1] = {
        RenderDrawItem{geo.get(), nullptr, nullptr, 6U, false, 0U, 0U, 0U, nullptr, 0U},
    };
    DrawList list;
    list.items = static_cast<const RenderDrawItem*>(items);
    list.count = 1U;
    DrawListTable draw_lists(&alloc);
    draw_lists.bind(pass_hash, list);

    PassPrograms programs;
    programs.raster = program.get();
    u32 submit_count = 0U;
    REQUIRE(execute_frame(compiled, tmpl, records, table, programs, raster, alloc, d, &submit_count, &draw_lists));
    REQUIRE_FALSE(d.has_errors());
    CHECK(submit_count == 1U);

    const u32 a = c0->read_pixel(dim / 2U, dim / 2U); // accum (Additive) — TWO overlapping draws accumulate
    const u32 r = c1->read_pixel(dim / 2U, dim / 2U); // reveal (RevealageMultiply, cleared to 1)
    // accum: additive 2× of (0.1,0.05,0.025) → (51,26,13). ⛔ R≈51 (not ≈26) is the additive proof; a dropped
    // attachment-1 or an opaque blend would break one of these three.
    CHECK((a & 0xFFU) >= 44U);
    CHECK((a & 0xFFU) <= 58U);          // R ≈ 51 (0.2) — ADDITIVE, not opaque (which would be ≈26)
    CHECK(((a >> 8U) & 0xFFU) >= 20U);
    CHECK(((a >> 8U) & 0xFFU) <= 32U);  // G ≈ 26 (0.1)
    CHECK(((a >> 16U) & 0xFFU) >= 7U);
    CHECK(((a >> 16U) & 0xFFU) <= 20U); // B ≈ 13 (0.05)
    // reveal: 1·(1-0.5)·(1-0.5) = 0.25 → R ≈ 64. ⛔ a ZERO-clear would leave 0; an opaque write would leave 128 — the
    // multiplicative-clear-to-1 + RevealageMultiply blend are BOTH required to land here.
    CHECK((r & 0xFFU) >= 54U);
    CHECK((r & 0xFFU) <= 74U); // R ≈ 64 (0.25)
}
} // namespace

TEST_CASE("raf7 frame graph executes on the Vulkan device via the command encoder", "[gpu][vulkan][raf7]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx = crd::gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U, nullptr, "raf7-gpu-vk");
    auto raster = crd::gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    run_graph_gpu(*ctx, *raster, alloc);
    run_mrt_gpu(*ctx, *raster, alloc);
    run_mrt_blend_gpu(*ctx, *raster, alloc);
    run_bindless_gpu(*ctx, *raster, alloc);
    run_shadow_gpu(*ctx, *raster, alloc);
    run_indirect_gpu(*ctx, *raster, alloc, false);
    run_fullscreen_gpu(*ctx, *raster, alloc);
    run_multidraw_gpu(*ctx, *raster, alloc);
    run_scene_drawlist_gpu(*ctx, *raster, alloc);
    run_depth_float_gpu(*ctx, *raster, alloc);
}

#if defined(_WIN32)
TEST_CASE("raf7 frame graph executes on the D3D12 device via the command encoder", "[gpu][dx12][raf7]")
{
    auto gctx = crd::gpu::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    auto raster = crd::gpu::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());
    crd::memory::TlsfAllocator alloc(4U << 20U, nullptr, "raf7-gpu-dx12");
    run_graph_gpu(*gctx, *raster, alloc);
    run_mrt_gpu(*gctx, *raster, alloc);
    run_mrt_blend_gpu(*gctx, *raster, alloc);
    run_bindless_gpu(*gctx, *raster, alloc);
    run_shadow_gpu(*gctx, *raster, alloc);
    run_indirect_gpu(*gctx, *raster, alloc, true);
    run_fullscreen_gpu(*gctx, *raster, alloc);
    run_multidraw_gpu(*gctx, *raster, alloc);
    run_scene_drawlist_gpu(*gctx, *raster, alloc);
    run_depth_float_gpu(*gctx, *raster, alloc);
}
#endif
