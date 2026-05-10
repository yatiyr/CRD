#pragma once

// crd-draw -- DebugVizComponent (Phase 3.1 v1a-draw d3, ADR-0066 sec 11-12).
//
// Per-entity opt-in marker for the debug visualization system. Attach this
// component to any entity you want auto-visualized; `DebugVizSystem`
// (registered in `SchedulePhase::PostRender`) will dispatch every
// registered visualizer for that entity's components.
//
// Per-entity flags select which visual aspects are emitted; omitting a
// flag tells visualizers to skip the corresponding emission. Flags are a
// COOPERATIVE contract -- visualizers are encouraged to honor them, but
// the registry itself does not enforce; misbehaving visualizers ignore
// flags and emit unconditionally.
//
// `tint` and `scale` are uniform multipliers visualizers may apply to
// emitted color/size. Defaults: white tint, unit scale.

#include <crd/core/types.hpp>
#include <crd/draw/types.hpp>

namespace crd::draw
{
struct DebugVizComponent
{
    enum Flag : crd::u32  // explicit underlying type — flags grow with consumer needs
    {
        None         = 0,
        AxisTriad    = 1U << 0,  // honored by Transform visualizer (default ON)
        Wireframe    = 1U << 1,  // honored by collider/mesh visualizers
        Solid        = 1U << 2,  // honored by collider/mesh visualizers
        ShowVelocity = 1U << 3,  // rigid body velocity arrow
        ShowAabb     = 1U << 4,  // broadphase AABB box
        ShowJoints   = 1U << 5,  // joint frames + limits
        ShowContacts = 1U << 6,  // contact points + normals
        Highlight    = 1U << 7,  // override tint to attention color
    };

    // Bitfield of `Flag` values. Default = AxisTriad (cheapest reasonable
    // default for an opted-in entity; consumers OR in the bits they want).
    crd::u32 flags = AxisTriad;

    // Uniform color tint applied per visualizer's discretion. White =
    // identity (no tint). Highlight flag may override.
    Color tint = {255, 255, 255, 255};

    // Uniform size multiplier applied per visualizer's discretion. 1.0 =
    // identity. PhysX `eSCALE` analogue per ADR-0066 sec 3.
    crd::f32 scale = 1.0F;

    [[nodiscard]] constexpr bool has_flag(Flag f) const noexcept
    {
        return (flags & static_cast<crd::u32>(f)) != 0U;
    }
};

} // namespace crd::draw
