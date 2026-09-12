#pragma once

// ECS component types for the eylem physics substrate. Phase 3.1 v1b-c.
//
// These are pure POD handles — they live in `crd-eylem` (interface module)
// so any system / cooker / tooling can register and query them without
// depending on `crd-eylem-rigid3d` (the concrete pool family + integrator).
//
// Storage hint for both components is SparseSet (per ADR-0050): rigid
// bodies are typically a small fraction of the scene's entity count
// (hundreds–thousands out of potentially millions), and the lifecycle is
// dominated by sparse add/remove rather than dense iteration. Archetype
// storage would archetype-explode on every body spawn/despawn.
//
// Lifecycle:
//   - `attach_rigid_body(world, e, scene, RigidBody{...})` (in
//     `crd-eylem-rigid3d`) does pool.insert + add_component in one call;
//     this is the single canonical attachment path.
//   - `detach_rigid_body(world, e, scene)` does the inverse: pool.remove
//     + remove_component. Use this before `world.destroy(e)` for any
//     entity with a RigidBodyComponent so the pool slot is recycled.
//   - The EylemSystem (also in `crd-eylem-rigid3d`) iterates these
//     components in `SchedulePhase::Physics` and integrates motion.

#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>

namespace crd::eylem
{
// Per-entity rigid body handle. Wraps a BodyId pointing into the scene's
// BodyPool. Owns no data of its own — the body's state (position, rotation,
// velocity, mass, inertia) lives in the AoSoA-8 BodyPool columns and is
// queried via the scene's API.
//
// `sync_to_transform`: when true (the default), the EylemSystem writes
// the integrated pose back to the entity's Transform every fixed step
// via World::set_translation / set_rotation_quat (so TransformPropagation
// picks up the change in PreRender). Set to false for entities whose
// Transform is driven by something else (e.g., a kinematic body following
// an animation curve via the cinematic-bridge in v4d).
struct RigidBodyComponent
{
    BodyId   body_id           = BodyId::null();
    crd::u8  sync_to_transform = 1U;
    crd::u8  _pad[3]           = {0U, 0U, 0U};
};

static_assert(sizeof(RigidBodyComponent) == 8, "RigidBodyComponent must pack to 8 bytes");
static_assert(alignof(RigidBodyComponent) == 4);

// Per-entity collider handle. Wraps a ColliderId pointing into the
// scene's ColliderPool. v1b-c supports a single collider per entity
// (the common case — a character's capsule, a box's box, a sphere's
// sphere). Compound bodies (a body with multiple colliders) are
// expressed as multiple ECS entities sharing a `ChildOf` relation
// targeting the parent body's entity, each child carrying its own
// ColliderComponent. v1b+ relations + the ColliderPool's per-body
// linked-list field carry the wiring.
struct ColliderComponent
{
    ColliderId collider_id = ColliderId::null();
};

static_assert(sizeof(ColliderComponent) == 4, "ColliderComponent must pack to 4 bytes");
static_assert(alignof(ColliderComponent) == 4);

} // namespace crd::eylem
