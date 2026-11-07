// Sampling replicated motion between and past snapshots (HESAP-NET, capability numerics.motion-sampling, owner
// crd-hesap-net): the dispatched batches a client's interpolation buffer and a server's lag compensation run. Nothing
// allocates.
//
//   lerp         a + (b - a) * t, per component. Not exact at t = 1, as its reference is not; a rewind sample
//                (crd/hesap/net/rewind.hpp) names an exact stamp, which needs no interpolation.
//   hermite      The cubic through two positions with two velocities, exact at both ends for finite values.
//   extrapolate  p + v * dt, or p + v * dt + a * (dt^2 / 2). Past the newest snapshot, or from a snapshot's position
//                to compose interpolation with extrapolation (the reference's mode).
//   nlerp        The normalised shortest-arc chord, its speed uneven across a wide arc. Finite for any input: when the
//                chord has no usable length it answers the identity.
//   slerp        The shortest arc at even speed, by Eberly's series in cos(angle) ("A Fast and Accurate Algorithm for
//                Computing SLERP", 2011), to the order that reaches float rounding, instead of acos and three sines:
//                lane arithmetic alone, so every level and platform gives the same bits. t is cut to [0, 1], where the
//                series holds.
//
// lerp, hermite and nlerp are bit for bit networked-physics-gdc-2015's own on vectorial's scalar backend
// (crd-hesap-motion-peer-tests); its SSE backend normalises by a reciprocal square root estimate that differs by CPU, so
// no replicated state may depend on it. The quaternions are the unit ones smallest_three_decode writes; slerp is not
// asked to mend one that is not.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/net/motion_dispatch.hpp>
#include <crd/hesap/net/motion_kernels.hpp>

namespace crd::hesap::net
{

inline void lerp(const crd::f32* a, const crd::f32* b, crd::f32 t, crd::usize count, crd::f32* out) noexcept
{
    motion_kernels().table->lerp_f32(a, b, t, count, out);
}

inline void lerp(const crd::f64* a, const crd::f64* b, crd::f64 t, crd::usize count, crd::f64* out) noexcept
{
    motion_kernels().table->lerp_f64(a, b, t, count, out);
}

inline void hermite(const HermiteSpan<crd::f32>& span, crd::usize count, crd::f32* out) noexcept
{
    motion_kernels().table->hermite_f32(span, count, out);
}

inline void hermite(const HermiteSpan<crd::f64>& span, crd::usize count, crd::f64* out) noexcept
{
    motion_kernels().table->hermite_f64(span, count, out);
}

inline void extrapolate(const crd::f32* position, const crd::f32* velocity, const crd::f32* acceleration, crd::f32 dt,
                        crd::usize count, crd::f32* out) noexcept
{
    motion_kernels().table->extrapolate_f32(position, velocity, acceleration, dt, count, out);
}

inline void extrapolate(const crd::f64* position, const crd::f64* velocity, const crd::f64* acceleration, crd::f64 dt,
                        crd::usize count, crd::f64* out) noexcept
{
    motion_kernels().table->extrapolate_f64(position, velocity, acceleration, dt, count, out);
}

inline void nlerp(const crd::f32* a, const crd::f32* b, crd::f32 t, crd::usize count, crd::f32* out) noexcept
{
    motion_kernels().table->nlerp(a, b, t, count, out);
}

inline void slerp(const crd::f32* a, const crd::f32* b, crd::f32 t, crd::usize count, crd::f32* out) noexcept
{
    motion_kernels().table->slerp(a, b, t, count, out);
}

} // namespace crd::hesap::net
