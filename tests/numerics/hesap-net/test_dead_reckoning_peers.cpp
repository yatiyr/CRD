// HESAP-NET's dead reckoning against its peers. CRD_BUILD_MOTION_VS_PEERS, on linux-gcc-public-checks (x64 Linux, whose
// long double has a 64-bit mantissa).
//   * KDIS's own DeadReckoningCalculator: positions of the algorithms it implements to the standard, within what its
//     single-precision fields and matrices allow. KDIS turns the orientation of a rotating entity by integrating Euler
//     rates, which is not the standard's matrix, so its attitude is not a reference; crd-hesap-net-tests holds ours to
//     the standard's DR and to the true rotation.
//   * IEEE 1278.1's closed forms, as Open-DIS's AbstractDeadReckoner writes them, restated in long double, and the same
//     expressions in double: below a turn of about 0.1 the doubles cancel, and the series keeps the precision.
//
// KDIS's differences, none followed: its fixed algorithms 6 and 9 still apply R1 and R2 for a nonzero rate; its body
// acceleration is the given one plus the rate crossed with the velocity, where ours is the rate of the velocity's body
// components; and at a turn below 1e-5 its algorithm 9 adds a * dt instead of a * dt^2 / 2.

#include "dead_reckoning_peers.hpp"
#include "kernel_levels.hpp"
#include "seeded_words.hpp"

#include <crd/hesap/net/dead_reckoning.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>

namespace hn = crd::hesap::net;
using crd::f64;
using crd::u32;
using crd::usize;
using dead_reckoning_peers::KdisFleet;
using dead_reckoning_peers::Update;
using hn::DeadReckoningAlgorithm;

namespace
{

[[nodiscard]] f64 unit_value(SeededWords& words) noexcept
{
    return static_cast<f64>(words.next_u32() >> 4U) / 268435456.0;
}

[[nodiscard]] float signed_float(SeededWords& words, f64 scale) noexcept
{
    return static_cast<float>(((2.0 * unit_value(words)) - 1.0) * scale);
}

struct Moved
{
    f64 x;
    f64 y;
    f64 z;
};

// Ours on one entity given KDIS's fields, the acceleration column given separately.
[[nodiscard]] Moved ours(const Update& update, const f64 (&acceleration)[3], f64 dt,
                         DeadReckoningAlgorithm algorithm) noexcept
{
    const hn::OrientationXyzw q = hn::orientation_from_dis_euler(
        {static_cast<f64>(update.psi), static_cast<f64>(update.theta), static_cast<f64>(update.phi)});
    const f64 v[3] = {static_cast<f64>(update.velocity[0]), static_cast<f64>(update.velocity[1]),
                      static_cast<f64>(update.velocity[2])};
    const f64 w[3] = {static_cast<f64>(update.rate[0]), static_cast<f64>(update.rate[1]),
                      static_cast<f64>(update.rate[2])};
    Moved at{};
    f64 velocity_out = 0.0;
    f64 orientation_out = 0.0;
    hn::dead_reckon(algorithm,
                    {{&update.position[0], &update.position[1], &update.position[2]},
                     {&v[0], &v[1], &v[2]},
                     {&acceleration[0], &acceleration[1], &acceleration[2]},
                     {&w[0], &w[1], &w[2]},
                     {&q.x, &q.y, &q.z, &q.w},
                     &dt},
                    1U,
                    {{&at.x, &at.y, &at.z},
                     {&velocity_out, &velocity_out, &velocity_out},
                     {&orientation_out, &orientation_out, &orientation_out, &orientation_out}});
    return at;
}

[[nodiscard]] Update random_update(SeededWords& words, int algorithm, f64 rate_scale) noexcept
{
    Update update{};
    for (usize k = 0U; k < 3U; ++k)
    {
        update.position[k] = static_cast<f64>(signed_float(words, 1000.0));
        update.velocity[k] = signed_float(words, 50.0);
        update.acceleration[k] = signed_float(words, 9.0);
        update.rate[k] = signed_float(words, rate_scale);
    }
    update.psi = signed_float(words, 3.14);
    update.theta = signed_float(words, 1.5);
    update.phi = signed_float(words, 3.14);
    update.algorithm = algorithm;
    return update;
}

struct Worst
{
    f64 gap = 0.0;
    f64 sum = 0.0;
    u32 count = 0U;

    void take(f64 value) noexcept
    {
        gap = std::fmax(gap, value);
        sum += value;
        ++count;
    }
};

} // namespace

TEST_CASE("KDIS: the world and rotating-body positions, within its single precision", "[dead-reckoning][peer]")
{
    constexpr DeadReckoningAlgorithm algorithms[] = {DeadReckoningAlgorithm::Fpw, DeadReckoningAlgorithm::Rpw,
                                                      DeadReckoningAlgorithm::Rvw, DeadReckoningAlgorithm::Fvw,
                                                      DeadReckoningAlgorithm::Rpb, DeadReckoningAlgorithm::Rvb};
    SeededWords words(0x4B444953ULL, 0U);
    for (const DeadReckoningAlgorithm algorithm : algorithms)
    {
        Worst worst;
        for (u32 round = 0U; round < 4000U; ++round)
        {
            Update update = random_update(words, static_cast<int>(algorithm), 3.0);
            const auto dt = static_cast<float>(0.05 + (1.45 * unit_value(words)));
            KdisFleet fleet(1U);
            fleet.reset(0U, update);
            f64 x = 0.0;
            f64 y = 0.0;
            f64 z = 0.0;
            fleet.run(&dt, 1U, &x, &y, &z);
            // KDIS's body acceleration is a + rate x v.
            const f64 w[3] = {static_cast<f64>(update.rate[0]), static_cast<f64>(update.rate[1]),
                              static_cast<f64>(update.rate[2])};
            const f64 v[3] = {static_cast<f64>(update.velocity[0]), static_cast<f64>(update.velocity[1]),
                              static_cast<f64>(update.velocity[2])};
            f64 a[3] = {static_cast<f64>(update.acceleration[0]), static_cast<f64>(update.acceleration[1]),
                        static_cast<f64>(update.acceleration[2])};
            if (algorithm == DeadReckoningAlgorithm::Rvb)
            {
                a[0] += (w[1] * v[2]) - (w[2] * v[1]);
                a[1] += (w[2] * v[0]) - (w[0] * v[2]);
                a[2] += (w[0] * v[1]) - (w[1] * v[0]);
            }
            const Moved got = ours(update, a, static_cast<f64>(dt), algorithm);
            const f64 dx = got.x - update.position[0];
            const f64 dy = got.y - update.position[1];
            const f64 dz = got.z - update.position[2];
            const f64 moved = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            const f64 ex = got.x - x;
            const f64 ey = got.y - y;
            const f64 ez = got.z - z;
            worst.take(std::sqrt((ex * ex) + (ey * ey) + (ez * ez)) / (1.0 + moved));
        }
        std::printf("KDIS algorithm %d: worst gap %.3e of the distance moved, mean %.3e\n", static_cast<int>(algorithm),
                    worst.gap, worst.sum / static_cast<f64>(worst.count));
        INFO("algorithm " << static_cast<int>(algorithm) << " worst " << worst.gap);
        CHECK(worst.gap < 2e-5);
    }
}

TEST_CASE("KDIS: with no rate at all, its fixed body algorithm is ours", "[dead-reckoning][peer]")
{
    SeededWords words(0x4B444954ULL, 0U);
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const Update update = random_update(words, static_cast<int>(DeadReckoningAlgorithm::Fpb), 0.0);
        const auto dt = static_cast<float>(0.05 + (1.45 * unit_value(words)));
        KdisFleet fleet(1U);
        fleet.reset(0U, update);
        f64 x = 0.0;
        f64 y = 0.0;
        f64 z = 0.0;
        fleet.run(&dt, 1U, &x, &y, &z);
        const f64 none[3] = {0.0, 0.0, 0.0};
        const Moved got = ours(update, none, static_cast<f64>(dt), DeadReckoningAlgorithm::Fpb);
        CHECK(std::fabs(got.x - x) < 1e-3);
        CHECK(std::fabs(got.y - y) < 1e-3);
        CHECK(std::fabs(got.z - z) < 1e-3);
    }
}

namespace
{

using Wide = long double;

// R1 and R2 applied, in x y z of a body-axis displacement.
struct Coefficients
{
    Wide v_axis;
    Wide v_plane;
    Wide v_cross;
    Wide a_axis;
    Wide a_plane;
    Wide a_cross;
};

// As AbstractDeadReckoner computes them, of magnitude mag and turn angle = mag * t.
template <class T> [[nodiscard]] Coefficients closed_forms(T mag, T t) noexcept
{
    const T mag2 = mag * mag;
    const T angle = mag * t;
    const T s = std::sin(angle);
    const T c = std::cos(angle);
    const T top = c + (angle * s) - static_cast<T>(1);
    return {(angle - s) / (mag2 * mag),
            s / mag,
            (static_cast<T>(1) - c) / mag2,
            ((static_cast<T>(0.5) * angle * angle) - top) / (mag2 * mag2),
            top / mag2,
            (s - (angle * c)) / (mag2 * mag)};
}

struct WideVector
{
    Wide values[3];
};

// R1 v + R2 a, carried out in long double from any coefficients.
[[nodiscard]] WideVector displacement(const Coefficients& c, const f64* v, const f64* a, const f64* w) noexcept
{
    const auto dot = [](const f64* p, const f64* q) noexcept
    {
        return (static_cast<Wide>(p[0]) * q[0]) + (static_cast<Wide>(p[1]) * q[1]) + (static_cast<Wide>(p[2]) * q[2]);
    };
    const Wide wv = dot(w, v);
    const Wide wa = dot(w, a);
    WideVector out{};
    for (usize k = 0U; k < 3U; ++k)
    {
        const usize n = (k + 1U) % 3U;
        const usize m = (k + 2U) % 3U;
        const Wide vx = (static_cast<Wide>(w[n]) * v[m]) - (static_cast<Wide>(w[m]) * v[n]);
        const Wide ax = (static_cast<Wide>(w[n]) * a[m]) - (static_cast<Wide>(w[m]) * a[n]);
        out.values[k] = (c.v_axis * w[k] * wv) + (c.v_plane * v[k]) + (c.v_cross * vx) + (c.a_axis * w[k] * wa) +
                        (c.a_plane * a[k]) + (c.a_cross * ax);
    }
    return out;
}

[[nodiscard]] f64 relative_gap(const WideVector& a, const WideVector& b) noexcept
{
    Wide gap = 0.0L;
    Wide size = 0.0L;
    for (usize k = 0U; k < 3U; ++k)
    {
        gap = std::fmax(gap, std::fabs(a.values[k] - b.values[k]));
        size = std::fmax(size, std::fabs(b.values[k]));
    }
    return static_cast<f64>(gap / (1e-3L + size));
}

} // namespace

TEST_CASE("The closed forms: ours meets them in long double, and doubles of them cancel where ours does not",
          "[dead-reckoning][peer]")
{
    SeededWords words(0x4F444953ULL, 0U);
    std::printf("turn    worst of ours    worst of the doubles\n");
    for (const f64 beta : {100.0, 10.0, 3.0, 1.5, 1.001, 0.999, 0.5, 0.2, 0.1})
    {
        Worst of_ours;
        Worst of_doubles;
        for (u32 round = 0U; round < 2000U; ++round)
        {
            f64 v[3];
            f64 a[3];
            f64 w[3];
            for (usize k = 0U; k < 3U; ++k)
            {
                v[k] = static_cast<f64>(signed_float(words, 50.0));
                a[k] = static_cast<f64>(signed_float(words, 9.0));
                w[k] = static_cast<f64>(signed_float(words, 1.0));
            }
            const f64 t = 0.05 + (1.45 * unit_value(words));
            const f64 length = std::sqrt((w[0] * w[0]) + (w[1] * w[1]) + (w[2] * w[2]));
            for (f64& component : w)
            {
                component *= beta / (length * t);
            }
            // From the origin at the identity, the position is R1 v + R2 a itself.
            const f64 origin = 0.0;
            const f64 q0 = 0.0;
            const f64 q1 = 1.0;
            f64 out[3];
            f64 ignore = 0.0;
            hn::dead_reckon(
                DeadReckoningAlgorithm::Rvb,
                {{&origin, &origin, &origin},
                 {&v[0], &v[1], &v[2]},
                 {&a[0], &a[1], &a[2]},
                 {&w[0], &w[1], &w[2]},
                 {&q0, &q0, &q0, &q1},
                 &t},
                1U, {{&out[0], &out[1], &out[2]}, {&ignore, &ignore, &ignore}, {&ignore, &ignore, &ignore, &ignore}});
            const Wide mag = std::sqrt((static_cast<Wide>(w[0]) * w[0]) + (static_cast<Wide>(w[1]) * w[1]) +
                                       (static_cast<Wide>(w[2]) * w[2]));
            const WideVector truth = displacement(closed_forms<Wide>(mag, static_cast<Wide>(t)), v, a, w);
            const WideVector mine{{static_cast<Wide>(out[0]), static_cast<Wide>(out[1]), static_cast<Wide>(out[2])}};
            of_ours.take(relative_gap(mine, truth));
            const f64 magnitude = std::sqrt((w[0] * w[0]) + (w[1] * w[1]) + (w[2] * w[2]));
            of_doubles.take(relative_gap(displacement(closed_forms<f64>(magnitude, t), v, a, w), truth));
        }
        std::printf("%7.3f  %.3e        %.3e\n", beta, of_ours.gap, of_doubles.gap);
        INFO("turn " << beta << " ours " << of_ours.gap << " doubles " << of_doubles.gap);
        // An angle of beta carries beta's own rounding into every term.
        CHECK(of_ours.gap < 1e-15 * (4.0 + beta));
    }
}
