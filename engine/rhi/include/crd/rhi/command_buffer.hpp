#pragma once

#include <crd/rhi/buffer.hpp>
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
};
} // namespace crd::rhi
