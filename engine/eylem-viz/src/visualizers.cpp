// crd-eylem-viz — visualizer implementations.
// Phase 3.1 v1b-d (per docs/phases/phase-3.1-eylem.md §v1b-d).

#include <crd/eylem_viz/eylem_viz.hpp>

#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/visualizer_registry.hpp>
#include <crd/eylem/collider.hpp>
#include <crd/eylem/components.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/collider_pool.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <algorithm>
#include <cmath>

namespace crd::eylem_viz
{
namespace
{
// File-scope pool pointers. Set by register_eylem_visualizers, read by
// the captureless visualizer functions. v1b-d-tier solution; one
// (BodyPool, ColliderPool) pair per process. If multiple eylem worlds
// per process becomes a real workload, revisit by adding per-World
// viz-context handles (out of v1b-d scope per phase plan ~150 LOC bound).
const crd::eylem_rigid3d::BodyPool*     g_body_pool     = nullptr;
const crd::eylem_rigid3d::ColliderPool* g_collider_pool = nullptr;

// ── RigidBodyComponent → velocity arrow ────────────────────────────────────

void visualize_rigid_body(const void*                       component,
                          crd::draw::RenderBuffer&          buf,
                          const crd::draw::VisualizerContext& ctx) noexcept
{
    if (g_body_pool == nullptr || ctx.world == nullptr)
    {
        return;
    }
    if (!ctx.viz->has_flag(crd::draw::DebugVizComponent::ShowVelocity))
    {
        return;
    }

    const auto* rbc = static_cast<const crd::eylem::RigidBodyComponent*>(component);
    if (rbc->body_id.is_null() || !g_body_pool->contains(rbc->body_id))
    {
        return;
    }

    // Read body state from pool — gives us linear_velocity. The arrow
    // origin is the entity's Transform.world translation (so the arrow
    // sits on the body's rendered position; TransformPropagation must
    // have run, which is guaranteed for the PostRender phase where
    // DebugVizSystem runs).
    const auto* tr = ctx.world->get_component<crd::scene::Transform>(ctx.entity);
    if (tr == nullptr)
    {
        return;
    }

    const crd::eylem::RigidBody body = g_body_pool->read(rbc->body_id);
    // v0c-1 typed: body.linear_velocity is Vec3<Velocity32>; viz speed
    // reduction is dimensionless raw — escape via .value at the boundary.
    const crd::math::Vec3f      v    = crd::math::to_raw_vec(body.linear_velocity);
    const crd::f32              speed =
        std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);

    // Below threshold: skip the arrow (would render as a tiny dot,
    // visually noisy).
    constexpr crd::f32 min_speed = 0.001F;
    if (speed < min_speed)
    {
        return;
    }

    // Origin = world translation (Transform.world's last column).
    const crd::math::Vec3f origin{tr->world.c3.x, tr->world.c3.y, tr->world.c3.z};

    // Direction = normalized velocity.
    const crd::math::Vec3f dir{v.x / speed, v.y / speed, v.z / speed};

    // Length scaled by speed and DebugVizComponent::scale, capped to
    // a sensible maximum so a body launched at 100 m/s doesn't draw a
    // 100 m arrow.
    constexpr crd::f32 max_arrow_length = 5.0F;
    const crd::f32     length = std::min(speed * ctx.viz->scale, max_arrow_length);

    const crd::draw::PrimFlags flags =
        crd::draw::PrimFlags::make(crd::draw::DepthMode::Always, ctx.category);

    crd::draw::arrow_to(buf, origin, dir, length,
                        crd::draw::kYellow,
                        /*head_size_ratio*/ 0.2F,
                        /*head_radius_ratio*/ 0.4F,
                        /*width_px*/ 2.0F, flags, /*lifetime_s*/ 0.0F);
}

// ── ColliderComponent → wireframe matching shape kind ─────────────────────

void visualize_collider(const void*                       component,
                        crd::draw::RenderBuffer&          buf,
                        const crd::draw::VisualizerContext& ctx) noexcept
{
    if (g_collider_pool == nullptr || ctx.world == nullptr)
    {
        return;
    }
    if (!ctx.viz->has_flag(crd::draw::DebugVizComponent::Wireframe))
    {
        return;
    }

    const auto* cc = static_cast<const crd::eylem::ColliderComponent*>(component);
    if (cc->collider_id.is_null() || !g_collider_pool->contains(cc->collider_id))
    {
        return;
    }

    const auto* tr = ctx.world->get_component<crd::scene::Transform>(ctx.entity);
    if (tr == nullptr)
    {
        return;
    }

    const crd::eylem::Collider collider = g_collider_pool->read(cc->collider_id);

    // Body-frame origin = Transform.world translation. Collider's local
    // pose composes on top of that. v1b-d ignores rotation composition
    // for the wireframe placement (sphere is rotation-invariant; box +
    // capsule visualizers ignore Transform/collider rotation in this
    // first pass — proper world*local matrix composition lands at
    // v1f when the eylem-aero / eylem-cine consumers actually need it).
    const crd::math::Vec3f body_origin{tr->world.c3.x, tr->world.c3.y, tr->world.c3.z};
    const crd::math::Vec3f world_pos{
        body_origin.x + collider.local_position.x,
        body_origin.y + collider.local_position.y,
        body_origin.z + collider.local_position.z};

    const crd::draw::PrimFlags flags =
        crd::draw::PrimFlags::make(crd::draw::DepthMode::Always, ctx.category);

    switch (collider.shape)
    {
    case crd::eylem::ColliderShape::Sphere:
        crd::draw::sphere_wire_to(buf, world_pos, collider.sphere.radius,
                                  crd::draw::kCyan,
                                  /*segments_long*/ 16U,
                                  /*segments_lat*/ 8U,
                                  /*width_px*/ 1.0F, flags, 0.0F);
        break;

    case crd::eylem::ColliderShape::Box:
    {
        // box_wire_to takes a Mat4f world; build a translation-only
        // matrix from world_pos via from_trs with identity rot + unit
        // scale. Rotation-aware composition lands at v1f.
        const crd::math::Mat4f xform = crd::math::from_trs(
            world_pos,
            crd::math::Quatf::identity(),
            crd::math::Vec3f{1.0F, 1.0F, 1.0F});
        crd::draw::box_wire_to(buf, xform, collider.box.half_extents,
                               crd::draw::kCyan,
                               /*width_px*/ 1.0F, flags, 0.0F);
        break;
    }

    case crd::eylem::ColliderShape::Capsule:
    {
        // Capsule axis = local Y by convention (per ColliderCapsule).
        // Endpoints are body-origin ± half_height along Y.
        const crd::f32         h = collider.capsule.half_height;
        const crd::math::Vec3f a{world_pos.x, world_pos.y - h, world_pos.z};
        const crd::math::Vec3f b{world_pos.x, world_pos.y + h, world_pos.z};
        crd::draw::capsule_wire_to(buf, a, b, collider.capsule.radius,
                                   crd::draw::kCyan,
                                   /*segments*/ 16U,
                                   /*width_px*/ 1.0F, flags, 0.0F);
        break;
    }

    // ConvexHull / Plane / TriangleMesh / Heightfield / Sdf — wireframe
    // representations land alongside their narrow-phase impls (v1d and
    // v1d-mesh / v1d-hf / Phase 3.1.5 sdf consumer). For v1b-d these
    // shape kinds simply produce no viz; not an error, just unsupported.
    case crd::eylem::ColliderShape::ConvexHull:
    case crd::eylem::ColliderShape::Plane:
    case crd::eylem::ColliderShape::TriangleMesh:
    case crd::eylem::ColliderShape::Heightfield:
    case crd::eylem::ColliderShape::Sdf:
    default:
        break;
    }
}

} // namespace

void register_eylem_visualizers(crd::draw::VisualizerRegistry&            registry,
                                const crd::eylem_rigid3d::BodyPool&       body_pool,
                                const crd::eylem_rigid3d::ColliderPool&   collider_pool)
{
    g_body_pool     = &body_pool;
    g_collider_pool = &collider_pool;

    registry.register_for<crd::eylem::RigidBodyComponent>(
        visualize_rigid_body, crd::draw::Category::Physics);
    registry.register_for<crd::eylem::ColliderComponent>(
        visualize_collider, crd::draw::Category::Physics);

    // Joint visualizer no-op shell. JointComponent does not exist yet
    // (joints land at v1f). Once `crd::eylem::JointComponent` ships, add
    // a `registry.register_for<JointComponent>(visualize_joint, ...)` here
    // — the shell exists in this comment so the v1f author knows where
    // to land it without re-discovering the eylem-viz module.
}

} // namespace crd::eylem_viz
