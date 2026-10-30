// The hesap-fft kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs transforms one plan per band
// of the planner (the leaf codelets, the Stockham passes, IP4-AoS, the standalone-hier and deep-split bands, the
// four-step and six-step; at f32, every forward the planner offers at each size) and the batched sub-FFT paths, f64 and
// f32, both directions, within the ADR-0092 §5 tolerance of the radix-2 reference, on aligned data and one word off
// (the four-step's plain-store branch); every level gives portable's bits (the family is bit-exact) and is
// bit-identical run to run; the words around the data stay as they were; signed zeros and subnormals are not flushed,
// and one infinity or NaN makes every output non-finite as in the reference.
// crd-hesap-fft-kernels-at-<level> runs these [kernels] cases under each forced level.

#include "kernel_levels.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/math/determinism_mode.hpp>
#include <crd/math/kernel_dispatch.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace fft = crd::hesap::fft;
using crd::CpuLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::Array;
using crd::containers::Span;
using crd::hesap::Complex;
using crd::math::KernelLevel;
using fft::FftDirection;
using fft::FftKernels;

namespace
{

using crd::test::kernel_level_runs_here;

// The process's kernel determinism mode for one scope, the earlier one restored after it.
class DeterminismScope
{
public:
    explicit DeterminismScope(crd::math::DeterminismMode mode) : m_before(crd::math::kernel_determinism())
    {
        crd::math::set_kernel_determinism(mode);
    }
    ~DeterminismScope() { crd::math::set_kernel_determinism(m_before); }
    DeterminismScope(const DeterminismScope&) = delete;
    DeterminismScope& operator=(const DeterminismScope&) = delete;
    DeterminismScope(DeterminismScope&&) = delete;
    DeterminismScope& operator=(DeterminismScope&&) = delete;

private:
    crd::math::DeterminismMode m_before;
};

// The guard words before and after the data, in T units.
constexpr usize kGuard = 16;

template <typename T> T guard_word()
{
    using Bits = std::conditional_t<sizeof(T) == 8, u64, u32>;
    return std::bit_cast<T>(static_cast<Bits>(0x7FF4A5A5A5A5A5A5ULL & ~Bits{0})); // a signalling-NaN pattern
}

template <typename T> bool same_bits(T a, T b)
{
    using Bits = std::conditional_t<sizeof(T) == 8, u64, u32>;
    return std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b);
}

// Values in [-1, 1) from a 64-bit LCG.
template <typename T> void fill_lcg(T* v, usize count, u64 seed)
{
    u64 s = seed;
    for (usize i = 0; i < count; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        v[i] = static_cast<T>(static_cast<double>(s >> 11) / 9007199254740992.0 * 2.0 - 1.0);
    }
}

// fill_lcg's values, or negative zeros only.
template <typename T> void fill_input(T* v, usize count, u64 seed, bool negative_zeros)
{
    if (!negative_zeros)
    {
        fill_lcg(v, count, seed);
        return;
    }
    for (usize i = 0; i < count; ++i)
    {
        v[i] = -T(0);
    }
}

// The interleaved data of n complex values `shift` T words past the front guard, guards filled.
template <typename T> struct Buffer
{
    Array<T> words;
    usize n;
    usize shift;
    Buffer(crd::memory::IAllocator* alloc, usize count, usize offset) : words(alloc), n(count), shift(offset)
    {
        words.resize(2 * n + 2 * kGuard + shift);
        for (usize i = 0; i < words.size(); ++i)
        {
            words[i] = guard_word<T>();
        }
    }
    T* values() { return words.data() + kGuard + shift; }
    [[nodiscard]] const T* values() const { return words.data() + kGuard + shift; }
    Complex<T>* data() { return reinterpret_cast<Complex<T>*>(values()); }
    [[nodiscard]] const Complex<T>* data() const { return reinterpret_cast<const Complex<T>*>(values()); }
    [[nodiscard]] bool guards_kept() const
    {
        for (usize i = 0; i < words.size(); ++i)
        {
            const bool inside = i >= kGuard + shift && i < kGuard + shift + 2 * n;
            if (!inside && !same_bits(words[i], guard_word<T>()))
            {
                return false;
            }
        }
        return true;
    }
};

// max |a - b| / (1 + max |b|) over n complex values.
template <typename T> double rel_error(const Complex<T>* a, const Complex<T>* b, usize n)
{
    double maxref = 0.0;
    double maxerr = 0.0;
    for (usize k = 0; k < n; ++k)
    {
        maxref = std::max(maxref, std::hypot(static_cast<double>(b[k].re), static_cast<double>(b[k].im)));
        maxerr = std::max(maxerr, std::hypot(static_cast<double>(a[k].re) - static_cast<double>(b[k].re),
                                             static_cast<double>(a[k].im) - static_cast<double>(b[k].im)));
    }
    return maxerr / (1.0 + maxref);
}

// The ADR-0092 §5 classes of test_fft.cpp: f64 1e-12; f32 2e-4, 1e-3 from the four-step band.
template <typename T> double tolerance(usize n)
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        (void)n;
        return 1e-12;
    }
    else
    {
        return n >= (usize{1} << 19) ? 1e-3 : 2e-4;
    }
}

// Every size up to 2^21: each band the planner has, f64 and f32 (its per-type choices differ within a band).
constexpr u32 kLog2Max = 21;

// Every level the host runs transforms `plan` in direction `dir` within the reference's tolerance, bit-identical run to
// run and to portable, the guards kept. The four-step's NT stores take aligned data; one word in, f64 takes its
// plain-store branch. Negative zeros, whose signs every level must keep as portable does, take the aligned data. An
// input's reference serves each of its shifts. `every_input` false runs the random input, aligned, only.
template <typename T>
void check_plan_every_level(crd::memory::IAllocator* alloc, const fft::FftPlan<T>& plan, FftDirection dir,
                            bool every_input = true)
{
    const usize n = plan.size();
    for (const bool negative_zeros : {false, true})
    {
        if (negative_zeros && !every_input)
        {
            continue;
        }
        Buffer<T> want(alloc, n, 0);
        fill_input(want.values(), 2 * n, 0x5EEDULL + n, negative_zeros);
        plan.execute_reference(Span<Complex<T>>(want.data(), n), dir);
        const usize shifts = (negative_zeros || !every_input) ? 1U : 2U;
        for (usize shift = 0; shift < shifts; ++shift)
        {
            Buffer<T> portable(alloc, n, shift);
            usize ran = 0;
            for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
            {
                if (!kernel_level_runs_here(level))
                {
                    continue;
                }
                const fft::FftTypeKernels<T>& k = fft::kernels_of<T>(*level.table);
                Buffer<T> got(alloc, n, shift);
                Buffer<T> again(alloc, n, shift);
                fill_input(got.values(), 2 * n, 0x5EEDULL + n, negative_zeros);
                std::memcpy(again.values(), got.values(), 2 * n * sizeof(T));
                k.execute(plan.state(), got.data(), dir);
                k.execute(plan.state(), again.data(), dir);
                if (ran == 0U) // portable, levels[0]: the bits every other level gives
                {
                    std::memcpy(portable.values(), got.values(), 2 * n * sizeof(T));
                }
                ++ran;
                const double rel = rel_error(got.data(), want.data(), n);
                INFO(crd::cpu_level_name(level.level) << " n=" << n << " dir=" << static_cast<int>(dir) << " shift="
                                                      << shift << " -0=" << negative_zeros << " rel=" << rel);
                CHECK(rel < tolerance<T>(n));
                CHECK(std::memcmp(got.values(), again.values(), 2 * n * sizeof(T)) == 0);
                CHECK(std::memcmp(got.values(), portable.values(), 2 * n * sizeof(T)) == 0);
                CHECK(got.guards_kept());
            }
            CHECK(ran >= 1U); // portable runs everywhere
        }
    }
}

template <typename T> void check_every_level()
{
    crd::memory::TlsfAllocator alloc(usize{1} << 30);
    for (u32 lg = 0; lg <= kLog2Max; ++lg)
    {
        const fft::FftPlan<T> plan(&alloc, usize{1} << lg);
        for (const FftDirection dir : {FftDirection::Forward, FftDirection::Inverse})
        {
            check_plan_every_level(&alloc, plan, dir);
        }
    }
}

// The batched paths: N=8 (an odd batch, one pair, four-transform steps and a last pair) and N=16 (steps and a last
// pair, two steps) inside and past the over-2 cache budget, the batched leaves
// (16..256 at a multiple of the vector width; at one and three tiles of codelet_lanes.hpp, so avx512 runs a two-tile
// step and an odd last tile), the radix-2/4/8 butterflies at three tiles and a column (a two-tile step, an odd tile and
// the scalar tail in one call) and the hierarchical sub-FFTs at their block width. Each shape takes random inputs and
// negative zeros, whose signs every level must keep as portable does.
template <typename T> void check_batched_every_level()
{
    constexpr usize tile = 32 / sizeof(T);
    crd::memory::TlsfAllocator alloc(usize{1} << 28);
    struct Shape
    {
        usize m;
        usize batch;
    };
    const Shape shapes[] = {
        {8, 2},
        {8, 6},
        {8, 7},
        {16, 6},
        {16, 8},
        {16, 256},
        {32, 16},
        {64, 8},
        {128, 16},
        {256, 24},
        {1024, fft::detail::block_width<T>(1024)},
        {2048, fft::detail::block_width<T>(2048)},
        {4096, fft::detail::block_width<T>(4096)},
        {256, fft::detail::block_width<T>(256)},
        {512, 3},
        {16, tile},
        {32, 3 * tile},
        {64, 3 * tile},
        {16, 3 * tile + 1},
        {32, 3 * tile + 1},
    };
    for (const Shape& shape : shapes)
    {
        const fft::FftPlan<T> plan(&alloc, shape.m);
        const usize total = shape.m * shape.batch;
        for (const FftDirection dir : {FftDirection::Forward, FftDirection::Inverse})
        {
            for (const bool negative_zeros : {false, true})
            {
                Array<Complex<T>> input(&alloc);
                input.resize(total);
                if (negative_zeros)
                {
                    for (usize i = 0; i < total; ++i)
                    {
                        input[i] = Complex<T>{-T(0), -T(0)};
                    }
                }
                else
                {
                    fill_lcg(reinterpret_cast<T*>(input.data()), 2 * total, 0xBA7CULL + total);
                }
                Array<Complex<T>> portable(&alloc);
                portable.resize(total);
                usize ran = 0;
                for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
                {
                    if (!kernel_level_runs_here(level))
                    {
                        continue;
                    }
                    const fft::FftTypeKernels<T>& k = fft::kernels_of<T>(*level.table);
                    Buffer<T> got(&alloc, total, 0);
                    std::memcpy(got.data(), input.data(), total * sizeof(Complex<T>));
                    k.execute_batched(plan.state(), got.data(), shape.batch, dir);
                    if (ran == 0U) // portable, levels[0]: the bits every other level gives
                    {
                        std::memcpy(portable.data(), got.data(), total * sizeof(Complex<T>));
                    }
                    ++ran;
                    Array<Complex<T>> single(&alloc);
                    single.resize(shape.m);
                    double worst = 0.0;
                    for (usize t = 0; t < shape.batch; ++t)
                    {
                        for (usize i = 0; i < shape.m; ++i)
                        {
                            single[i] = input[i * shape.batch + t];
                        }
                        plan.execute_reference(Span<Complex<T>>(single.data(), shape.m), dir);
                        Array<Complex<T>> column(&alloc);
                        column.resize(shape.m);
                        for (usize i = 0; i < shape.m; ++i)
                        {
                            column[i] = got.data()[i * shape.batch + t];
                        }
                        worst = std::max(worst, rel_error(column.data(), single.data(), shape.m));
                    }
                    INFO(crd::cpu_level_name(level.level)
                         << " m=" << shape.m << " batch=" << shape.batch << " dir=" << static_cast<int>(dir)
                         << " -0=" << negative_zeros << " rel=" << worst);
                    CHECK(worst < tolerance<T>(shape.m));
                    CHECK(std::memcmp(got.data(), portable.data(), total * sizeof(Complex<T>)) == 0);
                    CHECK(got.guards_kept());
                }
            }
        }
    }
}

// Special inputs. Only zeros, signed zeros and subnormals: every sum is exact there and only a twiddle product rounds,
// so a level stays near the reference and gives portable's bits, and a flush-to-zero (an error the size of the result)
// shows. One infinity or one NaN: every output depends on it, so every complex output is non-finite, on a level as in
// the reference (NaN payloads may differ across levels).
template <typename T> void check_plan_specials(crd::memory::IAllocator* alloc, const fft::FftPlan<T>& plan)
{
    const usize n = plan.size();
    const T inf = std::numeric_limits<T>::infinity();
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const T tiny = std::numeric_limits<T>::denorm_min();
    const fft::FftTypeKernels<T>& portable = fft::kernels_of<T>(*fft::fft_kernel_levels()[0].table);
    for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        const fft::FftTypeKernels<T>& k = fft::kernels_of<T>(*level.table);
        {
            Buffer<T> got(alloc, n, 0);
            Buffer<T> want(alloc, n, 0);
            for (usize i = 0; i < 2 * n; ++i)
            {
                const T pick[] = {T(0), -T(0), tiny, -tiny * T(3)};
                got.values()[i] = pick[i % 4U];
            }
            std::memcpy(want.values(), got.values(), 2 * n * sizeof(T));
            Buffer<T> bits(alloc, n, 0);
            std::memcpy(bits.values(), got.values(), 2 * n * sizeof(T));
            k.execute(plan.state(), got.data(), FftDirection::Forward);
            portable.execute(plan.state(), bits.data(), FftDirection::Forward);
            plan.execute_reference(Span<Complex<T>>(want.data(), n), FftDirection::Forward);
            double maxref = 0.0;
            double maxerr = 0.0;
            for (usize i = 0; i < 2 * n; ++i)
            {
                const double a = static_cast<double>(got.values()[i]);
                const double b = static_cast<double>(want.values()[i]);
                maxref = std::max(maxref, std::fabs(b));
                maxerr = std::max(maxerr, std::fabs(a - b));
            }
            INFO(crd::cpu_level_name(level.level) << " subnormals n=" << n << " err=" << maxerr << " ref=" << maxref);
            CHECK(maxref > 0.0);
            CHECK(maxerr <= 0.25 * maxref);
            CHECK(std::memcmp(got.values(), bits.values(), 2 * n * sizeof(T)) == 0);
            CHECK(got.guards_kept());
        }
        for (const T special : {inf, nan})
        {
            Buffer<T> got(alloc, n, 0);
            Buffer<T> want(alloc, n, 0);
            fill_lcg(got.values(), 2 * n, 0x1F1FULL + n);
            got.values()[2 * (n / 3)] = special;
            std::memcpy(want.values(), got.values(), 2 * n * sizeof(T));
            k.execute(plan.state(), got.data(), FftDirection::Forward);
            plan.execute_reference(Span<Complex<T>>(want.data(), n), FftDirection::Forward);
            usize finite = 0;
            for (usize i = 0; i < n; ++i)
            {
                const Complex<T> a = got.data()[i];
                const Complex<T> b = want.data()[i];
                finite += (std::isfinite(a.re) && std::isfinite(a.im)) ? 1U : 0U;
                finite += (std::isfinite(b.re) && std::isfinite(b.im)) ? 1U : 0U;
            }
            INFO(crd::cpu_level_name(level.level) << " special n=" << n);
            CHECK(finite == 0U);
            CHECK(got.guards_kept());
        }
    }
}

template <typename T> void check_specials_every_level()
{
    crd::memory::TlsfAllocator alloc(usize{1} << 26);
    for (const usize n : {usize{1024}, usize{4096}, usize{65536}})
    {
        const fft::FftPlan<T> plan(&alloc, n);
        check_plan_specials(&alloc, plan);
    }
}

} // namespace

TEST_CASE("fft kernels: the archive holds portable and each body level the build compiles", "[fft][kernels]")
{
    const auto levels = fft::fft_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == CpuLevel::Avx512;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512)));
}

TEST_CASE("fft kernels: every level the host runs matches the reference on every band (f64)", "[fft][kernels]")
{
    check_every_level<crd::f64>();
}

TEST_CASE("fft kernels: every level the host runs matches the reference on every band (f32)", "[fft][kernels]")
{
    check_every_level<crd::f32>();
}

TEST_CASE("fft kernels: every level's batched paths match the reference per column (f64)", "[fft][kernels]")
{
    check_batched_every_level<crd::f64>();
}

TEST_CASE("fft kernels: every level's batched paths match the reference per column (f32)", "[fft][kernels]")
{
    check_batched_every_level<crd::f32>();
}

TEST_CASE("fft kernels: every level keeps the reference's non-finite and subnormal results", "[fft][kernels]")
{
    check_specials_every_level<crd::f64>();
    check_specials_every_level<crd::f32>();
}

// The canonical planner's algorithm of each direction at every size to 8M (fft::detail::planned_forward and
// planned_inverse), one letter a size: Stockham, standalone-hier, IP4-AoS, the four-pass deep-split, the four-step, the
// six-step, the tiled four-step. Every planned forward builds at its size, and a plan built under CrossPlatform
// reports the pair on every level.
TEST_CASE("fft kernels: the planner's algorithm of every size", "[fft]")
{
    const auto algorithm = [](char letter)
    {
        switch (letter)
        {
            case 'h':
                return fft::FftAlgorithm::StandaloneHier;
            case 'I':
                return fft::FftAlgorithm::Ip4;
            case 'D':
                return fft::FftAlgorithm::DeepSplit4;
            case 'F':
                return fft::FftAlgorithm::FourStep;
            case 'X':
                return fft::FftAlgorithm::SixStep;
            case 'T':
                return fft::FftAlgorithm::TiledFourStep;
            default:
                return fft::FftAlgorithm::Stockham;
        }
    };
    // index = log2(n), 2^0 .. 2^23
    constexpr const char* f32_forward = "SSSSSSSSSShIhhIIIDDDTFFF";
    constexpr const char* f32_inverse = "SSSSSSSSSSSISSIIIFFFFFFF";
    constexpr const char* f64_forward = "SSSSSSSSSSIIIIIIIDDDXFFF";
    constexpr const char* f64_inverse = "SSSSSSSSSSIIIIIIIFFFFFFF";
    crd::memory::TlsfAllocator alloc(usize{1} << 26);
    for (u32 lg = 0; lg <= 23; ++lg)
    {
        const usize n = usize{1} << lg;
        INFO("n=" << n);
        CHECK(fft::detail::planned_forward<crd::f32>(n) == algorithm(f32_forward[lg]));
        CHECK(fft::detail::planned_inverse<crd::f32>(n) == algorithm(f32_inverse[lg]));
        CHECK(fft::detail::planned_forward<crd::f64>(n) == algorithm(f64_forward[lg]));
        CHECK(fft::detail::planned_inverse<crd::f64>(n) == algorithm(f64_inverse[lg]));
        CHECK(fft::detail::forward_builds<crd::f32>(n, fft::detail::planned_forward<crd::f32>(n)));
        CHECK(fft::detail::forward_builds<crd::f64>(n, fft::detail::planned_forward<crd::f64>(n)));
        if (lg <= 16)
        {
            const DeterminismScope canonical(crd::math::DeterminismMode::CrossPlatform);
            const fft::FftPlan<crd::f32> plan(&alloc, n);
            CHECK(plan.state().forward == algorithm(f32_forward[lg]));
            CHECK(plan.state().inverse == algorithm(f32_inverse[lg]));
        }
    }
}

namespace
{
// SIMD-DISPATCH's row-close board times every forward fft::detail::forward_builds offers at a size against the planned
// one (bench_fft_f32, bench_fft_vs_refs): each forward here on every level the host runs (the random input, aligned),
// with the planner's inverse. The process's own plan of the size is check_every_level's.
template <typename T> void check_forced_forwards(u32 lg_lo, u32 lg_hi)
{
    crd::memory::TlsfAllocator alloc(usize{1} << 30);
    for (u32 lg = lg_lo; lg <= lg_hi; ++lg)
    {
        const usize n = usize{1} << lg;
        const fft::FftAlgorithm own =
            fft::detail::level_forward<T>(n, fft::fft_kernels().level, crd::math::kernel_determinism());
        for (const fft::FftAlgorithm forward : fft::detail::kFftAlgorithms)
        {
            if (forward == own || !fft::detail::forward_builds<T>(n, forward))
            {
                continue;
            }
            INFO("n=" << n << " forward=" << fft::fft_algorithm_name(forward));
            const fft::FftPlan<T> plan(&alloc, n, forward);
            CHECK(plan.state().forward == forward);
            CHECK(plan.state().inverse == fft::detail::planned_inverse<T>(n));
            check_plan_every_level(&alloc, plan, FftDirection::Forward, false);
        }
    }
}
} // namespace

TEST_CASE("fft kernels: every level runs each f32 forward the planner offers from 1K to 64K", "[fft][kernels]")
{
    check_forced_forwards<crd::f32>(10, 16);
}

TEST_CASE("fft kernels: every level runs each f32 forward the planner offers from 128K to 1M", "[fft][kernels]")
{
    check_forced_forwards<crd::f32>(17, 20);
}

TEST_CASE("fft kernels: every level runs each f32 forward the planner offers at 2M and 4M", "[fft][kernels]")
{
    check_forced_forwards<crd::f32>(21, 22);
}

TEST_CASE("fft kernels: every level runs each f64 forward the planner offers from 1K to 64K", "[fft][kernels]")
{
    check_forced_forwards<crd::f64>(10, 16);
}

TEST_CASE("fft kernels: every level runs each f64 forward the planner offers from 128K to 1M", "[fft][kernels]")
{
    check_forced_forwards<crd::f64>(17, 20);
}

TEST_CASE("fft kernels: every level runs each f64 forward the planner offers at 2M and 4M", "[fft][kernels]")
{
    check_forced_forwards<crd::f64>(21, 22);
}

namespace
{
using crd::math::DeterminismMode;

constexpr CpuLevel kEveryLevel[] = {CpuLevel::Portable, CpuLevel::Sse2,   CpuLevel::Sse42,
                                    CpuLevel::Avx2,     CpuLevel::Avx512, CpuLevel::Neon};

// The board's row of a type, level and size, or null.
const fft::detail::FftLevelPlan* board_row(usize element_bytes, CpuLevel level, u32 log2)
{
    for (const fft::detail::FftLevelPlan& row : fft::detail::kFftLevelPlans)
    {
        if (row.element_bytes == element_bytes && row.level == level && row.log2 == log2)
        {
            return &row;
        }
    }
    return nullptr;
}

// Every level and size to 8M: the deterministic modes plan canonically, Default plans the board's row where it has
// one. Returns how many plans Default changes.
template <typename T> usize default_changes()
{
    usize changed = 0;
    for (const CpuLevel level : kEveryLevel)
    {
        for (u32 lg = 0; lg <= 23; ++lg)
        {
            const usize n = usize{1} << lg;
            INFO(crd::cpu_level_name(level) << " n=" << n << " bytes=" << sizeof(T));
            const fft::FftAlgorithm canonical = fft::detail::planned_forward<T>(n);
            CHECK(fft::detail::level_forward<T>(n, level, DeterminismMode::CrossPlatform) == canonical);
            CHECK(fft::detail::level_forward<T>(n, level, DeterminismMode::BackwardCompat) == canonical);
            const fft::detail::FftLevelPlan* row = board_row(sizeof(T), level, lg);
            const fft::FftAlgorithm own = fft::detail::level_forward<T>(n, level, DeterminismMode::Default);
            CHECK(own == (row != nullptr ? row->forward : canonical));
            changed += own != canonical ? 1U : 0U;
        }
    }
    return changed;
}
} // namespace

// The board's table (level_plans_gen.hpp): one row per type, level and size, each a forward its size builds and of a
// level fft builds a unit for, and switching to a deterministic mode changes a plan exactly where the board found a
// level's winner other than the canonical planner's forward (f64 2K at avx2 among them).
TEST_CASE("fft kernels: each level's board plans, and the canonical plan in the deterministic modes", "[fft]")
{
    usize differing = 0;
    for (const fft::detail::FftLevelPlan& row : fft::detail::kFftLevelPlans)
    {
        const usize n = usize{1} << row.log2;
        INFO(crd::cpu_level_name(row.level) << " n=" << n << " bytes=" << int{row.element_bytes}
                                            << " forward=" << fft::fft_algorithm_name(row.forward));
        REQUIRE((row.element_bytes == 4U || row.element_bytes == 8U));
        const bool f32 = row.element_bytes == 4U;
        CHECK((f32 ? fft::detail::forward_builds<crd::f32>(n, row.forward)
                   : fft::detail::forward_builds<crd::f64>(n, row.forward)));
        const fft::FftAlgorithm canonical =
            f32 ? fft::detail::planned_forward<crd::f32>(n) : fft::detail::planned_forward<crd::f64>(n);
        CHECK(board_row(row.element_bytes, row.level, row.log2) == &row);
        // A row names a level fft builds a unit for (a selection without one runs the unit below and its rows).
        const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
        if ((row.level == CpuLevel::Portable || (x64 && row.level != CpuLevel::Neon)) &&
            static_cast<int>(row.level) <= CRD_TEST_KERNEL_LEVELS_TOP)
        {
            bool built = false;
            for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
            {
                built = built || level.level == row.level;
            }
            CHECK(built);
        }
        differing += row.forward != canonical ? 1U : 0U;
    }
    CHECK(default_changes<crd::f32>() + default_changes<crd::f64>() == differing);
}

// A plan built with no algorithm named reads the process's kernel level and determinism mode.
TEST_CASE("fft kernels: a plan takes its level's board plan under Default and the canonical one otherwise",
          "[fft][kernels]")
{
    crd::memory::TlsfAllocator alloc(usize{1} << 28);
    const CpuLevel level = fft::fft_kernels().level;
    for (const DeterminismMode mode :
         {DeterminismMode::Default, DeterminismMode::CrossPlatform, DeterminismMode::BackwardCompat})
    {
        const DeterminismScope scope(mode);
        for (u32 lg = 0; lg <= 16; ++lg)
        {
            const usize n = usize{1} << lg;
            INFO(crd::cpu_level_name(level) << " mode=" << static_cast<int>(mode) << " n=" << n);
            const fft::FftPlan<crd::f32> single(&alloc, n);
            const fft::FftPlan<crd::f64> wide(&alloc, n);
            CHECK(single.state().forward == fft::detail::level_forward<crd::f32>(n, level, mode));
            CHECK(single.state().inverse == fft::detail::planned_inverse<crd::f32>(n));
            CHECK(wide.state().forward == fft::detail::level_forward<crd::f64>(n, level, mode));
            CHECK(wide.state().inverse == fft::detail::planned_inverse<crd::f64>(n));
        }
    }
    CHECK(crd::math::kernel_determinism() == DeterminismMode::Default);
}

namespace
{
// Under Default each level runs its own board plan of a size: bit-identical run to run, within the ADR-0092 §5 class
// of the radix-2 reference, and within twice that class of portable's own plan (the declared cross-level tolerance);
// under the deterministic modes every level runs the canonical plan and gives portable's bits.
template <typename T> void check_own_plans_every_level()
{
    crd::memory::TlsfAllocator alloc(usize{1} << 30);
    const auto levels = fft::fft_kernel_levels();
    const fft::FftTypeKernels<T>& portable = fft::kernels_of<T>(*levels[0].table);
    for (u32 lg = 10; lg <= 20; ++lg)
    {
        const usize n = usize{1} << lg;
        Buffer<T> input(&alloc, n, 0);
        fill_lcg(input.values(), 2 * n, 0xC0DEULL + n);
        const auto run = [&](const fft::FftTypeKernels<T>& k, const fft::FftPlan<T>& plan)
        {
            Buffer<T> out(&alloc, n, 0);
            std::memcpy(out.values(), input.values(), 2 * n * sizeof(T));
            k.execute(plan.state(), out.data(), FftDirection::Forward);
            return out;
        };
        const fft::FftPlan<T> canonical(&alloc, n, fft::detail::planned_forward<T>(n));
        Buffer<T> want(&alloc, n, 0);
        std::memcpy(want.values(), input.values(), 2 * n * sizeof(T));
        canonical.execute_reference(Span<Complex<T>>(want.data(), n), FftDirection::Forward);
        const fft::FftPlan<T> portable_own(
            &alloc, n, fft::detail::level_forward<T>(n, CpuLevel::Portable, DeterminismMode::Default));
        const Buffer<T> portable_bits = run(portable, portable_own);
        const Buffer<T> canonical_bits = run(portable, canonical);
        for (const KernelLevel<FftKernels>& level : levels)
        {
            if (!kernel_level_runs_here(level))
            {
                continue;
            }
            const fft::FftTypeKernels<T>& k = fft::kernels_of<T>(*level.table);
            const fft::FftPlan<T> own(&alloc, n,
                                      fft::detail::level_forward<T>(n, level.level, DeterminismMode::Default));
            const Buffer<T> got = run(k, own);
            const Buffer<T> again = run(k, own);
            const Buffer<T> exact = run(k, canonical);
            const double rel = rel_error(got.data(), want.data(), n);
            const double across = rel_error(got.data(), portable_bits.data(), n);
            INFO(crd::cpu_level_name(level.level)
                 << " n=" << n << " own=" << fft::fft_algorithm_name(own.state().forward) << " rel=" << rel
                 << " across=" << across);
            CHECK(std::memcmp(got.values(), again.values(), 2 * n * sizeof(T)) == 0);
            CHECK(rel < tolerance<T>(n));
            CHECK(across < 2.0 * tolerance<T>(n));
            CHECK(std::memcmp(exact.values(), canonical_bits.values(), 2 * n * sizeof(T)) == 0);
            CHECK(got.guards_kept());
            CHECK(exact.guards_kept());
        }
    }
}
} // namespace

TEST_CASE("fft kernels: every level's own f32 plan stays within the cross-level tolerance, the canonical one exact",
          "[fft][kernels]")
{
    check_own_plans_every_level<crd::f32>();
}

TEST_CASE("fft kernels: every level's own f64 plan stays within the cross-level tolerance, the canonical one exact",
          "[fft][kernels]")
{
    check_own_plans_every_level<crd::f64>();
}

TEST_CASE("fft kernels: the process runs the level its CPU selection resolves", "[fft][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = fft::fft_kernel_levels();
    const KernelLevel<FftKernels>& chosen = fft::fft_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<FftKernels>(levels, s.selected, s.kernel_features,
                                                                 crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
