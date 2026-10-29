// The dispatch-cost board (SIMD-DISPATCH, ADR-0134 §8): what a call through a crd kernel table costs against Google
// Highway's HWY_DYNAMIC_DISPATCH, its design peer. Both sides convert binary16 to binary32 (exact on every level and
// target). Each side runs two arms over the same body: its dispatched entry on every call (crd's production
// convert_f16_to_f32, Highway's HWY_DYNAMIC_DISPATCH) and the selected body's pointer taken once (the control: an
// indirect call without the dispatch), so each side's dispatch cost is the difference of its two arms. Arrays of 1 to
// 65,536 elements show the call cost where it dominates and both sides' bodies where it vanishes. The arms are timed
// round by round in a rotating order, the median of the rounds reported per call; after every timed sample the output
// must hold the bits of the portable level's conversion of the same input and the guard words after it must be
// untouched, and every arm first converts all 65,536 binary16 patterns (NaN only as a class), or the run exits 1.
// CRD_CPU_LEVEL selects crd's level (scripts/run_bench_dispatch.sh runs each); Highway's dispatch is its own.
#include "highway_peer.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/math/float_convert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/construct.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace
{
using crd::f32;
using crd::u16;
using crd::u32;
using crd::usize;

constexpr usize kPatterns = usize{1} << 16U;
constexpr usize kGuardWords = 16;
constexpr u32 kGuard = 0xFFC0DEADU;
constexpr usize kSizes[] = {1, 8, 64, 1024, 65536};
// Conversions per timed sample: enough calls that the shortest array's sample takes milliseconds.
constexpr usize kElementsPerSample = usize{1} << 22U;
constexpr usize kMinCalls = 64;
constexpr int kRounds = 21;

enum Arm : crd::u8
{
    CrdTable,
    CrdPointer,
    HighwayDispatch,
    HighwayPointer,
};
constexpr int kArms = 4;
constexpr const char* kArmNames[kArms] = {"crd table", "crd pointer", "highway dispatch", "highway pointer"};

void crd_table(const u16* src, f32* dst, usize n, usize calls) noexcept
{
    for (usize c = 0; c < calls; ++c)
    {
        crd::math::convert_f16_to_f32(std::span<const u16>(src, n), std::span<f32>(dst, n));
    }
}

void crd_pointer(const u16* src, f32* dst, usize n, usize calls) noexcept
{
    const auto convert = crd::math::float_convert_kernels().table->f16_to_f32;
    for (usize c = 0; c < calls; ++c)
    {
        convert(src, dst, n);
    }
}

void run(Arm arm, const u16* src, f32* dst, usize n, usize calls) noexcept
{
    switch (arm)
    {
        case CrdTable:
            crd_table(src, dst, n, calls);
            break;
        case CrdPointer:
            crd_pointer(src, dst, n, calls);
            break;
        case HighwayDispatch:
            crd_peer::highway::f16_to_f32_dispatched(src, dst, n, calls);
            break;
        case HighwayPointer:
            crd_peer::highway::f16_to_f32_pointer(src, dst, n, calls);
            break;
    }
}

bool is_nan(u32 bits) noexcept
{
    return (bits & 0x7F800000U) == 0x7F800000U && (bits & 0x007FFFFFU) != 0;
}

// Whether dst[0, n) holds want's bits (NaN as a class when nan_class) and the guard words after it are untouched.
bool check(const char* what, Arm arm, const f32* dst, const f32* want, usize n, bool nan_class) noexcept
{
    for (usize i = 0; i < n; ++i)
    {
        const u32 got = std::bit_cast<u32>(dst[i]);
        const u32 expected = std::bit_cast<u32>(want[i]);
        if (got != expected && !(nan_class && is_nan(got) && is_nan(expected)))
        {
            std::printf("!MISMATCH! %s, %s at n=%zu: element %zu is 0x%08X, portable 0x%08X\n", what, kArmNames[arm], n,
                        i, got, expected);
            return false;
        }
    }
    for (usize g = 0; g < kGuardWords; ++g)
    {
        if (std::bit_cast<u32>(dst[n + g]) != kGuard)
        {
            std::printf("!MISMATCH! %s, %s at n=%zu: guard word %zu written\n", what, kArmNames[arm], n, g);
            return false;
        }
    }
    return true;
}

void set_guards(f32* dst, usize n) noexcept
{
    for (usize g = 0; g < kGuardWords; ++g)
    {
        dst[n + g] = std::bit_cast<f32>(kGuard);
    }
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(usize{16} << 20U);
    const char* const oom = "bench_dispatch_cost: the bench arena is out of memory";
    auto* patterns = crd::memory::allocate_array_or_fatal<u16>(alloc, kPatterns, 64U, oom);
    auto* src = crd::memory::allocate_array_or_fatal<u16>(alloc, kPatterns, 64U, oom);
    auto* want = crd::memory::allocate_array_or_fatal<f32>(alloc, kPatterns, 64U, oom);
    auto* dst = crd::memory::allocate_array_or_fatal<f32>(alloc, kPatterns + kGuardWords, 64U, oom);
    const crd::math::FloatConvertKernels& portable = *crd::math::float_convert_levels()[0].table;
    std::printf("conversion kernel level %s (selected %s)\npeer: %s\n",
                crd::cpu_level_name(crd::math::float_convert_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected), crd_peer::highway::isa());

    bool ok = true;
    // Every binary16 pattern once through each arm (signed zeros, subnormals, infinities and NaNs included).
    for (usize i = 0; i < kPatterns; ++i)
    {
        patterns[i] = static_cast<u16>(i);
    }
    portable.f16_to_f32(patterns, want, kPatterns);
    for (int arm = 0; arm < kArms; ++arm)
    {
        set_guards(dst, kPatterns);
        run(static_cast<Arm>(arm), patterns, dst, kPatterns, 1);
        ok = check("every pattern", static_cast<Arm>(arm), dst, want, kPatterns, true) && ok;
    }

    // The timed input: seeded finite values and infinities (no NaN, so the timed rows compare every bit).
    u32 state = 0x2545F491U;
    for (usize i = 0; i < kPatterns; ++i)
    {
        u16 bits = 0x7E00U;
        while ((bits & 0x7C00U) == 0x7C00U && (bits & 0x03FFU) != 0)
        {
            state = state * 1664525U + 1013904223U;
            bits = static_cast<u16>(state >> 16U);
        }
        src[i] = bits;
    }
    portable.f16_to_f32(src, want, kPatterns);

    std::printf("%8s %12s %12s %10s %12s %12s %10s   (ns per call; dispatch = table or dispatch minus pointer)\n", "n",
                "crd table", "crd pointer", "dispatch", "hwy dispatch", "hwy pointer", "dispatch");
    for (const usize n : kSizes)
    {
        const usize calls = std::max(kMinCalls, kElementsPerSample / n);
        double times[kArms][kRounds] = {};
        for (int arm = 0; arm < kArms; ++arm)
        {
            run(static_cast<Arm>(arm), src, dst, n, calls / 8U + 1U); // warm
        }
        for (int round = 0; round < kRounds && ok; ++round)
        {
            for (int k = 0; k < kArms; ++k)
            {
                const auto arm = static_cast<Arm>((k + round) % kArms);
                set_guards(dst, n);
                std::memset(dst, 0, n * sizeof(f32));
                const auto t0 = std::chrono::steady_clock::now();
                run(arm, src, dst, n, calls);
                const auto t1 = std::chrono::steady_clock::now();
                times[arm][round] = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(calls);
                ok = check("timed", arm, dst, want, n, false) && ok;
            }
        }
        if (!ok)
        {
            break;
        }
        double median[kArms] = {};
        for (int arm = 0; arm < kArms; ++arm)
        {
            std::sort(times[arm], times[arm] + kRounds);
            median[arm] = times[arm][kRounds / 2];
        }
        std::printf("%8zu %12.3f %12.3f %+10.3f %12.3f %12.3f %+10.3f\n", n, median[CrdTable], median[CrdPointer],
                    median[CrdTable] - median[CrdPointer], median[HighwayDispatch], median[HighwayPointer],
                    median[HighwayDispatch] - median[HighwayPointer]);
    }
    if (!ok)
    {
        std::printf("bench_dispatch_cost: an arm's output left the portable level's bits\n");
        return 1;
    }
    return 0;
}
