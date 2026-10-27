// The hesap-stats kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the scalar block
// functions' bits for the Philox and Threefry fills, across the counters' word wraps, and the portable level's bits for
// the Student-t score reduction. crd-hesap-stats-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/stats/stats.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

namespace st = crd::hesap::stats;
using crd::CpuLevel;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::KernelLevel;
using st::StatsKernels;

namespace
{

constexpr usize kMaxBlocks = 37; // every block count 0..37 meets each body's vector loop and its remainder

bool runs_here(const KernelLevel<StatsKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

// Philox blocks [first, first + blocks) from the scalar block function, in next_u64 order.
void philox_reference(u64 seed, u64 stream, u64 first, u64* out, usize blocks)
{
    const u32 key[2] = {static_cast<u32>(seed), static_cast<u32>(seed >> 32)};
    for (usize b = 0; b < blocks; ++b)
    {
        const u64 p = first + b;
        const u32 counter[4] = {static_cast<u32>(p), static_cast<u32>(p >> 32), static_cast<u32>(stream),
                                static_cast<u32>(stream >> 32)};
        const st::PhiloxBlock block = st::philox4x32(counter, key);
        out[2U * b] = (static_cast<u64>(block.v[1]) << 32) | block.v[0];
        out[2U * b + 1U] = (static_cast<u64>(block.v[3]) << 32) | block.v[2];
    }
}

// Threefry blocks counter .. counter + blocks - 1 from the scalar block function, carrying through every word.
void threefry_reference(const u64 (&key)[4], const u64 (&counter)[4], u64* out, usize blocks)
{
    u64 ctr[4] = {counter[0], counter[1], counter[2], counter[3]};
    for (usize b = 0; b < blocks; ++b)
    {
        const st::ThreefryBlock block = st::threefry4x64(ctr, key);
        for (usize w = 0; w < 4U; ++w)
        {
            out[4U * b + w] = block.v[w];
        }
        ctr[0] += 1U;
        for (usize w = 1; w < 4U && ctr[w - 1U] == 0U; ++w)
        {
            ctr[w] += 1U;
        }
    }
}

} // namespace

TEST_CASE("stats kernels: the archive holds portable and each body level the build compiles", "[stats][kernels]")
{
    const auto levels = st::stats_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool avx2 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
}

TEST_CASE("stats kernels: every level the host runs gives the scalar Philox and Threefry blocks", "[stats][kernels]")
{
    // Positions straddling the 2^32 word wrap of the Philox counter and the 2^64 wrap of the position.
    const u64 philox_firsts[] = {0U, 1U, 0xFFFFFFFCULL, 0xFFFFFFFFFFFFFFFAULL, 0x123456789ABCDEFULL};
    // Threefry counters straddling the carries out of word 0, of words 0 and 1, and of words 0 to 2.
    const u64 threefry_counters[][4] = {{0U, 0U, 0U, 0U},
                                        {0xFFFFFFFFFFFFFFFEULL, 0U, 0U, 0U},
                                        {0xFFFFFFFFFFFFFFFDULL, 0xFFFFFFFFFFFFFFFFULL, 7U, 0U},
                                        {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 3U},
                                        {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 1U, 2U}};
    const u64 key[4] = {0xABCDEF0123456789ULL, 0x0F1E2D3C4B5A6978ULL, 0x1111111111111111ULL, 0x9E3779B97F4A7C15ULL};
    usize ran = 0;
    for (const KernelLevel<StatsKernels>& level : st::stats_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        const StatsKernels& k = *level.table;
        usize philox_bad = 0;
        usize threefry_bad = 0;
        for (usize blocks = 0; blocks <= kMaxBlocks; ++blocks)
        {
            for (const u64 first : philox_firsts)
            {
                u64 got[2U * kMaxBlocks + 1U];
                u64 want[2U * kMaxBlocks + 1U];
                got[2U * blocks] = 0x5A5A5A5A5A5A5A5AULL; // a guard word past the blocks
                k.philox4x32_blocks(0xDEADBEEFCAFEF00DULL, 0x0123456789ABCDEFULL, first, got, blocks);
                philox_reference(0xDEADBEEFCAFEF00DULL, 0x0123456789ABCDEFULL, first, want, blocks);
                for (usize w = 0; w < 2U * blocks; ++w)
                {
                    philox_bad += got[w] == want[w] ? 0U : 1U;
                }
                philox_bad += got[2U * blocks] == 0x5A5A5A5A5A5A5A5AULL ? 0U : 1U;
            }
            for (const auto& counter : threefry_counters)
            {
                u64 got[4U * kMaxBlocks + 1U];
                u64 want[4U * kMaxBlocks + 1U];
                got[4U * blocks] = 0x5A5A5A5A5A5A5A5AULL;
                k.threefry4x64_blocks(key, counter, got, blocks);
                threefry_reference(key, counter, want, blocks);
                for (usize w = 0; w < 4U * blocks; ++w)
                {
                    threefry_bad += got[w] == want[w] ? 0U : 1U;
                }
                threefry_bad += got[4U * blocks] == 0x5A5A5A5A5A5A5A5AULL ? 0U : 1U;
            }
        }
        CHECK(philox_bad == 0U);
        CHECK(threefry_bad == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

TEST_CASE("stats kernels: every level the host runs gives the portable Student-t score bits", "[stats][kernels]")
{
    // Zeros of both signs, tiny and huge magnitudes, and a spread of ordinary values, from an odd offset.
    f64 xs[203];
    st::PhiloxRng rng(2026, 10);
    for (usize i = 0; i < 203U; ++i)
    {
        xs[i] = (rng.next_f64() - 0.5) * 12.0;
    }
    xs[3] = 0.0;
    xs[4] = -0.0;
    xs[10] = 1e-160;
    xs[11] = -3e-12;
    xs[17] = 1e140;
    xs[30] = -7.5e75;
    const f64 nus[] = {0.3, 1.0, 5.0, 31.7, 1e6};
    const StatsKernels& portable = *st::stats_kernel_levels()[0].table;
    usize ran = 0;
    for (const KernelLevel<StatsKernels>& level : st::stats_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        usize bad = 0;
        for (const f64 nu : nus)
        {
            for (usize n = 0; n <= 41U; ++n)
            {
                const u64 got = std::bit_cast<u64>(level.table->student_t_nu_score_sum(xs + 1, n, nu));
                const u64 want = std::bit_cast<u64>(portable.student_t_nu_score_sum(xs + 1, n, nu));
                bad += got == want ? 0U : 1U;
            }
            const u64 got = std::bit_cast<u64>(level.table->student_t_nu_score_sum(xs + 1, 202U, nu));
            const u64 want = std::bit_cast<u64>(portable.student_t_nu_score_sum(xs + 1, 202U, nu));
            bad += got == want ? 0U : 1U;
        }
        CHECK(bad == 0U);
    }
    CHECK(ran >= 1U);
}

TEST_CASE("stats kernels: the process runs the level its CPU selection resolves", "[stats][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = st::stats_kernel_levels();
    const KernelLevel<StatsKernels>& chosen = st::stats_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<StatsKernels>(levels, s.selected, s.kernel_features,
                                                                   crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
