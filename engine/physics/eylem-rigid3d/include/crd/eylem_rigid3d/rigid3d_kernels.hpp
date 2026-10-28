// The body chunk and the table of the eylem-rigid3d kernels (SIMD-DISPATCH, ADR-0134 §5): one table per CPU level the
// kernel unit is built for (engine/physics/eylem-rigid3d/kernels/rigid3d_kernels.cpp), resolved by rigid3d_kernels()
// in crd/eylem_rigid3d/rigid3d_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles
// nothing from it. The body step is bit-exact across levels (ADR-0063, ADR-0134 §7).

#pragma once

#include <crd/core/types.hpp>

namespace crd::eylem_rigid3d
{

// The AoSoA lane width of every eylem-rigid3d chunk: one data layout for every build and CPU level (ADR-0062 §4,
// ADR-0134 §5).
inline constexpr crd::usize kChunkLanes = 8;

// kChunkLanes bodies as plain f32 columns, one 32-byte line each, then the per-lane integer side-bands.
struct alignas(32) BodyChunk
{
    // Current-frame integrated state (19 columns).
    alignas(32) crd::f32 pos_x[kChunkLanes]{};
    alignas(32) crd::f32 pos_y[kChunkLanes]{};
    alignas(32) crd::f32 pos_z[kChunkLanes]{};
    alignas(32) crd::f32 rot_x[kChunkLanes]{};
    alignas(32) crd::f32 rot_y[kChunkLanes]{};
    alignas(32) crd::f32 rot_z[kChunkLanes]{};
    alignas(32) crd::f32 rot_w[kChunkLanes]{};
    alignas(32) crd::f32 linvel_x[kChunkLanes]{};
    alignas(32) crd::f32 linvel_y[kChunkLanes]{};
    alignas(32) crd::f32 linvel_z[kChunkLanes]{};
    alignas(32) crd::f32 angvel_x[kChunkLanes]{};
    alignas(32) crd::f32 angvel_y[kChunkLanes]{};
    alignas(32) crd::f32 angvel_z[kChunkLanes]{};
    alignas(32) crd::f32 inv_mass[kChunkLanes]{};
    alignas(32) crd::f32 inv_inertia_x[kChunkLanes]{};
    alignas(32) crd::f32 inv_inertia_y[kChunkLanes]{};
    alignas(32) crd::f32 inv_inertia_z[kChunkLanes]{};
    alignas(32) crd::f32 linear_damping[kChunkLanes]{};
    alignas(32) crd::f32 angular_damping[kChunkLanes]{};
    // The pose at the start of the substep, which RigidBodyInterpolationSystem lerps from (7 columns).
    alignas(32) crd::f32 prev_pos_x[kChunkLanes]{};
    alignas(32) crd::f32 prev_pos_y[kChunkLanes]{};
    alignas(32) crd::f32 prev_pos_z[kChunkLanes]{};
    alignas(32) crd::f32 prev_rot_x[kChunkLanes]{};
    alignas(32) crd::f32 prev_rot_y[kChunkLanes]{};
    alignas(32) crd::f32 prev_rot_z[kChunkLanes]{};
    alignas(32) crd::f32 prev_rot_w[kChunkLanes]{};
    crd::u32 flags[kChunkLanes]{};     // RigidBodyFlags bits
    crd::u8 generation[kChunkLanes]{}; // the BodyId generation last issued for the slot; 0 = never allocated
    crd::u8 live[kChunkLanes]{};       // 0 = free slot, 1 = allocated
};

struct Rigid3dKernels
{
    // One fixed substep over chunks[0, count): every lane's pose is copied to its prev columns, then each live lane
    // with inv_mass > 0 integrates (v += g dt; v *= 1 - linear_damping dt; p += v dt; w *= 1 - angular_damping dt;
    // q += (dt / 2) (w, 0) q, normalized unless its length is at most 1e-5), every product and sum rounded on its own
    // in the order crd::math's Vec3 and Quat operations use.
    void (*step_bodies)(BodyChunk* chunks, crd::usize count, const crd::f32 gravity[3], crd::f32 dt) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const Rigid3dKernels kRigid3dKernelsPortable;
extern const Rigid3dKernels kRigid3dKernelsSse2;
extern const Rigid3dKernels kRigid3dKernelsAvx2;
extern const Rigid3dKernels kRigid3dKernelsAvx512;

} // namespace crd::eylem_rigid3d
