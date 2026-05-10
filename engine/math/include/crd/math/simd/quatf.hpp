// Quatf — quaternion using Vec4f storage. Phase 3.1 v0a.
//
// Layout: (x, y, z, w) where w is the scalar part. Hamilton product
// convention. All ops written in lane arithmetic so the compiler keeps
// data in SIMD registers across composed operations.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4f.hpp>

namespace crd::math::simd
{
struct alignas(16) Quatf
{
    Vec4f xyzw;  // (x, y, z, w)

    Quatf() noexcept = default;

    CRD_FORCEINLINE explicit Quatf(Vec4f v) noexcept : xyzw(v) {}

    CRD_FORCEINLINE Quatf(f32 x, f32 y, f32 z, f32 w) noexcept : xyzw(x, y, z, w) {}

    [[nodiscard]] CRD_FORCEINLINE static Quatf identity() noexcept
    {
        return Quatf(0.0F, 0.0F, 0.0F, 1.0F);
    }

    [[nodiscard]] CRD_FORCEINLINE f32 x() const noexcept { return xyzw.lane(0); }
    [[nodiscard]] CRD_FORCEINLINE f32 y() const noexcept { return xyzw.lane(1); }
    [[nodiscard]] CRD_FORCEINLINE f32 z() const noexcept { return xyzw.lane(2); }
    [[nodiscard]] CRD_FORCEINLINE f32 w() const noexcept { return xyzw.lane(3); }
};

// Hamilton product: q = a * b.
//   q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y
//   q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x
//   q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
//   q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
//
// Implemented via lane extracts because the SIMD shuffle dance buys little
// here vs the clarity loss; eylem v1 will switch to the AoSoA-8 path which
// is the actual hot path.
CRD_FORCEINLINE Quatf operator*(Quatf a, Quatf b) noexcept
{
    f32 av[4]; a.xyzw.store(av);
    f32 bv[4]; b.xyzw.store(bv);
    return Quatf(av[3] * bv[0] + av[0] * bv[3] + av[1] * bv[2] - av[2] * bv[1],
                 av[3] * bv[1] - av[0] * bv[2] + av[1] * bv[3] + av[2] * bv[0],
                 av[3] * bv[2] + av[0] * bv[1] - av[1] * bv[0] + av[2] * bv[3],
                 av[3] * bv[3] - av[0] * bv[0] - av[1] * bv[1] - av[2] * bv[2]);
}

CRD_FORCEINLINE Quatf operator+(Quatf a, Quatf b) noexcept { return Quatf(a.xyzw + b.xyzw); }
CRD_FORCEINLINE Quatf operator-(Quatf a, Quatf b) noexcept { return Quatf(a.xyzw - b.xyzw); }
CRD_FORCEINLINE Quatf operator*(Quatf a, f32 s)   noexcept { return Quatf(a.xyzw * s); }
CRD_FORCEINLINE Quatf operator*(f32 s, Quatf a)   noexcept { return Quatf(s * a.xyzw); }

[[nodiscard]] CRD_FORCEINLINE Quatf conjugate(Quatf q) noexcept
{
    return Quatf(-q.x(), -q.y(), -q.z(), q.w());
}

[[nodiscard]] CRD_FORCEINLINE f32 dot(Quatf a, Quatf b) noexcept { return dot(a.xyzw, b.xyzw); }

[[nodiscard]] CRD_FORCEINLINE f32 length(Quatf q) noexcept
{
    f32 lanes[4];
    sqrt(Vec4f(dot(q, q))).store(lanes);
    return lanes[0];
}

[[nodiscard]] CRD_FORCEINLINE Quatf normalize(Quatf q) noexcept
{
    const f32 len = length(q);
    if (len == 0.0F) return Quatf::identity();
    return Quatf(q.xyzw * (1.0F / len));
}

// Rotate a 3D vector by the quaternion. Standard formula:
//   v' = q * v_pure * q_conj  (v_pure = (vx, vy, vz, 0))
[[nodiscard]] CRD_FORCEINLINE Vec4f rotate(Quatf q, f32 vx, f32 vy, f32 vz) noexcept
{
    const Quatf v_pure(vx, vy, vz, 0.0F);
    const Quatf rotated = (q * v_pure) * conjugate(q);
    return rotated.xyzw;  // w lane is ~0 (drift; caller can ignore)
}

}  // namespace crd::math::simd
