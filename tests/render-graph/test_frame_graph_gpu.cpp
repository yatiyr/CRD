// RAF-7 live-GPU wiring — the unified frame graph executing on a REAL device via the REAL command encoder.
//
// A two-pass graph — scene.raster (storage-pulled triangle into a transient colour target) then transfer.copy (the
// transient into an output target) — is compiled (scheduled: scene before copy) and executed on Vulkan AND D3D12.
// execute() invokes the built-in executor RECORD functions, which emit the canonical command model into the raster
// context's ICommandEncoder, which lowers to real draws + copies. The output pixels prove the whole path:
//   FrameGraphTemplate → compile → record functions → canonical commands → encoder → backend → GPU.
//
// (MRT / indirect / shadow / bindless through the graph need COHERENT frame-graph transients — draw_storage_mrt etc.
// provably do not work with independently-created standalone targets — which is a backend transient-allocation
// capability distinct from this encoder-recording path; see the D-007 RAF-7 row.)
//
// GPU tests skip cleanly when no device / no shader-object capability is present.

#include <crd/rendergraph/frame_graph.hpp>

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
    REQUIRE(rp::register_builtin_executors(schemas, d) == 9U);
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 9U);

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
        p.payload.params.push_back(rp::ParamValue{rp::pass_param_id("depth_compare"), tv_enum(0U)});
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
    auto encoder = raster.create_command_encoder();
    REQUIRE(encoder != nullptr);
    REQUIRE(execute(compiled, tmpl, records, table, programs, *encoder, d));
    REQUIRE_FALSE(d.has_errors());

    // The scene pass rendered the triangle into `scene`; the copy pass reproduced it in `copy`.
    CHECK((scene_target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red triangle centre
    CHECK((copy_target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U);  // copied through the frame graph
    CHECK(((copy_target->read_pixel(0U, 0U) >> 16U) & 0xFFU) >= 250U);     // blue clear corner, copied
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
}
#endif
