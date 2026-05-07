#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>

namespace crd::scene
{
namespace relations
{
struct ChildOf;
}
class World;

// TransformPropagation — Phase 3.0 v1j (ADR-0054).
//
// Walks dirty subtrees rooted at entities with TransformDirtyFlag,
// recomputes the world matrix as `parent.world * local`, and clears
// the dirty flag via Commands.
//
// Phase: PreRender. Render extract reads `transform.world`; physics
// (Phase 3.1) writes during Physics phase, propagation runs in
// PreRender, render observes the propagated state.
//
// Reads:
//   - Relation<ChildOf> (parent lookup + reverse-index DFS)
//   - Transform (translation, rotation, scale of self + parent's world)
// Writes:
//   - Transform.world
//   - TransformDirtyFlag (removed, via Commands)
//
// Determinism: ChildOf reverse-index iteration is `Array<EntityId>`
// insertion order — stable across runs given identical registration
// sequence. DFS is deterministic. Floating-point order is fixed by
// code. Single-threaded (per ADR-0054).
//
// Cross-domain notes:
//   - Robotics IK at 1 kHz: register the controller as a fixed-step
//     system in the Physics phase that writes joint Transforms; this
//     PreRender propagation runs once per frame, render gets the
//     final pose. For 1 kHz visualization (rare), the system can also
//     be marked fixed_step() = true.
//   - Aerospace orbital scales: register a TransformF64 component +
//     TransformPropagationF64 mirror system (independent of this one).
//   - DAW spatial-audio scenes: same as games, but the audio engine
//     reads `transform.world` of source entities each audio block.
class TransformPropagation : public ISystem
{
public:
    TransformPropagation() noexcept = default;

    [[nodiscard]] SchedulePhase phase() const override { return SchedulePhase::PreRender; }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"TransformPropagation"};
    }

    void run(World& world) override;
};

} // namespace crd::scene
