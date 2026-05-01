#include <crd/renderer/renderer.hpp>

namespace crd::renderer
{
void Renderer::submit(const Renderable& renderable)
{
    m_renderables.push_back(renderable);
}

void Renderer::clear() noexcept
{
    m_renderables.clear();
}

bool Renderer::build_frame(const Camera& camera, const crd::shader::Runtime& shader_runtime, FramePlan& out) const
{
    out.draw_items.clear();
    const auto view_projection = camera.view_projection();

    for (const auto& renderable : m_renderables)
    {
        if (renderable.vertex_buffer == nullptr || renderable.vertex_count == 0 || !renderable.variant.is_valid())
        {
            return false;
        }

        crd::shader::VariantPipelineDesc handoff;
        if (!shader_runtime.describe_variant(renderable.variant, handoff))
        {
            return false;
        }

        DrawItem item;
        item.model = crd::math::to_mat4(renderable.transform);
        item.view_projection = view_projection;
        item.vertex_buffer = renderable.vertex_buffer;
        item.vertex_count = renderable.vertex_count;
        item.material_instance_id = renderable.material_instance_id;
        item.variant = renderable.variant;
        item.handoff = std::move(handoff);
        out.draw_items.push_back(std::move(item));
    }

    return true;
}
} // namespace crd::renderer
