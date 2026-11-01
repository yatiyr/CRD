// The batch small-float conversion kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the
// level the process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/math/float_convert.hpp>

#include <cstring>

namespace crd::math
{
namespace
{

namespace sf = small_float;

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// 45 elements: two 16-lane steps and a 13-element tail (five 8-lane steps and a 5-element tail, eleven 4-lane steps
// and a 1-element tail), so every level's vector loop and its tail run.
constexpr crd::usize kCheckCount = 45U;

// The narrowing inputs: the conversion tests' special patterns (every format's zeros, subnormals, rounding ties and
// their neighbours, overflow, the deep underflow stochastic rounding stages through 64 bits, infinities and NaN
// payloads), then pi, -123.456 and 0.1.
constexpr crd::u32 kCheckBits[kCheckCount] = {
    0x00000000U, 0x80000000U, 0x00000001U, 0x807FFFFFU, 0x00800000U, 0x3F800000U, 0xBF800000U, 0x3F801000U, 0x3F802000U,
    0x3F803000U, 0x477FE000U, 0x477FF000U, 0x47800000U, 0xC77FF000U, 0x33800000U, 0x33000000U, 0x33000001U, 0x2F800000U,
    0x1F800000U, 0x0F800000U, 0x43E00000U, 0x43F00000U, 0x43E80000U, 0x47600000U, 0x47700000U, 0x3B000000U, 0x3A800000U,
    0x37800000U, 0x7F7FFFFFU, 0xFF7FFFFFU, 0x7F800000U, 0xFF800000U, 0x7FC00000U, 0xFFC00000U, 0x7F800001U, 0x7FA5A5A5U,
    0xFFBFFFFFU, 0x7FFFFFFFU, 0x3F807FFFU, 0x3F808000U, 0x3F818000U, 0x3F7FFFFFU, 0x40490FDBU, 0xC2F6E979U, 0x3DCCCCCDU,
};

struct CheckData
{
    crd::u16 halves[kCheckCount];  // the widening inputs
    crd::u32 rand[kCheckCount];    // the stochastic draws
    crd::u32 widened[kCheckCount]; // the answers, from the scalar converts at compile time
    crd::u16 f16[kCheckCount];
    crd::u16 bf16[kCheckCount];
    crd::u8 e4m3[kCheckCount];
    crd::u8 e5m2[kCheckCount];
    crd::u16 f16_sr[kCheckCount];
    crd::u16 bf16_sr[kCheckCount];
    crd::u8 e4m3_sr[kCheckCount];
    crd::u8 e5m2_sr[kCheckCount];
};

// The f16 classes first (zeros, subnormals, the smallest and largest normals, one, infinities, quiet and signalling
// NaNs), then a Weyl sequence over the 16-bit patterns; the draws are the top halves of a 64-bit Weyl sequence.
constexpr crd::u16 kCheckHalfClasses[] = {0x0000U, 0x8000U, 0x0001U, 0x03FFU, 0x8200U, 0x0400U, 0x7BFFU, 0x3C00U,
                                          0xC000U, 0x7C00U, 0xFC00U, 0x7E00U, 0xFE01U, 0x7C01U, 0x7FFFU};

constexpr CheckData make_check_data() noexcept
{
    CheckData d{};
    constexpr crd::usize classes = sizeof(kCheckHalfClasses) / sizeof(kCheckHalfClasses[0]);
    for (crd::usize i = 0U; i < kCheckCount; ++i)
    {
        d.halves[i] = i < classes ? kCheckHalfClasses[i] : static_cast<crd::u16>(0x9E37U * (i + 1U));
        d.rand[i] = static_cast<crd::u32>((0x9E3779B97F4A7C15ULL * (i + 1U)) >> 32U);
        const crd::f32 x = sf::bits_as<crd::f32>(kCheckBits[i]);
        d.widened[i] = sf::bits_as<crd::u32>(f16_bits_to_f32(d.halves[i]));
        d.f16[i] = f32_to_f16_bits(x);
        d.bf16[i] = f32_to_bf16_bits(x);
        d.e4m3[i] = f32_to_fp8_e4m3_bits(x);
        d.e5m2[i] = f32_to_fp8_e5m2_bits(x);
        d.f16_sr[i] = f32_to_f16_bits_sr(x, d.rand[i]);
        d.bf16_sr[i] = f32_to_bf16_bits_sr(x, d.rand[i]);
        d.e4m3_sr[i] = f32_to_fp8_e4m3_bits_sr(x, d.rand[i]);
        d.e5m2_sr[i] = f32_to_fp8_e5m2_bits_sr(x, d.rand[i]);
    }
    return d;
}

constexpr CheckData kCheck = make_check_data();

template <typename T> bool same(const T* got, const T* want) noexcept
{
    for (crd::usize i = 0U; i < kCheckCount; ++i)
    {
        if (got[i] != want[i])
        {
            return false;
        }
    }
    return true;
}

void resolve_float_convert() noexcept
{
    (void)float_convert_kernels();
}

constinit crd::KernelFamily g_family{"math.float-convert", &resolve_float_convert, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

// CRD_KERNELS_FLOAT_CONVERT: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_SSE42) | (1 << CRD_KERNEL_LEVEL_AVX2) |
                                (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_FLOAT_CONVERT & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, PORTABLE), "the portable level is always built");

constexpr KernelLevel<FloatConvertKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kFloatConvertKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, SSE2)
    {crd::CpuLevel::Sse2, &kFloatConvertKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, SSE42)
    {crd::CpuLevel::Sse42, &kFloatConvertKernelsSse42, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, AVX2)
    {crd::CpuLevel::Avx2, &kFloatConvertKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, AVX512)
    {crd::CpuLevel::Avx512, &kFloatConvertKernelsAvx512, {}},
#endif
};

} // namespace

const char* float_convert_self_check(const FloatConvertKernels& table) noexcept
{
    // The f32 inputs and outputs move as bytes, so a signalling NaN reaches the kernel and its answer unchanged.
    crd::f32 src[kCheckCount];
    crd::f32 wide[kCheckCount];
    crd::u32 wide_bits[kCheckCount];
    crd::u16 h[kCheckCount]; // the 16-bit outputs
    crd::u8 b[kCheckCount];  // the 8-bit outputs
    std::memcpy(src, kCheckBits, sizeof src);
    table.f16_to_f32(kCheck.halves, wide, kCheckCount);
    std::memcpy(wide_bits, wide, sizeof wide_bits);
    if (!same(wide_bits, kCheck.widened))
    {
        return "f16_to_f32";
    }
    table.f32_to_f16(src, h, kCheckCount);
    if (!same(h, kCheck.f16))
    {
        return "f32_to_f16";
    }
    table.f32_to_bf16(src, h, kCheckCount);
    if (!same(h, kCheck.bf16))
    {
        return "f32_to_bf16";
    }
    table.f32_to_fp8_e4m3(src, b, kCheckCount);
    if (!same(b, kCheck.e4m3))
    {
        return "f32_to_fp8_e4m3";
    }
    table.f32_to_fp8_e5m2(src, b, kCheckCount);
    if (!same(b, kCheck.e5m2))
    {
        return "f32_to_fp8_e5m2";
    }
    table.f32_to_f16_sr(src, h, kCheck.rand, kCheckCount);
    if (!same(h, kCheck.f16_sr))
    {
        return "f32_to_f16_sr";
    }
    table.f32_to_bf16_sr(src, h, kCheck.rand, kCheckCount);
    if (!same(h, kCheck.bf16_sr))
    {
        return "f32_to_bf16_sr";
    }
    table.f32_to_fp8_e4m3_sr(src, b, kCheck.rand, kCheckCount);
    if (!same(b, kCheck.e4m3_sr))
    {
        return "f32_to_fp8_e4m3_sr";
    }
    table.f32_to_fp8_e5m2_sr(src, b, kCheck.rand, kCheckCount);
    if (!same(b, kCheck.e5m2_sr))
    {
        return "f32_to_fp8_e5m2_sr";
    }
    return nullptr;
}

std::span<const KernelLevel<FloatConvertKernels>> float_convert_levels() noexcept
{
    return kLevels;
}

const KernelLevel<FloatConvertKernels>& float_convert_kernels() noexcept
{
    static const KernelLevel<FloatConvertKernels>& selected =
        process_checked_kernel_level<FloatConvertKernels>(kLevels, &float_convert_self_check, g_family);
    return selected;
}

} // namespace crd::math
