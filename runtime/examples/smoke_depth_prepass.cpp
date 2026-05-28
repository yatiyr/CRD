#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/renderer/forward_render_path.hpp>
#include <crd/renderer/frame_graph.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_depth, "SmokeDepthPrepass", crd::log::LogLevel::Trace)

namespace
{
// Null resolver — used for the frame graph validation test (no draw items need pipelines).
class NullResolver final : public crd::renderer::PipelineResolver
{
public:
    [[nodiscard]] crd::rhi::Pipeline*
    resolve_pipeline(const crd::shader::VariantPipelineDesc& /*handoff*/) noexcept override
    {
        return nullptr;
    }
};

// Build a minimal triangle mesh for GPU upload.
crd::renderer::MeshResource make_triangle_mesh(crd::memory::IAllocator* a)
{
    crd::renderer::MeshResource mesh(a);

    struct Vertex
    {
        float pos[3];
        float normal[3];
        float uv[2];
        float tangent[4];
    };
    static_assert(sizeof(Vertex) == crd::renderer::kMeshVertexStride);

    const Vertex verts[3] = {
        {{ 0.0F,  0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
    };
    const crd::u32 indices[3] = {0, 1, 2};

    const auto* vb = reinterpret_cast<const crd::u8*>(verts);
    for (crd::usize i = 0; i < sizeof(verts); ++i)
        mesh.vertices.push_back(vb[i]);

    const auto* ib = reinterpret_cast<const crd::u8*>(indices);
    for (crd::usize i = 0; i < sizeof(indices); ++i)
        mesh.indices.push_back(ib[i]);

    crd::renderer::MeshPrimitive prim{};
    prim.vertex_count       = 3;
    prim.index_count        = 3;
    prim.vertex_byte_offset = 0;
    prim.index_byte_offset  = 0;
    mesh.primitives.push_back(prim);
    return mesh;
}
} // namespace

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    // Create headless Vulkan instance (no GLFW surface extensions needed).
    auto instance = crd::rhi::create_vulkan_instance(
        {.application_name = crd::containers::String("smoke_depth_prepass"), .enable_validation = false});
    if (instance == nullptr)
    {
        CRD_LOG_WARN(g_log_smoke_depth, "No Vulkan instance — skipping depth prepass smoke");
        crd::log::flush();
        crd::log::shutdown();
        return 0;
    }

    auto device = instance->create_device({});
    if (device == nullptr)
    {
        CRD_LOG_WARN(g_log_smoke_depth, "No Vulkan device available — skipping depth prepass smoke");
        crd::log::flush();
        crd::log::shutdown();
        return 0;
    }

    // Descriptor allocator for per-frame sets.
    auto alloc = device->create_descriptor_allocator({2, 512});
    if (alloc == nullptr)
    {
        CRD_LOG_ERROR(g_log_smoke_depth, "Failed to create descriptor allocator");
        return 1;
    }

    // Create ForwardRenderPath (depth + color render targets).
    const crd::rhi::Extent2D extent{640, 480};
    NullResolver resolver;
    auto frp = crd::renderer::ForwardRenderPath::create(*device, resolver, *alloc, extent, 2);
    if (frp == nullptr)
    {
        CRD_LOG_ERROR(g_log_smoke_depth, "ForwardRenderPath::create failed");
        return 1;
    }

    CRD_LOG_INFO(g_log_smoke_depth, "ForwardRenderPath created — color + depth render targets allocated");

    // Upload a triangle mesh.
    crd::memory::TlsfAllocator cpu_alloc{256ULL << 20};
    auto cpu_mesh = make_triangle_mesh(&cpu_alloc);
    auto gpu_mesh = crd::renderer::GpuUploader::upload_mesh(cpu_mesh, *device);
    CRD_ASSERT(gpu_mesh.vertex_buffer != nullptr);
    CRD_ASSERT(gpu_mesh.index_buffer != nullptr);

    CRD_LOG_INFO(g_log_smoke_depth, "Triangle mesh uploaded to GPU");

    // Run one frame: submit an indexed draw item and execute the depth + color passes.
    // The draw item has no material and no variant (handoff is empty), so NullResolver
    // returns nullptr and the item is skipped. The important thing is that both render
    // passes open/close correctly, and the pipeline layout + descriptor machinery works.
    alloc->begin_frame(0);

    crd::renderer::DrawList draw_list;
    // No items — just verifying the pass infrastructure.

    crd::renderer::FrameContext ctx;
    ctx.frame_index = 0;

    crd::renderer::FrameGraph fg;
    frp->build(fg, draw_list, ctx);

    [[maybe_unused]] const bool built = fg.build();
    CRD_ASSERT(built);

    // Execute the frame graph — creates Vulkan barriers and issues rendering commands.
    auto cmd = device->create_command_buffer();
    CRD_ASSERT(cmd != nullptr);

    cmd->begin();
    fg.execute(*device, *cmd);
    cmd->end();

    device->graphics_queue().submit_and_wait(*cmd);
    device->wait_idle();

    CRD_LOG_INFO(g_log_smoke_depth, "Frame executed: depth-prepass + color-pass completed without error");

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
