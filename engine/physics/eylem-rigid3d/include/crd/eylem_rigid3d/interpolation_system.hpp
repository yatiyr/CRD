#pragma once

// RigidBodyInterpolationSystem — variable-rate render-side interpolator
// that bridges fixed-step physics output to smooth visuals. Phase 3.1
// v1b-e (per docs/phases/phase-3.1-eylem.md §v1b-e).
//
// Why this system exists
// ──────────────────────
// EylemSystem runs at a FIXED cadence (e.g. 60 Hz, fixed_dt = 1/60 s)
// inside Physics phase via World::step_fixed. The render frame rate is
// VARIABLE (vsync, 144 Hz, whatever the GPU produces). Without a
// bridge, two visual artefacts emerge:
//
//   1. Stutter. When (frame_dt / fixed_dt) is non-integer, some render
//      frames see 0 substeps run and others see 2 — producing 16/33 ms
//      visual gaps that look jerky even at high framerate.
//
//   2. Discrete jumps. The fixed-step integrator produces poses at
//      multiples of fixed_dt; the renderer samples Transform at frame
//      time. Without interpolation the renderer always sees the most-
//      recent integrated pose, which is at most fixed_dt stale and
//      discrete in time.
//
// The fix is the canonical Glenn Fiedler "Fix Your Timestep" §5
// (https://gafferongames.com/post/fix_your_timestep/) interpolation:
//
//     alpha = accumulator / fixed_dt   ∈ [0, 1)
//     visible_pose = lerp(prev_pose, curr_pose, alpha)
//
// `prev_pose` and `curr_pose` are the integrator's poses BEFORE and
// AFTER the most recent substep. EylemSystem snapshots curr→prev at
// the START of each substep (via BodyPool::snapshot_state_to_prev),
// then integrates and writes new curr. This system reads both columns
// and lerps — World::fixed_step_alpha(fixed_dt) provides alpha.
//
// Architecture decisions locked at v1b-e:
//
//   1. Lives in `crd-eylem-rigid3d` (concrete-pool module). Reads
//      BodyPool's prev/curr columns directly. The `RigidBodyComponent`
//      it iterates stays in `crd-eylem` so non-rigid3d backends (e.g.
//      a future `crd-eylem-rigid2d` or articulated solver) can re-use
//      the component identity.
//
//   2. Phase = `SchedulePhase::PreRender`. fixed_step() = false. Runs
//      EVERY frame regardless of substep count. Must run BEFORE
//      `TransformPropagation` (also PreRender) so the propagation pass
//      sees the lerped local pose. Achieve this by registering the
//      interpolation system FIRST in the PreRender phase.
//
//   3. Edge cases:
//      - alpha = 0  → output = prev (we ran the substep but no
//                                     accumulator left; renderer
//                                     should see the previous pose
//                                     until accumulator fills).
//        WAIT — this is wrong for the common case. After the integrator
//        runs N substeps and consumes (N · fixed_dt) of accumulator,
//        if frame_dt is exactly N · fixed_dt, accumulator hits 0 and
//        we should show curr (the freshly integrated pose). Re-read
//        Fiedler §5: alpha = accumulator/fixed_dt MEANS "fraction of
//        the way from the LAST integrated pose to the NEXT one we'll
//        compute". So at alpha=0 we should show the LAST integrated
//        pose = curr. At alpha→1 we'd be approaching the next pose
//        which we DON'T HAVE YET.
//        Fiedler's trick: render lags ONE FRAME behind. Display
//        lerp(prev, curr, alpha) — alpha=0 means "right after step,
//        display is at prev"; alpha=1 means "about to step, display
//        is at curr". This makes the displayed pose interpolate
//        SMOOTHLY between prev (= what we used to show) and curr
//        (= what we'll show next time alpha=0). The cost is one
//        frame of latency, which is the standard tradeoff.
//      - body_id null            → skip (no body)
//      - !pool.contains(body_id) → skip (stale handle)
//      - sync_to_transform == 0  → skip (user owns Transform)
//      - prev == curr (no substep ran this frame) → still write the
//        identical pose; no harm, no allocation.
//
//   4. Quaternion interpolation uses Nlerp (normalize(lerp(q0, q1, α))),
//      not Slerp. For substeps small enough that prev and curr are
//      close in angular space (which is the case at 60 Hz physics),
//      Nlerp is visually indistinguishable from Slerp and ~6× faster
//      (no acos/sin). Sign-flip handled: if dot(prev, curr) < 0,
//      negate curr's components in the lerp to take the short path.
//      Standard pattern from id Tech / UE4 anim.
//
//   5. Determinism is NOT a requirement here. The interpolation result
//      drives only Transform.translation/rotation, which feeds the
//      renderer. The integrator's curr columns (which propagate to the
//      next substep) are NOT touched by this system — they remain
//      bit-identical regardless of how many times this system runs.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/scene/system.hpp>

namespace crd::eylem_rigid3d
{
class BodyPool;
}

namespace crd::eylem_rigid3d
{
class RigidBodyInterpolationSystem final : public crd::scene::ISystem
{
public:
    // Constructed against the same body pool + PhysicsConfig as the
    // EylemSystem. The fixed_dt is captured at construction; if the
    // user changes PhysicsConfig::fixed_dt mid-run they must
    // re-construct this system too (matches EylemSystem's contract).
    RigidBodyInterpolationSystem(BodyPool& body_pool, const crd::eylem::PhysicsConfig& config) noexcept;

    [[nodiscard]] crd::scene::SchedulePhase phase() const override
    {
        return crd::scene::SchedulePhase::PreRender;
    }

    // Variable-rate. Runs every frame regardless of physics substep
    // count.
    [[nodiscard]] bool fixed_step() const noexcept override { return false; }

    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"RigidBodyInterpolationSystem"};
    }

    // Iterate (Transform, RigidBodyComponent), read prev+curr from the
    // body pool, lerp by world.fixed_step_alpha(fixed_dt), write the
    // result through World::set_translation / set_rotation_quat so
    // TransformPropagation refreshes Transform.world this frame.
    void run(crd::scene::World& world) override;

private:
    BodyPool*                 m_body_pool = nullptr;
    crd::eylem::PhysicsConfig m_config{};
};

} // namespace crd::eylem_rigid3d
