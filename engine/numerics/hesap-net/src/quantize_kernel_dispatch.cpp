// The quantisation kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/fnv1a.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/hesap/net/quantize.hpp>

#include <bit>

namespace crd::hesap::net
{
namespace
{

using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;

// CRD_KERNELS_QUANTIZE: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512) |
                                (1 << CRD_KERNEL_LEVEL_NEON);
static_assert((CRD_KERNELS_QUANTIZE & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_QUANTIZE, PORTABLE), "the portable level is always built");

constexpr KernelLevel<QuantizeKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kQuantizeKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_QUANTIZE, SSE2)
    {crd::CpuLevel::Sse2, &kQuantizeKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_QUANTIZE, AVX2)
    {crd::CpuLevel::Avx2, &kQuantizeKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_QUANTIZE, AVX512)
    {crd::CpuLevel::Avx512, &kQuantizeKernelsAvx512, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_QUANTIZE, NEON)
    {crd::CpuLevel::Neon, &kQuantizeKernelsNeon, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// 37 values are whole vectors and a padded tail at every level (avx512: two of 16; avx2: four of 8; sse2 and neon:
// nine of 4). The bounded answers are the scalars of quantize.hpp evaluated at compile time; the values pass both
// bounds on purpose. The quaternion answers are the FNV-1a of the bits, which crd-hesap-quantize-peer-tests holds to
// the peers through the same kernels.
constexpr usize kCount = 37U;

constexpr f32 value_at(usize i) noexcept
{
    return (static_cast<f32>((i * 41U) % 97U) / 8.0F) - 6.0F;
}

constexpr BoundedFloat make_bounded() noexcept
{
    BoundedFloat format{};
    (void)bounded_float(-4.0F, 4.5F, 0.001F, format);
    return format;
}

constexpr BoundedFloat kBounded = make_bounded();

struct BoundedAnswers
{
    u32 codes[kCount];
    f32 values[kCount];
};

constexpr BoundedAnswers make_bounded_answers() noexcept
{
    BoundedAnswers answers{};
    for (usize i = 0U; i < kCount; ++i)
    {
        answers.codes[i] = bounded_encode(kBounded, value_at(i));
        (void)bounded_decode(kBounded, answers.codes[i], answers.values[i]);
    }
    return answers;
}

constexpr BoundedAnswers kBoundedAnswers = make_bounded_answers();

bool bounded_encode_known(const QuantizeKernels& table) noexcept
{
    f32 values[kCount];
    for (usize i = 0U; i < kCount; ++i)
    {
        values[i] = value_at(i);
    }
    u32 codes[kCount];
    table.bounded_encode(kBounded, values, kCount, codes);
    u32 differ = 0U;
    for (usize i = 0U; i < kCount; ++i)
    {
        differ |= codes[i] ^ kBoundedAnswers.codes[i];
    }
    return differ == 0U;
}

bool bounded_decode_known(const QuantizeKernels& table) noexcept
{
    u32 codes[kCount];
    for (usize i = 0U; i < kCount; ++i)
    {
        codes[i] = kBoundedAnswers.codes[i];
    }
    // One code past the top and one past 2^31 are refused and decode to min.
    codes[3] = kBounded.max_code + 1U;
    codes[36] = 0x80000000U;
    f32 values[kCount];
    if (table.bounded_decode(kBounded, codes, kCount, values) != 2U)
    {
        return false;
    }
    u32 differ = 0U;
    for (usize i = 0U; i < kCount; ++i)
    {
        const f32 want = (i == 3U || i == 36U) ? kBounded.min : kBoundedAnswers.values[i];
        differ |= std::bit_cast<u32>(values[i]) ^ std::bit_cast<u32>(want);
    }
    return differ == 0U;
}

constexpr SmallestThree make_quaternion_format() noexcept
{
    SmallestThree format{};
    (void)smallest_three(9U, format);
    return format;
}

constexpr SmallestThree kQuaternion = make_quaternion_format();

constexpr u64 kEncodeAnswer = 0x70679B588B80CE7CULL;
constexpr u64 kDecodeAnswer = 0xC573733D539685B5ULL;
constexpr usize kDecodeRefused = 6U;

bool smallest_three_encode_known(const QuantizeKernels& table) noexcept
{
    // Not unit, which the encoder takes: each input is a component of the same simple sequence.
    f32 xyzw[4U * kCount];
    for (usize i = 0U; i < 4U * kCount; ++i)
    {
        xyzw[i] = value_at(i) / 8.0F;
    }
    u64 words[kCount];
    table.smallest_three_encode(kQuaternion, xyzw, kCount, words);
    return crd::fnv1a_64(words, sizeof(words)) == kEncodeAnswer;
}

bool smallest_three_decode_known(const QuantizeKernels& table) noexcept
{
    u64 words[kCount];
    for (usize i = 0U; i < kCount; ++i)
    {
        words[i] = (static_cast<u64>(i + 1U) * 0x9E3779B97F4A7C15ULL) >> (64U - smallest_three_word_bits(kQuaternion));
    }
    f32 xyzw[4U * kCount];
    const usize refused = table.smallest_three_decode(kQuaternion, words, kCount, xyzw);
    return refused == kDecodeRefused && crd::fnv1a_64(xyzw, sizeof(xyzw)) == kDecodeAnswer;
}

void resolve_quantize() noexcept
{
    (void)quantize_kernels();
}

constinit crd::KernelFamily g_family{"hesap.quantize", &resolve_quantize, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* quantize_self_check(const QuantizeKernels& table) noexcept
{
    if (!bounded_encode_known(table))
    {
        return "bounded_encode";
    }
    if (!bounded_decode_known(table))
    {
        return "bounded_decode";
    }
    if (!smallest_three_encode_known(table))
    {
        return "smallest_three_encode";
    }
    return smallest_three_decode_known(table) ? nullptr : "smallest_three_decode";
}

std::span<const KernelLevel<QuantizeKernels>> quantize_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<QuantizeKernels>& quantize_kernels() noexcept
{
    static const KernelLevel<QuantizeKernels>& selected =
        crd::process_checked_kernel_level<QuantizeKernels>(kLevels, &quantize_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::net
