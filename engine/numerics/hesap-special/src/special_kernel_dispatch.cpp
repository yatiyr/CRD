// The hesap-special kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the
// process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/special/special_dispatch.hpp>

#include <bit>
#include <cmath>
#include <limits>

namespace crd::hesap::special
{
namespace
{

using crd::f64;
using crd::KernelLevel;
using crd::u64;
using crd::usize;

// CRD_KERNELS_SPECIAL: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_SPECIAL & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, PORTABLE), "the portable level is always built");

constexpr KernelLevel<SpecialKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kSpecialKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, SSE2)
    {crd::CpuLevel::Sse2, &kSpecialKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, AVX2)
    {crd::CpuLevel::Avx2, &kSpecialKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, AVX512)
    {crd::CpuLevel::Avx512, &kSpecialKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// Thirteen arguments: at avx512 one eight-lane step, one four-lane step and a scalar tail (four-lane steps and a tail
// at avx2, two-lane steps and a tail at sse2). The lanes outside the half-plane (0.25, the reflection at -2.5, NaN,
// the pole at -3, +inf) sit inside the vector steps, so the scalar functions replace them, and the half-plane's ends
// (the largest finite value, 0.5) are lanes; 171.75 overflows tgamma. Every level gives the scalar functions' bits,
// so each answer is the portable table's; each finite one is within 2.5e-14 relative of mpmath at 200 bits (the
// functions' gate is 1e-13). A NaN answer is checked as a NaN: the sign of a NaN an operation produces differs
// between x86-64 and AArch64.
constexpr usize kCheckCount = 13U;
constexpr f64 kCheckX[kCheckCount] = {
    std::numeric_limits<f64>::max(), 0.25, 10.0, -2.5, 7.25, 171.75, 0.5, std::numeric_limits<f64>::quiet_NaN(),
    33.0, -3.0, 1.5, std::numeric_limits<f64>::infinity(), 4.75};
constexpr u64 kNan = 0x7FF8000000000000ULL; // an answer checked as any NaN
constexpr u64 kInf = 0x7FF0000000000000ULL;
constexpr u64 kLgammaBits[kCheckCount] = {
    kInf, 0x3FF49BBD81C16EF8ULL, 0x40299A8921A7F7D2ULL, 0xBFACCBF9F5ED0FE0ULL, 0x401C35701A50FF0CULL,
    0x4086336E1ABCCF70ULL, 0x3FE250D048E7A1C0ULL, kNan, 0x405463B59B942083ULL, kInf, 0xBFBEEB95B094C180ULL, kInf,
    0x400677F44ABA1ADEULL};
constexpr u64 kTgammaBits[kCheckCount] = {
    kInf, 0x400D013FC47EEEE5ULL, 0x411626000000001EULL, 0xBFEE3FF812E32177ULL, 0x40920D86288356D0ULL, kInf,
    0x3FFC5BF891B4EF6DULL, kNan, 0x474956AD0AAE3368ULL, kNan, 0x3FEC5BF891B4EF6DULL, kInf, 0x40309611A1BAAD44ULL};

bool known(const f64 (&got)[kCheckCount], const u64 (&want)[kCheckCount]) noexcept
{
    for (usize i = 0U; i < kCheckCount; ++i)
    {
        if (want[i] == kNan ? !std::isnan(got[i]) : std::bit_cast<u64>(got[i]) != want[i])
        {
            return false;
        }
    }
    return true;
}

void resolve_special() noexcept
{
    (void)special_kernels();
}

constinit crd::KernelFamily g_family{"hesap.special", &resolve_special, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* special_self_check(const SpecialKernels& table) noexcept
{
    f64 out[kCheckCount];
    table.lgamma_lanczos(out, kCheckX, kCheckCount);
    if (!known(out, kLgammaBits))
    {
        return "lgamma_lanczos";
    }
    table.tgamma_lanczos(out, kCheckX, kCheckCount);
    if (!known(out, kTgammaBits))
    {
        return "tgamma_lanczos";
    }
    return nullptr;
}

std::span<const KernelLevel<SpecialKernels>> special_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<SpecialKernels>& special_kernels() noexcept
{
    static const KernelLevel<SpecialKernels>& selected =
        crd::process_checked_kernel_level<SpecialKernels>(kLevels, &special_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::special
