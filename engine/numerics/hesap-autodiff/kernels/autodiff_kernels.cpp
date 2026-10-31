// The hesap-autodiff kernels, compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5): the
// canned functions' forward gradient on this level's jets (crd/hesap/autodiff/autodiff_kernels.hpp). The table is this
// level's (CRD_KERNEL_TABLE); everything else has internal linkage or lives in the level namespace.

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/autodiff/autodiff_kernels.hpp>
#include <crd/hesap/autodiff/canned_functions.hpp>
#include <crd/hesap/autodiff/jet_simd.hpp>

namespace crd::hesap::autodiff::inline CRD_MATH_LEVEL_NS
{
namespace
{

// Eight directions a pass: two Vec4d registers, one Vec8d at avx512.
constexpr int kCannedTile = 8;

void gradient_f64(CannedFunction which, const crd::f64* x, int n, crd::f64* g) noexcept
{
    forward::JetPackD<kCannedTile> scratch[kCannedMaxN];
    canned::visit(which, [&](const auto& f) { forward::gradient(f, x, n, g, scratch); });
}

} // namespace

constexpr auto kGradientF64 = &gradient_f64;

} // namespace crd::hesap::autodiff::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::autodiff
{

const AutodiffKernels CRD_KERNEL_TABLE(kAutodiffKernels) = {CRD_MATH_LEVEL_NS::kGradientF64};

} // namespace crd::hesap::autodiff
