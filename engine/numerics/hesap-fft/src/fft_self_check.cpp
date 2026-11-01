// The known-answer self-check of the hesap-fft kernels (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp): the four
// routines of a table (execute and execute_batched, f64 and f32) over Stockham plans of every size from 2 to 128,
// built here in general code on a stack arena. A plan gives the same bits on every level (fft_kernels.hpp), so each
// routine's answer is one FNV-1a digest of its outputs' bits, the portable table's. Every output was checked once
// against an mpmath DFT at 200 bits: within 4.1e-16 (f64) and 1.0e-7 (f32) of its transform's largest output, the
// subnormal forward within 5.1e-12 and 9.2e-5.
//
// The cases run every path of a transform below the generated passes' sizes (fft_kernels.cpp): execute takes the
// leaf codelets (n <= 32; f64's lane-trick codelets at 8, 16 and 32) and the Stockham passes with an odd and an even
// log2, both directions, and one forward whose results are subnormal. execute_batched takes f64's register-resident
// N = 8 and N = 16 codelets (an even batch), the generated leaf codelets (a forward over a batch of whole tiles) and
// the radix-8 passes with each remainder stage, over a batch of whole vectors of the widest level and a scalar tail
// (11 f64, 19 f32 transforms).
//
// Every input is a small integer, nonzero in both parts; every output has a NaN guard after it. Nothing here reaches
// fft_kernels(), whose first call runs this check: the plans are built for Stockham by name and run through the
// candidate table. The scratch is one 8 KiB transform buffer and a 12 KiB arena for one plan at a time on the stack;
// nothing is allocated.

#include <crd/core/fnv1a.hpp>
#include <crd/core/platform.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>
#include <type_traits>

namespace crd::hesap::fft
{
namespace
{

using crd::f32;
using crd::f64;
using crd::i64;
using crd::u64;
using crd::usize;

// Nonzero, at most 2 in magnitude, and without a period that divides a vector width, so no lane repeats another's.
constexpr i64 kValues[7] = {1, -2, 2, -1, -2, 1, 2};

constexpr i64 value(usize k, usize salt) noexcept
{
    return kValues[(k * (salt % 5U + 2U) + salt * 3U + k / 7U) % 7U];
}

// One transform of the check: n points, `batch` transforms for execute_batched (0: execute; kTailBatch: the type's
// kTail), `tiny` scaling the inputs so the results are subnormal.
struct Case
{
    usize n;
    usize batch;
    FftDirection dir;
    bool tiny;
};

constexpr FftDirection kF = FftDirection::Forward;
constexpr FftDirection kI = FftDirection::Inverse;

template <typename T> constexpr usize kTail = std::is_same_v<T, f64> ? 11U : 19U;
constexpr usize kTile = 8U; // whole tiles of both types' leaf codelets, and even
constexpr usize kTailBatch = ~usize{0};

constexpr Case kCases[] = {
    {2U, 0U, kF, false},     {2U, 0U, kI, false},          {4U, 0U, kF, false},
    {4U, 0U, kI, false},     {8U, 0U, kF, false},          {8U, 0U, kI, false},
    {16U, 0U, kF, false},    {16U, 0U, kI, false},         {16U, 0U, kF, true},
    {32U, 0U, kF, false},    {32U, 0U, kI, false},         {64U, 0U, kF, false},
    {64U, 0U, kI, false},    {128U, 0U, kF, false},        {128U, 0U, kI, false},
    {8U, kTile, kF, false},  {8U, kTailBatch, kF, false},  {8U, kTailBatch, kI, false},
    {16U, kTile, kF, false}, {16U, kTailBatch, kF, false}, {16U, kTailBatch, kI, false},
    {32U, kTile, kF, false}, {32U, kTailBatch, kF, false}, {32U, kTailBatch, kI, false},
    {64U, kTile, kF, false}, {64U, 3U, kI, false},
};

// The transforms a case runs: 1 for execute.
template <typename T> constexpr usize transforms(const Case& c) noexcept
{
    if (c.batch == kTailBatch)
    {
        return kTail<T>;
    }
    return c.batch == 0U ? 1U : c.batch;
}

// The answers: each routine's digest over its cases in kCases order.
struct Answers
{
    u64 execute;
    u64 execute_batched;
    const char* names[2];
};
constexpr Answers kAnswers64{0xC892F40D09DCAF09ULL, 0x6CF7F82909D3C7DEULL, {"f64 execute", "f64 execute_batched"}};
constexpr Answers kAnswers32{0xCC3A7A3D345F533FULL, 0x9F5EABF56E31535EULL, {"f32 execute", "f32 execute_batched"}};
template <typename T> constexpr const Answers& answers() noexcept
{
    if constexpr (std::is_same_v<T, f64>)
    {
        return kAnswers64;
    }
    else
    {
        return kAnswers32;
    }
}

// 2^-1040 and 2^-140: the integer inputs times this put a transform's results among the subnormals.
template <typename T> T tiny() noexcept
{
    if constexpr (std::is_same_v<T, f64>)
    {
        return std::bit_cast<f64>(u64{0x0000000400000000ULL});
    }
    else
    {
        return std::bit_cast<f32>(crd::u32{0x00000200U});
    }
}

template <typename T> using Bits = std::conditional_t<sizeof(T) == sizeof(u64), u64, crd::u32>;

// One output's bits into a routine's digest.
template <typename T> u64 fold(u64 h, T v) noexcept
{
    return crd::fnv1a_64_le(std::bit_cast<Bits<T>>(v), h);
}

constexpr usize kLargestN = 128U;
constexpr usize kDataBytes = 8192U; // the largest case: 64 f64 points by 8 transforms
constexpr usize kGuards = 2U;
constexpr usize kArenaBytes = 12288U; // the 128-point f64 plan with its alignment padding

template <typename T> constexpr usize kDataPoints = kDataBytes / sizeof(Complex<T>);

template <typename T> constexpr bool fits() noexcept
{
    return std::ranges::all_of(kCases, [](const Case& c)
                               { return c.n <= kLargestN && c.n * transforms<T>(c) <= kDataPoints<T>; });
}
static_assert(fits<f64>() && fits<f32>(), "a case outgrows the plans or the transform buffer");

template <typename T> const T kGuard = std::numeric_limits<T>::quiet_NaN();

template <typename T> bool guard_kept(T v) noexcept
{
    return std::bit_cast<Bits<T>>(v) == std::bit_cast<Bits<T>>(kGuard<T>);
}

// One element type's two routines; out of line, so the two types' scratch never shares a frame.
template <typename T> CRD_NOINLINE const char* check_type(const FftTypeKernels<T>& k) noexcept
{
    alignas(64) Complex<T> data[kDataPoints<T> + kGuards];
    alignas(64) unsigned char arena_bytes[kArenaBytes];
    const T scale = tiny<T>();
    u64 digest[2] = {crd::kFnv1a64Offset, crd::kFnv1a64Offset};
    // One plan per size; each routine's cases keep their kCases order, as the sizes ascend in it.
    for (usize n = 2U; n <= kLargestN; n *= 2U)
    {
        crd::memory::LinearAllocator arena(arena_bytes, kArenaBytes, "fft self-check");
        const FftPlan<T> plan(&arena, n, FftAlgorithm::Stockham);
        for (usize index = 0U; index < std::size(kCases); ++index)
        {
            const Case& c = kCases[index];
            if (c.n != n)
            {
                continue;
            }
            const usize points = c.n * transforms<T>(c);
            for (usize i = 0U; i < points; ++i)
            {
                data[i].re = static_cast<T>(value(i, 2U * index)) * (c.tiny ? scale : static_cast<T>(1));
                data[i].im = static_cast<T>(value(i, 2U * index + 1U)) * (c.tiny ? scale : static_cast<T>(1));
            }
            data[points].re = kGuard<T>;
            data[points].im = kGuard<T>;
            const usize routine = c.batch == 0U ? 0U : 1U;
            if (routine == 0U)
            {
                k.execute(plan.state(), data, c.dir);
            }
            else
            {
                k.execute_batched(plan.state(), data, transforms<T>(c), c.dir);
            }
            if (!guard_kept(data[points].re) || !guard_kept(data[points].im))
            {
                return answers<T>().names[routine];
            }
            for (usize i = 0U; i < points; ++i)
            {
                digest[routine] = fold(fold(digest[routine], data[i].re), data[i].im);
            }
        }
    }
    if (digest[0] != answers<T>().execute)
    {
        return answers<T>().names[0];
    }
    return digest[1] != answers<T>().execute_batched ? answers<T>().names[1] : nullptr;
}

} // namespace

const char* fft_self_check(const FftKernels& table) noexcept
{
    const char* failed = check_type(table.f64_kernels);
    return failed != nullptr ? failed : check_type(table.f32_kernels);
}

} // namespace crd::hesap::fft
