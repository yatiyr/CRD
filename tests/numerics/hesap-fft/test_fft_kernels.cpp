// The hesap-fft kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs transforms one plan per band
// of the planner (the leaf codelets, the Stockham passes, IP4-AoS, the standalone-hier and deep-split bands, the
// four-step and six-step) and the batched sub-FFT paths, f64 and f32, both directions, within the ADR-0092 §5
// tolerance of the radix-2 reference, on aligned data and one word off (the four-step's plain-store branch); every
// level gives portable's bits (the family is bit-exact) and is bit-identical run to run; the words around the data
// stay as they were; signed zeros and subnormals are not flushed, and one infinity or NaN makes every output
// non-finite as in the reference.
// crd-hesap-fft-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/fft/fft.hpp>
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

bool runs_here(const KernelLevel<FftKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

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
    Complex<T>* data() { return reinterpret_cast<Complex<T>*>(values()); }
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

template <typename T> void check_every_level()
{
    crd::memory::TlsfAllocator alloc(usize{1} << 30);
    for (u32 lg = 0; lg <= kLog2Max; ++lg)
    {
        const usize n = usize{1} << lg;
        const fft::FftPlan<T> plan(&alloc, n);
        for (const FftDirection dir : {FftDirection::Forward, FftDirection::Inverse})
        {
            // The four-step's NT stores take aligned data; one word in, f64 takes its plain-store branch.
            for (const usize shift : {usize{0}, usize{1}})
            {
                Buffer<T> want(&alloc, n, 0);
                fill_lcg(want.values(), 2 * n, 0x5EEDULL + n);
                plan.execute_reference(Span<Complex<T>>(want.data(), n), dir);
                Buffer<T> portable(&alloc, n, shift);
                usize ran = 0;
                for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
                {
                    if (!runs_here(level))
                    {
                        continue;
                    }
                    const fft::FftTypeKernels<T>& k = fft::kernels_of<T>(*level.table);
                    Buffer<T> got(&alloc, n, shift);
                    Buffer<T> again(&alloc, n, shift);
                    fill_lcg(got.values(), 2 * n, 0x5EEDULL + n);
                    std::memcpy(again.values(), got.values(), 2 * n * sizeof(T));
                    k.execute(plan.state(), got.data(), dir);
                    k.execute(plan.state(), again.data(), dir);
                    if (ran == 0U) // portable, levels[0]: the bits every other level gives
                    {
                        std::memcpy(portable.values(), got.values(), 2 * n * sizeof(T));
                    }
                    ++ran;
                    const double rel = rel_error(got.data(), want.data(), n);
                    INFO(crd::cpu_level_name(level.level)
                         << " n=" << n << " dir=" << static_cast<int>(dir) << " shift=" << shift << " rel=" << rel);
                    CHECK(rel < tolerance<T>(n));
                    CHECK(std::memcmp(got.values(), again.values(), 2 * n * sizeof(T)) == 0);
                    CHECK(std::memcmp(got.values(), portable.values(), 2 * n * sizeof(T)) == 0);
                    CHECK(got.guards_kept());
                }
                CHECK(ran >= 1U); // portable runs everywhere
            }
        }
    }
}

// The batched paths: N=8 (even and odd batch), N=16 inside and past the over-2 cache budget, the batched leaves
// (16..256 at a multiple of the vector width) and the hierarchical sub-FFTs at their block width.
template <typename T> void check_batched_every_level()
{
    crd::memory::TlsfAllocator alloc(usize{1} << 28);
    struct Shape
    {
        usize m;
        usize batch;
    };
    const Shape shapes[] = {
        {8, 6},
        {8, 7},
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
    };
    for (const Shape& shape : shapes)
    {
        const fft::FftPlan<T> plan(&alloc, shape.m);
        const usize total = shape.m * shape.batch;
        for (const FftDirection dir : {FftDirection::Forward, FftDirection::Inverse})
        {
            Array<Complex<T>> input(&alloc);
            input.resize(total);
            fill_lcg(reinterpret_cast<T*>(input.data()), 2 * total, 0xBA7CULL + total);
            Array<Complex<T>> portable(&alloc);
            portable.resize(total);
            usize ran = 0;
            for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
            {
                if (!runs_here(level))
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
                INFO(crd::cpu_level_name(level.level) << " m=" << shape.m << " batch=" << shape.batch
                                                      << " dir=" << static_cast<int>(dir) << " rel=" << worst);
                CHECK(worst < tolerance<T>(shape.m));
                CHECK(std::memcmp(got.data(), portable.data(), total * sizeof(Complex<T>)) == 0);
                CHECK(got.guards_kept());
            }
        }
    }
}

// Special inputs. Only zeros, signed zeros and subnormals: every sum is exact there and only a twiddle product rounds,
// so a level stays near the reference and gives portable's bits, and a flush-to-zero (an error the size of the result)
// shows. One infinity or one NaN: every output depends on it, so every complex output is non-finite, on a level as in
// the reference (NaN payloads may differ across levels).
template <typename T> void check_specials_every_level()
{
    crd::memory::TlsfAllocator alloc(usize{1} << 26);
    const T inf = std::numeric_limits<T>::infinity();
    const T nan = std::numeric_limits<T>::quiet_NaN();
    const T tiny = std::numeric_limits<T>::denorm_min();
    const fft::FftTypeKernels<T>& portable = fft::kernels_of<T>(*fft::fft_kernel_levels()[0].table);
    for (const KernelLevel<FftKernels>& level : fft::fft_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        const fft::FftTypeKernels<T>& k = fft::kernels_of<T>(*level.table);
        for (const usize n : {usize{1024}, usize{4096}, usize{65536}})
        {
            const fft::FftPlan<T> plan(&alloc, n);
            {
                Buffer<T> got(&alloc, n, 0);
                Buffer<T> want(&alloc, n, 0);
                for (usize i = 0; i < 2 * n; ++i)
                {
                    const T pick[] = {T(0), -T(0), tiny, -tiny * T(3)};
                    got.values()[i] = pick[i % 4U];
                }
                std::memcpy(want.values(), got.values(), 2 * n * sizeof(T));
                Buffer<T> bits(&alloc, n, 0);
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
                INFO(crd::cpu_level_name(level.level)
                     << " subnormals n=" << n << " err=" << maxerr << " ref=" << maxref);
                CHECK(maxref > 0.0);
                CHECK(maxerr <= 0.25 * maxref);
                CHECK(std::memcmp(got.values(), bits.values(), 2 * n * sizeof(T)) == 0);
                CHECK(got.guards_kept());
            }
            for (const T special : {inf, nan})
            {
                Buffer<T> got(&alloc, n, 0);
                Buffer<T> want(&alloc, n, 0);
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
}

} // namespace

TEST_CASE("fft kernels: the archive holds portable and each body level the build compiles", "[fft][kernels]")
{
    const auto levels = fft::fft_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool sse2 = false;
    bool avx2 = false;
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
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
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
