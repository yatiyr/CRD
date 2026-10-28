// The eylem-rigid3d kernels of crd/eylem_rigid3d/rigid3d_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS). The body step is written once over a register of lanes: one chunk of eight on the level's Vec8f,
// and at avx512 two chunks a step on Vec16f (chunk c in the low half, c + 1 in the high one) with an odd last chunk on
// Vec8f. It runs the operations of the scalar integrator in its order (crd::math's Vec3 and Quat operators and
// try_normalize), every product and sum rounded on its own and a true division, and the lanes are independent, so
// every level gives the portable bits.

#include <crd/eylem_rigid3d/rigid3d_kernels.hpp>
#include <crd/math/simd/vec8f.hpp>
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#endif

#include <type_traits>

namespace crd::eylem_rigid3d::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::usize;
using crd::math::simd::Vec8f;

// try_normalize's default epsilon for f32 (crd/math/scalar.hpp default_epsilon).
constexpr f32 kNormalizeEpsilon = 1.0e-5F;

using Column = f32 (BodyChunk::*)[kChunkLanes];

// The chunks a register of V holds.
template <typename V> constexpr usize kChunks = sizeof(V) / sizeof(Vec8f);

// A column of one step's chunks: a's eight lanes, then b's at avx512 (b is a again on a Vec8f step).
template <typename V> CRD_FORCEINLINE V load_column(const BodyChunk& a, const BodyChunk& b, Column col) noexcept
{
    if constexpr (std::is_same_v<V, Vec8f>)
    {
        (void)b;
        return Vec8f::load_aligned(a.*col);
    }
#if CRD_SIMD_HAS_AVX512
    else
    {
        return crd::math::simd::Vec16f(Vec8f::load_aligned(a.*col), Vec8f::load_aligned(b.*col));
    }
#endif
}

CRD_FORCEINLINE void store_column(Vec8f v, BodyChunk& a, BodyChunk& /*b: a again*/, Column col) noexcept
{
    v.store_aligned(a.*col);
}

#if CRD_SIMD_HAS_AVX512
CRD_FORCEINLINE void store_column(crd::math::simd::Vec16f v, BodyChunk& a, BodyChunk& b, Column col) noexcept
{
    low_half(v).store_aligned(a.*col);
    high_half(v).store_aligned(b.*col);
}
#endif

// The chunks [first, last), kChunks<V> a step.
template <typename V> void step_run(BodyChunk* chunks, usize first, usize last, const f32 gravity[3], f32 dt) noexcept
{
    const V zero = V::zero();
    const V one = V::one();
    const V vdt(dt);
    const V half_dt(0.5F * dt);
    const V eps(kNormalizeEpsilon);
    const V gdt_x(gravity[0] * dt);
    const V gdt_y(gravity[1] * dt);
    const V gdt_z(gravity[2] * dt);
    for (usize c = first; c < last; c += kChunks<V>)
    {
        BodyChunk& a = chunks[c];
        BodyChunk& b = chunks[c + kChunks<V> - 1];
        const auto load = [&a, &b](Column col) noexcept
        {
            return load_column<V>(a, b, col);
        };
        const auto store = [&a, &b](V v, Column col) noexcept
        {
            store_column(v, a, b, col);
        };
        const V px = load(&BodyChunk::pos_x);
        const V py = load(&BodyChunk::pos_y);
        const V pz = load(&BodyChunk::pos_z);
        const V qx = load(&BodyChunk::rot_x);
        const V qy = load(&BodyChunk::rot_y);
        const V qz = load(&BodyChunk::rot_z);
        const V qw = load(&BodyChunk::rot_w);

        // The render-interpolation snapshot: every lane, free ones included (no consumer reads a free lane).
        store(px, &BodyChunk::prev_pos_x);
        store(py, &BodyChunk::prev_pos_y);
        store(pz, &BodyChunk::prev_pos_z);
        store(qx, &BodyChunk::prev_rot_x);
        store(qy, &BodyChunk::prev_rot_y);
        store(qz, &BodyChunk::prev_rot_z);
        store(qw, &BodyChunk::prev_rot_w);

        // A lane integrates when it is live and inv_mass > 0 (a NaN inv_mass fails, as the scalar test does).
        alignas(64) f32 live[kChunks<V> * kChunkLanes];
        for (usize i = 0; i < kChunkLanes; ++i)
        {
            live[i] = a.live[i] != 0U ? 1.0F : 0.0F;
            if constexpr (kChunks<V> == 2)
            {
                live[kChunkLanes + i] = b.live[i] != 0U ? 1.0F : 0.0F;
            }
        }
        const V live_mask = cmp_gt(V::load_aligned(live), zero);
        const V active = cmp_gt(select(live_mask, load(&BodyChunk::inv_mass), zero), zero);

        // Linear: v += g dt, v *= 1 - linear_damping dt, p += v dt.
        const V lin_factor = one - load(&BodyChunk::linear_damping) * vdt;
        const V vx = (load(&BodyChunk::linvel_x) + gdt_x) * lin_factor;
        const V vy = (load(&BodyChunk::linvel_y) + gdt_y) * lin_factor;
        const V vz = (load(&BodyChunk::linvel_z) + gdt_z) * lin_factor;
        const V npx = px + vx * vdt;
        const V npy = py + vy * vdt;
        const V npz = pz + vz * vdt;

        // Angular: w *= 1 - angular_damping dt, q_dot = (w, 0) q left to right with its zero terms (0 * inf is
        // NaN and the products carry the signed zeros), q += (dt / 2) q_dot.
        const V ang_factor = one - load(&BodyChunk::angular_damping) * vdt;
        const V wx = load(&BodyChunk::angvel_x) * ang_factor;
        const V wy = load(&BodyChunk::angvel_y) * ang_factor;
        const V wz = load(&BodyChunk::angvel_z) * ang_factor;
        const V dx = ((zero * qx + wx * qw) + wy * qz) - wz * qy;
        const V dy = ((zero * qy - wx * qz) + wy * qw) + wz * qx;
        const V dz = ((zero * qz + wx * qy) - wy * qx) + wz * qw;
        const V dw = ((zero * qw - wx * qx) - wy * qy) - wz * qz;
        const V nqx = qx + half_dt * dx;
        const V nqy = qy + half_dt * dy;
        const V nqz = qz + half_dt * dz;
        const V nqw = qw + half_dt * dw;

        // try_normalize: a length at most epsilon keeps the quaternion; a NaN length divides. A kept lane divides by
        // one, so no lane divides by a zero (or underflowed) length the scalar body never divides by.
        const V len = sqrt(((nqx * nqx + nqy * nqy) + nqz * nqz) + nqw * nqw);
        const V keep = cmp_le(len, eps);
        const V divisor = select(keep, one, len);
        const V rx = select(keep, nqx, nqx / divisor);
        const V ry = select(keep, nqy, nqy / divisor);
        const V rz = select(keep, nqz, nqz / divisor);
        const V rw = select(keep, nqw, nqw / divisor);

        store(select(active, vx, load(&BodyChunk::linvel_x)), &BodyChunk::linvel_x);
        store(select(active, vy, load(&BodyChunk::linvel_y)), &BodyChunk::linvel_y);
        store(select(active, vz, load(&BodyChunk::linvel_z)), &BodyChunk::linvel_z);
        store(select(active, npx, px), &BodyChunk::pos_x);
        store(select(active, npy, py), &BodyChunk::pos_y);
        store(select(active, npz, pz), &BodyChunk::pos_z);
        store(select(active, wx, load(&BodyChunk::angvel_x)), &BodyChunk::angvel_x);
        store(select(active, wy, load(&BodyChunk::angvel_y)), &BodyChunk::angvel_y);
        store(select(active, wz, load(&BodyChunk::angvel_z)), &BodyChunk::angvel_z);
        store(select(active, rx, qx), &BodyChunk::rot_x);
        store(select(active, ry, qy), &BodyChunk::rot_y);
        store(select(active, rz, qz), &BodyChunk::rot_z);
        store(select(active, rw, qw), &BodyChunk::rot_w);
    }
}

void step_bodies(BodyChunk* chunks, usize count, const f32 gravity[3], f32 dt) noexcept
{
    usize c = 0;
#if CRD_SIMD_HAS_AVX512
    c = count & ~usize{1};
    step_run<crd::math::simd::Vec16f>(chunks, 0, c, gravity, dt);
#endif
    step_run<Vec8f>(chunks, c, count, gravity, dt);
}

} // namespace

} // namespace crd::eylem_rigid3d::inline CRD_MATH_LEVEL_NS

namespace crd::eylem_rigid3d
{

const Rigid3dKernels CRD_KERNEL_TABLE(kRigid3dKernels) = {
    CRD_MATH_LEVEL_NS::step_bodies,
};

} // namespace crd::eylem_rigid3d
