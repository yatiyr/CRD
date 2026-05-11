#pragma once

// PhysicsConfig — scene-level tunables. Captured at scene construction;
// mutating after step() is not supported (some fields drive contact-cache
// hashing + island bookkeeping invariants).

#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::eylem
{
struct PhysicsConfig
{
    // World gravity, m/s^2. Default is Earth Y-up.
    crd::math::Vec3f gravity{0.0F, -9.81F, 0.0F};

    // Fixed timestep (seconds). The driver passes integer multiples of this
    // to step(); a sub-frame accumulator handles the remainder. 1/60 = 60 Hz.
    crd::f32 fixed_dt = 1.0F / 60.0F;

    // Sequential Impulses iteration counts. Higher = more constraint
    // accuracy, lower throughput. Defaults from Catto GDC 2005.
    crd::u32 velocity_iterations = 8;
    crd::u32 position_iterations = 3;

    // Capacity hints for storage pre-sizing. Ignored by null impl; v1b
    // uses them for AoSoA chunk pre-allocation.
    crd::u32 max_bodies              = 65536;
    crd::u32 max_contacts_per_pair   = 4;

    // Contact cache parameters.
    crd::f32 contact_offset             = 0.02F; // separation distance for pre-contact
    crd::f32 contact_breaking_threshold = 0.02F; // distance at which a cached contact is dropped

    // Sleep heuristics. A body whose linear and angular speeds stay under
    // the thresholds for sleep_time_threshold seconds is put to sleep.
    crd::f32 sleep_linear_threshold  = 0.01F;
    crd::f32 sleep_angular_threshold = 0.01F;
    crd::f32 sleep_time_threshold    = 0.5F;

    // Determinism contract (ADR-0063).
    DeterminismMode determinism = DeterminismMode::CrossPlatform;

    // Solver tunables.
    bool warm_starting_enabled = true;
    bool ccd_enabled           = false; // global default; per-body override via flag

    // ---------------------------------------------------------------------------
    // Allocator strategy (Phase 3.1 v1b-a; doc: docs/systems/eylem-allocators.md)
    //
    // The eylem scene draws from a tiered allocator model:
    //
    //   1. `persistent_alloc` — scene-lifetime. Hosts body / collider / joint
    //      pools, the contact cache, persistent islands, and variable-size
    //      geometry (hull vertices, mesh data). Expected to be a
    //      `crd::memory::TlsfAllocator` (O(1) alloc/free, bounded
    //      fragmentation; the canonical real-time choice). May be nullptr,
    //      in which case the scene falls back to `default_allocator()`.
    //
    //   2. `solver_scratch` — per-step bump arena. Reset every `step()` call.
    //      Hosts the broadphase pair list, the contact manifold staging
    //      area, and per-island solver scratch buffers. Expected to be a
    //      `crd::memory::LinearAllocator`. May be nullptr → fallback to
    //      `default_allocator()`. The scene NEVER `deallocate()`s through
    //      this; it `reset()`s the arena at end of step.
    //
    //   3. Per-fiber broadphase / narrow-phase scratch uses the existing
    //      `crd::jobs::frame_alloc()` (frame-lifetime, free at frame end).
    //      NOT held in PhysicsConfig because it's a process-wide singleton
    //      from `crd-jobs`.
    //
    // Pointers are non-owning. The application owns the allocators and
    // guarantees they outlive the scene. Captured at scene construction;
    // changing them after the first step() is undefined.
    crd::memory::IAllocator* persistent_alloc = nullptr;
    crd::memory::IAllocator* solver_scratch   = nullptr;
};

} // namespace crd::eylem
