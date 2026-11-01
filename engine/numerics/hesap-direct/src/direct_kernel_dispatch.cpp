// The hesap-direct kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/direct/direct_dispatch.hpp>

#include <bit>
#include <cstring>
#include <type_traits>

namespace crd::hesap::direct
{
namespace
{

using crd::i64;
using crd::KernelLevel;
using crd::usize;

// CRD_KERNELS_DIRECT: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_DIRECT & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, PORTABLE), "the portable level is always built");

constexpr KernelLevel<DirectKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kDirectKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, SSE2)
    {crd::CpuLevel::Sse2, &kDirectKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, AVX2)
    {crd::CpuLevel::Avx2, &kDirectKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, AVX512)
    {crd::CpuLevel::Avx512, &kDirectKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// One n by n panel per element type, n = 3w + 5 for avx512's w lanes, every entry an integer in [-2, 2] and the
// diagonal in [1, 3]; the right-hand sides of the solves are the panel times a solution of entries in +-{1, 2, 3}.
// Every product and sum is an exact integer, so the levels that round a multiply and an add and the ones that fuse them
// give the same bits, and so does every order: the answers are those integers, evaluated at compile time. The shapes
// run every loop of every level: n gives two-vector passes, a whole vector and a partial one; 7 diagonal columns a
// whole block, a partial one and the backward solve's far terms; 6 below columns a group of four and two single ones;
// 19 reduced columns avx512's group of sixteen and a tail; w + 3 right-hand sides a whole vector and a partial one.
constexpr usize kBelowColumns = 6U;
constexpr usize kProductRhs = 2U;
constexpr usize kReduceColumns = 19U;
constexpr usize kReduceRows = 9U;
constexpr usize kSolveColumns = 7U;

// Entry (r, k) of the panel, column k at panel + k * n.
constexpr i64 panel_entry(usize r, usize k) noexcept
{
    return r == k ? static_cast<i64>(k % 3U) + 1 : static_cast<i64>((r * 7U + k * 3U + 1U) % 5U) - 2;
}

// Entry (j, c) of the solutions.
constexpr i64 kSolutionValues[6] = {1, -2, 3, -1, 2, -3};

constexpr i64 solution(usize j, usize c) noexcept
{
    return kSolutionValues[(j * 5U + c * 3U) % 6U];
}

template <typename T> struct DirectCheck
{
    static constexpr usize kN = 3U * (64U / sizeof(T)) + 5U;
    static constexpr usize kRhs = 64U / sizeof(T) + 3U;

    T panel[kN * kN];
    T below_x[kBelowColumns];
    T below_y[kN]; // forward_below: y, which becomes y + A^T x over panel columns 0 to 5
    T below_want[kN];
    T diagonal_b[kN];                  // forward_diagonal: L x = b over the whole panel
    T diagonal_want[kN];               // solution column 0
    T product_want[kN * kProductRhs];  // block_forward_below: panel columns 0 to 5 times panel rows 0 to 5
    T reduce_y[kReduceColumns * kRhs]; // block_backward_below: y less the panel times the panel read row-major
    T reduce_want[kReduceColumns * kRhs];
    T block_b[kSolveColumns * kRhs]; // block_forward_diagonal: L D = B, row j of D at j * kRhs
    T block_y[kSolveColumns * kRhs]; // block_backward_diagonal: L^T D = Y
    T block_want[kSolveColumns * kRhs];
};

template <typename T> constexpr DirectCheck<T> make_direct_check() noexcept
{
    constexpr usize n = DirectCheck<T>::kN;
    constexpr usize rhs = DirectCheck<T>::kRhs;
    DirectCheck<T> d{};
    for (usize k = 0; k < n; ++k)
    {
        for (usize r = 0; r < n; ++r)
        {
            d.panel[k * n + r] = static_cast<T>(panel_entry(r, k));
        }
    }
    for (usize r = 0; r < kBelowColumns; ++r)
    {
        d.below_x[r] = static_cast<T>(solution(r, 0U));
    }
    for (usize i = 0; i < n; ++i)
    {
        i64 below = solution(i, 1U);
        for (usize r = 0; r < kBelowColumns; ++r)
        {
            below += panel_entry(i, r) * solution(r, 0U);
        }
        i64 b = 0;
        for (usize j = 0; j <= i; ++j)
        {
            b += panel_entry(i, j) * solution(j, 0U);
        }
        d.below_y[i] = static_cast<T>(solution(i, 1U));
        d.below_want[i] = static_cast<T>(below);
        d.diagonal_b[i] = static_cast<T>(b);
        d.diagonal_want[i] = static_cast<T>(solution(i, 0U));
        for (usize c = 0; c < kProductRhs; ++c)
        {
            i64 out = 0;
            for (usize k = 0; k < kBelowColumns; ++k)
            {
                out += panel_entry(i, k) * panel_entry(k, c);
            }
            d.product_want[c * n + i] = static_cast<T>(out);
        }
    }
    for (usize k = 0; k < kReduceColumns; ++k)
    {
        for (usize c = 0; c < rhs; ++c)
        {
            i64 acc = 0;
            for (usize r = 0; r < kReduceRows; ++r)
            {
                const usize at = r * rhs + c; // the panel read row-major as the weights
                acc += panel_entry(r, k) * panel_entry(at % n, at / n);
            }
            d.reduce_y[c * kReduceColumns + k] = static_cast<T>(solution(k, c));
            d.reduce_want[c * kReduceColumns + k] = static_cast<T>(solution(k, c) - acc);
        }
    }
    for (usize j = 0; j < kSolveColumns; ++j)
    {
        for (usize c = 0; c < rhs; ++c)
        {
            i64 b = 0;
            for (usize i = 0; i <= j; ++i)
            {
                b += panel_entry(j, i) * solution(i, c);
            }
            i64 y = 0;
            for (usize k = j; k < kSolveColumns; ++k)
            {
                y += panel_entry(k, j) * solution(k, c);
            }
            d.block_b[j * rhs + c] = static_cast<T>(b);
            d.block_y[j * rhs + c] = static_cast<T>(y);
            d.block_want[j * rhs + c] = static_cast<T>(solution(j, c));
        }
    }
    return d;
}

constexpr DirectCheck<crd::f64> kCheckF64 = make_direct_check<crd::f64>();
constexpr DirectCheck<crd::f32> kCheckF32 = make_direct_check<crd::f32>();

// The names a failure reports, in DirectTypeKernels' order.
constexpr const char* kF64Routines[6] = {"f64 forward_below",          "f64 forward_diagonal",
                                         "f64 block_forward_below",    "f64 block_backward_below",
                                         "f64 block_forward_diagonal", "f64 block_backward_diagonal"};
constexpr const char* kF32Routines[6] = {"f32 forward_below",          "f32 forward_diagonal",
                                         "f32 block_forward_below",    "f32 block_backward_below",
                                         "f32 block_forward_diagonal", "f32 block_backward_diagonal"};

template <typename T, usize N> bool same_bits(const T (&got)[N], const T (&want)[N]) noexcept
{
    using Bits = std::conditional_t<sizeof(T) == sizeof(crd::u64), crd::u64, crd::u32>;
    for (usize i = 0; i < N; ++i)
    {
        if (std::bit_cast<Bits>(got[i]) != std::bit_cast<Bits>(want[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename T>
const char* check_type(const DirectTypeKernels<T>& k, const DirectCheck<T>& d, const char* const (&names)[6]) noexcept
{
    constexpr usize n = DirectCheck<T>::kN;
    constexpr usize rhs = DirectCheck<T>::kRhs;
    T x[n];
    std::memcpy(x, d.below_y, sizeof(x));
    k.forward_below(dense::GemvtRows<T>{d.panel, d.below_x, x, n, 1U, kBelowColumns, n});
    if (!same_bits(x, d.below_want))
    {
        return names[0];
    }
    std::memcpy(x, d.diagonal_b, sizeof(x));
    k.forward_diagonal(x, d.panel, n, n);
    if (!same_bits(x, d.diagonal_want))
    {
        return names[1];
    }
    T out[n * kProductRhs];
    k.block_forward_below(PanelProduct<T>{out, d.panel, d.panel, n, n, kBelowColumns, n, kProductRhs});
    if (!same_bits(out, d.product_want))
    {
        return names[2];
    }
    T y[kReduceColumns * rhs];
    T acc[kReduceColumns * rhs];
    std::memcpy(y, d.reduce_y, sizeof(y));
    k.block_backward_below(
        PanelReduce<T>{y, acc, d.panel, d.panel, kReduceColumns, n, kReduceColumns, kReduceRows, rhs});
    if (!same_bits(y, d.reduce_want))
    {
        return names[3];
    }
    T block[kSolveColumns * rhs];
    std::memcpy(block, d.block_b, sizeof(block));
    k.block_forward_diagonal(block, rhs, d.panel, n, kSolveColumns);
    if (!same_bits(block, d.block_want))
    {
        return names[4];
    }
    std::memcpy(block, d.block_y, sizeof(block));
    k.block_backward_diagonal(block, rhs, d.panel, n, kSolveColumns);
    if (!same_bits(block, d.block_want))
    {
        return names[5];
    }
    return nullptr;
}

void resolve_direct() noexcept
{
    (void)direct_kernels();
}

constinit crd::KernelFamily g_family{"hesap.direct", &resolve_direct, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* direct_self_check(const DirectKernels& table) noexcept
{
    const char* failed = check_type(table.f64_kernels, kCheckF64, kF64Routines);
    return failed != nullptr ? failed : check_type(table.f32_kernels, kCheckF32, kF32Routines);
}

std::span<const KernelLevel<DirectKernels>> direct_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<DirectKernels>& direct_kernels() noexcept
{
    static const KernelLevel<DirectKernels>& selected =
        crd::process_checked_kernel_level<DirectKernels>(kLevels, &direct_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::direct
