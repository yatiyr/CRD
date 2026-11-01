// The hesap-stats kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/stats/counter_rng_core.hpp>
#include <crd/hesap/stats/stats_dispatch.hpp>

#include <bit>

namespace crd::hesap::stats
{
namespace
{

using crd::f64;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;

// CRD_KERNELS_STATS: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_STATS & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_STATS, PORTABLE), "the portable level is always built");

constexpr KernelLevel<StatsKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kStatsKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_STATS, SSE2)
    {crd::CpuLevel::Sse2, &kStatsKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_STATS, AVX2)
    {crd::CpuLevel::Avx2, &kStatsKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_STATS, AVX512)
    {crd::CpuLevel::Avx512, &kStatsKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// The generators' answers are their scalar block functions evaluated at compile time (counter_rng_core.hpp, whose
// zero-key blocks the generator tests hold to the Random123 known answers). 93 Philox blocks run every group loop at
// every level (avx512: 48, two 16s, 8, 4 and a scalar block; avx2: three 24s, two 8s, 4 and one; sse2: five 16s, three
// 4s and one), and the position crosses 2^32 at block 61, inside a group at each level. Threefry runs 47 blocks
// (avx512: 24, two 8s, 4 and a 3; avx2: three 12s, two 4s and a 3; sse2: six 7s, a 3 and two 1s), then 45 for the
// one-block tail (avx512: 24, two 8s, 4 and a 1; avx2: three 12s, two 4s and a 1), and its counter carries out of its
// low word at block 19, inside a group at each level.
constexpr usize kPhiloxBlocks = 93U;
constexpr u64 kPhiloxSeed = 0x243F6A8885A308D3ULL;
constexpr u64 kPhiloxStream = 0x0123456789ABCDEFULL;
constexpr u64 kPhiloxFirst = (u64{1} << 32U) - 61U;
constexpr usize kThreefryBlocks = 47U;
constexpr usize kThreefryShort = 45U;
constexpr u64 kThreefryKey[4] = {0x13198A2E03707344ULL, 0xA4093822299F31D0ULL, 0x082EFA98EC4E6C89ULL,
                                 0x452821E638D01377ULL};
constexpr u64 kThreefryCounter[4] = {~u64{0} - 18U, ~u64{0}, 0xBE5466CF34E90C6CULL, 0U};

struct GeneratorAnswers
{
    u32 philox[kPhiloxBlocks * 4U];
    u64 threefry[kThreefryBlocks * 4U];
};

constexpr GeneratorAnswers make_generator_answers() noexcept
{
    GeneratorAnswers a{};
    const u32 key[2] = {static_cast<u32>(kPhiloxSeed), static_cast<u32>(kPhiloxSeed >> 32U)};
    for (usize b = 0U; b < kPhiloxBlocks; ++b)
    {
        const u64 position = kPhiloxFirst + b;
        const u32 counter[4] = {static_cast<u32>(position), static_cast<u32>(position >> 32U),
                                static_cast<u32>(kPhiloxStream), static_cast<u32>(kPhiloxStream >> 32U)};
        const PhiloxBlock block = philox4x32(counter, key);
        for (usize w = 0U; w < 4U; ++w)
        {
            a.philox[(4U * b) + w] = block.v[w];
        }
    }
    u64 ctr[4] = {kThreefryCounter[0], kThreefryCounter[1], kThreefryCounter[2], kThreefryCounter[3]};
    for (usize b = 0U; b < kThreefryBlocks; ++b)
    {
        const ThreefryBlock block = threefry4x64(ctr, kThreefryKey);
        for (usize w = 0U; w < 4U; ++w)
        {
            a.threefry[(4U * b) + w] = block.v[w];
        }
        threefry_counter_add(ctr, 1U);
    }
    return a;
}

constexpr GeneratorAnswers kGeneratorAnswers = make_generator_answers();

// The Student-t score sum over 15 elements (avx512: an eight-term step, a four-term step and three; elsewhere three
// four-term steps and three) with nu = 4, so 1/nu and (nu + 1) / (2 nu) are exact. Every level keeps the same four
// lane sums in the same order, so the answer is the portable table's bits, 0.15 ulp from mpmath at 200 bits.
constexpr usize kStudentCount = 15U;
constexpr f64 kStudentX[kStudentCount] = {-2.5, 0.5,  1.25, -0.75, 3.0,  0.125, -1.5, 2.25,
                                          -4.0, 0.875, 1.75, -0.25, 6.5, -3.25, 1.0};
constexpr f64 kStudentNu = 4.0;
constexpr u64 kStudentBits = 0xBFF691BE7BA65CC2ULL; // -1.4105820493767145

bool philox_known(const StatsKernels& table, usize blocks) noexcept
{
    u32 out[kPhiloxBlocks * 4U];
    table.philox4x32_blocks(kPhiloxSeed, kPhiloxStream, kPhiloxFirst, out, blocks);
    for (usize i = 0U; i < 4U * blocks; ++i)
    {
        if (out[i] != kGeneratorAnswers.philox[i])
        {
            return false;
        }
    }
    return true;
}

bool threefry_known(const StatsKernels& table, usize blocks) noexcept
{
    u64 out[kThreefryBlocks * 4U];
    table.threefry4x64_blocks(kThreefryKey, kThreefryCounter, out, blocks);
    for (usize i = 0U; i < 4U * blocks; ++i)
    {
        if (out[i] != kGeneratorAnswers.threefry[i])
        {
            return false;
        }
    }
    return true;
}

void resolve_stats() noexcept
{
    (void)stats_kernels();
}

constinit crd::KernelFamily g_family{"hesap.stats", &resolve_stats, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* stats_self_check(const StatsKernels& table) noexcept
{
    if (!philox_known(table, kPhiloxBlocks))
    {
        return "philox4x32_blocks";
    }
    if (!threefry_known(table, kThreefryBlocks) || !threefry_known(table, kThreefryShort))
    {
        return "threefry4x64_blocks";
    }
    if (std::bit_cast<u64>(table.student_t_nu_score_sum(kStudentX, kStudentCount, kStudentNu)) != kStudentBits)
    {
        return "student_t_nu_score_sum";
    }
    return nullptr;
}

std::span<const KernelLevel<StatsKernels>> stats_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<StatsKernels>& stats_kernels() noexcept
{
    static const KernelLevel<StatsKernels>& selected =
        crd::process_checked_kernel_level<StatsKernels>(kLevels, &stats_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::stats
