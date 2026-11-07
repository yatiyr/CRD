// HESAP-NET's snapshot interpolation against its reference, at every kernel level the host runs: lerp, hermite, their
// composition with extrapolation and nlerp bit for bit with networked-physics-gdc-2015's own functions on vectorial's
// scalar backend, and slerp against that peer's and the true arc. CRD_BUILD_MOTION_VS_PEERS.

#include "kernel_levels.hpp"
#include "motion_peers.hpp"
#include "seeded_words.hpp"

#include <crd/hesap/net/motion.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hn = crd::hesap::net;
using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::usize;
using hn::MotionKernels;
using motion_peers::Body;
using motion_peers::Pose;

namespace
{

constexpr usize kBatch = 67U;

[[nodiscard]] f32 unit_value(SeededWords& words) noexcept
{
    return static_cast<f32>(words.next_u32() >> 8U) / 16777216.0F;
}

// A value a snapshot's float can hold, over many magnitudes.
[[nodiscard]] f32 random_value(SeededWords& words) noexcept
{
    const f32 magnitude = std::ldexp(1.0F, static_cast<int>(words.next_u32() % 41U) - 20);
    return ((unit_value(words) * 2.0F) - 1.0F) * magnitude;
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

// Two snapshots of a batch: the peer's bodies and hesap's flat arrays of the same values.
struct Snapshots
{
    Body a[kBatch];
    Body b[kBatch];
    f32 a_position[3U * kBatch];
    f32 b_position[3U * kBatch];
    f32 a_velocity[3U * kBatch];
    f32 b_velocity[3U * kBatch];
    f32 a_orientation[4U * kBatch];
    f32 b_orientation[4U * kBatch];

    void fill(SeededWords& words) noexcept
    {
        for (usize i = 0U; i < kBatch; ++i)
        {
            for (usize c = 0U; c < 3U; ++c)
            {
                a[i].position[c] = random_value(words);
                b[i].position[c] = random_value(words);
                a[i].velocity[c] = random_value(words);
                b[i].velocity[c] = random_value(words);
                a_position[(3U * i) + c] = a[i].position[c];
                b_position[(3U * i) + c] = b[i].position[c];
                a_velocity[(3U * i) + c] = a[i].velocity[c];
                b_velocity[(3U * i) + c] = b[i].velocity[c];
            }
            random_unit(words, a[i].orientation);
            random_unit(words, b[i].orientation);
            std::memcpy(a_orientation + (4U * i), a[i].orientation, sizeof(a[i].orientation));
            std::memcpy(b_orientation + (4U * i), b[i].orientation, sizeof(b[i].orientation));
        }
    }
};

template <typename Check> void for_each_level(const Check& check)
{
    for (const KernelLevel<MotionKernels>& level : hn::motion_kernel_levels())
    {
        if (crd::test::kernel_level_runs_here(level))
        {
            INFO(crd::cpu_level_name(level.level));
            check(*level.table);
        }
    }
}

[[nodiscard]] bool same_position(const f32* flat, const Pose& pose) noexcept
{
    return std::memcmp(flat, pose.position, sizeof(pose.position)) == 0;
}

} // namespace

TEST_CASE("motion peer: positions bit for bit with InterpolateSnapshot_Linear and _Hermite", "[motion-peer]")
{
    SeededWords words(41U, 1U);
    Snapshots s;
    f32 lerped[3U * kBatch];
    f32 cubic[3U * kBatch];
    for (u32 round = 0U; round < 4000U; ++round)
    {
        s.fill(words);
        // In the span and past both ends, which the peer's buffer also reaches when it runs dry.
        const f32 t = (round % 4U == 0U) ? (unit_value(words) * 3.0F) - 1.0F : unit_value(words);
        const f32 step = (unit_value(words) * 0.5F) + 0.001F;
        for_each_level(
            [&](const MotionKernels& table)
            {
                table.lerp_f32(s.a_position, s.b_position, t, 3U * kBatch, lerped);
                table.hermite_f32({s.a_position, s.a_velocity, s.b_position, s.b_velocity, t, step}, 3U * kBatch, cubic);
                bool same = true;
                for (usize i = 0U; i < kBatch; ++i)
                {
                    Pose pose;
                    motion_peers::linear(t, s.a[i], s.b[i], pose);
                    same = same && same_position(lerped + (3U * i), pose);
                    motion_peers::hermite(t, step, s.a[i], s.b[i], pose);
                    same = same && same_position(cubic + (3U * i), pose);
                }
                CHECK(same);
            });
    }
}

TEST_CASE("motion peer: extrapolation then Hermite is InterpolateSnapshot_Hermite_WithExtrapolation", "[motion-peer]")
{
    SeededWords words(41U, 2U);
    Snapshots s;
    f32 from[3U * kBatch];
    f32 to[3U * kBatch];
    f32 cubic[3U * kBatch];
    for (u32 round = 0U; round < 4000U; ++round)
    {
        s.fill(words);
        const f32 t = unit_value(words);
        const f32 step = (unit_value(words) * 0.5F) + 0.001F;
        const f32 extrapolation = unit_value(words) * 0.25F;
        for_each_level(
            [&](const MotionKernels& table)
            {
                table.extrapolate_f32(s.a_position, s.a_velocity, nullptr, extrapolation, 3U * kBatch, from);
                table.extrapolate_f32(s.b_position, s.b_velocity, nullptr, extrapolation, 3U * kBatch, to);
                table.hermite_f32({from, s.a_velocity, to, s.b_velocity, t, step}, 3U * kBatch, cubic);
                bool same = true;
                for (usize i = 0U; i < kBatch; ++i)
                {
                    Pose pose;
                    motion_peers::hermite_extrapolated(t, step, extrapolation, s.a[i], s.b[i], pose);
                    same = same && same_position(cubic + (3U * i), pose);
                }
                CHECK(same);
            });
    }
}

TEST_CASE("motion peer: nlerp bit for bit with vectorial's", "[motion-peer]")
{
    SeededWords words(41U, 3U);
    Snapshots s;
    f32 ours[4U * kBatch];
    for (u32 round = 0U; round < 4000U; ++round)
    {
        s.fill(words);
        const f32 t = unit_value(words);
        for_each_level(
            [&](const MotionKernels& table)
            {
                table.nlerp(s.a_orientation, s.b_orientation, t, kBatch, ours);
                bool same = true;
                for (usize i = 0U; i < kBatch; ++i)
                {
                    f32 want[4];
                    motion_peers::nlerp(t, s.a_orientation + (4U * i), s.b_orientation + (4U * i), want);
                    same = same && std::memcmp(ours + (4U * i), want, sizeof(want)) == 0;
                }
                CHECK(same);
            });
    }
}

TEST_CASE("motion peer: slerp stays nearer the true arc than vectorial's", "[motion-peer]")
{
    // The peer takes acos and three sines in float through the C library, so its bits are the platform's; hesap's series
    // gives the same bits everywhere and has to give no less accuracy for it.
    SeededWords words(41U, 4U);
    Snapshots s;
    f32 ours[4U * kBatch];
    long double worst_ours = 0.0L;
    long double worst_peer = 0.0L;
    long double worst_gap = 0.0L;
    for (u32 round = 0U; round < 4000U; ++round)
    {
        s.fill(words);
        const f32 t = unit_value(words);
        hn::slerp(s.a_orientation, s.b_orientation, t, kBatch, ours);
        for (usize i = 0U; i < kBatch; ++i)
        {
            const f32* a = s.a_orientation + (4U * i);
            const f32* b = s.b_orientation + (4U * i);
            long double cosine = 0.0L;
            for (usize c = 0U; c < 4U; ++c)
            {
                cosine += static_cast<long double>(a[c]) * static_cast<long double>(b[c]);
            }
            const long double sign = (cosine >= 0.0L) ? 1.0L : -1.0L;
            const long double angle = std::acos(std::fabs(cosine));
            const long double from_a = std::sin((1.0L - static_cast<long double>(t)) * angle) / std::sin(angle);
            const long double from_b = sign * std::sin(static_cast<long double>(t) * angle) / std::sin(angle);
            f32 peer[4];
            motion_peers::slerp(t, a, b, peer);
            for (usize c = 0U; c < 4U; ++c)
            {
                const long double arc = (from_a * static_cast<long double>(a[c])) + (from_b * static_cast<long double>(b[c]));
                const auto mine = static_cast<long double>(ours[(4U * i) + c]);
                const auto theirs = static_cast<long double>(peer[c]);
                worst_ours = std::fmax(worst_ours, std::fabs(mine - arc));
                worst_peer = std::fmax(worst_peer, std::fabs(theirs - arc));
                worst_gap = std::fmax(worst_gap, std::fabs(mine - theirs));
            }
        }
    }
    std::printf("slerp: worst component error, hesap %.3e, peer %.3e; worst gap %.3e\n", static_cast<double>(worst_ours),
                static_cast<double>(worst_peer), static_cast<double>(worst_gap));
    CHECK(worst_ours <= worst_peer);
    CHECK(worst_gap < 2e-4L); // The peer's own error, the larger by far.
}

TEST_CASE("motion peer: every level's slerp bit for bit with the portable level", "[motion-peer]")
{
    SeededWords words(41U, 5U);
    Snapshots s;
    f32 want[4U * kBatch];
    f32 got[4U * kBatch];
    for (u32 round = 0U; round < 2000U; ++round)
    {
        s.fill(words);
        const f32 t = unit_value(words);
        hn::kMotionKernelsPortable.slerp(s.a_orientation, s.b_orientation, t, kBatch, want);
        for_each_level(
            [&](const MotionKernels& table)
            {
                table.slerp(s.a_orientation, s.b_orientation, t, kBatch, got);
                CHECK(std::memcmp(want, got, sizeof(want)) == 0);
            });
    }
}
