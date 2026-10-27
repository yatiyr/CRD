// The hesap-special kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the out-of-line
// scalar Lanczos lgamma's and tgamma's bits, for the half-plane lanes and for the reflection, pole, NaN and infinity
// lanes mixed into any vector, over every length and offset that meets the vector loop and its tail.
// crd-hesap-special-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/special/detail/lanczos_core.hpp>
#include <crd/hesap/special/special.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace sf = crd::hesap::special;
using crd::CpuLevel;
using crd::f64;
using crd::u64;
using crd::usize;
using crd::math::KernelLevel;
using sf::SpecialKernels;

namespace
{

constexpr usize kInputs = 211;
constexpr u64 kGuard = 0x5A5A5A5A5A5A5A5AULL;

bool runs_here(const KernelLevel<SpecialKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

// Ordinary arguments of both half-planes, with the edge cases spread so that every vector position meets them.
void fill_inputs(f64 (&xs)[kInputs])
{
    u64 s = 0x9E3779B97F4A7C15ULL;
    for (usize i = 0; i < kInputs; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
        xs[i] = (u - 0.25) * 60.0; // [-15, 45)
    }
    const f64 inf = std::numeric_limits<f64>::infinity();
    const f64 edges[] = {0.5,    0.49999999999999994,
                         1.0,    2.0,
                         0.0,    -0.0,
                         -1.0,   -7.0,
                         -2.5,   -0.5,
                         1e-300, -1e-300,
                         5e-324, -1e-17,
                         171.6,  172.0,
                         1e306,  1.7976931348623157e308,
                         inf,    -inf,
                         0.75,   3.0e5,
                         -170.5, std::numeric_limits<f64>::quiet_NaN()};
    usize at = 1;
    for (const f64 e : edges)
    {
        xs[at] = e;
        at += 7; // co-prime with 4, so the edges land in every lane
    }
}

} // namespace

TEST_CASE("special kernels: the archive holds portable and each body level the build compiles", "[special][kernels]")
{
    const auto levels = sf::special_kernel_levels();
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

TEST_CASE("special kernels: every level the host runs gives the scalar Lanczos lgamma and tgamma bits",
          "[special][kernels]")
{
    f64 xs[kInputs];
    fill_inputs(xs);
    usize ran = 0;
    for (const KernelLevel<SpecialKernels>& level : sf::special_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        const SpecialKernels& k = *level.table;
        usize lgamma_bad = 0;
        usize tgamma_bad = 0;
        usize guard_bad = 0;
        for (usize offset = 0; offset < 4U; ++offset)
        {
            for (usize n = 0; offset + n < kInputs; n += (n < 41U ? 1U : 23U))
            {
                f64 lg[kInputs + 1];
                f64 tg[kInputs + 1];
                lg[n] = std::bit_cast<f64>(kGuard);
                tg[n] = std::bit_cast<f64>(kGuard);
                k.lgamma_lanczos(lg, xs + offset, n);
                k.tgamma_lanczos(tg, xs + offset, n);
                for (usize i = 0; i < n; ++i)
                {
                    const f64 x = xs[offset + i];
                    const u64 lgamma_want = std::bit_cast<u64>(sf::detail::lanczos_lgamma(x));
                    const u64 tgamma_want = std::bit_cast<u64>(sf::detail::lanczos_tgamma(x));
                    lgamma_bad += std::bit_cast<u64>(lg[i]) == lgamma_want ? 0U : 1U;
                    tgamma_bad += std::bit_cast<u64>(tg[i]) == tgamma_want ? 0U : 1U;
                }
                guard_bad += std::bit_cast<u64>(lg[n]) == kGuard ? 0U : 1U;
                guard_bad += std::bit_cast<u64>(tg[n]) == kGuard ? 0U : 1U;
            }
        }
        CHECK(lgamma_bad == 0U);
        CHECK(tgamma_bad == 0U);
        CHECK(guard_bad == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

TEST_CASE("special kernels: the process runs the level its CPU selection resolves", "[special][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = sf::special_kernel_levels();
    const KernelLevel<SpecialKernels>& chosen = sf::special_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<SpecialKernels>(levels, s.selected, s.kernel_features,
                                                                     crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
