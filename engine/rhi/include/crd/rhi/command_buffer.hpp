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
};
} // namespace crd::rhi
