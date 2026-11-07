// The replicated-motion kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the
// process runs (SIMD-DISPATCH).

#include <crd/core/fnv1a.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/hesap/net/motion_dispatch.hpp>

namespace crd::hesap::net
{
namespace
{

using crd::f32;
using crd::f64;
using crd::KernelLevel;
using crd::u64;
using crd::usize;

// CRD_KERNELS_MOTION: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512) |
                                (1 << CRD_KERNEL_LEVEL_NEON);
static_assert((CRD_KERNELS_MOTION & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_MOTION, PORTABLE), "the portable level is always built");

constexpr KernelLevel<MotionKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kMotionKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_MOTION, SSE2)
    {crd::CpuLevel::Sse2, &kMotionKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_MOTION, AVX2)
    {crd::CpuLevel::Avx2, &kMotionKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_MOTION, AVX512)
    {crd::CpuLevel::Avx512, &kMotionKernelsAvx512, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_MOTION, NEON)
    {crd::CpuLevel::Neon, &kMotionKernelsNeon, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// 37 components and 37 quaternions are whole vectors and a padded tail at every level (avx512: two of 16 floats, four of
// 8 doubles; avx2: four of 8 and nine of 4; sse2 and neon: nine of 4). The answers are the FNV-1a of the bits, which
// crd-hesap-net-tests holds to restated expressions and crd-hesap-motion-peer-tests to the references.
constexpr usize kCount = 37U;

template <class T>
constexpr T value_at(usize i, usize shift) noexcept
{
    return (static_cast<T>(((i + shift) * 41U) % 97U) / static_cast<T>(8)) - static_cast<T>(6);
}

// A rotation of (6, 1, 0, 4) / sqrt(53) with signs from the index: unit within rounding, as a decoded word is.
constexpr f32 component_at(usize i, usize field, usize shift) noexcept
{
    constexpr f32 inverse_length = 0.137360563F;
    constexpr f32 turns[4] = {6.0F, 1.0F, 0.0F, 4.0F};
    const f32 value = turns[(i + field + shift) % 4U];
    const f32 sign = (((i >> field) & 1U) != 0U) ? -1.0F : 1.0F;
    return value * inverse_length * sign;
}

template <class T>
struct Components
{
    T a[kCount];
    T b[kCount];
    T c[kCount];
    T d[kCount];
    T out[kCount];

    Components() noexcept
    {
        for (usize i = 0U; i < kCount; ++i)
        {
            a[i] = value_at<T>(i, 0U);
            b[i] = value_at<T>(i, 11U);
            c[i] = value_at<T>(i, 23U);
            d[i] = value_at<T>(i, 37U);
        }
    }

    [[nodiscard]] bool gives(u64 answer) const noexcept
    {
        return crd::fnv1a_64(out, sizeof(out)) == answer;
    }
};

struct Rotations
{
    f32 a[4U * kCount];
    f32 b[4U * kCount];
    f32 out[4U * kCount];

    Rotations() noexcept
    {
        for (usize i = 0U; i < kCount; ++i)
        {
            for (usize field = 0U; field < 4U; ++field)
            {
                a[(4U * i) + field] = component_at(i, field, 0U);
                b[(4U * i) + field] = component_at(i, field, 1U + (i % 3U));
            }
        }
    }

    [[nodiscard]] bool gives(u64 answer) const noexcept
    {
        return crd::fnv1a_64(out, sizeof(out)) == answer;
    }
};

constexpr u64 kLerpF32 = 0x5DAC39D13FF59523ULL;
constexpr u64 kLerpF64 = 0x3D584CCBA9A8A22BULL;
constexpr u64 kHermiteF32 = 0xBCF42E885EED4732ULL;
constexpr u64 kHermiteF64 = 0x3A5E41974DF5B10CULL;
constexpr u64 kLinearF32 = 0x9C9082FB4B8976E0ULL;
constexpr u64 kLinearF64 = 0x5E7B7B83D1A2E140ULL;
constexpr u64 kQuadraticF32 = 0x1074B8C27F2A4CCFULL;
constexpr u64 kQuadraticF64 = 0x1BFDE512441BC8BCULL;
constexpr u64 kNlerp = 0x0ED59501E6AA6A45ULL;
constexpr u64 kSlerp = 0x25561E6CACA3EB9CULL;

void resolve_motion() noexcept
{
    (void)motion_kernels();
}

constinit crd::KernelFamily g_family{"hesap.motion", &resolve_motion, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* motion_self_check(const MotionKernels& table) noexcept
{
    Components<f32> single;
    Components<f64> twin;
    table.lerp_f32(single.a, single.b, 0.375F, kCount, single.out);
    if (!single.gives(kLerpF32))
    {
        return "lerp_f32";
    }
    table.lerp_f64(twin.a, twin.b, 0.375, kCount, twin.out);
    if (!twin.gives(kLerpF64))
    {
        return "lerp_f64";
    }
    table.hermite_f32({single.a, single.b, single.c, single.d, 0.625F, 0.1F}, kCount, single.out);
    if (!single.gives(kHermiteF32))
    {
        return "hermite_f32";
    }
    table.hermite_f64({twin.a, twin.b, twin.c, twin.d, 0.625, 0.1}, kCount, twin.out);
    if (!twin.gives(kHermiteF64))
    {
        return "hermite_f64";
    }
    table.extrapolate_f32(single.a, single.b, nullptr, 0.1F, kCount, single.out);
    if (!single.gives(kLinearF32))
    {
        return "extrapolate_f32";
    }
    table.extrapolate_f64(twin.a, twin.b, nullptr, 0.1, kCount, twin.out);
    if (!twin.gives(kLinearF64))
    {
        return "extrapolate_f64";
    }
    table.extrapolate_f32(single.a, single.b, single.c, 0.1F, kCount, single.out);
    if (!single.gives(kQuadraticF32))
    {
        return "extrapolate_f32";
    }
    table.extrapolate_f64(twin.a, twin.b, twin.c, 0.1, kCount, twin.out);
    if (!twin.gives(kQuadraticF64))
    {
        return "extrapolate_f64";
    }
    Rotations rotations;
    table.nlerp(rotations.a, rotations.b, 0.375F, kCount, rotations.out);
    if (!rotations.gives(kNlerp))
    {
        return "nlerp";
    }
    table.slerp(rotations.a, rotations.b, 0.375F, kCount, rotations.out);
    return rotations.gives(kSlerp) ? nullptr : "slerp";
}

std::span<const KernelLevel<MotionKernels>> motion_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<MotionKernels>& motion_kernels() noexcept
{
    static const KernelLevel<MotionKernels>& selected =
        crd::process_checked_kernel_level<MotionKernels>(kLevels, &motion_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::net
