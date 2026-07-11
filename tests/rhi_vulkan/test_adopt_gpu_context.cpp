// test_adopt_gpu_context.cpp — D-008 C2-b: rhi-vulkan ADOPTS a VulkanGpuContext's device. Proves the two VkDevices the
// ADR-0099 audit found become ONE: an rhi Device built over the gpu-context's handles works, and destroying it does NOT
// destroy the shared device (the context outlives the Device and stays usable).

#include <crd/gpu/context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/types.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

TEST_CASE("D-008 C2-b: rhi-vulkan adopts a VulkanGpuContext device (ONE VkDevice)", "[rhi][vulkan][gpu][adopt]")
{
    // A WINDOWED context (C2-c) is feature-matched to rhi's own device (dynamic rendering + synchronization2 +
    // fillModeNonSolid + swapchain), so the renderer runs on the adopted device unchanged. No window is needed to CREATE
    // it (only to present — the swapchain path is the sandbox wiring, C2-c2).
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = false;

    auto ctx = crd::gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    REQUIRE(static_cast<crd::gpu::VulkanGpuContext*>(ctx.get())->render_capable()); // feature-complete + presentable

    // Build an rhi Device over the gpu-context's instance/device/queues — the renderer keeps its rhi API on ONE device.
    auto device = crd::rhi::create_vulkan_device_adopting(*ctx);
    REQUIRE(device != nullptr);                          // command pools + queues stood up on the SHARED VkDevice
    CHECK(device->supports_shader_int64() == static_cast<crd::gpu::VulkanGpuContext*>(ctx.get())->shader_int64());

    // Destroy the adopted Device. It must free ITS resources (pools/allocations) but NOT the shared VkDevice.
    device.reset();
    REQUIRE(ctx->valid());

    // Proof the shared VkDevice SURVIVED the adopted Device's destruction: we can adopt it AGAIN. (If C2-b had let the
    // rhi Device destroy the shared device, this second adoption would build over a dead handle.)
    auto device2 = crd::rhi::create_vulkan_device_adopting(*ctx);
    CHECK(device2 != nullptr);
}

TEST_CASE("D-008 C2-d: a graphics pipeline from an opaque IGpuProgram (closes I2)", "[rhi][vulkan][gpu][adopt][program]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = false;
    auto ctx     = crd::gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto device = crd::rhi::create_vulkan_device_adopting(*ctx);
    REQUIRE(device != nullptr);

    crd::memory::TlsfAllocator alloc(4U << 20U);

    // Attributeless triangle VS + FS → SPIR-V (the Vulkan backend's compiler) → IGpuProgram (opaque, no raw SPIR-V in a
    // public struct). The rhi pipeline takes the PROGRAM, not a ShaderModuleDesc::code. This is the ONE device C2-c made.
    static constexpr const char* kVs = "#version 450\nvoid main(){ vec2 p[3]=vec2[](vec2(0,-0.8),vec2(0.8,0.8),"
                                       "vec2(-0.8,0.8)); gl_Position=vec4(p[gl_VertexIndex],0,1); }\n";
    static constexpr const char* kFs = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o=vec4(1,0,0,1); }\n";

    const auto vs = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Vertex,
                                                    crd::containers::StringView(kVs, std::strlen(kVs)), "vs", &alloc);
    const auto fs = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Fragment,
                                                    crd::containers::StringView(kFs, std::strlen(kFs)), "fs", &alloc);
    REQUIRE(vs.ok);
    REQUIRE(fs.ok);

    auto vs_prog = ctx->create_program(crd::gpu::ShaderStage::Vertex,
                                       crd::containers::ConstSpan<crd::u8>(vs.spirv.data(), vs.spirv.size()));
    auto fs_prog = ctx->create_program(crd::gpu::ShaderStage::Fragment,
                                       crd::containers::ConstSpan<crd::u8>(fs.spirv.data(), fs.spirv.size()));
    REQUIRE(vs_prog != nullptr);
    REQUIRE(fs_prog != nullptr);

    crd::rhi::GraphicsPipelineDesc pd;
    pd.vertex_program       = vs_prog.get(); // the OPAQUE program — no ShaderModule, no raw SPIR-V in the desc
    pd.fragment_program     = fs_prog.get();
    pd.color_format         = crd::rhi::Format::B8G8R8A8Unorm;
    pd.use_dynamic_viewport = true;

    auto pipeline = device->create_graphics_pipeline(pd);
    REQUIRE(pipeline != nullptr); // the rhi pipeline built from opaque programs on the shared device
}
