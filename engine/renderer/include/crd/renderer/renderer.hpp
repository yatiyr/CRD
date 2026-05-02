#pragma once

#include <crd/containers/array.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/types.hpp>
#include <crd/shader/runtime.hpp>

namespace crd::renderer
{

struct Camera
{
    crd::math::Mat4f view = crd::math::Mat4f::identity();
    crd::math::Mat4f projection = crd::math::Mat4f::identity();

    [[nodiscard]] crd::math::Mat4f view_projection() const noexcept { return projection * view; }
};

// Per-frame data shared between Renderer and IRenderPath.
// Passed to build_frame() and forwarded to IRenderPath::build() each frame.
struct FrameContext
{
    Camera camera;
    crd::math::Vec3f camera_position{}; // world-space camera origin; used for depth sorting
    rhi::Extent2D viewport{};
    crd::u32 frame_index = 0;           // monotonically increasing frame counter
};

// Controls how a renderable is bucketed and depth-sorted within a frame.
enum class DrawBucket : crd::u8
{
    Opaque,      // depth-sorted front-to-back (ascending distance); feeds early-Z prepass
    Masked,      // depth-sorted front-to-back; alpha-tested; no blending
    Translucent, // depth-sorted back-to-front (descending distance); alpha-blended
};

struct Renderable
{
    crd::math::Transformf transform = crd::math::Transformf::identity();
    crd::rhi::Buffer*   vertex_buffer = nullptr;
    crd::u32            vertex_count  = 0;
    crd::rhi::Buffer*   index_buffer  = nullptr; // null → non-indexed draw
    crd::u32            index_count   = 0;
    crd::rhi::IndexType index_type    = crd::rhi::IndexType::Uint32;
    crd::u64 material_instance_id = 0;
    crd::shader::VariantHandle variant{};
    DrawBucket bucket = DrawBucket::Opaque;
};

struct DrawItem
{
    crd::math::Mat4f model = crd::math::Mat4f::identity();
    crd::math::Mat4f view_projection = crd::math::Mat4f::identity();
    crd::rhi::Buffer*   vertex_buffer = nullptr;
    crd::u32            vertex_count  = 0;
    crd::rhi::Buffer*   index_buffer  = nullptr;
    crd::u32            index_count   = 0;
    crd::rhi::IndexType index_type    = crd::rhi::IndexType::Uint32;
    crd::u64 material_instance_id = 0;
    crd::shader::VariantHandle variant{};
    crd::shader::VariantPipelineDesc handoff{};
    crd::f32 depth = 0.0F; // squared camera distance; populated by build_frame for sorting
};

// Pre-bucketed, depth-sorted output of Renderer::build_frame().
// Passed directly to IRenderPath::build() each frame.
struct DrawList
{
    crd::containers::Array<DrawItem> opaque{};
    crd::containers::Array<DrawItem> masked{};
    crd::containers::Array<DrawItem> translucent{};

    [[nodiscard]] bool empty() const noexcept
    {
        return opaque.empty() && masked.empty() && translucent.empty();
    }

    [[nodiscard]] crd::usize total_count() const noexcept
    {
        return opaque.size() + masked.size() + translucent.size();
    }

    void clear() noexcept
    {
        opaque.clear();
        masked.clear();
        translucent.clear();
    }
};

// Injectable pipeline-resolution interface.
// Kept for IRenderPath implementations and unit-test fakes.
// Concrete render paths own a VariantKey → Pipeline* cache internally.
class PipelineResolver
{
public:
    virtual ~PipelineResolver() = default;
    [[nodiscard]] virtual crd::rhi::Pipeline*
    resolve_pipeline(const crd::shader::VariantPipelineDesc& handoff) noexcept = 0;
};

class Renderer
{
public:
    // Queue a renderable for the next frame.
    void submit(const Renderable& renderable);

    // Drop all queued renderables. Call at the start of each frame before re-submitting.
    void clear() noexcept;

    // Classify submitted renderables into DrawList buckets, compute per-item squared
    // camera distance, and depth-sort each bucket:
    //   opaque + masked  — ascending  (front-to-back, minimises overdraw)
    //   translucent      — descending (back-to-front, correct alpha compositing)
    //
    // Returns false if any submitted renderable has a null vertex buffer, zero vertex
    // count, or a variant that the shader runtime cannot describe.
    [[nodiscard]] bool build_frame(const FrameContext& ctx,
                                   const crd::shader::Runtime& shader_runtime,
                                   DrawList& out) const;

    [[nodiscard]] crd::usize renderable_count() const noexcept { return m_renderables.size(); }

private:
    crd::containers::Array<Renderable> m_renderables{};
};

} // namespace crd::renderer
