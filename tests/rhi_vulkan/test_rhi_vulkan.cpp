#include <crd/gpu/program.hpp> // D-008 C2-d4: shaders are opaque IGpuProgram (crd::gpu::ShaderStage), minted via create_program
#include <crd/gpu/vulkan_context.hpp> // D-008 C2-f: create_vulkan_gpu_context — the ONE VkDevice owner rhi adopts
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/platform.hpp>
#include <crd/rhi/vulkan_backend.hpp>
#include <crd/rhi/vulkan_validation_capture.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

// D-008 C2-f: rhi-vulkan no longer creates a VkDevice. A rhi Device is ADOPTED from a VulkanGpuContext — the ONE owner of
// the VkInstance/VkDevice/queues (ADR-0099). This holder keeps the context alive alongside the borrowed device (`ctx`
// declared first ⇒ destroyed last, after `device`). `make_adopted_gpu(false)` = a windowed/render-capable context (for
// swapchain/present tests); the default is headless compute.
struct AdoptedGpu
{
    std::unique_ptr<crd::gpu::IGpuContext> ctx;
    std::unique_ptr<crd::rhi::Device>      device;
};

[[nodiscard]] AdoptedGpu make_adopted_gpu(bool headless = true)
{
    AdoptedGpu adopted;
    crd::gpu::GpuContextConfig cfg;
    cfg.headless          = headless;
    cfg.enable_validation = true; // matches the retired create_vulkan_instance default; ValidationCapture needs it
    adopted.ctx           = crd::gpu::create_vulkan_gpu_context(cfg);
    if (adopted.ctx != nullptr) { adopted.device = crd::rhi::create_vulkan_device_adopting(*adopted.ctx); }
    return adopted;
}
} // namespace

TEST_CASE("Vulkan instance enumerates at least one adapter", "[rhi][vulkan]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan adapter enumeration test");
        return;
    }

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);

    crd::containers::Array<crd::rhi::AdapterInfo> adapters;
    instance->enumerate_adapters(adapters);
    REQUIRE(adapters.size() >= 1U);
}

TEST_CASE("Vulkan device bootstrap creates a swapchain for an invisible window", "[rhi][vulkan]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan/window-backed test");
        return;
    }

    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    crd::platform::WindowDesc window_desc;
    window_desc.visible = false;
    window_desc.title = crd::containers::String("crd-rhi-vulkan-tests");
    auto window = crd::platform::Window::create(ctx, window_desc);
    REQUIRE(window.is_valid());

    auto gpu = make_adopted_gpu(/*headless*/ false);
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    auto swapchain = device->create_swapchain(
        {window.native_handle(), {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);
    REQUIRE(swapchain->desc().extent.width > 0U);
    REQUIRE(swapchain->desc().extent.height > 0U);
}

TEST_CASE("Vulkan command buffer and frame loop can execute a triangle frame", "[rhi][vulkan]")
{
#if defined(NDEBUG)
    SUCCEED("Release build: triangle frame integration test is skipped due to driver-dependent optimization/runtime "
            "variance");
#else
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan/window-backed test");
        return;
    }

    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    crd::platform::WindowDesc window_desc;
    window_desc.visible = false;
    window_desc.title = crd::containers::String("crd-rhi-vulkan-frame-tests");
    auto window = crd::platform::Window::create(ctx, window_desc);
    REQUIRE(window.is_valid());

    auto gpu = make_adopted_gpu(/*headless*/ false);
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    auto swapchain = device->create_swapchain(
        {window.native_handle(), {640, 360}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    crd::containers::Array<crd::u8> fs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv));
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.frag.spv", fs_spv));

    auto vs = device->create_program(crd::rhi::ShaderStage::Vertex, crd::containers::make_span(vs_spv.data(), vs_spv.size()));
    auto fs_module = device->create_program(crd::rhi::ShaderStage::Fragment, crd::containers::make_span(fs_spv.data(), fs_spv.size()));
    REQUIRE(vs != nullptr);
    REQUIRE(fs_module != nullptr);

    struct Vertex
    {
        float pos[2];
        float color[3];
    };

    const Vertex vertices[] = {
        {{0.0F, -0.5F}, {1.0F, 0.0F, 0.0F}}, {{0.5F, 0.5F}, {0.0F, 1.0F, 0.0F}}, {{-0.5F, 0.5F}, {0.0F, 0.0F, 1.0F}}};

    auto vertex_buffer = device->create_buffer(
        {sizeof(vertices), crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    REQUIRE(vertex_buffer != nullptr);

    auto gpu_only_buffer = device->create_buffer(
        {sizeof(vertices), crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(gpu_only_buffer != nullptr);
    REQUIRE(gpu_only_buffer->map() == nullptr);

    auto color_image = device->create_image(
        {{128, 128},
         crd::rhi::Format::B8G8R8A8Unorm,
         crd::rhi::enum_bits(crd::rhi::ImageUsage::Sampled) | crd::rhi::enum_bits(crd::rhi::ImageUsage::TransferDst),
         1,
         1});
    REQUIRE(color_image != nullptr);
    void* mapped = vertex_buffer->map();
    REQUIRE(mapped != nullptr);
    std::memcpy(mapped, vertices, sizeof(vertices));
    vertex_buffer->unmap();

    const crd::rhi::VertexBindingDesc binding{0, sizeof(Vertex), crd::rhi::VertexInputRate::Vertex};
    const crd::rhi::VertexAttributeDesc attributes[] = {{0, 0, crd::rhi::Format::R32G32Sfloat, 0},
                                                        {1, 0, crd::rhi::Format::R32G32B32Sfloat, sizeof(float) * 2U}};
    auto pipeline = device->create_graphics_pipeline({vs.get(),
                                                      fs_module.get(),
                                                      crd::rhi::PrimitiveTopology::TriangleList,
                                                      {swapchain->desc().extent.width, swapchain->desc().extent.height},
                                                      swapchain->desc().color_format,
                                                      crd::rhi::Format::Undefined,
                                                      crd::containers::make_span(&binding, 1),
                                                      crd::containers::make_span(attributes),
                                                      false,
                                                      false});
    REQUIRE(pipeline != nullptr);

    auto command_buffer = device->create_command_buffer();
    REQUIRE(command_buffer != nullptr);

    REQUIRE(swapchain->acquire_next_image());
    command_buffer->begin();
    command_buffer->transition_image(swapchain->current_image(),
                                     crd::rhi::ImageAccess::Undefined, crd::rhi::ImageAccess::ColorWrite);
    command_buffer->begin_rendering(
        {{swapchain->desc().extent.width, swapchain->desc().extent.height},
         {&swapchain->current_image(), crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.0F, 0.1F, 0.2F, 1.0F}},
         nullptr});
    command_buffer->bind_pipeline(*pipeline);
    command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
    command_buffer->draw(3, 0);
    command_buffer->end_rendering();
    command_buffer->transition_image(swapchain->current_image(),
                                     crd::rhi::ImageAccess::ColorWrite, crd::rhi::ImageAccess::Present);
    command_buffer->end();

    REQUIRE(device->graphics_queue().submit(*command_buffer, *swapchain));
    device->wait_idle();
#endif
}

// ─── Phase 3.0 v1o1 — RHI Fence + non-blocking submit (ADR-0061 §"Layer 1") ───
//
// Exercises the real Vulkan path: vkCreateFence → vkQueueSubmit(fence) →
// vkWaitForFences → vkResetFences → vkDestroyFence. Skipped on
// CRD_PLATFORM_HEADLESS=1 (CI runners) since device creation needs a real
// GPU.
TEST_CASE("Vulkan Fence: non-blocking submit signals fence on completion",
          "[rhi][vulkan][fence]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan fence/submit test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    auto fence = device->create_fence();
    REQUIRE(fence != nullptr);

    // Newly-created fence is unsignalled.
    CHECK_FALSE(fence->is_signaled());

    // Reset on a fresh fence is a no-op (still unsignalled).
    (*fence).reset();
    CHECK_FALSE(fence->is_signaled());

    // Empty-but-valid command buffer.
    auto cmd = device->create_command_buffer();
    REQUIRE(cmd != nullptr);
    cmd->begin();
    cmd->end();

    // Non-blocking submit. The fence flips when the GPU completes the
    // (empty) command buffer; we wait synchronously to assert the
    // transition.
    device->graphics_queue().submit(*cmd, *fence);
    fence->wait();
    CHECK(fence->is_signaled());

    // Reset re-arms the fence; reused for a second submit.
    (*fence).reset();
    CHECK_FALSE(fence->is_signaled());

    auto cmd2 = device->create_command_buffer();
    REQUIRE(cmd2 != nullptr);
    cmd2->begin();
    cmd2->end();
    device->graphics_queue().submit(*cmd2, *fence);
    fence->wait();
    CHECK(fence->is_signaled());

    // Drain before destroying device-owned objects.
    device->wait_idle();
}

// =====================================================================
// Phase 3.1.7.6 v0a — Vulkan compute pipeline factory integration
// (ADR-0080 D1 additive-only). v0b will exercise dispatch; v0a just
// pins that the pipeline object can be created, holds a real
// VkPipeline, and tears down cleanly under ASan.
// =====================================================================

TEST_CASE("Vulkan create_compute_pipeline rejects null shader cleanly", "[rhi][vulkan][compute][v0a]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_program  = nullptr;
    desc.pipeline_layout = nullptr;
    auto pipeline = device->create_compute_pipeline(desc);

    REQUIRE(pipeline == nullptr); // null shader → factory rejects, no crash
}

TEST_CASE("Vulkan create_compute_pipeline rejects wrong-stage shader cleanly", "[rhi][vulkan][compute][v0a]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv));
    auto vs = device->create_program(crd::rhi::ShaderStage::Vertex, crd::containers::make_span(vs_spv.data(), vs_spv.size()));
    REQUIRE(vs != nullptr);

    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_program = vs.get(); // wrong stage — should reject
    auto pipeline = device->create_compute_pipeline(desc);

    REQUIRE(pipeline == nullptr);
}

TEST_CASE("Vulkan create_compute_pipeline succeeds with valid compute shader", "[rhi][vulkan][compute][v0a]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0a.comp.spv", cs_spv));
    auto cs = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs_spv.data(), cs_spv.size()));
    REQUIRE(cs != nullptr);

    // Path A: synthesised empty pipeline layout (desc.pipeline_layout == nullptr).
    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_program  = cs.get();
    desc.pipeline_layout = nullptr;
    auto pipeline = device->create_compute_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    CHECK(pipeline->desc().compute_program == cs.get());

    // Lifecycle: explicit reset triggers destruction of VkPipeline + owned layout
    // (ASan should not flag any UAF / leak — that's the v0a smoke contract).
    pipeline.reset();
    REQUIRE(pipeline == nullptr);

    device->wait_idle();
}

TEST_CASE("Vulkan compute pipeline + caller-provided PipelineLayout", "[rhi][vulkan][compute][v0a]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0a.comp.spv", cs_spv));
    auto cs = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs_spv.data(), cs_spv.size()));
    REQUIRE(cs != nullptr);

    // Build a real layout with one storage-buffer binding at set 0 / binding 0
    // (the conventional v9 GPU LBVH input-AABBs binding).
    crd::rhi::DescriptorBinding bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc set0_desc{};
    set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(bindings, 1);
    auto set0 = device->create_descriptor_set_layout(set0_desc);
    REQUIRE(set0 != nullptr);

    const crd::rhi::DescriptorSetLayout* layouts[] = {set0.get()};
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    auto layout = device->create_pipeline_layout(layout_desc);
    REQUIRE(layout != nullptr);

    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_program  = cs.get();
    desc.pipeline_layout = layout.get();
    auto pipeline = device->create_compute_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    device->wait_idle();
}

TEST_CASE("Vulkan compute pipeline multi-create/destroy cycle (ASan-clean)", "[rhi][vulkan][compute][v0a]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0a.comp.spv", cs_spv));

    // 8 cycles: create + drop. Validation layer + ASan must stay quiet.
    constexpr int k_cycles = 8;
    for (int i = 0; i < k_cycles; ++i)
    {
        auto cs = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs_spv.data(), cs_spv.size()));
        REQUIRE(cs != nullptr);

        crd::rhi::ComputePipelineDesc desc{};
        desc.compute_program = cs.get();
        auto pipeline = device->create_compute_pipeline(desc);
        REQUIRE(pipeline != nullptr);
    } // RAII destruction each iteration

    device->wait_idle();
}

TEST_CASE("Vulkan create_buffer with BufferUsage::Storage works (D2 revision)", "[rhi][vulkan][compute][v0a]")
{
    // ADR-0080 D2 revision: storage buffers reuse the existing Buffer
    // interface with BufferUsage::Storage flag. This test pins that the
    // existing factory already supports the v9-LBVH 4-KB storage buffer
    // use case without any new IStorageBuffer split.
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    constexpr crd::u64 storage_bytes = 4 * 1024; // 4 KB
    auto buffer = device->create_buffer(
        {storage_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buffer != nullptr);
    CHECK(buffer->desc().size_bytes == storage_bytes);
    CHECK(crd::rhi::has_flag(buffer->desc().usage, crd::rhi::BufferUsage::Storage));

    device->wait_idle();
}

// =====================================================================
// Phase 3.1.7.6 v0b — Vulkan first-light compute dispatch integration.
//
// End-to-end: compile compute shader → create pipeline with spec const →
// allocate storage buffer (GpuToCpu host-visible coherent) → bind + dispatch
// → submit + wait → host readback validates per-element write.
//
// Explicit barriers (v0c) NOT needed: queue.submit(cmd, fence) +
// fence.wait() + MemoryUsage::GpuToCpu (host-coherent) provides the
// canonical "no barrier needed" path. Production code with non-coherent
// memory or compute→graphics handoff uses v0c compute_barrier.
// =====================================================================

TEST_CASE("Vulkan compute dispatch end-to-end (first-light)", "[rhi][vulkan][compute][v0b]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    // --- Shader + spec-const (D6) ---
    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs_spv));
    auto cs = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs_spv.data(), cs_spv.size()));
    REQUIRE(cs != nullptr);

    constexpr crd::u32 base_offset = 1000;
    crd::rhi::SpecializationConstantEntry spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_program = cs.get();
    // Layout assembled below.

    // --- Descriptor + pipeline layout (set 0 / binding 0 / storage buffer) ---
    crd::rhi::DescriptorBinding bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc set0_desc{};
    set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(bindings, 1);
    auto set0 = device->create_descriptor_set_layout(set0_desc);
    REQUIRE(set0 != nullptr);

    const crd::rhi::DescriptorSetLayout* layouts[] = {set0.get()};
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    auto layout = device->create_pipeline_layout(layout_desc);
    REQUIRE(layout != nullptr);

    pipe_desc.pipeline_layout = layout.get();
    pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&spec_entry, 1);
    pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&base_offset),
                                            sizeof(base_offset));
    auto pipeline = device->create_compute_pipeline(pipe_desc);
    REQUIRE(pipeline != nullptr);

    // --- Storage buffer (host-visible coherent for readback) ---
    constexpr crd::u32 element_count = 64; // == local_size_x; 1 workgroup
    constexpr crd::u64 buffer_bytes  = element_count * sizeof(crd::u32);
    auto storage = device->create_buffer(
        {buffer_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    REQUIRE(storage != nullptr);

    // Pre-zero buffer so the post-dispatch readback proves writes happened.
    auto* mapped_init = static_cast<crd::u32*>(storage->map());
    REQUIRE(mapped_init != nullptr);
    for (crd::u32 i = 0; i < element_count; ++i) { mapped_init[i] = 0; }
    storage->unmap();

    // --- Descriptor allocator + descriptor set bind ---
    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight                 = 1;
    alloc_desc.max_sets_per_frame               = 4;
    alloc_desc.max_storage_buffers_per_frame    = 4;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    REQUIRE(desc_alloc != nullptr);
    desc_alloc->begin_frame(0);
    auto desc_set = desc_alloc->allocate(*set0);
    REQUIRE(desc_set != nullptr);
    desc_set->update_buffer(0, *storage, 0, buffer_bytes);

    // --- Record + submit + wait ---
    auto cmd   = device->create_command_buffer();
    auto fence = device->create_fence();
    REQUIRE(cmd != nullptr);
    REQUIRE(fence != nullptr);

    cmd->begin();
    cmd->bind_compute_pipeline(*pipeline);
    crd::rhi::DescriptorSet* sets[] = {desc_set.get()};
    cmd->bind_compute_descriptor_sets(
        *layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
    cmd->dispatch(1, 1, 1); // 1 workgroup of 64 threads
    cmd->end();

    device->graphics_queue().submit(*cmd, *fence);
    fence->wait();

    // --- Host readback: each element == index + base_offset ---
    auto* mapped = static_cast<crd::u32*>(storage->map());
    REQUIRE(mapped != nullptr);
    for (crd::u32 i = 0; i < element_count; ++i)
    {
        CHECK(mapped[i] == i + base_offset);
    }
    storage->unmap();

    device->wait_idle();
}

// =====================================================================
// Phase 3.1.7.6 v0c — Vulkan buffer_barrier end-to-end (two compute
// passes with the barrier between them).
//
// pass 1 (compute_v0b_dispatch.comp): buf_a[i] = i + base_offset
// buffer_barrier(buf_a, ComputeShaderWrite → ComputeShaderRead)
// pass 2 (compute_v0c_doubler.comp):  buf_b[i] = 2 * buf_a[i]
// host readback validates buf_b[i] == 2 * (i + base_offset)
//
// Validation-layer discriminator (manually confirmed during development;
// not asserted programmatically because installing a custom debug
// messenger from a test requires backend-internal hooks not yet
// surfaced): commenting out the buffer_barrier call triggers
// `VUID-vkCmdDispatch-None-...` (write-after-write hazard on buf_a).
// With the barrier in place, validation stays quiet — confirmed under
// VK_LAYER_KHRONOS_validation with the default debug_callback at
// engine/rhi-vulkan/src/vulkan_backend.cpp:253.
// =====================================================================

TEST_CASE("Vulkan buffer_barrier between two compute passes (first-light)",
          "[rhi][vulkan][compute][v0c]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";

    // --- Shader modules (pass 1 = v0b dispatch, pass 2 = v0c doubler) ---
    crd::containers::Array<crd::u8> cs1_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs1_spv));
    auto cs1 = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs1_spv.data(), cs1_spv.size()));
    REQUIRE(cs1 != nullptr);

    crd::containers::Array<crd::u8> cs2_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0c_doubler.comp.spv", cs2_spv));
    auto cs2 = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs2_spv.data(), cs2_spv.size()));
    REQUIRE(cs2 != nullptr);

    // --- Pass 1 layout: 1 storage buffer (set 0 / binding 0, write) ---
    crd::rhi::DescriptorBinding p1_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc p1_set0_desc{};
    p1_set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(p1_bindings, 1);
    auto p1_set0 = device->create_descriptor_set_layout(p1_set0_desc);
    const crd::rhi::DescriptorSetLayout* p1_layouts[] = {p1_set0.get()};
    crd::rhi::PipelineLayoutDesc p1_layout_desc{};
    p1_layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(p1_layouts, 1);
    auto p1_layout = device->create_pipeline_layout(p1_layout_desc);

    constexpr crd::u32 base_offset = 1000;
    crd::rhi::SpecializationConstantEntry p1_spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc p1_pipe_desc{};
    p1_pipe_desc.compute_program  = cs1.get();
    p1_pipe_desc.pipeline_layout = p1_layout.get();
    p1_pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&p1_spec_entry, 1);
    p1_pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&base_offset),
                                            sizeof(base_offset));
    auto p1_pipeline = device->create_compute_pipeline(p1_pipe_desc);
    REQUIRE(p1_pipeline != nullptr);

    // --- Pass 2 layout: 2 storage buffers (binding 0 = input read, binding 1 = output write) ---
    crd::rhi::DescriptorBinding p2_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc p2_set0_desc{};
    p2_set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(p2_bindings, 2);
    auto p2_set0 = device->create_descriptor_set_layout(p2_set0_desc);
    const crd::rhi::DescriptorSetLayout* p2_layouts[] = {p2_set0.get()};
    crd::rhi::PipelineLayoutDesc p2_layout_desc{};
    p2_layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(p2_layouts, 1);
    auto p2_layout = device->create_pipeline_layout(p2_layout_desc);

    crd::rhi::ComputePipelineDesc p2_pipe_desc{};
    p2_pipe_desc.compute_program  = cs2.get();
    p2_pipe_desc.pipeline_layout = p2_layout.get();
    auto p2_pipeline = device->create_compute_pipeline(p2_pipe_desc);
    REQUIRE(p2_pipeline != nullptr);

    // --- Buffers (buf_a: GPU-only intermediate; buf_b: host-visible output) ---
    constexpr crd::u32 element_count = 64;
    constexpr crd::u64 buffer_bytes  = element_count * sizeof(crd::u32);
    auto buf_a = device->create_buffer(
        {buffer_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto buf_b = device->create_buffer(
        {buffer_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    REQUIRE(buf_a != nullptr);
    REQUIRE(buf_b != nullptr);
    auto* bzinit = static_cast<crd::u32*>(buf_b->map());
    for (crd::u32 i = 0; i < element_count; ++i) { bzinit[i] = 0; }
    buf_b->unmap();

    // --- Descriptor allocator + sets ---
    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 1;
    alloc_desc.max_sets_per_frame            = 8;
    alloc_desc.max_storage_buffers_per_frame = 8;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    desc_alloc->begin_frame(0);

    auto p1_set = desc_alloc->allocate(*p1_set0);
    p1_set->update_buffer(0, *buf_a, 0, buffer_bytes);

    auto p2_set = desc_alloc->allocate(*p2_set0);
    p2_set->update_buffer(0, *buf_a, 0, buffer_bytes);
    p2_set->update_buffer(1, *buf_b, 0, buffer_bytes);

    // --- Record + submit ---
    auto cmd   = device->create_command_buffer();
    auto fence = device->create_fence();

    cmd->begin();
    cmd->bind_compute_pipeline(*p1_pipeline);
    crd::rhi::DescriptorSet* p1_sets[] = {p1_set.get()};
    cmd->bind_compute_descriptor_sets(
        *p1_layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(p1_sets, 1));
    cmd->dispatch(1, 1, 1); // pass 1: buf_a[i] = i + base_offset

    // The barrier under test. Comment this out + re-run to see the
    // validation-layer VUID confirming the discriminator (manual check).
    cmd->buffer_barrier(*buf_a, crd::rhi::BufferAccess::ComputeShaderWrite,
                        crd::rhi::BufferAccess::ComputeShaderRead);

    cmd->bind_compute_pipeline(*p2_pipeline);
    crd::rhi::DescriptorSet* p2_sets[] = {p2_set.get()};
    cmd->bind_compute_descriptor_sets(
        *p2_layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(p2_sets, 1));
    cmd->dispatch(1, 1, 1); // pass 2: buf_b[i] = 2 * buf_a[i]
    cmd->end();

    device->graphics_queue().submit(*cmd, *fence);
    fence->wait();

    // --- Host readback: buf_b[i] == 2 * (i + base_offset) ---
    auto* mapped = static_cast<crd::u32*>(buf_b->map());
    REQUIRE(mapped != nullptr);
    for (crd::u32 i = 0; i < element_count; ++i)
    {
        CHECK(mapped[i] == 2U * (i + base_offset));
    }
    buf_b->unmap();
    device->wait_idle();
}

// =====================================================================
// Phase 3.1.7.6 v0d — async compute substrate Vulkan integration.
//
// First-light: two compute submissions on (possibly distinct) queues
// synchronized via a binary semaphore. Pass 1 writes `i + 1000` to
// buf_a on compute queue; signals sem1. Pass 2 waits on sem1 at
// ComputeShader stage on graphics queue, reads buf_a, writes
// `2 * buf_a[i]` to buf_b; signals fence. Host waits on fence, reads
// buf_b, validates each of 64 elements equals `2 * (i + 1000)`.
//
// Hardware path differentiation:
//   - has_dedicated_compute_queue() == true  → real cross-queue sync via VkSemaphore
//   - has_dedicated_compute_queue() == false → fallback: same queue,
//     semaphore still serializes (FallbackGracefully default).
//
// Validation discriminator (manually confirmed): removing the
// SemaphoreWait from pass 2's SubmitInfo triggers
// `VUID-vkCmdDispatch-None-...` write-after-write hazard on buf_a
// when the queue path is async.
// =====================================================================

TEST_CASE("Vulkan async-compute cross-queue semaphore first-light", "[rhi][vulkan][compute][v0d]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    // The cross-queue submit is exactly where family-mismatch VUIDs live — this test must stay
    // validation-SILENT, not merely correct (the 00074 print-and-pass regression trap).
    crd::rhi::ValidationCapture capture(*device);

    // Report hardware path for the test log — both paths must pass.
    const bool dedicated = device->has_dedicated_compute_queue();
    INFO("dedicated_compute_queue = " << (dedicated ? "true" : "false (fallback)"));

    const auto shader_dir = fs::executable_dir() / "shaders";

    crd::containers::Array<crd::u8> cs1_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs1_spv));
    auto cs1 = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs1_spv.data(), cs1_spv.size()));
    REQUIRE(cs1 != nullptr);

    crd::containers::Array<crd::u8> cs2_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0c_doubler.comp.spv", cs2_spv));
    auto cs2 = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs2_spv.data(), cs2_spv.size()));
    REQUIRE(cs2 != nullptr);

    // Pass-1 layout (write to set0/binding0).
    crd::rhi::DescriptorBinding p1_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc p1_set0_desc{};
    p1_set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(p1_bindings, 1);
    auto p1_set0 = device->create_descriptor_set_layout(p1_set0_desc);
    const crd::rhi::DescriptorSetLayout* p1_layouts[] = {p1_set0.get()};
    crd::rhi::PipelineLayoutDesc p1_layout_desc{};
    p1_layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(p1_layouts, 1);
    auto p1_layout = device->create_pipeline_layout(p1_layout_desc);

    constexpr crd::u32 base_offset = 1000;
    crd::rhi::SpecializationConstantEntry p1_spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc p1_pipe_desc{};
    p1_pipe_desc.compute_program  = cs1.get();
    p1_pipe_desc.pipeline_layout = p1_layout.get();
    p1_pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&p1_spec_entry, 1);
    p1_pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&base_offset),
                                            sizeof(base_offset));
    auto p1_pipeline = device->create_compute_pipeline(p1_pipe_desc);

    // Pass-2 layout (read set0/binding0, write set0/binding1).
    crd::rhi::DescriptorBinding p2_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc p2_set0_desc{};
    p2_set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(p2_bindings, 2);
    auto p2_set0 = device->create_descriptor_set_layout(p2_set0_desc);
    const crd::rhi::DescriptorSetLayout* p2_layouts[] = {p2_set0.get()};
    crd::rhi::PipelineLayoutDesc p2_layout_desc{};
    p2_layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(p2_layouts, 1);
    auto p2_layout = device->create_pipeline_layout(p2_layout_desc);

    crd::rhi::ComputePipelineDesc p2_pipe_desc{};
    p2_pipe_desc.compute_program  = cs2.get();
    p2_pipe_desc.pipeline_layout = p2_layout.get();
    auto p2_pipeline = device->create_compute_pipeline(p2_pipe_desc);

    constexpr crd::u32 element_count = 64;
    constexpr crd::u64 buffer_bytes  = element_count * sizeof(crd::u32);
    auto buf_a = device->create_buffer(
        {buffer_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto buf_b = device->create_buffer(
        {buffer_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    auto* bzinit = static_cast<crd::u32*>(buf_b->map());
    for (crd::u32 i = 0; i < element_count; ++i) { bzinit[i] = 0; }
    buf_b->unmap();

    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 1;
    alloc_desc.max_sets_per_frame            = 8;
    alloc_desc.max_storage_buffers_per_frame = 8;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    desc_alloc->begin_frame(0);
    auto p1_set = desc_alloc->allocate(*p1_set0);
    p1_set->update_buffer(0, *buf_a, 0, buffer_bytes);
    auto p2_set = desc_alloc->allocate(*p2_set0);
    p2_set->update_buffer(0, *buf_a, 0, buffer_bytes);
    p2_set->update_buffer(1, *buf_b, 0, buffer_bytes);

    // cmd1 is SUBMITTED on compute_queue() — it must come from the matching-family pool (D139
    // create_command_buffer_for_queue; a graphics-pool buffer on a dedicated compute family fires
    // VUID-vkQueueSubmit-pCommandBuffers-00074). cmd2 goes to the graphics queue → the default pool.
    auto cmd1 = device->create_command_buffer_for_queue(device->compute_queue());
    auto cmd2 = device->create_command_buffer();
    REQUIRE(cmd1 != nullptr);
    auto sem  = device->create_semaphore();
    auto fence_pass1 = device->create_fence();
    auto fence_pass2 = device->create_fence();
    REQUIRE(sem != nullptr);

    // Pass 1 — compute queue. Writes bufA; signals sem.
    cmd1->begin();
    cmd1->bind_compute_pipeline(*p1_pipeline);
    crd::rhi::DescriptorSet* p1_sets[] = {p1_set.get()};
    cmd1->bind_compute_descriptor_sets(
        *p1_layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(p1_sets, 1));
    cmd1->dispatch(1, 1, 1);
    cmd1->end();

    crd::rhi::Semaphore* signal_sems[] = {sem.get()};
    crd::rhi::SubmitInfo info1{};
    info1.command_buffer    = cmd1.get();
    info1.signal_fence      = fence_pass1.get();
    info1.signal_semaphores =
        crd::containers::ConstSpan<crd::rhi::Semaphore*>(signal_sems, 1);
    device->compute_queue().submit(info1);

    // Pass 2 — graphics queue (or fallback). Waits on sem at ComputeShader
    // stage; reads bufA; writes bufB; signals fence.
    cmd2->begin();
    cmd2->bind_compute_pipeline(*p2_pipeline);
    crd::rhi::DescriptorSet* p2_sets[] = {p2_set.get()};
    cmd2->bind_compute_descriptor_sets(
        *p2_layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(p2_sets, 1));
    cmd2->dispatch(1, 1, 1);
    cmd2->end();

    crd::rhi::SemaphoreWait waits[] = {
        {.semaphore = sem.get(), .wait_stage = crd::rhi::PipelineStage::ComputeShader},
    };
    crd::rhi::SubmitInfo info2{};
    info2.command_buffer  = cmd2.get();
    info2.signal_fence    = fence_pass2.get();
    info2.wait_semaphores =
        crd::containers::ConstSpan<crd::rhi::SemaphoreWait>(waits, 1);
    device->graphics_queue().submit(info2);

    fence_pass2->wait();

    auto* mapped = static_cast<crd::u32*>(buf_b->map());
    REQUIRE(mapped != nullptr);
    for (crd::u32 i = 0; i < element_count; ++i)
    {
        CHECK(mapped[i] == 2U * (i + base_offset));
    }
    buf_b->unmap();
    CHECK(capture.error_count() == 0U); // family-matched pools on BOTH queues — 00074 stays dead
    CHECK(capture.warning_count() == 0U);
    device->wait_idle();
}

TEST_CASE("Vulkan compute_queue pointer-identity contract", "[rhi][vulkan][compute][v0d]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    // ADR-0080 D9: if no dedicated compute queue exists, compute_queue()
    // returns the same Queue& as graphics_queue() (pointer identity).
    // If dedicated exists, they're distinct.
    if (device->has_dedicated_compute_queue())
    {
        CHECK(&device->compute_queue() != &device->graphics_queue());
    }
    else
    {
        CHECK(&device->compute_queue() == &device->graphics_queue());
    }
}

TEST_CASE("Vulkan buffer_barrier no-op when from == to", "[rhi][vulkan][compute][v0c]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    auto buf = device->create_buffer(
        {256, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage), crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buf != nullptr);

    auto cmd = device->create_command_buffer();
    cmd->begin();
    // Same-state barrier should be a cheap no-op (early return in impl).
    // Validation layer would flag a redundant pipeline barrier; the impl
    // skips it before we get there.
    cmd->buffer_barrier(*buf, crd::rhi::BufferAccess::ComputeShaderWrite,
                        crd::rhi::BufferAccess::ComputeShaderWrite);
    cmd->end();
    device->wait_idle();
    SUCCEED("same-state barrier elided cleanly");
}

TEST_CASE("Vulkan compute dispatch_indirect (D4 indirect path)", "[rhi][vulkan][compute][v0b]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);

    // Same shader / pipeline assembly as the direct dispatch test,
    // but with workgroup counts stored in an indirect buffer.
    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs_spv));
    auto cs = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::make_span(cs_spv.data(), cs_spv.size()));

    crd::rhi::DescriptorBinding bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc set0_desc{};
    set0_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(bindings, 1);
    auto set0 = device->create_descriptor_set_layout(set0_desc);
    const crd::rhi::DescriptorSetLayout* layouts[] = {set0.get()};
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    auto layout = device->create_pipeline_layout(layout_desc);

    constexpr crd::u32 base_offset = 7;
    crd::rhi::SpecializationConstantEntry spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_program  = cs.get();
    pipe_desc.pipeline_layout = layout.get();
    pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&spec_entry, 1);
    pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&base_offset),
                                            sizeof(base_offset));
    auto pipeline = device->create_compute_pipeline(pipe_desc);
    REQUIRE(pipeline != nullptr);

    // Storage buffer (output)
    constexpr crd::u32 element_count = 64;
    constexpr crd::u64 buffer_bytes  = element_count * sizeof(crd::u32);
    auto storage = device->create_buffer(
        {buffer_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    auto* zinit = static_cast<crd::u32*>(storage->map());
    for (crd::u32 i = 0; i < element_count; ++i) { zinit[i] = 0; }
    storage->unmap();

    // Indirect buffer holds VkDispatchIndirectCommand = {x, y, z}.
    crd::u32 dispatch_counts[3] = {1, 1, 1};
    auto indirect = device->create_buffer(
        {sizeof(dispatch_counts),
         crd::rhi::BufferUsage::Indirect | crd::rhi::BufferUsage::TransferDst,
         crd::rhi::MemoryUsage::CpuToGpu});
    auto* indirect_data = static_cast<crd::u32*>(indirect->map());
    REQUIRE(indirect_data != nullptr);
    indirect_data[0] = dispatch_counts[0];
    indirect_data[1] = dispatch_counts[1];
    indirect_data[2] = dispatch_counts[2];
    indirect->unmap();

    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight                 = 1;
    alloc_desc.max_sets_per_frame               = 4;
    alloc_desc.max_storage_buffers_per_frame    = 4;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    desc_alloc->begin_frame(0);
    auto desc_set = desc_alloc->allocate(*set0);
    desc_set->update_buffer(0, *storage, 0, buffer_bytes);

    auto cmd   = device->create_command_buffer();
    auto fence = device->create_fence();
    cmd->begin();
    cmd->bind_compute_pipeline(*pipeline);
    crd::rhi::DescriptorSet* sets[] = {desc_set.get()};
    cmd->bind_compute_descriptor_sets(
        *layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
    cmd->dispatch_indirect(*indirect, 0);
    cmd->end();

    device->graphics_queue().submit(*cmd, *fence);
    fence->wait();

    auto* mapped = static_cast<crd::u32*>(storage->map());
    for (crd::u32 i = 0; i < element_count; ++i)
    {
        CHECK(mapped[i] == i + base_offset);
    }
    storage->unmap();
    device->wait_idle();
}

// ADR-0085 S7 test policy: relocate everything; count what moved.
namespace
{
class DefragAllPolicy final : public crd::rhi::IDefragPolicy
{
public:
    [[nodiscard]] bool should_defrag(const crd::rhi::Buffer& /*b*/) override { return true; }
    [[nodiscard]] bool should_defrag(const crd::rhi::Image& /*i*/) override { return true; }
    void on_relocated(const crd::rhi::Buffer& /*b*/, crd::u32 /*gen*/) override { ++buffer_relocations; }
    void on_relocated(const crd::rhi::Image& /*i*/, crd::u32 /*gen*/) override { ++image_relocations; }

    crd::u32 buffer_relocations = 0;
    crd::u32 image_relocations  = 0;
};

// ADR-0085 S7 test residency policy: evict the oldest device-local buffers to host
// until enough is freed.
class EvictFirstPolicy final : public crd::rhi::IResidencyPolicy
{
public:
    [[nodiscard]] crd::u64 evict(crd::rhi::IGpuResidencyContext& ctx, crd::u64 needed) override
    {
        crd::u64 freed = 0;
        while (freed < needed && ctx.resident_buffer_count() > 0)
        {
            crd::rhi::Buffer* b = ctx.resident_buffer_at(0);
            if (b == nullptr)
            {
                break;
            }
            const crd::u64 f = ctx.evict_to_host(*b);
            if (f == 0)
            {
                break; // nothing more can be shed (e.g. integrated GPU, all DEVICE_LOCAL)
            }
            freed += f;
            ++evict_count;
        }
        return freed;
    }

    crd::u32 evict_count = 0;
};
} // namespace

// ---------------------------------------------------------------------------
// ADR-0085 S6 — GpuAllocator (VkDeviceMemory suballocation) ValidationCapture tests
// ---------------------------------------------------------------------------

TEST_CASE("S6 GPU suballocator shares one block across many small allocations", "[rhi][vulkan][gpualloc]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    constexpr crd::u32                          n = 64;
    std::unique_ptr<crd::rhi::Buffer>           bufs[n];
    for (crd::u32 i = 0; i < n; ++i)
    {
        bufs[i] = device->create_buffer(
            {256, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
        REQUIRE(bufs[i] != nullptr);
    }
    // 64 x 256 B buffers must NOT need 64 VkDeviceMemory blocks — proves suballocation.
    const crd::u32 blocks = crd::rhi::vulkan_resident_block_count(*device);
    CHECK(blocks >= 1U);
    CHECK(blocks < n);
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S6 GPU suballocations are distinct non-overlapping regions (host round-trip)", "[rhi][vulkan][gpualloc]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    constexpr crd::u32                n    = 32;
    constexpr crd::u32                k_size = 1024;
    std::unique_ptr<crd::rhi::Buffer> bufs[n];
    // Write a per-buffer pattern into ALL buffers first; if any two suballocations
    // overlapped, a later write would corrupt an earlier one.
    for (crd::u32 i = 0; i < n; ++i)
    {
        bufs[i] = device->create_buffer(
            {k_size, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
        REQUIRE(bufs[i] != nullptr);
        auto* p = static_cast<crd::u8*>(bufs[i]->map());
        REQUIRE(p != nullptr);
        std::memset(p, static_cast<int>(i & 0xFF), k_size);
    }
    // Read back: every buffer still holds its own pattern -> regions are disjoint.
    for (crd::u32 i = 0; i < n; ++i)
    {
        const auto* p = static_cast<const crd::u8*>(bufs[i]->map());
        REQUIRE(p != nullptr);
        CHECK(p[0] == static_cast<crd::u8>(i & 0xFF));
        CHECK(p[k_size - 1] == static_cast<crd::u8>(i & 0xFF));
    }
    CHECK(capture.error_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S6 GPU dedicated allocation path for large resources", "[rhi][vulkan][gpualloc]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    // 20 MiB > the 16 MiB dedicated threshold -> own VkDeviceMemory, not pooled.
    auto big = device->create_buffer({crd::u64{20} << 20,
                                      crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex),
                                      crd::rhi::MemoryUsage::CpuToGpu});
    REQUIRE(big != nullptr);
    auto* p = static_cast<crd::u8*>(big->map());
    REQUIRE(p != nullptr);
    p[0]                       = 0xAB;
    p[(crd::u64{20} << 20) - 1] = 0xCD;
    CHECK(p[0] == 0xAB);
    CHECK(p[(crd::u64{20} << 20) - 1] == 0xCD);
    CHECK(capture.error_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S6 GPU buffer + image coexist without bufferImageGranularity errors", "[rhi][vulkan][gpualloc]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    // A small buffer (linear) immediately followed by a small optimal-tiling image
    // (non-linear). Separate linear/non-linear pools keep them off a shared
    // bufferImageGranularity page -> zero validation errors on any GPU.
    auto buf = device->create_buffer(
        {4096, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage), crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buf != nullptr);
    auto img = device->create_image({{64, 64},
                                     crd::rhi::Format::R8G8B8A8Unorm,
                                     crd::rhi::enum_bits(crd::rhi::ImageUsage::Sampled) |
                                         crd::rhi::enum_bits(crd::rhi::ImageUsage::TransferDst),
                                     1,
                                     1});
    REQUIRE(img != nullptr);
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
    device->wait_idle();
}

// ---------------------------------------------------------------------------
// ADR-0085 S7 — GPU defragmentation ValidationCapture tests
// ---------------------------------------------------------------------------

TEST_CASE("S7 buffer defrag relocates and preserves contents", "[rhi][vulkan][gpudefrag]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    constexpr crd::u32                n    = 16;
    constexpr crd::u32                k_size = 4096;
    std::unique_ptr<crd::rhi::Buffer> bufs[n];
    for (crd::u32 i = 0; i < n; ++i)
    {
        bufs[i] = device->create_buffer(
            {k_size, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex) | crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc) |
                        crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
             crd::rhi::MemoryUsage::CpuToGpu});
        REQUIRE(bufs[i] != nullptr);
        auto* p = static_cast<crd::u8*>(bufs[i]->map());
        REQUIRE(p != nullptr);
        std::memset(p, static_cast<int>(i & 0xFF), k_size);
    }
    // Free the even-indexed buffers to punch holes, then defragment.
    for (crd::u32 i = 0; i < n; i += 2)
    {
        bufs[i].reset();
    }

    DefragAllPolicy policy;
    crd::rhi::vulkan_defragment(*device, policy);

    // Survivors (odd indices) keep their contents at the (now relocated) address.
    for (crd::u32 i = 1; i < n; i += 2)
    {
        const auto* p = static_cast<const crd::u8*>(bufs[i]->map());
        REQUIRE(p != nullptr);
        CHECK(p[0] == static_cast<crd::u8>(i & 0xFF));
        CHECK(p[k_size - 1] == static_cast<crd::u8>(i & 0xFF));
    }
    CHECK(policy.buffer_relocations >= 1U); // something actually moved
    CHECK(capture.error_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S7 image defrag relocates a populated image with zero validation errors", "[rhi][vulkan][gpudefrag]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    // TransferSrc|Dst required for vkCmdCopyImage; ColorAttachment for a defined layout.
    auto img = device->create_image({{128, 128},
                                     crd::rhi::Format::R8G8B8A8Unorm,
                                     crd::rhi::enum_bits(crd::rhi::ImageUsage::ColorAttachment) |
                                         crd::rhi::enum_bits(crd::rhi::ImageUsage::TransferSrc) |
                                         crd::rhi::enum_bits(crd::rhi::ImageUsage::TransferDst),
                                     1,
                                     1});
    REQUIRE(img != nullptr);

    // Move it out of UNDEFINED (defrag skips UNDEFINED images — no content to preserve).
    auto cmd = device->create_command_buffer();
    cmd->begin();
    cmd->transition_image(*img, crd::rhi::ImageAccess::Undefined, crd::rhi::ImageAccess::ColorWrite);
    cmd->end();
    device->graphics_queue().submit_and_wait(*cmd);

    DefragAllPolicy policy;
    crd::rhi::vulkan_defragment(*device, policy);
    CHECK(policy.image_relocations >= 1U); // the image moved
    CHECK(capture.error_count() == 0U);    // barriers + subresource copy were correct
    CHECK(capture.warning_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S7 residency relocation preserves buffer data across device<->host", "[rhi][vulkan][gpuresidency]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    constexpr crd::u32 k_size = 4096;
    auto               buf   = device->create_buffer(
        {k_size,
                       crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex) | crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
                       crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buf != nullptr);
    CHECK(buf->map() == nullptr); // device-local is not host-mappable

    // Evict to host -> now mappable; write a known pattern.
    REQUIRE(crd::rhi::vulkan_evict_to_host(*device, *buf) > 0U);
    auto* p = static_cast<crd::u8*>(buf->map());
    REQUIRE(p != nullptr);
    for (crd::u32 i = 0; i < k_size; ++i) { p[i] = static_cast<crd::u8>(i & 0xFF); }

    // Re-promote to device (host->device copy), then evict again (device->host copy):
    // the pattern must survive BOTH relocation copies.
    crd::rhi::vulkan_make_resident(*device, *buf);
    CHECK(buf->map() == nullptr); // device-local again
    REQUIRE(crd::rhi::vulkan_evict_to_host(*device, *buf) > 0U);
    const auto* q = static_cast<const crd::u8*>(buf->map());
    REQUIRE(q != nullptr);
    for (crd::u32 i = 0; i < k_size; ++i) { CHECK(q[i] == static_cast<crd::u8>(i & 0xFF)); }
    CHECK(capture.error_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S7 residency auto-evicts device-local memory over the budget", "[rhi][vulkan][gpuresidency]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    EvictFirstPolicy   policy;
    constexpr crd::u32 k_size = 256U * 1024U;
    crd::rhi::vulkan_configure_residency(*device, &policy, crd::u64{4} * k_size); // budget = 4 buffers

    std::unique_ptr<crd::rhi::Buffer> bufs[8];
    for (crd::u32 i = 0; i < 8; ++i)
    {
        bufs[i] = device->create_buffer(
            {k_size,
             crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) | crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc) |
                 crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
             crd::rhi::MemoryUsage::GpuOnly});
        REQUIRE(bufs[i] != nullptr);
    }
    // 8 device-local buffers against a 4-buffer budget must have engaged the policy,
    // and the resident device-local total must stay near the budget.
    CHECK(policy.evict_count >= 1U);
    CHECK(crd::rhi::vulkan_device_local_used(*device) <= crd::u64{5} * k_size);
    CHECK(capture.error_count() == 0U);
    device->wait_idle();
}

TEST_CASE("S7 NullDefragPolicy relocates nothing", "[rhi][vulkan][gpudefrag]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    auto gpu = make_adopted_gpu();
    auto& device = gpu.device;
    REQUIRE(device != nullptr);
    crd::rhi::ValidationCapture capture(*device);

    auto buf = device->create_buffer(
        {4096, crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    REQUIRE(buf != nullptr);

    crd::rhi::NullDefragPolicy policy; // default: relocates nothing
    crd::rhi::vulkan_defragment(*device, policy);
    CHECK(capture.error_count() == 0U);
    device->wait_idle();
}
