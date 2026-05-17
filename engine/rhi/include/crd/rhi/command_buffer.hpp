#pragma once

#include <crd/rhi/buffer.hpp>
#include <crd/rhi/compute_pipeline.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/image.hpp>
#include <crd/rhi/pipeline.hpp>

namespace crd::rhi
{
class CommandBuffer
{
public:
    virtual ~CommandBuffer() = default;

    virtual void begin() = 0;
    virtual void end() = 0;
    virtual void reset() = 0;

    virtual void begin_rendering(const RenderingInfo& info) = 0;
    virtual void end_rendering() = 0;

    virtual void bind_pipeline(Pipeline& pipeline) = 0;
    virtual void bind_vertex_buffer(Buffer& buffer, crd::u64 offset_bytes) = 0;
    virtual void bind_index_buffer(Buffer& buffer, crd::u64 offset_bytes, IndexType type) = 0;
    virtual void draw(crd::u32 vertex_count, crd::u32 first_vertex) = 0;
    virtual void draw_indexed(crd::u32 index_count, crd::u32 first_index, crd::i32 vertex_offset) = 0;

    // Instanced variant. Issues `vertex_count` x `instance_count` invocations
    // of the vertex shader, with gl_InstanceIndex ranging over
    // [first_instance, first_instance + instance_count). Used by crd-draw
    // for screen-space-quad-expanded line rendering (6 verts/instance).
    virtual void draw_instanced(crd::u32 vertex_count, crd::u32 instance_count,
                                crd::u32 first_vertex, crd::u32 first_instance) = 0;

    // Copy bytes from a host-visible staging buffer into a device buffer.
    virtual void copy_buffer(Buffer& src, Buffer& dst,
                             crd::u64 src_offset, crd::u64 dst_offset,
                             crd::u64 size_bytes) = 0;

    // Copy a staging buffer into an image (one region per mip level).
    // The image must already be in TransferDst layout.
    virtual void copy_buffer_to_image(Buffer& src, Image& dst,
                                      crd::containers::ConstSpan<BufferImageCopy> regions) = 0;

    // Blit src → dst with linear filtering. Both images must already be in TransferSrc / TransferDst
    // layout respectively (frame graph transitions handle this before the pass executes).
    virtual void blit_image(Image& src, Image& dst,
                            Extent2D src_extent, Extent2D dst_extent) noexcept = 0;

    virtual void transition_image(Image& image, ImageAccess from, ImageAccess to) noexcept = 0;

    // Write push constant data into the command stream.
    // `layout` must be the layout used by the currently-bound (or soon-to-be-bound) pipeline.
    // `stages` selects which shader stages see the data.
    // `offset` + `size` must fit within the range declared in PipelineLayoutDesc.
    virtual void push_constants(PipelineLayout& layout, ShaderStage stages,
                                crd::u32 offset, crd::u32 size, const void* data) = 0;

    // Bind one or more descriptor sets at indices [first_set, first_set + sets.size()).
    // Only rebind what changes — lower-indexed sets remain bound across compatible pipelines.
    virtual void bind_descriptor_sets(PipelineLayout& layout, crd::u32 first_set,
                                      crd::containers::ConstSpan<DescriptorSet*> sets) = 0;

    // Dynamic viewport/scissor — only valid when the bound pipeline was created with use_dynamic_viewport = true.
    virtual void set_viewport(Extent2D extent) noexcept = 0;
    virtual void set_scissor(Rect2D rect) noexcept = 0;

    // ---------------------------------------------------------------
    // Phase 3.1.7.6 v0b-v0c additions — ALL NEW VIRTUALS GO AT THE END.
    // See Device.hpp § vtable-stability discipline for why inserting
    // virtuals in the middle of an interface mis-dispatches downstream
    // callers (silent wrong-method in win-release; 2026-05-17 SEGV).
    // ---------------------------------------------------------------

    // Phase 3.1.7.6 v0c (ADR-0080 D8) — backend-agnostic buffer barrier.
    // Pipeline barrier on a buffer's access state. Typed-enum API (not raw
    // VkAccessFlags) so backend can pick exact srcStageMask/dstStageMask
    // without over-barrier. Same-queue-only path; cross-queue ownership
    // transfer ships at v0d via the async compute queue API.
    //
    // Mirrors `transition_image` shape — single buffer, from→to access.
    // Span-based multi-buffer batched barrier is a documented follow-on
    // when a real consumer needs it (current v9 LBVH single-buffer path
    // does not).
    virtual void buffer_barrier(Buffer& buffer, BufferAccess from, BufferAccess to) noexcept = 0;

    // Phase 3.1.7.6 v0b (ADR-0080) — compute dispatch surface.
    //
    // Sibling methods to graphics, not polymorphic over a shared base
    // (bind_pipeline → VK_PIPELINE_BIND_POINT_GRAPHICS;
    //  bind_compute_pipeline → VK_PIPELINE_BIND_POINT_COMPUTE).
    //
    // Push constants for compute reuse the existing `push_constants`
    // method above with `ShaderStage::Compute` in the stages mask
    // (ADR-0080 D5: same 128-byte budget as graphics — Vulkan minimum
    // guarantee).
    virtual void bind_compute_pipeline(ComputePipeline& pipeline) = 0;

    // Mirrors graphics `bind_descriptor_sets` — same parameter shape;
    // separate method because Vulkan binds to VK_PIPELINE_BIND_POINT_COMPUTE.
    virtual void bind_compute_descriptor_sets(PipelineLayout& layout, crd::u32 first_set,
                                              crd::containers::ConstSpan<DescriptorSet*> sets) = 0;

    // ADR-0080 D4 — `group_count_*` are WORKGROUP counts, not thread counts.
    // Total threads = group_count_x * group_count_y * group_count_z *
    // (local_size_x * local_size_y * local_size_z from the shader).
    virtual void dispatch(crd::u32 group_count_x, crd::u32 group_count_y, crd::u32 group_count_z) = 0;

    // Indirect dispatch reads the workgroup counts from a GPU buffer
    // at the given offset (as a VkDispatchIndirectCommand: 3 × u32).
    // Buffer must have been created with BufferUsage::Indirect (added
    // alongside this method).
    virtual void dispatch_indirect(Buffer& buffer, crd::u64 offset_bytes) = 0;
};
} // namespace crd::rhi
