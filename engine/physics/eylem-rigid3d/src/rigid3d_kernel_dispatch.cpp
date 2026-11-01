// The eylem-rigid3d kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/eylem_rigid3d/rigid3d_dispatch.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace crd::eylem_rigid3d
{
namespace
{

using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;

// CRD_KERNELS_RIGID3D: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_RIGID3D & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, PORTABLE), "the portable level is always built");

constexpr KernelLevel<Rigid3dKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kRigid3dKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, SSE2)
    {crd::CpuLevel::Sse2, &kRigid3dKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, AVX2)
    {crd::CpuLevel::Avx2, &kRigid3dKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, AVX512)
    {crd::CpuLevel::Avx512, &kRigid3dKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// Three chunks at dt = 1/60, so avx512 runs a sixteen-lane step and an eight-lane tail and the other levels three
// eight-lane steps. Lanes 3, 9 and 21 do not integrate (free, a NaN and a negative inv_mass) and lane 6 has inv_mass
// 0; lane 12 holds a zero quaternion and lane 15 one of length 1e-6, which try_normalize keeps; lane 18 has signed
// zero velocities and lane 23 is damped past 1/dt. The answers are the integrator's operations in the kernel's order
// evaluated at compile time, each rounded once as every level rounds it (the square root by an integer root), so they
// are every level's bits.
constexpr usize kCheckChunks = 3U;
constexpr usize kCheckLanes = kCheckChunks * kChunkLanes;
constexpr usize kFloatValues = 26U * kChunkLanes; // the float columns, contiguous before the side-bands
static_assert(offsetof(BodyChunk, flags) == kFloatValues * sizeof(f32), "the float columns are contiguous");
constexpr f32 kCheckDt = 1.0F / 60.0F;
constexpr f32 kCheckGravity[3] = {0.5F, -9.81F, -0.0F};
constexpr f32 kNormalizeEpsilon = 1.0e-5F; // try_normalize's f32 default, as the kernel's

struct CheckBodies
{
    BodyChunk chunks[kCheckChunks];
};

// sqrt(s) rounded to nearest, at compile time: the integer root of s's significand scaled to 61 bits, rounded to 24.
constexpr f32 rounded_sqrt(f32 s) noexcept
{
    if (!(s > 0.0F) || s == std::numeric_limits<f32>::infinity())
    {
        return s == 0.0F || s > 0.0F ? s : std::numeric_limits<f32>::quiet_NaN();
    }
    const u32 bits = std::bit_cast<u32>(s);
    u64 m = bits & 0x7FFFFFU;
    int e = static_cast<int>(bits >> 23U);
    if (e == 0)
    {
        e = 1;
    }
    else
    {
        m |= 0x800000U;
    }
    e -= 150; // s = m 2^e
    while (m < 0x800000U)
    {
        m <<= 2U;
        e -= 2;
    }
    if (e % 2 != 0)
    {
        m <<= 1U;
        --e;
    }
    u64 n = m << 36U; // below 2^61; sqrt(s) = sqrt(n) 2^((e - 36) / 2)
    u64 root = 0U;
    for (u64 bit = u64{1} << 62U; bit != 0U; bit >>= 2U)
    {
        if (n >= root + bit)
        {
            n -= root + bit;
            root = (root >> 1U) + bit;
        }
        else
        {
            root >>= 1U;
        }
    }
    int shift = 0;
    while ((root >> shift) >= (u64{1} << 24U))
    {
        ++shift;
    }
    u64 q = root >> shift;
    const u64 rem = root & ((u64{1} << shift) - 1U);
    const u64 half = u64{1} << (shift - 1);
    if (rem > half || (rem == half && (n != 0U || (q & 1U) != 0U)))
    {
        ++q;
    }
    if (q == (u64{1} << 24U))
    {
        q >>= 1U;
        ++shift;
    }
    const int exponent = (e - 36) / 2 + shift + 23 + 127;
    return std::bit_cast<f32>((static_cast<u32>(exponent) << 23U) | static_cast<u32>(q & 0x7FFFFFU));
}

constexpr CheckBodies make_check_input() noexcept
{
    CheckBodies in{};
    for (usize i = 0; i < kCheckLanes; ++i)
    {
        BodyChunk& b = in.chunks[i / kChunkLanes];
        const usize l = i % kChunkLanes;
        const f32 x = static_cast<f32>(i);
        b.pos_x[l] = 1.25F * x - 9.0F;
        b.pos_y[l] = 0.1F * x + 2.0F;
        b.pos_z[l] = -0.3F * x;
        b.rot_x[l] = 0.05F * x + 0.1F;
        b.rot_y[l] = -0.2F;
        b.rot_z[l] = 0.3F - 0.01F * x;
        b.rot_w[l] = 0.9F;
        b.linvel_x[l] = 1.0F - 0.2F * x;
        b.linvel_y[l] = 3.5F;
        b.linvel_z[l] = 0.125F * x - 0.75F;
        b.angvel_x[l] = 0.5F * x - 4.0F;
        b.angvel_y[l] = 2.25F;
        b.angvel_z[l] = -1.0F - 0.1F * x;
        b.inv_mass[l] = 1.0F;
        b.linear_damping[l] = 0.05F * static_cast<f32>(i % 4U);
        b.angular_damping[l] = 0.02F * static_cast<f32>(i % 5U);
        b.live[l] = 1U;
    }
    in.chunks[0].live[3] = 0U;
    in.chunks[0].inv_mass[6] = 0.0F;
    in.chunks[1].inv_mass[1] = std::numeric_limits<f32>::quiet_NaN();
    in.chunks[2].inv_mass[5] = -1.0F;
    BodyChunk& c1 = in.chunks[1];
    c1.rot_x[4] = 0.0F;
    c1.rot_y[4] = -0.0F;
    c1.rot_z[4] = 0.0F;
    c1.rot_w[4] = 0.0F;
    c1.rot_x[7] = 1.0e-6F;
    c1.rot_y[7] = 0.0F;
    c1.rot_z[7] = 0.0F;
    c1.rot_w[7] = 0.0F;
    BodyChunk& c2 = in.chunks[2];
    c2.linvel_x[2] = -0.0F;
    c2.linvel_z[2] = 0.0F;
    c2.angvel_x[2] = -0.0F;
    c2.angvel_y[2] = 0.0F;
    c2.angvel_z[2] = -0.0F;
    c2.linear_damping[7] = 100.0F;
    return in;
}

constexpr CheckBodies make_check_answers() noexcept
{
    CheckBodies out = make_check_input();
    const f32 dt = kCheckDt;
    const f32 half_dt = 0.5F * dt;
    const f32 gdt[3] = {kCheckGravity[0] * dt, kCheckGravity[1] * dt, kCheckGravity[2] * dt};
    for (usize i = 0; i < kCheckLanes; ++i)
    {
        BodyChunk& b = out.chunks[i / kChunkLanes];
        const usize l = i % kChunkLanes;
        const f32 qx = b.rot_x[l];
        const f32 qy = b.rot_y[l];
        const f32 qz = b.rot_z[l];
        const f32 qw = b.rot_w[l];
        b.prev_pos_x[l] = b.pos_x[l];
        b.prev_pos_y[l] = b.pos_y[l];
        b.prev_pos_z[l] = b.pos_z[l];
        b.prev_rot_x[l] = qx;
        b.prev_rot_y[l] = qy;
        b.prev_rot_z[l] = qz;
        b.prev_rot_w[l] = qw;
        if (b.live[l] == 0U || !(b.inv_mass[l] > 0.0F))
        {
            continue;
        }
        const f32 lin = 1.0F - b.linear_damping[l] * dt;
        b.linvel_x[l] = (b.linvel_x[l] + gdt[0]) * lin;
        b.linvel_y[l] = (b.linvel_y[l] + gdt[1]) * lin;
        b.linvel_z[l] = (b.linvel_z[l] + gdt[2]) * lin;
        b.pos_x[l] = b.pos_x[l] + b.linvel_x[l] * dt;
        b.pos_y[l] = b.pos_y[l] + b.linvel_y[l] * dt;
        b.pos_z[l] = b.pos_z[l] + b.linvel_z[l] * dt;
        const f32 ang = 1.0F - b.angular_damping[l] * dt;
        const f32 wx = b.angvel_x[l] * ang;
        const f32 wy = b.angvel_y[l] * ang;
        const f32 wz = b.angvel_z[l] * ang;
        b.angvel_x[l] = wx;
        b.angvel_y[l] = wy;
        b.angvel_z[l] = wz;
        const f32 nqx = qx + half_dt * (((0.0F * qx + wx * qw) + wy * qz) - wz * qy);
        const f32 nqy = qy + half_dt * (((0.0F * qy - wx * qz) + wy * qw) + wz * qx);
        const f32 nqz = qz + half_dt * (((0.0F * qz + wx * qy) - wy * qx) + wz * qw);
        const f32 nqw = qw + half_dt * (((0.0F * qw - wx * qx) - wy * qy) - wz * qz);
        const f32 len = rounded_sqrt(((nqx * nqx + nqy * nqy) + nqz * nqz) + nqw * nqw);
        const bool keep = len <= kNormalizeEpsilon;
        b.rot_x[l] = keep ? nqx : nqx / len;
        b.rot_y[l] = keep ? nqy : nqy / len;
        b.rot_z[l] = keep ? nqz : nqz / len;
        b.rot_w[l] = keep ? nqw : nqw / len;
    }
    return out;
}

constexpr CheckBodies kCheckInput = make_check_input();
constexpr CheckBodies kCheckAnswers = make_check_answers();

void resolve_rigid3d() noexcept
{
    (void)rigid3d_kernels();
}

constinit crd::KernelFamily g_family{"eylem.rigid3d", &resolve_rigid3d, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* rigid3d_self_check(const Rigid3dKernels& table) noexcept
{
    BodyChunk work[kCheckChunks];
    std::memcpy(static_cast<void*>(work), kCheckInput.chunks, sizeof(work));
    table.step_bodies(work, kCheckChunks, kCheckGravity, kCheckDt);
    for (usize c = 0; c < kCheckChunks; ++c)
    {
        f32 got[kFloatValues];
        f32 want[kFloatValues];
        std::memcpy(got, &work[c], sizeof(got));
        std::memcpy(want, &kCheckAnswers.chunks[c], sizeof(want));
        for (usize i = 0; i < kFloatValues; ++i)
        {
            if (std::isnan(want[i]) ? !std::isnan(got[i]) : std::bit_cast<u32>(got[i]) != std::bit_cast<u32>(want[i]))
            {
                return "step_bodies";
            }
        }
    }
    return nullptr;
}

std::span<const KernelLevel<Rigid3dKernels>> rigid3d_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<Rigid3dKernels>& rigid3d_kernels() noexcept
{
    static const KernelLevel<Rigid3dKernels>& selected =
        crd::process_checked_kernel_level<Rigid3dKernels>(kLevels, &rigid3d_self_check, g_family);
    return selected;
}

} // namespace crd::eylem_rigid3d
