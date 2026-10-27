// The eylem-rigid3d kernels of crd/eylem_rigid3d/rigid3d_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS). The body step runs eight lanes a step on the level's Vec8f with the operations of the scalar
// integrator in its order (crd::math's Vec3 and Quat operators and try_normalize), every product and sum rounded on
// its own and a true division, so every level gives the portable bits.

#include <crd/eylem_rigid3d/rigid3d_kernels.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::eylem_rigid3d::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::usize;
using crd::math::simd::Vec8f;

// try_normalize's default epsilon for f32 (crd/math/scalar.hpp default_epsilon).
constexpr f32 kNormalizeEpsilon = 1.0e-5F;

void step_bodies(BodyChunk* chunks, usize count, const f32 gravity[3], f32 dt) noexcept
{
    const Vec8f zero = Vec8f::zero();
    const Vec8f one = Vec8f::one();
    const Vec8f vdt(dt);
    const Vec8f half_dt(0.5F * dt);
    const Vec8f eps(kNormalizeEpsilon);
    const Vec8f gdt_x(gravity[0] * dt);
    const Vec8f gdt_y(gravity[1] * dt);
    const Vec8f gdt_z(gravity[2] * dt);
    for (usize c = 0; c < count; ++c)
    {
        BodyChunk& b = chunks[c];
        const Vec8f px = Vec8f::load_aligned(b.pos_x);
        const Vec8f py = Vec8f::load_aligned(b.pos_y);
        const Vec8f pz = Vec8f::load_aligned(b.pos_z);
        const Vec8f qx = Vec8f::load_aligned(b.rot_x);
        const Vec8f qy = Vec8f::load_aligned(b.rot_y);
        const Vec8f qz = Vec8f::load_aligned(b.rot_z);
        const Vec8f qw = Vec8f::load_aligned(b.rot_w);

        // The render-interpolation snapshot: every lane, free ones included (no consumer reads a free lane).
        px.store_aligned(b.prev_pos_x);
        py.store_aligned(b.prev_pos_y);
        pz.store_aligned(b.prev_pos_z);
        qx.store_aligned(b.prev_rot_x);
        qy.store_aligned(b.prev_rot_y);
        qz.store_aligned(b.prev_rot_z);
        qw.store_aligned(b.prev_rot_w);

        // A lane integrates when it is live and inv_mass > 0 (a NaN inv_mass fails, as the scalar test does).
        alignas(32) f32 live[kChunkLanes];
        for (usize i = 0; i < kChunkLanes; ++i)
        {
            live[i] = b.live[i] != 0U ? 1.0F : 0.0F;
        }
        const Vec8f live_mask = cmp_gt(Vec8f::load_aligned(live), zero);
        const Vec8f active = cmp_gt(select(live_mask, Vec8f::load_aligned(b.inv_mass), zero), zero);

        // Linear: v += g dt, v *= 1 - linear_damping dt, p += v dt.
        const Vec8f lin_factor = one - Vec8f::load_aligned(b.linear_damping) * vdt;
        const Vec8f vx = (Vec8f::load_aligned(b.linvel_x) + gdt_x) * lin_factor;
        const Vec8f vy = (Vec8f::load_aligned(b.linvel_y) + gdt_y) * lin_factor;
        const Vec8f vz = (Vec8f::load_aligned(b.linvel_z) + gdt_z) * lin_factor;
        const Vec8f npx = px + vx * vdt;
        const Vec8f npy = py + vy * vdt;
        const Vec8f npz = pz + vz * vdt;

        // Angular: w *= 1 - angular_damping dt, q_dot = (w, 0) q left to right with its zero terms (0 * inf is
        // NaN and the products carry the signed zeros), q += (dt / 2) q_dot.
        const Vec8f ang_factor = one - Vec8f::load_aligned(b.angular_damping) * vdt;
        const Vec8f wx = Vec8f::load_aligned(b.angvel_x) * ang_factor;
        const Vec8f wy = Vec8f::load_aligned(b.angvel_y) * ang_factor;
        const Vec8f wz = Vec8f::load_aligned(b.angvel_z) * ang_factor;
        const Vec8f dx = ((zero * qx + wx * qw) + wy * qz) - wz * qy;
        const Vec8f dy = ((zero * qy - wx * qz) + wy * qw) + wz * qx;
        const Vec8f dz = ((zero * qz + wx * qy) - wy * qx) + wz * qw;
        const Vec8f dw = ((zero * qw - wx * qx) - wy * qy) - wz * qz;
        const Vec8f nqx = qx + half_dt * dx;
        const Vec8f nqy = qy + half_dt * dy;
        const Vec8f nqz = qz + half_dt * dz;
        const Vec8f nqw = qw + half_dt * dw;

        // try_normalize: a length at most epsilon keeps the quaternion; a NaN length divides.
        const Vec8f len = sqrt(((nqx * nqx + nqy * nqy) + nqz * nqz) + nqw * nqw);
        const Vec8f keep = cmp_le(len, eps);
        const Vec8f rx = select(keep, nqx, nqx / len);
        const Vec8f ry = select(keep, nqy, nqy / len);
        const Vec8f rz = select(keep, nqz, nqz / len);
        const Vec8f rw = select(keep, nqw, nqw / len);

        select(active, vx, Vec8f::load_aligned(b.linvel_x)).store_aligned(b.linvel_x);
        select(active, vy, Vec8f::load_aligned(b.linvel_y)).store_aligned(b.linvel_y);
        select(active, vz, Vec8f::load_aligned(b.linvel_z)).store_aligned(b.linvel_z);
        select(active, npx, px).store_aligned(b.pos_x);
        select(active, npy, py).store_aligned(b.pos_y);
        select(active, npz, pz).store_aligned(b.pos_z);
        select(active, wx, Vec8f::load_aligned(b.angvel_x)).store_aligned(b.angvel_x);
        select(active, wy, Vec8f::load_aligned(b.angvel_y)).store_aligned(b.angvel_y);
        select(active, wz, Vec8f::load_aligned(b.angvel_z)).store_aligned(b.angvel_z);
        select(active, rx, qx).store_aligned(b.rot_x);
        select(active, ry, qy).store_aligned(b.rot_y);
        select(active, rz, qz).store_aligned(b.rot_z);
        select(active, rw, qw).store_aligned(b.rot_w);
    }
}

} // namespace

} // namespace crd::eylem_rigid3d::inline CRD_MATH_LEVEL_NS

namespace crd::eylem_rigid3d
{

const Rigid3dKernels CRD_KERNEL_TABLE(kRigid3dKernels) = {
    CRD_MATH_LEVEL_NS::step_bodies,
};

} // namespace crd::eylem_rigid3d
