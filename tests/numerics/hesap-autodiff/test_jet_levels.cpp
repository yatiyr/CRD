// SIMD-DISPATCH (ADR-0134 §5, §7): the forward jets (crd/hesap/autodiff/jet_simd.hpp) take the register layout of the
// CPU level of the unit that compiles them and give the same bits on every level. kernels/jet_level_probe.cpp is
// compiled once per level; each level the host executes runs every operation, slope and driver over the same inputs as
// the portable level, in this one process, and general code's jets give those bits too. The scalar Jet<f64, N> is the
// oracle on well-conditioned inputs. With the canned gradients' table (crd/hesap/autodiff/autodiff_dispatch.hpp) they
// are [kernels] cases, which the forced-level arms run once per level and the emulated avx512 arm under SDE.

#include "jet_level_probe.hpp"
#include "jet_probe_ops.hpp"
#include "kernel_levels.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/autodiff/autodiff_dispatch.hpp>
#include <crd/hesap/autodiff/canned_functions.hpp>
#include <crd/hesap/autodiff/jet.hpp>
#include <crd/hesap/autodiff/jet_simd.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/backend.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace probe = crd::test::jet_probe;
namespace ad = crd::hesap::autodiff;
using crd::CpuLevel;
using crd::f64;
using crd::u64;
using crd::usize;

namespace
{

struct Level
{
    CpuLevel level;
    const probe::Table* table;
};

// CRD_KERNELS_JET_PROBE: the levels crd_add_cpu_kernels built (CMakeLists.txt).
constexpr Level kLevels[] = {
    {CpuLevel::Portable, &probe::kJetProbePortable},
#if CRD_KERNEL_BUILT(CRD_KERNELS_JET_PROBE, SSE2)
    {CpuLevel::Sse2, &probe::kJetProbeSse2},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_JET_PROBE, AVX2)
    {CpuLevel::Avx2, &probe::kJetProbeAvx2},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_JET_PROBE, AVX512)
    {CpuLevel::Avx512, &probe::kJetProbeAvx512},
#endif
};

using crd::test::kernel_level_runs_here;

// A quiet NaN with a payload no operation produces: the words a probe must leave as they were.
constexpr u64 kSentinel = 0x7FF8DEADBEEF1234ULL;

void fill_sentinel(f64* out, usize n)
{
    for (usize i = 0; i < n; ++i)
    {
        out[i] = std::bit_cast<f64>(kSentinel);
    }
}

// Bit-equal, or both NaN (IEEE 754 leaves a two-NaN operation's payload open).
bool same(f64 x, f64 y)
{
    const u64 a = std::bit_cast<u64>(x);
    const u64 b = std::bit_cast<u64>(y);
    constexpr u64 inf = 0x7FF0000000000000ULL;
    return a == b || ((a & ~(1ULL << 63U)) > inf && (b & ~(1ULL << 63U)) > inf);
}

// Signed zeros, subnormals, the extremes, infinities, NaNs with payloads, the exp overflow and underflow edges, π/2.
constexpr u64 kSpecials[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x3FF0000000000000ULL, 0xBFF8000000000000ULL, 0x0000000000000001ULL,
    0x800FFFFFFFFFFFFFULL, 0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
    0x7FF8000000000000ULL, 0xFFF5A5A5A5A5A5A5ULL, 0x4086300000000000ULL, 0xC087480000000000ULL, 0x01A56E1FC2F8F359ULL,
    0x3FF921FB54442D18ULL, 0x4000000000000000ULL, 0xC000000000000000ULL,
};
constexpr usize kSpecialCount = sizeof(kSpecials) / sizeof(kSpecials[0]);
constexpr usize kWellConditionedSets = 8;
constexpr usize kInputSets = kWellConditionedSets + 3 * kSpecialCount;

struct Lcg
{
    u64 state;
    u64 next()
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        return state >> 11U;
    }
    f64 uni(f64 lo, f64 hi) { return lo + ((hi - lo) * static_cast<f64>(next()) * 0x1p-53); }
};

f64 special(usize i)
{
    return std::bit_cast<f64>(kSpecials[i % kSpecialCount]);
}

// Sets below kWellConditionedSets keep a and b in [0.3, 1.5] (every slope finite); the others put each special in a,
// in b and in the partials, beside ordinary values.
probe::Input make_input(usize set)
{
    Lcg rng{0x5DEECE66DULL + set};
    probe::Input in{};
    in.a = rng.uni(0.3, 1.2);
    in.b = rng.uni(0.5, 1.5);
    in.s = rng.uni(0.5, 2.0);
    for (int k = 0; k < probe::kMaxN; ++k)
    {
        in.da[k] = rng.uni(-1.0, 1.0);
        in.db[k] = rng.uni(-1.0, 1.0);
    }
    if (set < kWellConditionedSets)
    {
        return in;
    }
    const usize i = set - kWellConditionedSets;
    const usize kind = i / kSpecialCount;
    if (kind == 0)
    {
        in.a = special(i);
        in.s = special(i + 5U);
    }
    else if (kind == 1)
    {
        in.b = special(i);
        in.a = special(i + 3U);
    }
    for (int k = 0; k < probe::kMaxN; k += 3)
    {
        in.da[k] = special(i + static_cast<usize>(k));
        in.db[k + 1 < probe::kMaxN ? k + 1 : k] = special(i + static_cast<usize>(k) + 7U);
    }
    return in;
}

// The scalar Jet<f64, N> oracle of every operation, in the probe's layout.
template <int N> usize jet_ops(const probe::Input& in, f64* out)
{
    ad::forward::Jet<f64, N> a(in.a);
    ad::forward::Jet<f64, N> b(in.b);
    for (int k = 0; k < N; ++k)
    {
        a.v[k] = in.da[k];
        b.v[k] = in.db[k];
    }
    return probe::ops_with<N>(a, b, in.s, out);
}

template <int... N> usize jet_ops_all(const probe::Input& in, f64* out)
{
    usize at = 0;
    ((at += jet_ops<N>(in, out + at)), ...);
    return at;
}

// The guard words after each result of the ops layout still hold the sentinel.
bool guards_hold(const f64* out)
{
    usize at = 0;
    for (const int n : probe::kSizes)
    {
        for (usize op = 0; op < probe::kOpCount; ++op)
        {
            at += 1 + static_cast<usize>(n);
            for (usize g = 0; g < probe::kGuard; ++g)
            {
                if (std::bit_cast<u64>(out[at + g]) != kSentinel)
                {
                    return false;
                }
            }
            at += probe::kGuard;
        }
    }
    return at == probe::kOpWords;
}

// The bytes of a level's widest register (crd/math/simd/backend.hpp).
crd::u32 widest_register(CpuLevel level)
{
    if (level == CpuLevel::Avx512)
    {
        return 64U;
    }
    if (level == CpuLevel::Avx2)
    {
        return 32U;
    }
    return 16U;
}

// The expected layout of JetPackD<n> on a level whose widest register has `widest` bytes: a Vec8d for every eight
// partials at avx512 (a Vec4d for the last four or fewer), else Vec4d registers; each aligned to min(its size, widest).
probe::Layout expected_layout(crd::u32 widest, int n)
{
    const bool avx512 = widest == 64U;
    probe::Layout l{};
    for (int left = n; left > 0;)
    {
        const int width = avx512 && left > 4 ? 8 : 4;
        ++l.regs;
        l.lanes += static_cast<crd::u32>(width);
        left -= width;
    }
    const crd::u32 head = avx512 && n > 4 ? 64U : 32U;
    l.align = head < widest ? head : widest;
    const crd::u32 bytes = l.lanes * 8U;
    l.size = l.align + (((bytes + l.align - 1U) / l.align) * l.align);
    return l;
}

} // namespace

TEST_CASE("jets: each level's unit lays out its own registers", "[autodiff][jet-levels][kernels]")
{
    REQUIRE(kLevels[0].level == CpuLevel::Portable);
    for (const Level& level : kLevels)
    {
        INFO("level " << crd::cpu_level_name(level.level));
        for (usize i = 0; i < probe::kSizeCount; ++i)
        {
            INFO("N " << probe::kSizes[i]);
            const probe::Layout want = expected_layout(widest_register(level.level), probe::kSizes[i]);
            const probe::Layout& got = level.table->layout[i];
            CHECK(got.regs == want.regs);
            CHECK(got.lanes == want.lanes);
            CHECK(got.align == want.align);
            CHECK(got.size == want.size);
        }
    }
    // General code's jets follow its own level the same way.
    const auto general = static_cast<crd::u32>(crd::math::simd::kNativeRegisterBytes);
    const probe::Layout want = expected_layout(general, 9);
    const probe::Layout got = probe::layout<9>();
    CHECK(got.regs == want.regs);
    CHECK(got.lanes == want.lanes);
    CHECK(got.align == want.align);
    CHECK(got.size == want.size);
}

TEST_CASE("jets: every level the host runs gives the portable bits", "[autodiff][jet-levels][kernels]")
{
    static f64 ref[probe::kOpWords];
    static f64 got[probe::kOpWords];
    static f64 oracle[probe::kOpWords];
    const probe::Table& portable = *kLevels[0].table;
    usize compared = 0;
    for (usize set = 0; set < kInputSets; ++set)
    {
        INFO("input set " << set);
        const probe::Input in = make_input(set);
        fill_sentinel(ref, probe::kOpWords);
        REQUIRE(portable.ops(in, ref) == probe::kOpWords);
        REQUIRE(guards_hold(ref));
        for (const Level& level : kLevels)
        {
            if (!kernel_level_runs_here(level.level))
            {
                continue;
            }
            INFO("level " << crd::cpu_level_name(level.level));
            fill_sentinel(got, probe::kOpWords);
            REQUIRE(level.table->ops(in, got) == probe::kOpWords);
            CHECK(guards_hold(got));
            usize bad = 0;
            for (usize w = 0; w < probe::kOpWords; ++w)
            {
                bad += same(got[w], ref[w]) ? 0U : 1U;
            }
            CHECK(bad == 0U);
            ++compared;
        }
        // General code's jets.
        fill_sentinel(got, probe::kOpWords);
        REQUIRE(probe::ops(in, got) == probe::kOpWords);
        usize bad = 0;
        for (usize w = 0; w < probe::kOpWords; ++w)
        {
            bad += same(got[w], ref[w]) ? 0U : 1U;
        }
        CHECK(bad == 0U);
        // The scalar Jet oracle (mul+add where the pack rounds an fma once) on the well-conditioned sets.
        if (set < kWellConditionedSets)
        {
            fill_sentinel(oracle, probe::kOpWords);
            REQUIRE(jet_ops_all<1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32>(in, oracle) == probe::kOpWords);
            usize off = 0;
            for (usize w = 0; w < probe::kOpWords; ++w)
            {
                if (std::bit_cast<u64>(oracle[w]) == kSentinel)
                {
                    continue;
                }
                const f64 scale = std::fabs(oracle[w]) > 1.0 ? std::fabs(oracle[w]) : 1.0;
                off += std::fabs(ref[w] - oracle[w]) <= 1e-12 * scale ? 0U : 1U;
            }
            CHECK(off == 0U);
        }
    }
    CHECK(compared >= kInputSets); // portable runs everywhere
}

TEST_CASE("jets: the drivers give the portable bits on every level and tile", "[autodiff][jet-levels][kernels]")
{
    f64 x[probe::kMaxN];
    for (int i = 0; i < probe::kMaxN; ++i)
    {
        x[i] = 0.15 + (0.07 * static_cast<f64>(i)) - (i % 3 == 0 ? 0.6 : 0.0);
    }
    static f64 ref[probe::kDriverWords];
    static f64 got[probe::kDriverWords];
    fill_sentinel(ref, probe::kDriverWords);
    const usize words = kLevels[0].table->drivers(x, ref);
    REQUIRE(words <= probe::kDriverWords);
    const auto check = [&](const char* who)
    {
        INFO(who);
        usize bad = 0;
        for (usize w = 0; w < probe::kDriverWords; ++w)
        {
            bad += same(got[w], ref[w]) ? 0U : 1U; // the guard words and the words past the end included
        }
        CHECK(bad == 0U);
    };
    for (const Level& level : kLevels)
    {
        if (!kernel_level_runs_here(level.level))
        {
            continue;
        }
        fill_sentinel(got, probe::kDriverWords);
        CHECK(level.table->drivers(x, got) == words);
        check(crd::cpu_level_name(level.level));
    }
    fill_sentinel(got, probe::kDriverWords);
    CHECK(probe::drivers(x, got) == words);
    check("general code");
    // The guard words after each output (the first gradient's: n = 1).
    for (usize g = 0; g < probe::kGuard; ++g)
    {
        CHECK(std::bit_cast<u64>(ref[1 + g]) == kSentinel);
    }
    CHECK(std::bit_cast<u64>(ref[words - 1U]) == kSentinel);
}

TEST_CASE("jets: the canned gradients' table holds portable and each body level the build compiles",
          "[autodiff][kernels]")
{
    const auto levels = ad::autodiff_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(levels[i].table->gradient_f64 != nullptr);
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
    const crd::CpuSelection& s = crd::cpu_selection();
    CHECK(&ad::autodiff_kernels() == &crd::math::select_kernel_level<ad::AutodiffKernels>(
                                         levels, s.selected, s.kernel_features, crd::math::kernel_level_cap()));
}

TEST_CASE("jets: every canned gradient level the host runs gives general code's bits", "[autodiff][kernels]")
{
    f64 x[ad::kCannedMaxN + 1];
    for (int i = 0; i <= ad::kCannedMaxN; ++i)
    {
        x[i] = 0.4 - (0.09 * static_cast<f64>(i)) + (i % 2 == 0 ? 0.0 : 0.37);
    }
    constexpr int lengths[] = {1, 2, 5, 8, 9, 16, 31, 32};
    constexpr ad::CannedFunction functions[] = {ad::CannedFunction::Rosenbrock, ad::CannedFunction::Sphere,
                                                ad::CannedFunction::Cubes};
    for (const ad::CannedFunction f : functions)
    {
        for (const int n : lengths)
        {
            INFO("function " << static_cast<int>(f) << ", n " << n);
            f64 want[ad::kCannedMaxN + 1];
            ad::forward::JetPackD<8> scratch[ad::kCannedMaxN];
            ad::canned::visit(f, [&](const auto& fn) { ad::forward::gradient(fn, x, n, want, scratch); });
            for (const auto& level : ad::autodiff_kernel_levels())
            {
                if (!kernel_level_runs_here(level))
                {
                    continue;
                }
                INFO("level " << crd::cpu_level_name(level.level));
                f64 got[ad::kCannedMaxN + 1];
                got[n] = std::bit_cast<f64>(kSentinel);
                level.table->gradient_f64(f, x, n, got);
                usize bad = 0;
                for (int k = 0; k < n; ++k)
                {
                    bad += same(got[k], want[k]) ? 0U : 1U;
                }
                CHECK(bad == 0U);
                CHECK(std::bit_cast<u64>(got[n]) == kSentinel);
            }
        }
    }
}
