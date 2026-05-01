#include <crd/renderer/renderer.hpp>

#include <algorithm>

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

bool Renderer::build_frame(const FrameContext& ctx, const shader::Runtime& shader_runtime, DrawList& out) const
{
    out.clear();
    const auto vp = ctx.camera.view_projection();

    for (const auto& renderable : m_renderables)
    {
        if (renderable.vertex_buffer == nullptr || renderable.vertex_count == 0 || !renderable.variant.is_valid())
            return false;

        shader::VariantPipelineDesc handoff;
        if (!shader_runtime.describe_variant(renderable.variant, handoff))
            return false;

        DrawItem item;
        item.model = crd::math::to_mat4(renderable.transform);
        item.view_projection = vp;
        item.vertex_buffer = renderable.vertex_buffer;
        item.vertex_count = renderable.vertex_count;
        item.material_instance_id = renderable.material_instance_id;
        item.variant = renderable.variant;
        item.handoff = std::move(handoff);

        // Squared camera distance: no sqrt, stable sort key, no branch on zero length.
        const crd::math::Vec3f obj_pos{item.model.c3.x, item.model.c3.y, item.model.c3.z};
        const crd::math::Vec3f delta = obj_pos - ctx.camera_position;
        item.depth = crd::math::dot(delta, delta);

        switch (renderable.bucket)
        {
        case DrawBucket::Opaque:      out.opaque.push_back(std::move(item));      break;
        case DrawBucket::Masked:      out.masked.push_back(std::move(item));      break;
        case DrawBucket::Translucent: out.translucent.push_back(std::move(item)); break;
        }
    }

    // Front-to-back: opaque + masked minimise overdraw and feed early-Z correctly.
    std::sort(out.opaque.begin(), out.opaque.end(),
              [](const DrawItem& a, const DrawItem& b) { return a.depth < b.depth; });
    std::sort(out.masked.begin(), out.masked.end(),
              [](const DrawItem& a, const DrawItem& b) { return a.depth < b.depth; });

    // Back-to-front: translucent requires correct alpha compositing order.
    std::sort(out.translucent.begin(), out.translucent.end(),
              [](const DrawItem& a, const DrawItem& b) { return a.depth > b.depth; });

    return true;
}

} // namespace crd::renderer
