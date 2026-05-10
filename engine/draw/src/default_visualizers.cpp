// crd-draw -- default visualizers impl (Phase 3.1 v1a-draw d3).

#include <crd/draw/default_visualizers.hpp>

#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/visualizer_registry.hpp>
#include <crd/scene/transform.hpp>

namespace crd::draw
{
namespace
{
// Transform → axis triad at the entity's world matrix.
//
// Transform.world is populated by TransformPropagation in PreRender, so
// by the time DebugVizSystem runs in PostRender it is current for every
// entity that was marked dirty (via set_translation / set_rotation_* /
// set_scale / mark_transform_subtree_dirty). Entities created via plain
// add_component<Transform> without a subsequent dirty mark will see
// `world == identity` and the axis triad will appear at the origin --
// expected behavior; sandbox callers are responsible for marking dirty
// per the documented Transform writer contract (transform.hpp).
void visualize_transform(const void* component, RenderBuffer& buf,
                         const VisualizerContext& ctx) noexcept
{
    if (!ctx.viz->has_flag(DebugVizComponent::AxisTriad))
    {
        return;
    }
    const auto* t = static_cast<const crd::scene::Transform*>(component);

    const PrimFlags flags = PrimFlags::make(DepthMode::Always, ctx.category);

    // length = 1.0 * DebugVizComponent::scale; width 2 px; default
    // lifetime (immediate-mode -- 0 means cleared next frame by buffer.clear()).
    axis_triad_to(buf, t->world, /*length*/ 1.0F * ctx.viz->scale,
                  /*width_px*/ 2.0F, flags, /*lifetime_s*/ 0.0F);
}
} // namespace

void register_default_visualizers(VisualizerRegistry& registry)
{
    registry.register_for<crd::scene::Transform>(visualize_transform, Category::Scene);
}

} // namespace crd::draw
