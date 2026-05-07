#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::scene
{
class World;

// ComponentSet<Ts...> — Phase 3.0 v1h (ADR-0052 §3).
//
// Type-level set of component types used in ISystem::Reads / Writes
// declarations:
//
//   class TransformPropagation : public ISystem {
//   public:
//       using Reads  = ComponentSet<HierarchyNode, Relation<ChildOf>>;
//       using Writes = ComponentSet<Transform>;
//       // ...
//   };
//
// v1h ships the type machinery + a runtime helper to convert a set into
// a ComponentMask (`World::component_set_mask<Set>()` — see world.hpp).
// The masks are read by tooling (and by future auto-parallel scheduling
// in Phase 3.5+) but are NOT consumed by v1h's serial dispatcher.
template <typename... Ts> struct ComponentSet
{
};

// SchedulePhase — fixed 7-phase frame structure (ADR-0052 §4).
//
// Each phase runs to completion before the next begins. Within a phase,
// systems run in registration order. Auto-parallel scheduling within a
// phase (analyse Reads/Writes, dispatch independent systems in parallel)
// is reserved API; v1h ships serial-only.
enum class SchedulePhase : crd::u8
{
    PrePhysics    = 0, // input, AI decisions, physics-event preparation
    Physics       = 1, // physics step (Phase 3.1+)
    PostPhysics   = 2, // physics result reactions, contact-driven gameplay
    Update        = 3, // general gameplay logic
    PreRender     = 4, // transform propagation, animation pose, culling, GPU upload prep
    RenderExtract = 5, // build draw lists from current world state
    PostRender    = 6, // editor diagnostics, debug overlay, replay capture
};

inline constexpr crd::u8 kSchedulePhaseCount = 7;

// ISystem — the unit of frame-loop work. Phase 3.0 v1h (ADR-0052 §3).
//
// Subclass to declare a system. Override:
//   - phase() to pick a SchedulePhase.
//   - run(world, fence) to do the work. Submit jobs to `fence` for
//     parallel work; the schedule waits on it after run() returns and
//     before draining commands at the phase boundary.
//   - name() for diagnostics / profiler labels.
//   - fixed_step() (optional) to opt into fixed-rate iteration. Default
//     false = variable-rate (run once per step()/step_fixed() call).
//
// Reads / Writes type aliases declare the components the system touches.
// v1h does NOT use these for parallel scheduling — the masks are
// computed and exposed via World::component_set_mask<Set>() for tooling
// and future use.
class ISystem
{
public:
    using Reads  = ComponentSet<>;
    using Writes = ComponentSet<>;

    virtual ~ISystem() = default;

    [[nodiscard]] virtual SchedulePhase phase() const = 0;

    // Real work. v1h schedules systems serially within each phase — the
    // next system starts after this returns. Systems that submit jobs via
    // crd::jobs do their own wait before returning.
    //
    // ADR-0052 §3 specifies a `Counter& fence` parameter for caller-
    // managed parallel work; the current jobs API allocates Counters
    // internally rather than accepting a caller-provided one, so v1h
    // omits the fence parameter. v1h+1 (parallel par_each over Query
    // chunks) reintroduces it once the jobs API exposes a caller-managed
    // counter handle.
    virtual void run(World& world) = 0;

    [[nodiscard]] virtual crd::containers::StringView name() const = 0;

    // Fixed-step opt-in. Default false (variable-rate).
    //
    // When true:
    //   - step(dt) treats the system as variable-rate (runs once with dt).
    //   - step_fixed(dt, fixed_dt, max_substeps) runs the system N times
    //     where N = floor(accumulator / fixed_dt), clamped to max_substeps.
    //
    // v1h uses a SINGLE GLOBAL fixed_dt (the step_fixed argument). Per-
    // system fixed_dt is reserved for v1h+1 along with per-system
    // accumulators (Phase 3.1 physics will need it).
    [[nodiscard]] virtual bool fixed_step() const noexcept { return false; }
};

} // namespace crd::scene
