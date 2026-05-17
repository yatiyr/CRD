#include <crd/platform/filesystem.hpp>
#include <crd/platform/platform.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);

    auto device = instance->create_device({});
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    auto swapchain = device->create_swapchain(
        {window.native_handle(), {640, 360}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    crd::containers::Array<crd::u8> fs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv));
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.frag.spv", fs_spv));

    auto vs = device->create_shader_module(
        {crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(vs_spv.data(), vs_spv.size())});
    auto fs_module = device->create_shader_module(
        {crd::rhi::ShaderStage::Fragment, "main", crd::containers::make_span(fs_spv.data(), fs_spv.size())});
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader  = nullptr;
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv));
    auto vs = device->create_shader_module(
        {crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(vs_spv.data(), vs_spv.size())});
    REQUIRE(vs != nullptr);

    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader = vs.get(); // wrong stage — should reject
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0a.comp.spv", cs_spv));
    auto cs = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main", crd::containers::make_span(cs_spv.data(), cs_spv.size())});
    REQUIRE(cs != nullptr);

    // Path A: synthesised empty pipeline layout (desc.pipeline_layout == nullptr).
    crd::rhi::ComputePipelineDesc desc{};
    desc.compute_shader  = cs.get();
    desc.pipeline_layout = nullptr;
    auto pipeline = device->create_compute_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    CHECK(pipeline->desc().compute_shader == cs.get());

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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0a.comp.spv", cs_spv));
    auto cs = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main", crd::containers::make_span(cs_spv.data(), cs_spv.size())});
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
    desc.compute_shader  = cs.get();
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0a.comp.spv", cs_spv));

    // 8 cycles: create + drop. Validation layer + ASan must stay quiet.
    constexpr int kCycles = 8;
    for (int i = 0; i < kCycles; ++i)
    {
        auto cs = device->create_shader_module(
            {crd::rhi::ShaderStage::Compute, "main", crd::containers::make_span(cs_spv.data(), cs_spv.size())});
        REQUIRE(cs != nullptr);

        crd::rhi::ComputePipelineDesc desc{};
        desc.compute_shader = cs.get();
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    constexpr crd::u64 kStorageBytes = 4 * 1024; // 4 KB
    auto buffer = device->create_buffer(
        {kStorageBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    REQUIRE(buffer != nullptr);
    CHECK(buffer->desc().size_bytes == kStorageBytes);
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    // --- Shader + spec-const (D6) ---
    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs_spv));
    auto cs = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::make_span(cs_spv.data(), cs_spv.size())});
    REQUIRE(cs != nullptr);

    constexpr crd::u32 kBaseOffset = 1000;
    crd::rhi::SpecializationConstantEntry spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_shader = cs.get();
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
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&kBaseOffset),
                                            sizeof(kBaseOffset));
    auto pipeline = device->create_compute_pipeline(pipe_desc);
    REQUIRE(pipeline != nullptr);

    // --- Storage buffer (host-visible coherent for readback) ---
    constexpr crd::u32 kElementCount = 64; // == local_size_x; 1 workgroup
    constexpr crd::u64 kBufferBytes  = kElementCount * sizeof(crd::u32);
    auto storage = device->create_buffer(
        {kBufferBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    REQUIRE(storage != nullptr);

    // Pre-zero buffer so the post-dispatch readback proves writes happened.
    auto* mapped_init = static_cast<crd::u32*>(storage->map());
    REQUIRE(mapped_init != nullptr);
    for (crd::u32 i = 0; i < kElementCount; ++i) { mapped_init[i] = 0; }
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
    desc_set->update_buffer(0, *storage, 0, kBufferBytes);

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

    // --- Host readback: each element == index + kBaseOffset ---
    auto* mapped = static_cast<crd::u32*>(storage->map());
    REQUIRE(mapped != nullptr);
    for (crd::u32 i = 0; i < kElementCount; ++i)
    {
        CHECK(mapped[i] == i + kBaseOffset);
    }
    storage->unmap();

    device->wait_idle();
}

// =====================================================================
// Phase 3.1.7.6 v0c — Vulkan buffer_barrier end-to-end (two compute
// passes with the barrier between them).
//
// pass 1 (compute_v0b_dispatch.comp): buf_a[i] = i + kBaseOffset
// buffer_barrier(buf_a, ComputeShaderWrite → ComputeShaderRead)
// pass 2 (compute_v0c_doubler.comp):  buf_b[i] = 2 * buf_a[i]
// host readback validates buf_b[i] == 2 * (i + kBaseOffset)
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";

    // --- Shader modules (pass 1 = v0b dispatch, pass 2 = v0c doubler) ---
    crd::containers::Array<crd::u8> cs1_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs1_spv));
    auto cs1 = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::make_span(cs1_spv.data(), cs1_spv.size())});
    REQUIRE(cs1 != nullptr);

    crd::containers::Array<crd::u8> cs2_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0c_doubler.comp.spv", cs2_spv));
    auto cs2 = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::make_span(cs2_spv.data(), cs2_spv.size())});
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

    constexpr crd::u32 kBaseOffset = 1000;
    crd::rhi::SpecializationConstantEntry p1_spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc p1_pipe_desc{};
    p1_pipe_desc.compute_shader  = cs1.get();
    p1_pipe_desc.pipeline_layout = p1_layout.get();
    p1_pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&p1_spec_entry, 1);
    p1_pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&kBaseOffset),
                                            sizeof(kBaseOffset));
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
    p2_pipe_desc.compute_shader  = cs2.get();
    p2_pipe_desc.pipeline_layout = p2_layout.get();
    auto p2_pipeline = device->create_compute_pipeline(p2_pipe_desc);
    REQUIRE(p2_pipeline != nullptr);

    // --- Buffers (buf_a: GPU-only intermediate; buf_b: host-visible output) ---
    constexpr crd::u32 kElementCount = 64;
    constexpr crd::u64 kBufferBytes  = kElementCount * sizeof(crd::u32);
    auto buf_a = device->create_buffer(
        {kBufferBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto buf_b = device->create_buffer(
        {kBufferBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    REQUIRE(buf_a != nullptr);
    REQUIRE(buf_b != nullptr);
    auto* bzinit = static_cast<crd::u32*>(buf_b->map());
    for (crd::u32 i = 0; i < kElementCount; ++i) { bzinit[i] = 0; }
    buf_b->unmap();

    // --- Descriptor allocator + sets ---
    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 1;
    alloc_desc.max_sets_per_frame            = 8;
    alloc_desc.max_storage_buffers_per_frame = 8;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    desc_alloc->begin_frame(0);

    auto p1_set = desc_alloc->allocate(*p1_set0);
    p1_set->update_buffer(0, *buf_a, 0, kBufferBytes);

    auto p2_set = desc_alloc->allocate(*p2_set0);
    p2_set->update_buffer(0, *buf_a, 0, kBufferBytes);
    p2_set->update_buffer(1, *buf_b, 0, kBufferBytes);

    // --- Record + submit ---
    auto cmd   = device->create_command_buffer();
    auto fence = device->create_fence();

    cmd->begin();
    cmd->bind_compute_pipeline(*p1_pipeline);
    crd::rhi::DescriptorSet* p1_sets[] = {p1_set.get()};
    cmd->bind_compute_descriptor_sets(
        *p1_layout, 0, crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(p1_sets, 1));
    cmd->dispatch(1, 1, 1); // pass 1: buf_a[i] = i + kBaseOffset

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

    // --- Host readback: buf_b[i] == 2 * (i + kBaseOffset) ---
    auto* mapped = static_cast<crd::u32*>(buf_b->map());
    REQUIRE(mapped != nullptr);
    for (crd::u32 i = 0; i < kElementCount; ++i)
    {
        CHECK(mapped[i] == 2U * (i + kBaseOffset));
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({}); // FallbackGracefully default
    REQUIRE(device != nullptr);

    // Report hardware path for the test log — both paths must pass.
    const bool dedicated = device->has_dedicated_compute_queue();
    INFO("dedicated_compute_queue = " << (dedicated ? "true" : "false (fallback)"));

    const auto shader_dir = fs::executable_dir() / "shaders";

    crd::containers::Array<crd::u8> cs1_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs1_spv));
    auto cs1 = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::make_span(cs1_spv.data(), cs1_spv.size())});
    REQUIRE(cs1 != nullptr);

    crd::containers::Array<crd::u8> cs2_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0c_doubler.comp.spv", cs2_spv));
    auto cs2 = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::make_span(cs2_spv.data(), cs2_spv.size())});
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

    constexpr crd::u32 kBaseOffset = 1000;
    crd::rhi::SpecializationConstantEntry p1_spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc p1_pipe_desc{};
    p1_pipe_desc.compute_shader  = cs1.get();
    p1_pipe_desc.pipeline_layout = p1_layout.get();
    p1_pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&p1_spec_entry, 1);
    p1_pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&kBaseOffset),
                                            sizeof(kBaseOffset));
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
    p2_pipe_desc.compute_shader  = cs2.get();
    p2_pipe_desc.pipeline_layout = p2_layout.get();
    auto p2_pipeline = device->create_compute_pipeline(p2_pipe_desc);

    constexpr crd::u32 kElementCount = 64;
    constexpr crd::u64 kBufferBytes  = kElementCount * sizeof(crd::u32);
    auto buf_a = device->create_buffer(
        {kBufferBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto buf_b = device->create_buffer(
        {kBufferBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    auto* bzinit = static_cast<crd::u32*>(buf_b->map());
    for (crd::u32 i = 0; i < kElementCount; ++i) { bzinit[i] = 0; }
    buf_b->unmap();

    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 1;
    alloc_desc.max_sets_per_frame            = 8;
    alloc_desc.max_storage_buffers_per_frame = 8;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    desc_alloc->begin_frame(0);
    auto p1_set = desc_alloc->allocate(*p1_set0);
    p1_set->update_buffer(0, *buf_a, 0, kBufferBytes);
    auto p2_set = desc_alloc->allocate(*p2_set0);
    p2_set->update_buffer(0, *buf_a, 0, kBufferBytes);
    p2_set->update_buffer(1, *buf_b, 0, kBufferBytes);

    auto cmd1 = device->create_command_buffer();
    auto cmd2 = device->create_command_buffer();
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
    for (crd::u32 i = 0; i < kElementCount; ++i)
    {
        CHECK(mapped[i] == 2U * (i + kBaseOffset));
    }
    buf_b->unmap();
    device->wait_idle();
}

TEST_CASE("Vulkan compute_queue pointer-identity contract", "[rhi][vulkan][compute][v0d]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan compute test");
        return;
    }

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
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

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    // Same shader / pipeline assembly as the direct dispatch test,
    // but with workgroup counts stored in an indirect buffer.
    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> cs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "compute_v0b_dispatch.comp.spv", cs_spv));
    auto cs = device->create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::make_span(cs_spv.data(), cs_spv.size())});

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

    constexpr crd::u32 kBaseOffset = 7;
    crd::rhi::SpecializationConstantEntry spec_entry{0, 0, sizeof(crd::u32)};
    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_shader  = cs.get();
    pipe_desc.pipeline_layout = layout.get();
    pipe_desc.specialization_entries =
        crd::containers::ConstSpan<crd::rhi::SpecializationConstantEntry>(&spec_entry, 1);
    pipe_desc.specialization_data =
        crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&kBaseOffset),
                                            sizeof(kBaseOffset));
    auto pipeline = device->create_compute_pipeline(pipe_desc);
    REQUIRE(pipeline != nullptr);

    // Storage buffer (output)
    constexpr crd::u32 kElementCount = 64;
    constexpr crd::u64 kBufferBytes  = kElementCount * sizeof(crd::u32);
    auto storage = device->create_buffer(
        {kBufferBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    auto* zinit = static_cast<crd::u32*>(storage->map());
    for (crd::u32 i = 0; i < kElementCount; ++i) { zinit[i] = 0; }
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
    desc_set->update_buffer(0, *storage, 0, kBufferBytes);

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
    for (crd::u32 i = 0; i < kElementCount; ++i)
    {
        CHECK(mapped[i] == i + kBaseOffset);
    }
    storage->unmap();
    device->wait_idle();
}
