#pragma once

#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/compute_pipeline.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/fence.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/semaphore.hpp>
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

    // -----------------------------------------------------------------
    // Phase 3.1.7.6 v0a-v0d additions — ALL NEW VIRTUALS GO AT THE END.
    //
    // **vtable-stability discipline:** when extending this interface in
    // future phases, append at the END of the class declaration. Inserting
    // virtuals in the middle shifts every subsequent vtable slot, which
    // produces silent wrong-method dispatch when downstream TUs aren't
    // recompiled (or LTCG caches a stale vtable layout). 2026-05-17 SEGV
    // root-caused to exactly this: v0a inserted `create_compute_pipeline`
    // between `create_graphics_pipeline` and `create_command_buffer`,
    // shifting all subsequent slots, which caused engine/draw's call to
    // `create_pipeline_layout` to dispatch through a wrong slot in
    // win-release and return something that wasn't a VulkanPipelineLayout.
    // -----------------------------------------------------------------

    // Phase 3.1.7.6 v0a (ADR-0080) — compute pipeline factory. Pure
    // additive; existing graphics surface untouched. Bind / dispatch
    // ship at v0b. Returns nullptr on invalid desc (null shader, null
    // layout, etc.) — no exception path.
    [[nodiscard]] virtual std::unique_ptr<ComputePipeline> create_compute_pipeline(const ComputePipelineDesc& desc) = 0;
    // Phase 3.1.7.6 v0d (ADR-0080 D10) — binary semaphore factory for
    // cross-queue handoff (async compute → graphics, etc.).
    [[nodiscard]] virtual std::unique_ptr<Semaphore>     create_semaphore() = 0;
    // Phase 3.1.7.6 v0d (ADR-0080 D9) — async compute queue accessor.
    // **Pointer-identity contract**: if no dedicated compute queue family
    // exists (FallbackGracefully selected at device creation), this
    // returns the same Queue& as `graphics_queue()` — consumers may
    // check `&compute_queue() == &graphics_queue()` to skip cross-queue
    // semaphore setup. Documented + tested.
    [[nodiscard]] virtual Queue& compute_queue() noexcept = 0;
    // Reports whether the compute queue is a distinct hardware queue
    // (true) or a fallback alias of the graphics queue (false). Useful
    // for perf-UI annotation and consumer-side optimization hints.
    [[nodiscard]] virtual bool has_dedicated_compute_queue() const noexcept = 0;
};
} // namespace crd::rhi
