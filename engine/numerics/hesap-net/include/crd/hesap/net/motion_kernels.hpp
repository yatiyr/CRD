// The table of the replicated-motion kernels (HESAP-NET; SIMD-DISPATCH, ADR-0134 §5): one table per CPU level the kernel
// unit is built for (engine/numerics/hesap-net/kernels/motion_kernels.cpp), resolved by motion_kernels() in
// crd/hesap/net/motion_dispatch.hpp. Every level gives the same bits: no routine uses a fused operation, and a short
// tail runs the same body on a padded block. crd/hesap/net/motion.hpp states what the routines mean.
//
// The component routines work on flat arrays, so they take any layout and dimension. In all of them `out` may be one of
// the inputs exactly; any other overlap is the caller's error.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::net
{

// A span of `span` seconds from (p0, v0) to (p1, v1), velocities per second, sampled at `t` of it.
template <class T>
struct HermiteSpan
{
    const T* p0;
    const T* v0;
    const T* p1;
    const T* v1;
    T t;
    T span;
};

// A batch of the entities of one IEEE 1278.1 algorithm, as columns (crd/hesap/net/dead_reckoning.hpp states what they
// mean and which each algorithm reads and writes; a column it does not use may be null).
struct DeadReckoningInput
{
    const crd::f64* position[3];
    const crd::f64* velocity[3];
    const crd::f64* acceleration[3];
    const crd::f64* rate[3];
    const crd::f64* orientation[4];
    const crd::f64* dt;
};

struct DeadReckoningOutput
{
    crd::f64* position[3];
    crd::f64* velocity[3];
    crd::f64* orientation[4];
};

// The numbers of IEEE 1278.1's Dead Reckoning Algorithm field: fixed or rotating, position or velocity, world or body.
enum class DeadReckoningAlgorithm : crd::u8
{
    Static = 1U,
    Fpw = 2U,
    Rpw = 3U,
    Rvw = 4U,
    Fvw = 5U,
    Fpb = 6U,
    Rpb = 7U,
    Rvb = 8U,
    Fvb = 9U,
};

// Projective velocity blending of one column (crd/hesap/net/dead_reckoning.hpp). `acceleration` may be null.
template <class T> struct ProjectiveBlendSpan
{
    const T* from_position;
    const T* from_velocity;
    const T* position;
    const T* velocity;
    const T* acceleration;
    const T* elapsed;
    const T* span;
};

struct MotionKernels
{
    void (*lerp_f32)(const crd::f32* a, const crd::f32* b, crd::f32 t, crd::usize count, crd::f32* out) noexcept;
    void (*lerp_f64)(const crd::f64* a, const crd::f64* b, crd::f64 t, crd::usize count, crd::f64* out) noexcept;

    void (*hermite_f32)(const HermiteSpan<crd::f32>& span, crd::usize count, crd::f32* out) noexcept;
    void (*hermite_f64)(const HermiteSpan<crd::f64>& span, crd::usize count, crd::f64* out) noexcept;

    // `acceleration` may be null.
    void (*extrapolate_f32)(const crd::f32* position, const crd::f32* velocity, const crd::f32* acceleration,
                            crd::f32 dt, crd::usize count, crd::f32* out) noexcept;
    void (*extrapolate_f64)(const crd::f64* position, const crd::f64* velocity, const crd::f64* acceleration,
                            crd::f64 dt, crd::usize count, crd::f64* out) noexcept;

    // Quaternions interleaved as x y z w: `count` of them, four components each.
    void (*nlerp)(const crd::f32* a, const crd::f32* b, crd::f32 t, crd::usize count, crd::f32* out) noexcept;
    void (*slerp)(const crd::f32* a, const crd::f32* b, crd::f32 t, crd::usize count, crd::f32* out) noexcept;

    void (*dead_reckon)(DeadReckoningAlgorithm algorithm, const DeadReckoningInput& in, crd::usize count,
                        const DeadReckoningOutput& out) noexcept;

    void (*projective_blend_f32)(const ProjectiveBlendSpan<crd::f32>& span, crd::usize count, crd::f32* position,
                                 crd::f32* velocity) noexcept;
    void (*projective_blend_f64)(const ProjectiveBlendSpan<crd::f64>& span, crd::usize count, crd::f64* position,
                                 crd::f64* velocity) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const MotionKernels kMotionKernelsPortable;
extern const MotionKernels kMotionKernelsSse2;
extern const MotionKernels kMotionKernelsAvx2;
extern const MotionKernels kMotionKernelsAvx512;
extern const MotionKernels kMotionKernelsNeon;

} // namespace crd::hesap::net
