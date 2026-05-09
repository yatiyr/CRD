#pragma once

#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/fence.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/shader_module.hpp>
#include <crd/rhi/swapchain.hpp>

#include <memory>

namespace crd::rhi
{
class Device
{
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual BackendApi api() const noexcept = 0;

    // --- Resource factories ---
    [[nodiscard]] virtual std::unique_ptr<Swapchain>     create_swapchain(const SwapchainDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<Buffer>        create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<Image>         create_image(const ImageDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<ShaderModule>  create_shader_module(const ShaderModuleDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<Pipeline>      create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandBuffer> create_command_buffer() = 0;
    // Phase 3.0 v1o1 — fence factory for the non-waiting submit path.
    [[nodiscard]] virtual std::unique_ptr<Fence>         create_fence() = 0;

    // --- Descriptor system factories ---
    // Layouts are immutable after creation; create once at startup, reuse across frames.
    [[nodiscard]] virtual std::unique_ptr<DescriptorSetLayout>
    create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) = 0;

    // A PipelineLayout ties push constant ranges + set layouts together.
    // Shared by all pipeline variants that have the same binding shape.
    [[nodiscard]] virtual std::unique_ptr<PipelineLayout>
    create_pipeline_layout(const PipelineLayoutDesc& desc) = 0;

    // Create a ring-buffer descriptor allocator.
    // Call begin_frame(frame_index) once per frame, then allocate() as needed.
    [[nodiscard]] virtual std::unique_ptr<DescriptorAllocator>
    create_descriptor_allocator(const DescriptorAllocatorDesc& desc) = 0;

    [[nodiscard]] virtual Queue& graphics_queue() noexcept = 0;
    virtual void wait_idle() = 0;
};
} // namespace crd::rhi
