// The scalar transcendental tables of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the
// process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/math/scalar_dispatch.hpp>

#include <bit>

namespace crd::math
{
namespace
{

// CRD_KERNELS_SCALAR: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_SCALAR & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_SCALAR, PORTABLE), "the portable level is always built");

constexpr KernelLevel<ScalarMathKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kScalarMathKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SCALAR, AVX2)
    {crd::CpuLevel::Avx2, &kScalarMathKernelsAvx2, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// Each routine at two arguments, one in its core range and one through its range reduction, overflow-side or
// subnormal path, with the result bits every level returns (the table's contract: one rounding per fma either way).
// The bits are the portable table's; each is within one ulp of the mpmath value at 200 bits.
using Unary = double (*)(double) noexcept;
using Binary = double (*)(double, double) noexcept;

struct UnaryCheck
{
    const char* name;
    Unary ScalarMathKernels::* routine;
    double x[2];
    crd::u64 bits[2];
};

struct BinaryCheck
{
    const char* name;
    Binary ScalarMathKernels::* routine;
    double x[2];
    double y[2];
    crd::u64 bits[2];
};

constexpr UnaryCheck kUnaryChecks[] = {
    {"exp", &ScalarMathKernels::exp, {1.25, -710.5}, {0x400BEC38EDB0FAF0ULL, 0x0001F3C9D0FBE022ULL}},
    {"exp2", &ScalarMathKernels::exp2, {0.3, -1060.25}, {0x3FF3B2C47BFF8329ULL, 0x00000000000035D1ULL}},
    {"exp10", &ScalarMathKernels::exp10, {0.4, 2.5}, {0x40041857E9D4CC5FULL, 0x4073C3A4EDFA9759ULL}},
    {"expm1", &ScalarMathKernels::expm1, {1e-5, 3.5}, {0x3EE4F8BC681CDFB6ULL, 0x40400EC721396BDBULL}},
    {"log", &ScalarMathKernels::log, {2.5, 1e-310}, {0x3FED5240F0E0E078ULL, 0xC0864E69394D9508ULL}},
    {"log2", &ScalarMathKernels::log2, {0.75, 1e300}, {0xBFDA8FF971810A5EULL, 0x408F24A09F1A8B89ULL}},
    {"log10", &ScalarMathKernels::log10, {7.0, 1e-5}, {0x3FEB0B0B0B78CC3FULL, 0xC014000000000000ULL}},
    {"log1p", &ScalarMathKernels::log1p, {1e-6, 15.5}, {0x3EB0C6F713F927AAULL, 0x40066D4835172A55ULL}},
    {"sin", &ScalarMathKernels::sin, {0.5, 1e6}, {0x3FDEAEE8744B05F0ULL, 0xBFD6664B2568D867ULL}},
    {"cos", &ScalarMathKernels::cos, {0.5, 1e6}, {0x3FEC1528065B7D50ULL, 0x3FEDF9DF9906D32CULL}},
    {"tan", &ScalarMathKernels::tan, {0.4, 100.0}, {0x3FDB0F0B49DCDCD8ULL, 0xBFE2CA74D62B5D38ULL}},
    {"atan", &ScalarMathKernels::atan, {0.3, 50.0}, {0x3FD2A73A661EAF05ULL, 0x3FF8D0129ACD6D1CULL}},
    {"asin", &ScalarMathKernels::asin, {0.3, -0.9}, {0x3FD380159E14F6FEULL, 0xBFF1EA93705FA172ULL}},
    {"acos", &ScalarMathKernels::acos, {0.3, -0.9}, {0x3FF441F5ECBEEF58ULL, 0x400586476251E745ULL}},
    {"sinh", &ScalarMathKernels::sinh, {0.2, 5.0}, {0x3FC9C560CD35EF80ULL, 0x40528D0166F07374ULL}},
    {"cosh", &ScalarMathKernels::cosh, {0.2, 5.0}, {0x3FF0523184B1EE9DULL, 0x40528D6FCBEFF3AAULL}},
    {"tanh", &ScalarMathKernels::tanh, {0.2, 5.0}, {0x3FC9439830B3A590ULL, 0x3FEFFF419668DF11ULL}},
    {"asinh", &ScalarMathKernels::asinh, {0.5, 100.0}, {0x3FDECC2CAEC5160AULL, 0x40153180A93D3C2AULL}},
    {"acosh", &ScalarMathKernels::acosh, {1.5, 100.0}, {0x3FEECC2CAEC5160AULL, 0x401531738DCBC627ULL}},
    {"atanh", &ScalarMathKernels::atanh, {0.3, -0.8}, {0x3FD3CF2B50617C95ULL, 0xBFF193EA7AAD030BULL}},
};

constexpr BinaryCheck kBinaryChecks[] = {
    {"atan2", &ScalarMathKernels::atan2, {1.5, -0.1}, {-2.5, 3.0}, {0x4004CF33DEC5E9E0ULL, 0xBFA10F7317226AFDULL}},
    {"pow", &ScalarMathKernels::pow, {1.7, 2.0}, {3.3, -0.5}, {0x40170B0A6F6ECCFAULL, 0x3FE6A09E667F3BCDULL}},
};

// sincos at 2 and at 1e22, whose reduction needs the long multiple of pi: the sine's bits, then the cosine's.
constexpr double kSincosX[2] = {2.0, 1e22};
constexpr crd::u64 kSincosBits[2][2] = {{0x3FED18F6EAD1B446ULL, 0xBFDAA22657537205ULL},
                                        {0xBFEB453AB76BF397ULL, 0x3FE0BE2CEF01C8F4ULL}};

bool same(double got, crd::u64 want) noexcept
{
    return std::bit_cast<crd::u64>(got) == want;
}

void resolve_scalar() noexcept
{
    (void)scalar_math_level();
}

constinit crd::KernelFamily g_family{"math.scalar", &resolve_scalar, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* scalar_math_self_check(const ScalarMathKernels& table) noexcept
{
    for (const UnaryCheck& c : kUnaryChecks)
    {
        const Unary routine = table.*c.routine;
        if (!same(routine(c.x[0]), c.bits[0]) || !same(routine(c.x[1]), c.bits[1]))
        {
            return c.name;
        }
    }
    for (const BinaryCheck& c : kBinaryChecks)
    {
        const Binary routine = table.*c.routine;
        if (!same(routine(c.x[0], c.y[0]), c.bits[0]) || !same(routine(c.x[1], c.y[1]), c.bits[1]))
        {
            return c.name;
        }
    }
    for (crd::usize i = 0U; i < 2U; ++i)
    {
        double s = 0.0;
        double c = 0.0;
        table.sincos(kSincosX[i], s, c);
        if (!same(s, kSincosBits[i][0]) || !same(c, kSincosBits[i][1]))
        {
            return "sincos";
        }
    }
    return nullptr;
}

std::span<const KernelLevel<ScalarMathKernels>> scalar_math_levels() noexcept
{
    return kLevels;
}

const KernelLevel<ScalarMathKernels>& scalar_math_level() noexcept
{
    static const KernelLevel<ScalarMathKernels>& selected =
        process_checked_kernel_level<ScalarMathKernels>(kLevels, &scalar_math_self_check, g_family);
    return selected;
}

const ScalarMathKernels& scalar_math() noexcept
{
    static const ScalarMathKernels& selected = *scalar_math_level().table;
    return selected;
}

} // namespace crd::math
