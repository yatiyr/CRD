#pragma once

#include <crd/containers/array.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/transform.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/shader/runtime.hpp>

namespace crd::renderer
{
struct Camera
{
    crd::math::Mat4f view = crd::math::Mat4f::identity();
    crd::math::Mat4f projection = crd::math::Mat4f::identity();

    [[nodiscard]] crd::math::Mat4f view_projection() const noexcept { return projection * view; }
};

struct Renderable
{
    crd::math::Transformf transform = crd::math::Transformf::identity();
    crd::rhi::Buffer* vertex_buffer = nullptr;
    crd::u32 vertex_count = 0;
    crd::u64 material_instance_id = 0;
    crd::shader::VariantHandle variant{};
};

struct DrawItem
{
    crd::math::Mat4f model = crd::math::Mat4f::identity();
    crd::math::Mat4f view_projection = crd::math::Mat4f::identity();
    crd::rhi::Buffer* vertex_buffer = nullptr;
    crd::u32 vertex_count = 0;
    crd::u64 material_instance_id = 0;
    crd::shader::VariantHandle variant{};
    crd::shader::VariantPipelineDesc handoff{};
};

struct FramePlan
{
    crd::containers::Array<DrawItem> draw_items{};
};

class Renderer
{
public:
    void submit(const Renderable& renderable);
    void clear() noexcept;

    [[nodiscard]] bool build_frame(const Camera& camera, const crd::shader::Runtime& shader_runtime,
                                   FramePlan& out) const;

    [[nodiscard]] crd::usize renderable_count() const noexcept { return m_renderables.size(); }

private:
    crd::containers::Array<Renderable> m_renderables{};
};
} // namespace crd::renderer
