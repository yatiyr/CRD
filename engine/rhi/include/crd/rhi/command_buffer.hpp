#pragma once

#include <crd/rhi/buffer.hpp>
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
    virtual void draw(crd::u32 vertex_count, crd::u32 first_vertex) = 0;
};
} // namespace crd::rhi
