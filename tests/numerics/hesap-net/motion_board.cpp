// crd-hesap-motion-board: nanoseconds per body of hesap-net's snapshot interpolation at each kernel level the host runs,
// against networked-physics-gdc-2015's own functions on vectorial's scalar backend, on the same bodies. A whole body is
// its position and its orientation, as the peer's InterpolateSnapshot_* computes them. Each position and nlerp arm's bits
// are compared with the peer's, so a faster wrong answer fails; slerp's are held near by crd-hesap-motion-peer-tests.
//
// Usage: crd-hesap-motion-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round over few bodies (the smoke CTest)

#include "kernel_levels.hpp"
#include "motion_peers.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/motion.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hn = crd::hesap::net;
using crd::f32;
using crd::u32;
using crd::usize;
using hn::MotionKernels;
using motion_peers::Body;
using motion_peers::Pose;

namespace
{

template <typename T> using Array = crd::containers::Array<T>;
using Clock = std::chrono::steady_clock;

constexpr usize kMaxLevels = 8U;

volatile f32 g_sink = 0.0F;

template <typename Work> [[nodiscard]] double timed_ns(usize count, double best, const Work& work)
{
    const Clock::time_point start = Clock::now();
    work();
    const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const double per_body = elapsed / static_cast<double>(count);
    return per_body < best ? per_body : best;
}

[[nodiscard]] f32 unit_value(SeededWords& words) noexcept
{
    return static_cast<f32>(words.next_u32() >> 8U) / 16777216.0F;
}

void random_unit(SeededWords& words, f32* q) noexcept
{
    double v[4];
    double sum = 0.0;
    for (double& component : v)
    {
        component = (static_cast<double>(words.next_u32()) / 2147483648.0) - 1.0;
        sum += component * component;
    }
    const double scale = 1.0 / std::sqrt(sum > 1e-12 ? sum : 1.0);
    for (usize i = 0U; i < 4U; ++i)
    {
        q[i] = static_cast<f32>(v[i] * scale);
    }
}

struct Arms
{
    double linear = 1e30;
    double hermite = 1e30;
    double nlerp = 1e30;
    double slerp = 1e30;
};

} // namespace

int main(int argc, char** argv)
{
    usize bodies = 4096U;
    u32 rounds = 25U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--quick") == 0)
        {
            bodies = 64U;
            rounds = 1U;
        }
        else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    // Motion between two ticks of a real scene: a small turn and a small move.
    SeededWords words(51U, 1U);
    Array<Body> a;
    Array<Body> b;
    a.resize(bodies);
    b.resize(bodies);
    Array<f32> position_a;
    Array<f32> position_b;
    Array<f32> velocity_a;
    Array<f32> velocity_b;
    Array<f32> orientation_a;
    Array<f32> orientation_b;
    position_a.resize(3U * bodies);
    position_b.resize(3U * bodies);
    velocity_a.resize(3U * bodies);
    velocity_b.resize(3U * bodies);
    orientation_a.resize(4U * bodies);
    orientation_b.resize(4U * bodies);
    for (usize i = 0U; i < bodies; ++i)
    {
        for (usize c = 0U; c < 3U; ++c)
        {
            a[i].position[c] = (unit_value(words) * 2000.0F) - 1000.0F;
            a[i].velocity[c] = (unit_value(words) * 20.0F) - 10.0F;
            b[i].velocity[c] = a[i].velocity[c] + (unit_value(words) - 0.5F);
            b[i].position[c] = a[i].position[c] + (a[i].velocity[c] * 0.1F);
            position_a[(3U * i) + c] = a[i].position[c];
            position_b[(3U * i) + c] = b[i].position[c];
            velocity_a[(3U * i) + c] = a[i].velocity[c];
            velocity_b[(3U * i) + c] = b[i].velocity[c];
        }
        random_unit(words, a[i].orientation);
        f32 turn[4];
        random_unit(words, turn);
        f32 length2 = 0.0F;
        for (usize c = 0U; c < 4U; ++c)
        {
            b[i].orientation[c] = a[i].orientation[c] + (turn[c] * 0.1F);
            length2 += b[i].orientation[c] * b[i].orientation[c];
        }
        const f32 inverse = 1.0F / std::sqrt(length2);
        for (usize c = 0U; c < 4U; ++c)
        {
            b[i].orientation[c] *= inverse;
            orientation_a[(4U * i) + c] = a[i].orientation[c];
            orientation_b[(4U * i) + c] = b[i].orientation[c];
        }
    }

    Array<Pose> poses;
    Array<Pose> peer_poses;
    poses.resize(bodies);
    peer_poses.resize(bodies);
    Array<f32> position_out;
    Array<f32> orientation_out;
    Array<f32> peer_orientation;
    position_out.resize(3U * bodies);
    orientation_out.resize(4U * bodies);
    peer_orientation.resize(4U * bodies);

    constexpr f32 t = 0.4375F;
    constexpr f32 step = 0.1F;
    Arms peer;
    Arms ours[kMaxLevels];
    const auto levels = hn::motion_kernel_levels();
    bool same = true;
    for (u32 round = 0U; round < rounds; ++round)
    {
        peer.linear = timed_ns(bodies, peer.linear,
                               [&] { motion_peers::linear_batch(t, a.data(), b.data(), bodies, peer_poses.data()); });
        for (usize k = 0U; k < levels.size(); ++k)
        {
            if (!crd::test::kernel_level_runs_here(levels[k]))
            {
                continue;
            }
            const MotionKernels& table = *levels[k].table;
            ours[k].linear = timed_ns(bodies, ours[k].linear,
                                      [&]
                                      {
                                          table.lerp_f32(position_a.data(), position_b.data(), t, 3U * bodies,
                                                         position_out.data());
                                          table.slerp(orientation_a.data(), orientation_b.data(), t, bodies,
                                                      orientation_out.data());
                                      });
            for (usize i = 0U; i < bodies; ++i)
            {
                same = same && std::memcmp(position_out.data() + (3U * i), peer_poses[i].position,
                                           sizeof(peer_poses[i].position)) == 0;
            }
        }
        peer.hermite = timed_ns(bodies, peer.hermite,
                                [&] { motion_peers::hermite_batch(t, step, a.data(), b.data(), bodies, peer_poses.data()); });
        for (usize k = 0U; k < levels.size(); ++k)
        {
            if (!crd::test::kernel_level_runs_here(levels[k]))
            {
                continue;
            }
            const MotionKernels& table = *levels[k].table;
            ours[k].hermite = timed_ns(bodies, ours[k].hermite,
                                       [&]
                                       {
                                           table.hermite_f32({position_a.data(), velocity_a.data(), position_b.data(),
                                                              velocity_b.data(), t, step},
                                                             3U * bodies, position_out.data());
                                           table.slerp(orientation_a.data(), orientation_b.data(), t, bodies,
                                                       orientation_out.data());
                                       });
            for (usize i = 0U; i < bodies; ++i)
            {
                same = same && std::memcmp(position_out.data() + (3U * i), peer_poses[i].position,
                                           sizeof(peer_poses[i].position)) == 0;
            }
        }
        peer.nlerp = timed_ns(bodies, peer.nlerp,
                              [&]
                              {
                                  motion_peers::nlerp_batch(t, orientation_a.data(), orientation_b.data(), bodies,
                                                            peer_orientation.data());
                              });
        for (usize k = 0U; k < levels.size(); ++k)
        {
            if (!crd::test::kernel_level_runs_here(levels[k]))
            {
                continue;
            }
            const MotionKernels& table = *levels[k].table;
            ours[k].nlerp = timed_ns(
                bodies, ours[k].nlerp,
                [&] { table.nlerp(orientation_a.data(), orientation_b.data(), t, bodies, orientation_out.data()); });
            same = same && std::memcmp(orientation_out.data(), peer_orientation.data(), 4U * bodies * sizeof(f32)) == 0;
        }
        peer.slerp = timed_ns(bodies, peer.slerp,
                              [&]
                              {
                                  motion_peers::slerp_batch(t, orientation_a.data(), orientation_b.data(), bodies,
                                                            peer_orientation.data());
                              });
        for (usize k = 0U; k < levels.size(); ++k)
        {
            if (!crd::test::kernel_level_runs_here(levels[k]))
            {
                continue;
            }
            const MotionKernels& table = *levels[k].table;
            ours[k].slerp = timed_ns(
                bodies, ours[k].slerp,
                [&] { table.slerp(orientation_a.data(), orientation_b.data(), t, bodies, orientation_out.data()); });
        }
        g_sink = g_sink + orientation_out[0] + position_out[0] + peer_orientation[0] + peer_poses[0].position[0];
    }
    if (!same)
    {
        std::printf("FAILED: an arm's bits differ from the peer's\n");
        return 1;
    }

    std::printf("%zu bodies, best of %u rounds, ns per body\n\n", bodies, rounds);
    std::printf("| Arm | Linear + slerp | Hermite + slerp | nlerp | slerp |\n|---|---|---|---|---|\n");
    std::printf("| networked-physics (vectorial scalar) | %.2f | %.2f | %.2f | %.2f |\n", peer.linear, peer.hermite, peer.nlerp,
                peer.slerp);
    for (usize k = 0U; k < levels.size(); ++k)
    {
        if (crd::test::kernel_level_runs_here(levels[k]))
        {
            std::printf("| hesap %s | %.2f (%.2fx) | %.2f (%.2fx) | %.2f (%.2fx) | %.2f (%.2fx) |\n",
                        crd::cpu_level_name(levels[k].level), ours[k].linear, peer.linear / ours[k].linear,
                        ours[k].hermite, peer.hermite / ours[k].hermite, ours[k].nlerp, peer.nlerp / ours[k].nlerp,
                        ours[k].slerp, peer.slerp / ours[k].slerp);
        }
    }
    return 0;
}
