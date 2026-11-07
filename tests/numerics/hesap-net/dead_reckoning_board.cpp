// crd-hesap-dead-reckoning-board: nanoseconds per entity of hesap-net's dead reckoning at each kernel level the host
// runs, against KDIS's DeadReckoningCalculator stepping its Reset entities, on the same updates. Each arm's position is
// compared with the peer's within the peer's single precision, so a faster wrong answer fails. KDIS also turns the
// entity's Euler angles on the rotating algorithms; hesap turns the quaternion.
//
// Usage: crd-hesap-dead-reckoning-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round over few entities (the smoke CTest)

#include "dead_reckoning_peers.hpp"
#include "kernel_levels.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/dead_reckoning.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hn = crd::hesap::net;
using crd::f64;
using crd::u32;
using crd::usize;
using dead_reckoning_peers::KdisFleet;
using dead_reckoning_peers::Update;
using hn::DeadReckoningAlgorithm;
using hn::MotionKernels;

namespace
{

template <typename T> using Array = crd::containers::Array<T>;
using Clock = std::chrono::steady_clock;

volatile f64 g_sink = 0.0;

[[nodiscard]] f64 unit_value(SeededWords& words) noexcept
{
    return static_cast<f64>(words.next_u32() >> 4U) / 268435456.0;
}

[[nodiscard]] float signed_float(SeededWords& words, f64 scale) noexcept
{
    return static_cast<float>(((2.0 * unit_value(words)) - 1.0) * scale);
}

struct Fleet
{
    Array<f64> columns[17];
    Array<float> dt;

    explicit Fleet(usize count)
    {
        for (Array<f64>& column : columns)
        {
            column.resize(count);
        }
        dt.resize(count);
    }

    [[nodiscard]] hn::DeadReckoningInput input() const noexcept
    {
        return {{columns[0].data(), columns[1].data(), columns[2].data()},
                {columns[3].data(), columns[4].data(), columns[5].data()},
                {columns[6].data(), columns[7].data(), columns[8].data()},
                {columns[9].data(), columns[10].data(), columns[11].data()},
                {columns[12].data(), columns[13].data(), columns[14].data(), columns[15].data()},
                columns[16].data()};
    }
};

struct Row
{
    DeadReckoningAlgorithm algorithm;
    const char* name;
};

constexpr Row kRows[] = {{DeadReckoningAlgorithm::Fpw, "FPW"}, {DeadReckoningAlgorithm::Rpw, "RPW"},
                         {DeadReckoningAlgorithm::Rvw, "RVW"}, {DeadReckoningAlgorithm::Fvw, "FVW"},
                         {DeadReckoningAlgorithm::Rpb, "RPB"}, {DeadReckoningAlgorithm::Rvb, "RVB"}};

} // namespace

int main(int argc, char** argv)
{
    usize rounds = 25U;
    usize count = 4096U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            count = 64U;
        }
        else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<usize>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    const auto levels = hn::motion_kernel_levels();
    std::printf("%-6s %12s", "row", "KDIS");
    for (const crd::KernelLevel<MotionKernels>& level : levels)
    {
        if (crd::test::kernel_level_runs_here(level))
        {
            std::printf(" %16s", crd::cpu_level_name(level.level));
        }
    }
    std::printf("\n");

    Array<f64> out[10];
    Array<f64> peer_x;
    Array<f64> peer_y;
    Array<f64> peer_z;
    for (Array<f64>& column : out)
    {
        column.resize(count);
    }
    peer_x.resize(count);
    peer_y.resize(count);
    peer_z.resize(count);
    const hn::DeadReckoningOutput into{{out[0].data(), out[1].data(), out[2].data()},
                                       {out[3].data(), out[4].data(), out[5].data()},
                                       {out[6].data(), out[7].data(), out[8].data(), out[9].data()}};

    int failures = 0;
    for (const Row& row : kRows)
    {
        Fleet fleet(count);
        KdisFleet kdis(count);
        SeededWords words(0x4452424FULL, static_cast<crd::u64>(row.algorithm));
        for (usize i = 0U; i < count; ++i)
        {
            Update update{};
            for (usize k = 0U; k < 3U; ++k)
            {
                update.position[k] = static_cast<f64>(signed_float(words, 1000.0));
                update.velocity[k] = signed_float(words, 50.0);
                update.acceleration[k] = signed_float(words, 9.0);
                update.rate[k] = signed_float(words, 2.0);
            }
            update.psi = signed_float(words, 3.14);
            update.theta = signed_float(words, 1.5);
            update.phi = signed_float(words, 3.14);
            update.algorithm = static_cast<int>(row.algorithm);
            kdis.reset(i, update);
            const hn::OrientationXyzw q = hn::orientation_from_dis_euler(
                {static_cast<f64>(update.psi), static_cast<f64>(update.theta), static_cast<f64>(update.phi)});
            for (usize k = 0U; k < 3U; ++k)
            {
                fleet.columns[k][i] = update.position[k];
                fleet.columns[3U + k][i] = static_cast<f64>(update.velocity[k]);
                fleet.columns[6U + k][i] = static_cast<f64>(update.acceleration[k]);
                fleet.columns[9U + k][i] = static_cast<f64>(update.rate[k]);
            }
            // KDIS's body acceleration is a + rate x v (test_dead_reckoning_peers.cpp).
            if (row.algorithm == DeadReckoningAlgorithm::Rvb)
            {
                fleet.columns[6][i] +=
                    (fleet.columns[10][i] * fleet.columns[5][i]) - (fleet.columns[11][i] * fleet.columns[4][i]);
                fleet.columns[7][i] +=
                    (fleet.columns[11][i] * fleet.columns[3][i]) - (fleet.columns[9][i] * fleet.columns[5][i]);
                fleet.columns[8][i] +=
                    (fleet.columns[9][i] * fleet.columns[4][i]) - (fleet.columns[10][i] * fleet.columns[3][i]);
            }
            fleet.columns[12][i] = q.x;
            fleet.columns[13][i] = q.y;
            fleet.columns[14][i] = q.z;
            fleet.columns[15][i] = q.w;
            fleet.dt[i] = static_cast<float>(0.016 + (0.5 * unit_value(words)));
            fleet.columns[16][i] = static_cast<f64>(fleet.dt[i]);
        }

        double best_kdis = 1e30;
        double best[8] = {1e30, 1e30, 1e30, 1e30, 1e30, 1e30, 1e30, 1e30};
        const hn::DeadReckoningInput in = fleet.input();
        for (usize round = 0U; round < rounds; ++round)
        {
            Clock::time_point start = Clock::now();
            kdis.run(fleet.dt.data(), count, peer_x.data(), peer_y.data(), peer_z.data());
            double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
            best_kdis = std::fmin(best_kdis, elapsed / static_cast<double>(count));
            g_sink = peer_x[count / 2U];
            usize arm = 0U;
            for (const crd::KernelLevel<MotionKernels>& level : levels)
            {
                if (!crd::test::kernel_level_runs_here(level))
                {
                    continue;
                }
                start = Clock::now();
                level.table->dead_reckon(row.algorithm, in, count, into);
                elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
                best[arm] = std::fmin(best[arm], elapsed / static_cast<double>(count));
                ++arm;
                g_sink = out[0][count / 2U];
                for (usize i = 0U; i < count; ++i)
                {
                    const f64 gap = std::fabs(out[0][i] - peer_x[i]) + std::fabs(out[1][i] - peer_y[i]) +
                                    std::fabs(out[2][i] - peer_z[i]);
                    if (gap > 2e-2)
                    {
                        std::printf("%s: entity %zu is %g from the peer\n", row.name, i, gap);
                        ++failures;
                        break;
                    }
                }
            }
        }
        std::printf("%-6s %9.2f ns", row.name, best_kdis);
        for (usize arm = 0U; arm < 8U && best[arm] < 1e30; ++arm)
        {
            std::printf(" %7.2f (%5.2fx)", best[arm], best_kdis / best[arm]);
        }
        std::printf("\n");
    }
    return failures == 0 ? 0 : 1;
}
