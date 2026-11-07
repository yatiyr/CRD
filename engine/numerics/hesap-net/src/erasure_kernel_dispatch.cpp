// The erasure-code kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/net/erasure_dispatch.hpp>
#include <crd/hesap/net/gf256.hpp>

namespace crd::hesap::net
{
namespace
{

using crd::KernelLevel;
using crd::u8;
using crd::usize;

// CRD_KERNELS_ERASURE: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512) |
                                (1 << CRD_KERNEL_LEVEL_NEON);
static_assert((CRD_KERNELS_ERASURE & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_ERASURE, PORTABLE), "the portable level is always built");

constexpr KernelLevel<ErasureKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kErasureKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_ERASURE, SSE2)
    {crd::CpuLevel::Sse2, &kErasureKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_ERASURE, AVX2)
    {crd::CpuLevel::Avx2, &kErasureKernelsAvx2, {}},
#endif
#if CRD_KERNELS_ERASURE_AVX2_GFNI
    {crd::CpuLevel::Avx2, &kErasureKernelsAvx2Gfni, crd::CpuFeatureSet{}.with(crd::CpuFeature::Gfni)},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_ERASURE, AVX512)
    {crd::CpuLevel::Avx512, &kErasureKernelsAvx512, {}},
#endif
#if CRD_KERNELS_ERASURE_AVX512_GFNI
    {crd::CpuLevel::Avx512, &kErasureKernelsAvx512Gfni, crd::CpuFeatureSet{}.with(crd::CpuFeature::Gfni)},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_ERASURE, NEON)
    {crd::CpuLevel::Neon, &kErasureKernelsNeon, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// Five outputs of three 141-byte sources: a group of four and a single output at the shuffle levels, and 141 bytes
// are whole vectors and a 13-byte scalar remainder at every level (avx512: two of 64; avx2: four of 32; sse2 and
// neon: eight of 16). The coefficients hold 0, 1, 2, 0x80 and 0xFF, so every bit product and both nibble tables are
// used. The answers are gf256_mul evaluated at compile time; a second, accumulating pass of the same rows gives zero.
constexpr usize kSources = 3U;
constexpr usize kOutputs = 5U;
constexpr usize kBytes = 141U;
constexpr u8 kCoefficients[kOutputs][kSources] = {
    {0x01U, 0x02U, 0x80U}, {0xFFU, 0x00U, 0x1DU}, {0x53U, 0xCAU, 0x8EU}, {0x00U, 0x00U, 0x00U}, {0x02U, 0xFFU, 0x01U}};

constexpr u8 source_byte(usize j, usize i) noexcept
{
    return static_cast<u8>((i * 37U) + (j * 101U) + ((i * i) >> 3U) + 13U);
}

struct DotAnswers
{
    u8 out[kOutputs][kBytes];
};

constexpr DotAnswers make_dot_answers() noexcept
{
    DotAnswers a{};
    for (usize g = 0U; g < kOutputs; ++g)
    {
        for (usize i = 0U; i < kBytes; ++i)
        {
            u8 acc = 0U;
            for (usize j = 0U; j < kSources; ++j)
            {
                acc = static_cast<u8>(acc ^ gf256_mul(kCoefficients[g][j], source_byte(j, i)));
            }
            a.out[g][i] = acc;
        }
    }
    return a;
}

constexpr DotAnswers kDotAnswers = make_dot_answers();

bool dot_known(const ErasureKernels& table) noexcept
{
    u8 sources[kSources][kBytes];
    for (usize j = 0U; j < kSources; ++j)
    {
        for (usize i = 0U; i < kBytes; ++i)
        {
            sources[j][i] = source_byte(j, i);
        }
    }
    GfCoefficient rows[kOutputs * kSources];
    for (usize g = 0U; g < kOutputs; ++g)
    {
        for (usize j = 0U; j < kSources; ++j)
        {
            rows[(g * kSources) + j] = gf256_expand(kCoefficients[g][j]);
        }
    }
    const u8* source_rows[kSources] = {sources[0], sources[1], sources[2]};
    u8 out[kOutputs][kBytes];
    u8* outputs[kOutputs] = {out[0], out[1], out[2], out[3], out[4]};
    table.dot(rows, source_rows, kSources, outputs, kOutputs, kBytes, false);
    u8 differ = 0U;
    for (usize g = 0U; g < kOutputs; ++g)
    {
        for (usize i = 0U; i < kBytes; ++i)
        {
            differ = static_cast<u8>(differ | (out[g][i] ^ kDotAnswers.out[g][i]));
        }
    }
    table.dot(rows, source_rows, kSources, outputs, kOutputs, kBytes, true);
    for (usize g = 0U; g < kOutputs; ++g)
    {
        for (usize i = 0U; i < kBytes; ++i)
        {
            differ = static_cast<u8>(differ | out[g][i]);
        }
    }
    return differ == 0U;
}

// GF(2^8) addition over the same 141 bytes (whole vectors and a scalar remainder at every level) from odd offsets: the
// sum of the three sources, and accumulating the sum of two of them again leaves the third.
bool sum_known(const ErasureKernels& table) noexcept
{
    u8 sources[kSources][kBytes + 1U];
    for (usize j = 0U; j < kSources; ++j)
    {
        for (usize i = 0U; i < kBytes; ++i)
        {
            sources[j][i + 1U] = source_byte(j, i);
        }
    }
    const u8* source_rows[kSources] = {sources[0] + 1U, sources[1] + 1U, sources[2] + 1U};
    u8 out[kBytes + 1U];
    table.sum(out + 1U, source_rows, kSources, kBytes, false);
    u8 differ = 0U;
    for (usize i = 0U; i < kBytes; ++i)
    {
        differ = static_cast<u8>(differ | (out[i + 1U] ^ source_byte(0U, i) ^ source_byte(1U, i) ^ source_byte(2U, i)));
    }
    table.sum(out + 1U, source_rows, 2U, kBytes, true);
    for (usize i = 0U; i < kBytes; ++i)
    {
        differ = static_cast<u8>(differ | (out[i + 1U] ^ source_byte(2U, i)));
    }
    return differ == 0U;
}

void resolve_erasure() noexcept
{
    (void)erasure_kernels();
}

constinit crd::KernelFamily g_family{"hesap.erasure", &resolve_erasure, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* erasure_self_check(const ErasureKernels& table) noexcept
{
    if (!dot_known(table))
    {
        return "dot";
    }
    return sum_known(table) ? nullptr : "sum";
}

std::span<const KernelLevel<ErasureKernels>> erasure_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<ErasureKernels>& erasure_kernels() noexcept
{
    static const KernelLevel<ErasureKernels>& selected =
        crd::process_checked_kernel_level<ErasureKernels>(kLevels, &erasure_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::net
