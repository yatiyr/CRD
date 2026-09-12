#pragma once

// crd-eylem-viz — companion module bridging eylem to crd-draw's
// VisualizerRegistry. Phase 3.1 v1b-d (per docs/phases/phase-3.1-eylem.md).
//
// Architecture (per ADR-0066 §13 dependency-inverted plug-in pattern):
//
//   crd-eylem        — substrate POD types (no draw dep)
//   crd-eylem-rigid3d — concrete pools (no draw dep)
//   crd-draw         — RenderBuffer + VisualizerRegistry (no eylem dep)
//   crd-eylem-viz    — THIS module: registers eylem visualizers with the
//                      crd-draw registry. Pulls both eylem + draw, but
//                      keeps the substrate side free of any rendering dep.
//
// Why a companion module (not in crd-eylem itself):
//
//   - crd-eylem must remain consumable by headless / DAW / cooker /
//     scientific-computing builds that do not link crd-draw.
//   - crd-draw must remain consumable by builds that don't link eylem.
//   - The bridge that knows about BOTH lives separately and is opt-in.
//
// API surface is intentionally tiny:
//
//   register_eylem_visualizers(registry, body_pool, collider_pool)
//
// The pool references are captured into static thread_local pointers
// inside the .cpp so the registered (captureless-lambda) visualizers can
// read them at dispatch time. This is the v1b-d-tier solution; if a
// future workload needs multiple eylem worlds in the same process,
// revisit by adding a per-World viz-context handle.

namespace crd::draw
{
class VisualizerRegistry;
}

namespace crd::eylem_rigid3d
{
class BodyPool;
class ColliderPool;
}

namespace crd::eylem_viz
{
// Register eylem's built-in visualizers with the crd-draw registry.
//
//   - `RigidBodyComponent` → velocity arrow (when DebugVizComponent's
//     ShowVelocity flag is set). Position read from the entity's
//     `Transform.world` (so TransformPropagation must have run); velocity
//     read from `body_pool.read(rbc.body_id).linear_velocity`.
//
//   - `ColliderComponent` → wireframe matching the shape kind:
//        Sphere  → sphere_wire_to(center, radius)
//        Box     → box_wire_to(world * collider_local, half_extents)
//        Capsule → capsule_wire_to(a, b, radius)
//     Honoured by the DebugVizComponent's Wireframe flag (default ON for
//     v1b-d). Position composed from Transform.world * collider.local_pose.
//
//   - Joint visualizer is registered as a no-op shell — `JointComponent`
//     doesn't exist yet (joints land at v1f). The shell exists so that
//     when v1f adds the component, the visualizer appears automatically
//     for any entity carrying it.
//
// Pool references are captured as file-scope statics; only one
// (BodyPool, ColliderPool) pair can be active per process for v1b-d.
// `registry` accepts non-owning references; the caller owns lifetimes.
void register_eylem_visualizers(crd::draw::VisualizerRegistry&         registry,
                                const crd::eylem_rigid3d::BodyPool&    body_pool,
                                const crd::eylem_rigid3d::ColliderPool& collider_pool);

} // namespace crd::eylem_viz
