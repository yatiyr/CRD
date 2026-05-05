#include <crd/log/log.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/texture_resource.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_asset, "SmokeAssetImport", crd::log::LogLevel::Trace)

namespace
{
// Build a 4×4 RGBA8 checkerboard texture (black/white squares, 1 mip).
crd::renderer::TextureResource make_checker_texture(crd::memory::IAllocator* a)
{
    crd::renderer::TextureResource tex(a);
    tex.format    = crd::renderer::TextureFormat::RGBA8Unorm;
    tex.mip_count = 1;

    crd::renderer::MipLevel mip(a);
    mip.width  = 4;
    mip.height = 4;
    mip.pixels.resize(4 * 4 * 4); // 4×4 pixels × 4 bytes
    for (crd::u32 y = 0; y < 4; ++y)
    {
        for (crd::u32 x = 0; x < 4; ++x)
        {
            const crd::u32 idx    = (y * 4 + x) * 4;
            const crd::u8  value  = ((x + y) % 2 == 0) ? 0xFFU : 0x00U;
            mip.pixels[idx + 0]   = value;
            mip.pixels[idx + 1]   = value;
            mip.pixels[idx + 2]   = value;
            mip.pixels[idx + 3]   = 0xFFU;
        }
    }
    tex.mips.push_back(std::move(mip));
    return tex;
}

// Build a unit quad (4 verts, 2 triangles) in the kMeshVertexStride=48 format.
//   positions: unit square in XY, Z=0; normals: (0,0,1); uvs: corners; tangent: (1,0,0,1)
crd::renderer::MeshResource make_quad_mesh(crd::memory::IAllocator* a)
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

    const Vertex verts[4] = {
        {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 0.5F,  0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{-0.5F,  0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
    };
    const crd::u32 indices[6] = {0, 1, 2, 0, 2, 3};

    const auto* vb = reinterpret_cast<const crd::u8*>(verts);
    for (crd::usize i = 0; i < sizeof(verts); ++i)
    {
        mesh.vertices.push_back(vb[i]);
    }

    const auto* ib = reinterpret_cast<const crd::u8*>(indices);
    for (crd::usize i = 0; i < sizeof(indices); ++i)
    {
        mesh.indices.push_back(ib[i]);
    }

    crd::renderer::MeshPrimitive prim{};
    prim.vertex_count       = 4;
    prim.index_count        = 6;
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

    // Create Vulkan instance (headless — no GLFW surface extensions needed).
    auto instance = crd::rhi::create_vulkan_instance({.application_name = crd::containers::String("smoke_asset_import"),
                                                      .enable_validation = false});
    if (instance == nullptr)
    {
        CRD_LOG_WARN(g_log_smoke_asset, "No Vulkan instance — skipping GPU upload smoke");
        crd::log::flush();
        crd::log::shutdown();
        return 0;
    }

    auto device = instance->create_device({});
    if (device == nullptr)
    {
        CRD_LOG_WARN(g_log_smoke_asset, "No Vulkan device available — skipping GPU upload smoke");
        crd::log::flush();
        crd::log::shutdown();
        return 0;
    }

    auto* alloc = crd::memory::default_allocator();

    // Build CPU-side resources.
    auto cpu_tex  = make_checker_texture(alloc);
    auto cpu_mesh = make_quad_mesh(alloc);

    // Upload to GPU.
    auto gpu_tex  = crd::renderer::GpuUploader::upload_texture(cpu_tex, *device);
    auto gpu_mesh = crd::renderer::GpuUploader::upload_mesh(cpu_mesh, *device);

    CRD_ASSERT(gpu_tex.image != nullptr);
    CRD_ASSERT(gpu_mesh.vertex_buffer != nullptr);
    CRD_ASSERT(gpu_mesh.index_buffer != nullptr);

    CRD_LOG_INFO(g_log_smoke_asset, "GPU texture uploaded: {}×{}", cpu_tex.mips[0].width, cpu_tex.mips[0].height);
    CRD_LOG_INFO(g_log_smoke_asset, "GPU mesh uploaded: {} vertices, {} indices",
                 static_cast<crd::u32>(cpu_mesh.vertices.size() / crd::renderer::kMeshVertexStride),
                 static_cast<crd::u32>(cpu_mesh.indices.size() / sizeof(crd::u32)));

    device->wait_idle();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
