// The hesap-autodiff kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the
// process runs (SIMD-DISPATCH).

#include <crd/core/cpu_features.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/hesap/autodiff/autodiff_dispatch.hpp>

#include <bit>

namespace crd::hesap::autodiff
{
namespace
{

using crd::f64;
using crd::KernelLevel;
using crd::u64;

// CRD_KERNELS_AUTODIFF: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_AUTODIFF & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, PORTABLE), "the portable level is always built");

constexpr KernelLevel<AutodiffKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kAutodiffKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, SSE2)
    {crd::CpuLevel::Sse2, &kAutodiffKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, AVX2)
    {crd::CpuLevel::Avx2, &kAutodiffKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, AVX512)
    {crd::CpuLevel::Avx512, &kAutodiffKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// Each canned function's gradient at n = 32 (four eight-direction passes) and n = 13 (a pass and a five-direction
// one) at x_i = ((5 i mod 17) - 8) / 4, which lies in [-2, 2]. Every jet product and sum of these quarters is exact
// in f64, so the answers are the gradients' closed forms evaluated at compile time: Rosenbrock's
// -400 x_i (x_{i+1} - x_i^2) - 2 (1 - x_i) + 200 (x_i - x_{i-1}^2), with its end terms, Sphere's 2 x_i, Cubes' 3 x_i^2.
constexpr int kCheckMaxN = 32;
constexpr int kCheckSizes[2] = {kCheckMaxN, 13};
constexpr CannedFunction kCheckFunctions[3] = {CannedFunction::Rosenbrock, CannedFunction::Sphere,
                                               CannedFunction::Cubes};
static_assert(kCheckMaxN <= kCannedMaxN);

struct GradientAnswers
{
    f64 x[kCheckMaxN];
    f64 g[3][2][kCheckMaxN];
};

constexpr GradientAnswers make_gradient_answers() noexcept
{
    GradientAnswers a{};
    for (int i = 0; i < kCheckMaxN; ++i)
    {
        a.x[i] = static_cast<f64>(((5 * i) % 17) - 8) * 0.25;
    }
    for (int s = 0; s < 2; ++s)
    {
        const int n = kCheckSizes[s];
        for (int i = 0; i < n; ++i)
        {
            a.g[1][s][i] = 2.0 * a.x[i];
            a.g[2][s][i] = 3.0 * a.x[i] * a.x[i];
        }
        for (int i = 0; i + 1 < n; ++i)
        {
            const f64 d = a.x[i + 1] - (a.x[i] * a.x[i]);
            a.g[0][s][i] += (-400.0 * a.x[i] * d) - (2.0 * (1.0 - a.x[i]));
            a.g[0][s][i + 1] += 200.0 * d;
        }
    }
    return a;
}

constexpr GradientAnswers kGradientAnswers = make_gradient_answers();

void resolve_autodiff() noexcept
{
    (void)autodiff_kernels();
}

constinit crd::KernelFamily g_family{"hesap.autodiff", &resolve_autodiff, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* autodiff_self_check(const AutodiffKernels& table) noexcept
{
    for (int f = 0; f < 3; ++f)
    {
        for (int s = 0; s < 2; ++s)
        {
            f64 g[kCheckMaxN];
            table.gradient_f64(kCheckFunctions[f], kGradientAnswers.x, kCheckSizes[s], g);
            for (int i = 0; i < kCheckSizes[s]; ++i)
            {
                if (std::bit_cast<u64>(g[i]) != std::bit_cast<u64>(kGradientAnswers.g[f][s][i]))
                {
                    return "gradient_f64";
                }
            }
        }
    }
    return nullptr;
}

std::span<const KernelLevel<AutodiffKernels>> autodiff_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<AutodiffKernels>& autodiff_kernels() noexcept
{
    static const KernelLevel<AutodiffKernels>& selected =
        crd::process_checked_kernel_level<AutodiffKernels>(kLevels, &autodiff_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::autodiff
