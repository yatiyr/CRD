// The replicated-motion kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the
// process runs (SIMD-DISPATCH).

#include <crd/core/fnv1a.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/hesap/net/motion_dispatch.hpp>

#include <cstring>

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
constexpr u64 kDeadReckonBody = 0xE9923DF31DE4A410ULL;
constexpr u64 kDeadReckonWorld = 0x4FBC0172DF18BBA6ULL;
constexpr u64 kBlendF32 = 0xC7B38C6837753F78ULL;
constexpr u64 kBlendF64 = 0x6195A1EF2F32D299ULL;

// 37 entities whose turns fall on both sides of the series' end, on the same values as the batches above.
struct Reckoning
{
    f64 columns[17][kCount];
    f64 out[10][kCount];

    Reckoning() noexcept
    {
        for (usize i = 0U; i < kCount; ++i)
        {
            for (usize k = 0U; k < 12U; ++k)
            {
                columns[k][i] = value_at<f64>(i, 5U * k);
            }
            for (usize field = 0U; field < 4U; ++field)
            {
                columns[12U + field][i] = static_cast<f64>(component_at(i, field, 0U));
            }
            columns[16][i] = value_at<f64>(i, 3U) * 0.125;
        }
    }

    [[nodiscard]] bool reckons_to(const MotionKernels& table, DeadReckoningAlgorithm algorithm, u64 answer) noexcept
    {
        const DeadReckoningInput in{{columns[0], columns[1], columns[2]},
                                    {columns[3], columns[4], columns[5]},
                                    {columns[6], columns[7], columns[8]},
                                    {columns[9], columns[10], columns[11]},
                                    {columns[12], columns[13], columns[14], columns[15]},
                                    columns[16]};
        const DeadReckoningOutput into{
            {out[0], out[1], out[2]}, {out[3], out[4], out[5]}, {out[6], out[7], out[8], out[9]}};
        std::memset(out, 0, sizeof(out));
        table.dead_reckon(algorithm, in, kCount, into);
        return crd::fnv1a_64(out, sizeof(out)) == answer;
    }
};

template <class T>
[[nodiscard]] u64 blend_digest(const Components<T>& inputs,
                               void (*blend)(const ProjectiveBlendSpan<T>&, usize, T*, T*) noexcept) noexcept
{
    T elapsed[kCount];
    T span[kCount];
    T out[2][kCount];
    for (usize i = 0U; i < kCount; ++i)
    {
        elapsed[i] = static_cast<T>(i % 11U) * static_cast<T>(0.125);
        span[i] = static_cast<T>(i % 4U) * static_cast<T>(0.5);
    }
    blend(ProjectiveBlendSpan<T>{inputs.a, inputs.b, inputs.c, inputs.d, inputs.a, elapsed, span}, kCount, out[0],
          out[1]);
    return crd::fnv1a_64(out, sizeof(out));
}

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
    if (!rotations.gives(kSlerp))
    {
        return "slerp";
    }
    Reckoning reckoning;
    if (!reckoning.reckons_to(table, DeadReckoningAlgorithm::Rvb, kDeadReckonBody) ||
        !reckoning.reckons_to(table, DeadReckoningAlgorithm::Fvw, kDeadReckonWorld))
    {
        return "dead_reckon";
    }
    if (blend_digest<f32>(single, table.projective_blend_f32) != kBlendF32)
    {
        return "projective_blend_f32";
    }
    return blend_digest<f64>(twin, table.projective_blend_f64) == kBlendF64 ? nullptr : "projective_blend_f64";
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
