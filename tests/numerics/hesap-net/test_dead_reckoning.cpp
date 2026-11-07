// HESAP-NET's dead reckoning: every kernel level bit for bit with the portable one on every length, with nothing
// written past a batch or into a column the algorithm does not move (crd-hesap-net-kernels-at-<level> runs the
// [kernels] cases under each forced level); the nine algorithms held to the motion they mean by integrating it,
// independently of R1 and R2; the standard's closed forms restated; its world-to-body matrices through the Euler
// conversions; projective velocity blending to its restated expression, its ends and its derivative.

#include "kernel_levels.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/dead_reckoning.hpp>

#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace hn = crd::hesap::net;
using Catch::Approx;
using crd::f32;
using crd::f64;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using hn::DeadReckoningAlgorithm;
using hn::MotionKernels;

namespace
{

constexpr usize kMaxLength = 70U;
constexpr f64 kCanary = -1234.5;
constexpr DeadReckoningAlgorithm kAlgorithms[] = {
    DeadReckoningAlgorithm::Static, DeadReckoningAlgorithm::Fpw, DeadReckoningAlgorithm::Rpw,
    DeadReckoningAlgorithm::Rvw,    DeadReckoningAlgorithm::Fvw, DeadReckoningAlgorithm::Fpb,
    DeadReckoningAlgorithm::Rpb,    DeadReckoningAlgorithm::Rvb, DeadReckoningAlgorithm::Fvb};

[[nodiscard]] bool rotating(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Rpw || a == DeadReckoningAlgorithm::Rvw || a == DeadReckoningAlgorithm::Rpb ||
           a == DeadReckoningAlgorithm::Rvb;
}

[[nodiscard]] bool accelerating(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Rvw || a == DeadReckoningAlgorithm::Fvw || a == DeadReckoningAlgorithm::Rvb ||
           a == DeadReckoningAlgorithm::Fvb;
}

[[nodiscard]] bool on_body_axes(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Fpb || a == DeadReckoningAlgorithm::Rpb || a == DeadReckoningAlgorithm::Rvb ||
           a == DeadReckoningAlgorithm::Fvb;
}

struct Vec
{
    f64 x;
    f64 y;
    f64 z;
};

[[nodiscard]] Vec operator+(Vec a, Vec b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec operator-(Vec a, Vec b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec operator*(Vec a, f64 k) noexcept
{
    return {a.x * k, a.y * k, a.z * k};
}

[[nodiscard]] f64 dot(Vec a, Vec b) noexcept
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] Vec cross(Vec a, Vec b) noexcept
{
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] f64 length(Vec a) noexcept
{
    return std::sqrt(dot(a, a));
}

struct Quat
{
    f64 x;
    f64 y;
    f64 z;
    f64 w;
};

[[nodiscard]] Quat operator*(const Quat& a, const Quat& b) noexcept
{
    return {
        (a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y), (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
        (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w), (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z)};
}

[[nodiscard]] Vec rotate(const Quat& q, Vec v) noexcept
{
    const Vec axis{q.x, q.y, q.z};
    const Vec twice = cross(axis, v) * 2.0;
    return v + (twice * q.w) + cross(axis, twice);
}

// The rotation by |rate| * time about rate, by libm's trigonometry at a size where it does not cancel.
[[nodiscard]] Quat turn_of(Vec rate, f64 time) noexcept
{
    const f64 speed = length(rate);
    if (speed == 0.0)
    {
        return {0.0, 0.0, 0.0, 1.0};
    }
    const f64 half = speed * time * 0.5;
    const f64 scale = std::sin(half) / speed;
    return {rate.x * scale, rate.y * scale, rate.z * scale, std::cos(half)};
}

// One entity in the plain form the oracles work in.
struct Entity
{
    Vec position;
    Vec velocity;
    Vec acceleration;
    Vec rate;
    Quat orientation;
    f64 dt;
};

struct Reckoned
{
    Vec position;
    Vec velocity;
    Quat orientation;
};

// Calls a table on one entity. Columns the algorithm does not read are null, so a read of one would fault.
[[nodiscard]] Reckoned reckon_one(const MotionKernels& table, DeadReckoningAlgorithm algorithm,
                                  const Entity& e) noexcept
{
    Reckoned out{e.position, e.velocity, e.orientation};
    const bool turns = rotating(algorithm);
    const bool speeds = accelerating(algorithm);
    const bool body = on_body_axes(algorithm);
    const bool moves = algorithm != DeadReckoningAlgorithm::Static;
    const hn::DeadReckoningInput in{
        {&e.position.x, &e.position.y, &e.position.z},
        {moves ? &e.velocity.x : nullptr, moves ? &e.velocity.y : nullptr, moves ? &e.velocity.z : nullptr},
        {speeds ? &e.acceleration.x : nullptr, speeds ? &e.acceleration.y : nullptr,
         speeds ? &e.acceleration.z : nullptr},
        {turns ? &e.rate.x : nullptr, turns ? &e.rate.y : nullptr, turns ? &e.rate.z : nullptr},
        {(turns || body) ? &e.orientation.x : nullptr, (turns || body) ? &e.orientation.y : nullptr,
         (turns || body) ? &e.orientation.z : nullptr, (turns || body) ? &e.orientation.w : nullptr},
        moves ? &e.dt : nullptr};
    const hn::DeadReckoningOutput into{
        {&out.position.x, &out.position.y, &out.position.z},
        {speeds ? &out.velocity.x : nullptr, speeds ? &out.velocity.y : nullptr, speeds ? &out.velocity.z : nullptr},
        {turns ? &out.orientation.x : nullptr, turns ? &out.orientation.y : nullptr,
         turns ? &out.orientation.z : nullptr, turns ? &out.orientation.w : nullptr}};
    table.dead_reckon(algorithm, in, 1U, into);
    return out;
}

[[nodiscard]] const MotionKernels& portable() noexcept
{
    return hn::kMotionKernelsPortable;
}

[[nodiscard]] f64 unit_value(SeededWords& words) noexcept
{
    return static_cast<f64>(words.next_u32() >> 4U) / 268435456.0;
}

[[nodiscard]] f64 signed_value(SeededWords& words, f64 scale) noexcept
{
    return ((2.0 * unit_value(words)) - 1.0) * scale;
}

[[nodiscard]] Vec random_vec(SeededWords& words, f64 scale) noexcept
{
    return {signed_value(words, scale), signed_value(words, scale), signed_value(words, scale)};
}

[[nodiscard]] Quat random_orientation(SeededWords& words) noexcept
{
    Quat q{signed_value(words, 1.0), signed_value(words, 1.0), signed_value(words, 1.0), signed_value(words, 1.0)};
    const f64 scale = 1.0 / std::sqrt((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
    return {q.x * scale, q.y * scale, q.z * scale, q.w * scale};
}

// An entity whose rate is scaled so that |rate| * dt is exactly beta.
[[nodiscard]] Entity random_entity(SeededWords& words, f64 beta) noexcept
{
    Entity e{random_vec(words, 1000.0), random_vec(words, 50.0),   random_vec(words, 9.0),
             random_vec(words, 1.0),    random_orientation(words), 0.05 + (1.45 * unit_value(words))};
    if ((words.next_u32() & 1U) != 0U)
    {
        e.dt = -e.dt;
    }
    e.rate = e.rate * (beta / (length(e.rate) * std::fabs(e.dt)));
    return e;
}

// The body velocity held and the body acceleration integrated across [0, dt] by three-point Gauss-Legendre panels: at
// each node the true rotation since the update carries (velocity + acceleration * tau) into the body's first axes. No
// R1 or R2 is in it; panels small against the turn make its error far below the tolerance asked of it.
[[nodiscard]] Vec integrated_body_displacement(const Entity& e, bool turns) noexcept
{
    const Vec rate = turns ? e.rate : Vec{0.0, 0.0, 0.0};
    const f64 turn = length(rate) * std::fabs(e.dt);
    const auto panels = static_cast<usize>(8.0 + (turn * 32.0));
    const f64 h = e.dt / static_cast<f64>(panels);
    constexpr f64 node = 0.7745966692414834;
    constexpr f64 outer_weight = 5.0 / 18.0;
    constexpr f64 middle_weight = 8.0 / 18.0;
    Vec sum{0.0, 0.0, 0.0};
    Vec compensation{0.0, 0.0, 0.0};
    const auto at = [&](f64 tau) noexcept
    {
        return rotate(turn_of(rate, tau), e.velocity + (e.acceleration * tau));
    };
    for (usize i = 0U; i < panels; ++i)
    {
        const f64 middle = (static_cast<f64>(i) + 0.5) * h;
        const Vec part = ((at(middle - (node * h * 0.5)) + at(middle + (node * h * 0.5))) * outer_weight +
                          at(middle) * middle_weight) *
                         h;
        const Vec lifted = sum + (part - compensation);
        compensation = (lifted - sum) - (part - compensation);
        sum = lifted;
    }
    return sum;
}

[[nodiscard]] Entity without_unused(Entity e, DeadReckoningAlgorithm algorithm) noexcept
{
    if (!accelerating(algorithm))
    {
        e.acceleration = {0.0, 0.0, 0.0};
    }
    return e;
}

// ---- Batches across every length, in columns ----

struct Columns
{
    f64 values[17U][kMaxLength + 4U];

    [[nodiscard]] const f64* at(usize column) const noexcept { return values[column]; }
};

void fill_columns(Columns& c, SeededWords& words, usize count) noexcept
{
    constexpr f64 betas[] = {0.0, 1e-300, 1e-160, 1e-9, 1e-3, 0.5, 0.999, 1.001, 2.0, 3.14159, 40.0, 1e6};
    for (usize i = 0U; i < count; ++i)
    {
        Entity e = random_entity(words, betas[words.next_u32() % std::size(betas)]);
        const f64 fields[17] = {
            e.position.x,     e.position.y,     e.position.z,     e.velocity.x,    e.velocity.y, e.velocity.z,
            e.acceleration.x, e.acceleration.y, e.acceleration.z, e.rate.x,        e.rate.y,     e.rate.z,
            e.orientation.x,  e.orientation.y,  e.orientation.z,  e.orientation.w, e.dt};
        for (usize k = 0U; k < 17U; ++k)
        {
            c.values[k][i] = fields[k];
        }
    }
}

[[nodiscard]] hn::DeadReckoningInput input_of(const Columns& c) noexcept
{
    return {{c.at(0), c.at(1), c.at(2)},
            {c.at(3), c.at(4), c.at(5)},
            {c.at(6), c.at(7), c.at(8)},
            {c.at(9), c.at(10), c.at(11)},
            {c.at(12), c.at(13), c.at(14), c.at(15)},
            c.at(16)};
}

struct Outputs
{
    f64 values[10U][kMaxLength + 4U];

    void reset() noexcept
    {
        for (auto& column : values)
        {
            for (f64& value : column)
            {
                value = kCanary;
            }
        }
    }

    [[nodiscard]] hn::DeadReckoningOutput columns() noexcept
    {
        return {{values[0], values[1], values[2]},
                {values[3], values[4], values[5]},
                {values[6], values[7], values[8], values[9]}};
    }
};

[[nodiscard]] bool same_bits(f64 a, f64 b) noexcept
{
    return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
}

// An ulp-free measure: how far apart, against a floor that keeps a result near zero meaningful.
[[nodiscard]] f64 relative_gap(Vec a, Vec b, f64 scale) noexcept
{
    return length(a - b) / scale;
}

template <class T>
[[nodiscard]] T restated_blend_position(T from_position, T from_velocity, T position, T velocity, T acceleration,
                                        T elapsed, T span) noexcept
{
    T fraction = elapsed / span;
    fraction = (fraction >= static_cast<T>(0)) ? fraction : static_cast<T>(0);
    fraction = (fraction <= static_cast<T>(1)) ? fraction : static_cast<T>(1);
    fraction = (span > static_cast<T>(0)) ? fraction : static_cast<T>(1);
    const T blended = from_velocity + ((velocity - from_velocity) * fraction);
    const T accelerated = static_cast<T>(0.5) * acceleration * elapsed * elapsed;
    const T projected = from_position + (blended * elapsed) + accelerated;
    const T target = position + (velocity * elapsed) + accelerated;
    return projected + ((target - projected) * fraction);
}

} // namespace

TEST_CASE("dead reckoning kernels: every level gives portable's bits on every length, every algorithm",
          "[dead-reckoning][kernels]")
{
    Columns columns;
    Outputs expected;
    Outputs got;
    SeededWords words(0x44524D31ULL, 0U);
    for (usize count = 0U; count <= kMaxLength; ++count)
    {
        fill_columns(columns, words, count + 4U > kMaxLength + 4U ? kMaxLength + 4U : count + 4U);
        const hn::DeadReckoningInput in = input_of(columns);
        for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
        {
            expected.reset();
            portable().dead_reckon(algorithm, in, count, expected.columns());
            for (const KernelLevel<MotionKernels>& level : hn::motion_kernel_levels())
            {
                if (!crd::test::kernel_level_runs_here(level))
                {
                    continue;
                }
                got.reset();
                level.table->dead_reckon(algorithm, in, count, got.columns());
                bool same = true;
                for (usize k = 0U; k < 10U; ++k)
                {
                    for (usize i = 0U; i < kMaxLength + 4U; ++i)
                    {
                        same = same && same_bits(got.values[k][i], expected.values[k][i]);
                    }
                }
                INFO(crd::cpu_level_name(level.level)
                     << " algorithm " << static_cast<int>(algorithm) << " count " << count);
                CHECK(same);
            }
        }
    }
}

TEST_CASE("dead reckoning kernels: nothing past the batch and no column the algorithm does not move",
          "[dead-reckoning][kernels]")
{
    Columns columns;
    Outputs got;
    SeededWords words(0x44524D32ULL, 0U);
    fill_columns(columns, words, kMaxLength + 4U);
    const hn::DeadReckoningInput in = input_of(columns);
    for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
    {
        for (usize count = 0U; count <= kMaxLength; ++count)
        {
            got.reset();
            portable().dead_reckon(algorithm, in, count, got.columns());
            bool held = true;
            for (usize k = 0U; k < 10U; ++k)
            {
                const bool written = k < 3U || (k < 6U && accelerating(algorithm)) || (k >= 6U && rotating(algorithm));
                for (usize i = 0U; i < kMaxLength + 4U; ++i)
                {
                    held = held && ((written && i < count) || got.values[k][i] == kCanary);
                }
            }
            INFO("algorithm " << static_cast<int>(algorithm) << " count " << count);
            CHECK(held);
        }
    }
}

TEST_CASE("dead reckoning kernels: outputs may be the input columns", "[dead-reckoning][kernels]")
{
    Columns columns;
    SeededWords words(0x44524D33ULL, 0U);
    constexpr usize entities = 37U;
    fill_columns(columns, words, entities);
    for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
    {
        for (const KernelLevel<MotionKernels>& level : hn::motion_kernel_levels())
        {
            if (!crd::test::kernel_level_runs_here(level))
            {
                continue;
            }
            Outputs apart;
            apart.reset();
            level.table->dead_reckon(algorithm, input_of(columns), entities, apart.columns());
            Columns inplace = columns;
            const hn::DeadReckoningOutput same{
                {inplace.values[0], inplace.values[1], inplace.values[2]},
                {inplace.values[3], inplace.values[4], inplace.values[5]},
                {inplace.values[12], inplace.values[13], inplace.values[14], inplace.values[15]}};
            level.table->dead_reckon(algorithm, input_of(inplace), entities, same);
            bool held = true;
            for (usize i = 0U; i < entities; ++i)
            {
                for (usize k = 0U; k < 3U; ++k)
                {
                    held = held && same_bits(inplace.values[k][i], apart.values[k][i]);
                }
                for (usize k = 0U; k < 3U && accelerating(algorithm); ++k)
                {
                    held = held && same_bits(inplace.values[3U + k][i], apart.values[3U + k][i]);
                }
                for (usize k = 0U; k < 4U && rotating(algorithm); ++k)
                {
                    held = held && same_bits(inplace.values[12U + k][i], apart.values[6U + k][i]);
                }
            }
            INFO(crd::cpu_level_name(level.level) << " algorithm " << static_cast<int>(algorithm));
            CHECK(held);
        }
    }
}

TEST_CASE("dead reckoning: the public call runs the process's level, and only numbers 1 to 9 are algorithms",
          "[dead-reckoning]")
{
    const f64 x = 1.0;
    const f64 v = 2.0;
    const f64 dt = 0.25;
    f64 out = 0.0;
    f64 unused = 0.0;
    hn::dead_reckon(DeadReckoningAlgorithm::Fpw, {{&x, &x, &x}, {&v, &v, &v}, {}, {}, {}, &dt}, 1U,
                    {{&out, &unused, &unused}, {}, {}});
    CHECK(out == 1.5);
    CHECK_FALSE(hn::dead_reckoning_algorithm_known(0U));
    CHECK_FALSE(hn::dead_reckoning_algorithm_known(10U));
    CHECK_FALSE(hn::dead_reckoning_algorithm_known(255U));
    for (crd::u8 number = 1U; number <= 9U; ++number)
    {
        CHECK(hn::dead_reckoning_algorithm_known(number));
    }
}

TEST_CASE("dead reckoning: the world algorithms are the extrapolation and the static one holds", "[dead-reckoning]")
{
    SeededWords words(0x44524D34ULL, 0U);
    for (u32 round = 0U; round < 500U; ++round)
    {
        const Entity e = random_entity(words, 0.7);
        const Vec linear = e.position + (e.velocity * e.dt);
        const Vec quadratic = linear + (e.acceleration * (0.5 * e.dt * e.dt));
        CHECK(same_bits(reckon_one(portable(), DeadReckoningAlgorithm::Fpw, e).position.y, linear.y));
        CHECK(same_bits(reckon_one(portable(), DeadReckoningAlgorithm::Rpw, e).position.z, linear.z));
        CHECK(same_bits(reckon_one(portable(), DeadReckoningAlgorithm::Fvw, e).position.x, quadratic.x));
        const Reckoned turned = reckon_one(portable(), DeadReckoningAlgorithm::Rvw, e);
        CHECK(same_bits(turned.position.z, quadratic.z));
        CHECK(same_bits(turned.velocity.x, e.velocity.x + (e.acceleration.x * e.dt)));
        const Reckoned held = reckon_one(portable(), DeadReckoningAlgorithm::Static, e);
        CHECK(same_bits(held.position.x, e.position.x));
        CHECK(same_bits(held.orientation.w, e.orientation.w));
    }
}

TEST_CASE("dead reckoning: every algorithm follows the motion it means, at any turn", "[dead-reckoning]")
{
    // The turn per span from nothing through the series' end to many revolutions.
    constexpr f64 betas[] = {0.0,  1e-300, 1e-160, 1e-30, 1e-12, 1e-8, 1e-5, 1e-3, 0.01, 0.1,  0.5,  0.9,
                             0.99, 0.999,  1.0,    1.001, 1.01,  1.5,  3.14, 6.28, 10.0, 50.0, 400.0};
    SeededWords words(0x44524D35ULL, 0U);
    f64 worst_position = 0.0;
    f64 worst_orientation = 0.0;
    for (const f64 beta : betas)
    {
        for (u32 round = 0U; round < 40U; ++round)
        {
            const Entity drawn = random_entity(words, beta);
            for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
            {
                if (algorithm == DeadReckoningAlgorithm::Static)
                {
                    continue;
                }
                // From the origin, so the gap measures the integrals and not a large position's rounding.
                Entity e = without_unused(drawn, algorithm);
                e.position = {0.0, 0.0, 0.0};
                const Reckoned got = reckon_one(portable(), algorithm, e);
                const bool turns = rotating(algorithm);
                Vec moved{0.0, 0.0, 0.0};
                if (on_body_axes(algorithm))
                {
                    moved = rotate(e.orientation, integrated_body_displacement(e, turns));
                }
                else
                {
                    moved = (e.velocity * e.dt) + (e.acceleration * (0.5 * e.dt * e.dt));
                }
                const Vec reckoned = got.position;
                const f64 scale = 1.0 + length(moved);
                worst_position = std::fmax(worst_position, relative_gap(reckoned, moved, scale));
                INFO("beta " << beta << " algorithm " << static_cast<int>(algorithm));
                CHECK(relative_gap(reckoned, moved, scale) < 1e-12);
                if (accelerating(algorithm))
                {
                    const Vec gained = e.velocity + (e.acceleration * e.dt);
                    CHECK(got.velocity.x == gained.x);
                    CHECK(got.velocity.z == gained.z);
                }
                if (turns)
                {
                    // Many short true steps composed, the attitude the entity reaches.
                    const auto steps = static_cast<usize>(8.0 + (beta * 4.0));
                    Quat composed = e.orientation;
                    const Quat step = turn_of(e.rate, e.dt / static_cast<f64>(steps));
                    for (usize s = 0U; s < steps; ++s)
                    {
                        composed = composed * step;
                    }
                    const f64 gap =
                        std::fabs(composed.x - got.orientation.x) + std::fabs(composed.y - got.orientation.y) +
                        std::fabs(composed.z - got.orientation.z) + std::fabs(composed.w - got.orientation.w);
                    worst_orientation = std::fmax(worst_orientation, gap);
                    CHECK(gap < 1e-12);
                    const f64 unit = (got.orientation.x * got.orientation.x) + (got.orientation.y * got.orientation.y) +
                                     (got.orientation.z * got.orientation.z) + (got.orientation.w * got.orientation.w);
                    CHECK(std::fabs(unit - 1.0) < 8e-15);
                }
            }
        }
    }
    INFO("worst displacement " << worst_position << ", worst attitude " << worst_orientation);
    CHECK(worst_position < 1e-12);
}

TEST_CASE("dead reckoning: the standard's R1 and R2 and its matrix DR, restated from the closed forms",
          "[dead-reckoning]")
{
    // Where the closed forms do not cancel, IEEE 1278.1's expressions term by term in world-to-body matrices, as
    // Open-DIS's AbstractDeadReckoner computes them.
    SeededWords words(0x44524D36ULL, 0U);
    for (const f64 beta : {1.2, 2.5, 3.14159, 7.0, 31.0})
    {
        for (u32 round = 0U; round < 50U; ++round)
        {
            const Entity e = random_entity(words, beta);
            const f64 mag2 = dot(e.rate, e.rate);
            const f64 mag = std::sqrt(mag2);
            const f64 angle = mag * e.dt;
            const f64 s = std::sin(angle);
            const f64 c = std::cos(angle);
            const auto apply = [&](f64 axis, f64 plane, f64 skew, Vec v) noexcept
            {
                return (e.rate * (axis * dot(e.rate, v))) + (v * plane) + (cross(e.rate, v) * skew);
            };
            const Vec r1 = apply((angle - s) / (mag2 * mag), s / mag, (1.0 - c) / mag2, e.velocity);
            const f64 top = c + (angle * s) - 1.0;
            const Vec r2 = apply(((0.5 * angle * angle) - top) / (mag2 * mag2), top / mag2,
                                 (s - (angle * c)) / (mag2 * mag), e.acceleration);
            const Vec rpb = reckon_one(portable(), DeadReckoningAlgorithm::Rpb, e).position - e.position;
            const Vec rvb = reckon_one(portable(), DeadReckoningAlgorithm::Rvb, e).position - e.position;
            const Vec expect_rpb = rotate(e.orientation, r1);
            const Vec expect_rvb = rotate(e.orientation, r1 + r2);
            CHECK(relative_gap(rpb, expect_rpb, 1.0 + length(expect_rpb)) < 2e-12);
            CHECK(relative_gap(rvb, expect_rvb, 1.0 + length(expect_rvb)) < 2e-12);

            // [WtoB]t = DR [WtoB]0, with DR = (1 - cos) / mag^2 w w^T + cos I - sin / mag [w x]. A body axis in world
            // coordinates is a row of WtoB; test each row through the reckoned quaternion.
            const Quat turned = reckon_one(portable(), DeadReckoningAlgorithm::Rpb, e).orientation;
            const Vec axes[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
            const Vec rows0[3] = {rotate(e.orientation, axes[0]), rotate(e.orientation, axes[1]),
                                  rotate(e.orientation, axes[2])};
            const f64 k = (1.0 - c) / mag2;
            const f64 q = s / mag;
            const f64 dr[3][3] = {{(k * e.rate.x * e.rate.x) + c, (k * e.rate.x * e.rate.y) + (q * e.rate.z),
                                   (k * e.rate.x * e.rate.z) - (q * e.rate.y)},
                                  {(k * e.rate.y * e.rate.x) - (q * e.rate.z), (k * e.rate.y * e.rate.y) + c,
                                   (k * e.rate.y * e.rate.z) + (q * e.rate.x)},
                                  {(k * e.rate.z * e.rate.x) + (q * e.rate.y),
                                   (k * e.rate.z * e.rate.y) - (q * e.rate.x), (k * e.rate.z * e.rate.z) + c}};
            for (usize row = 0U; row < 3U; ++row)
            {
                const Vec expect = (rows0[0] * dr[row][0]) + (rows0[1] * dr[row][1]) + (rows0[2] * dr[row][2]);
                CHECK(length(rotate(turned, axes[row]) - expect) < 1e-13);
            }
        }
    }
}

TEST_CASE("dead reckoning: an entity's answer does not depend on the turns of the entities beside it",
          "[dead-reckoning]")
{
    Columns columns;
    Outputs together;
    SeededWords words(0x44524D3BULL, 0U);
    for (u32 round = 0U; round < 20U; ++round)
    {
        fill_columns(columns, words, kMaxLength);
        const hn::DeadReckoningInput in = input_of(columns);
        for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
        {
            together.reset();
            portable().dead_reckon(algorithm, in, kMaxLength, together.columns());
            bool same = true;
            for (usize i = 0U; i < kMaxLength; ++i)
            {
                const auto& c = columns.values;
                const Entity alone{{c[0][i], c[1][i], c[2][i]},
                                   {c[3][i], c[4][i], c[5][i]},
                                   {c[6][i], c[7][i], c[8][i]},
                                   {c[9][i], c[10][i], c[11][i]},
                                   {c[12][i], c[13][i], c[14][i], c[15][i]},
                                   c[16][i]};
                const Reckoned one = reckon_one(portable(), algorithm, alone);
                same = same && same_bits(together.values[0][i], one.position.x) &&
                       same_bits(together.values[1][i], one.position.y) &&
                       same_bits(together.values[2][i], one.position.z);
                if (accelerating(algorithm))
                {
                    same = same && same_bits(together.values[3][i], one.velocity.x) &&
                           same_bits(together.values[5][i], one.velocity.z);
                }
                if (rotating(algorithm))
                {
                    same = same && same_bits(together.values[6][i], one.orientation.x) &&
                           same_bits(together.values[7][i], one.orientation.y) &&
                           same_bits(together.values[8][i], one.orientation.z) &&
                           same_bits(together.values[9][i], one.orientation.w);
                }
            }
            INFO("algorithm " << static_cast<int>(algorithm));
            CHECK(same);
        }
    }
}

TEST_CASE("dead reckoning: a slow turn keeps its precision and meets the fixed algorithm", "[dead-reckoning]")
{
    SeededWords words(0x44524D37ULL, 0U);
    for (u32 round = 0U; round < 200U; ++round)
    {
        Entity e = random_entity(words, 1.0);
        e.position = {0.0, 0.0, 0.0};
        // With no rate at all the rotating algorithm is the fixed one, bit for bit.
        Entity still = e;
        still.rate = {0.0, 0.0, 0.0};
        const Reckoned rvb = reckon_one(portable(), DeadReckoningAlgorithm::Rvb, still);
        const Reckoned fvb = reckon_one(portable(), DeadReckoningAlgorithm::Fvb, still);
        CHECK(same_bits(rvb.position.x, fvb.position.x));
        CHECK(same_bits(rvb.position.y, fvb.position.y));
        CHECK(same_bits(rvb.position.z, fvb.position.z));
        CHECK(same_bits(rvb.orientation.w, e.orientation.w));
        // Across the series' end the two forms meet: a turn just below and just above it differs by its first
        // derivative.
        const f64 below = 1.0 - 1e-13;
        const f64 above = 1.0 + 1e-13;
        Entity under = e;
        Entity over = e;
        under.rate = e.rate * below;
        over.rate = e.rate * above;
        const Vec a = reckon_one(portable(), DeadReckoningAlgorithm::Rvb, under).position;
        const Vec b = reckon_one(portable(), DeadReckoningAlgorithm::Rvb, over).position;
        CHECK(length(a - b) < 1e-12 * (1.0 + length(a)));
    }
}

TEST_CASE("dead reckoning: reckoning on from a reckoned state is the same curve, backwards too", "[dead-reckoning]")
{
    SeededWords words(0x44524D38ULL, 0U);
    for (const DeadReckoningAlgorithm algorithm :
         {DeadReckoningAlgorithm::Rvb, DeadReckoningAlgorithm::Rpb, DeadReckoningAlgorithm::Rvw,
          DeadReckoningAlgorithm::Rpw, DeadReckoningAlgorithm::Fvb, DeadReckoningAlgorithm::Fvw})
    {
        for (u32 round = 0U; round < 200U; ++round)
        {
            Entity e = without_unused(random_entity(words, 0.2 + (5.0 * unit_value(words))), algorithm);
            const f64 total = e.dt;
            e.dt = total * 0.375;
            const Reckoned first = reckon_one(portable(), algorithm, e);
            Entity next = e;
            next.position = first.position;
            next.velocity = first.velocity;
            next.orientation = first.orientation;
            next.dt = total - e.dt;
            const Reckoned second = reckon_one(portable(), algorithm, next);
            e.dt = total;
            const Reckoned whole = reckon_one(portable(), algorithm, e);
            INFO("algorithm " << static_cast<int>(algorithm));
            CHECK(length(second.position - whole.position) < 2e-11);
            CHECK(std::fabs(second.orientation.w - whole.orientation.w) < 1e-13);

            Entity back = e;
            back.position = whole.position;
            back.velocity = whole.velocity;
            back.orientation = whole.orientation;
            back.dt = -total;
            const Reckoned returned = reckon_one(portable(), algorithm, back);
            CHECK(length(returned.position - e.position) < 2e-11);
            CHECK(std::fabs(returned.orientation.x - e.orientation.x) < 1e-13);
        }
    }
}

TEST_CASE("dead reckoning: a rate no turn can be made of is none, and the edges of dt", "[dead-reckoning]")
{
    constexpr f64 inf = std::numeric_limits<f64>::infinity();
    const f64 not_a_number = std::numeric_limits<f64>::quiet_NaN();
    SeededWords words(0x44524D39ULL, 0U);
    const Entity base = random_entity(words, 0.5);
    for (const Vec wild : {Vec{inf, 0.0, 0.0}, Vec{0.0, -inf, 1.0}, Vec{not_a_number, 1.0, 1.0}, Vec{1e200, 1e200, 0.0},
                           Vec{std::numeric_limits<f64>::max(), 0.0, 0.0}})
    {
        Entity e = base;
        e.rate = wild;
        for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
        {
            if (!rotating(algorithm))
            {
                continue;
            }
            Entity still = e;
            still.rate = {0.0, 0.0, 0.0};
            const Reckoned got = reckon_one(portable(), algorithm, e);
            const Reckoned none = reckon_one(portable(), algorithm, still);
            INFO("algorithm " << static_cast<int>(algorithm));
            CHECK(same_bits(got.position.x, none.position.x));
            CHECK(same_bits(got.position.z, none.position.z));
            CHECK(same_bits(got.orientation.x, none.orientation.x));
            CHECK(same_bits(got.orientation.w, none.orientation.w));
        }
    }
    // dt = 0 is the update itself; an enormous turn stays a unit quaternion.
    Entity zero = base;
    zero.dt = 0.0;
    for (const DeadReckoningAlgorithm algorithm : kAlgorithms)
    {
        const Reckoned got = reckon_one(portable(), algorithm, zero);
        CHECK(got.position.x == base.position.x);
        CHECK(got.position.z == base.position.z);
        CHECK(got.orientation.x == base.orientation.x);
    }
    Entity huge = base;
    huge.rate = {1e120, -3e120, 2e119};
    huge.dt = 7.0;
    const Reckoned spun = reckon_one(portable(), DeadReckoningAlgorithm::Rvb, huge);
    const f64 unit = (spun.orientation.x * spun.orientation.x) + (spun.orientation.y * spun.orientation.y) +
                     (spun.orientation.z * spun.orientation.z) + (spun.orientation.w * spun.orientation.w);
    CHECK(std::fabs(unit - 1.0) < 1e-14);
    CHECK(std::isfinite(spun.position.x));
    CHECK(std::isfinite(spun.position.y));
}

TEST_CASE("DIS Euler angles: the standard's world-to-body matrix, both ways, and straight up",
          "[dead-reckoning][euler]")
{
    SeededWords words(0x44524D3AULL, 0U);
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const hn::DisEuler euler{signed_value(words, std::numbers::pi), signed_value(words, std::numbers::pi / 2.0),
                                 signed_value(words, std::numbers::pi)};
        const hn::OrientationXyzw q = hn::orientation_from_dis_euler(euler);
        const Quat body_to_world{q.x, q.y, q.z, q.w};
        const f64 cp = std::cos(euler.psi);
        const f64 sp = std::sin(euler.psi);
        const f64 ct = std::cos(euler.theta);
        const f64 st = std::sin(euler.theta);
        const f64 cf = std::cos(euler.phi);
        const f64 sf = std::sin(euler.phi);
        // Each row of world to body, as KDIS's orientationReset and the standard write it, is a body axis in the world.
        const Vec rows[3] = {{ct * cp, ct * sp, -st},
                             {(sf * st * cp) - (cf * sp), (sf * st * sp) + (cf * cp), sf * ct},
                             {(cf * st * cp) + (sf * sp), (cf * st * sp) - (sf * cp), cf * ct}};
        CHECK(length(rotate(body_to_world, {1.0, 0.0, 0.0}) - rows[0]) < 1e-14);
        CHECK(length(rotate(body_to_world, {0.0, 1.0, 0.0}) - rows[1]) < 1e-14);
        CHECK(length(rotate(body_to_world, {0.0, 0.0, 1.0}) - rows[2]) < 1e-14);
        const hn::DisEuler back = hn::dis_euler_from_orientation(q);
        CHECK(std::fabs(back.psi - euler.psi) < 1e-11);
        CHECK(std::fabs(back.theta - euler.theta) < 1e-11);
        CHECK(std::fabs(back.phi - euler.phi) < 1e-11);
    }
    for (const f64 pitch : {std::numbers::pi / 2.0, -std::numbers::pi / 2.0})
    {
        for (u32 round = 0U; round < 200U; ++round)
        {
            const hn::DisEuler euler{signed_value(words, 3.0), pitch, signed_value(words, 3.0)};
            const hn::OrientationXyzw q = hn::orientation_from_dis_euler(euler);
            const hn::DisEuler back = hn::dis_euler_from_orientation(q);
            CHECK(back.theta == Catch::Approx(pitch).margin(1e-7));
            CHECK(back.phi == 0.0);
            const hn::OrientationXyzw again = hn::orientation_from_dis_euler(back);
            const Quat before{q.x, q.y, q.z, q.w};
            const Quat after{again.x, again.y, again.z, again.w};
            // The same attitude, whichever way the angle is shared out.
            for (const Vec axis : {Vec{1.0, 0.0, 0.0}, Vec{0.0, 1.0, 0.0}, Vec{0.0, 0.0, 1.0}})
            {
                CHECK(length(rotate(before, axis) - rotate(after, axis)) < 1e-7);
            }
        }
    }
}

namespace
{

template <class T> void check_blend_levels(u64 seed)
{
    constexpr usize most = 70U;
    SeededWords words(seed, 0U);
    const auto value = [&words](T scale) noexcept
    {
        return static_cast<T>(signed_value(words, static_cast<f64>(scale)));
    };
    T from_position[most];
    T from_velocity[most];
    T position[most];
    T velocity[most];
    T acceleration[most];
    T elapsed[most];
    T span[most];
    T zero[most] = {};
    for (usize count = 0U; count <= most; ++count)
    {
        for (usize i = 0U; i < most; ++i)
        {
            from_position[i] = value(static_cast<T>(100));
            from_velocity[i] = value(static_cast<T>(20));
            position[i] = value(static_cast<T>(100));
            velocity[i] = value(static_cast<T>(20));
            acceleration[i] = value(static_cast<T>(9));
            elapsed[i] = value(static_cast<T>(2));
            span[i] = (words.next_u32() % 5U == 0U) ? static_cast<T>(0) : value(static_cast<T>(1));
        }
        for (const KernelLevel<MotionKernels>& level : hn::motion_kernel_levels())
        {
            if (!crd::test::kernel_level_runs_here(level))
            {
                continue;
            }
            for (const T* rate : {static_cast<const T*>(acceleration), static_cast<const T*>(nullptr)})
            {
                T out_position[most + 2U];
                T out_velocity[most + 2U];
                for (usize i = 0U; i < most + 2U; ++i)
                {
                    out_position[i] = static_cast<T>(kCanary);
                    out_velocity[i] = static_cast<T>(kCanary);
                }
                const hn::ProjectiveBlendSpan<T> blend{from_position, from_velocity, position, velocity,
                                                       rate,          elapsed,       span};
                if constexpr (sizeof(T) == sizeof(f32))
                {
                    level.table->projective_blend_f32(blend, count, out_position, out_velocity);
                }
                else
                {
                    level.table->projective_blend_f64(blend, count, out_position, out_velocity);
                }
                bool same = true;
                for (usize i = 0U; i < count; ++i)
                {
                    const T expected =
                        restated_blend_position<T>(from_position[i], from_velocity[i], position[i], velocity[i],
                                                   rate != nullptr ? rate[i] : zero[i], elapsed[i], span[i]);
                    same = same && std::memcmp(&expected, &out_position[i], sizeof(T)) == 0;
                }
                for (usize i = count; i < most + 2U; ++i)
                {
                    same = same && out_position[i] == static_cast<T>(kCanary) &&
                           out_velocity[i] == static_cast<T>(kCanary);
                }
                INFO(crd::cpu_level_name(level.level) << " count " << count);
                CHECK(same);
            }
        }
    }
}

} // namespace

TEST_CASE("projective blend: every level gives the restated position, on every length", "[dead-reckoning][kernels]")
{
    check_blend_levels<f32>(0x50564231ULL);
    check_blend_levels<f64>(0x50564232ULL);
}

TEST_CASE("projective blend: where it starts, where it ends, its derivative and a step at the end", "[dead-reckoning]")
{
    SeededWords words(0x50564233ULL, 0U);
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const f64 from_position = signed_value(words, 100.0);
        const f64 from_velocity = signed_value(words, 20.0);
        const f64 position = signed_value(words, 100.0);
        const f64 velocity = signed_value(words, 20.0);
        const f64 acceleration = signed_value(words, 9.0);
        const f64 span = 0.05 + unit_value(words);
        const auto blend_at = [&](f64 elapsed, f64 width, f64& at, f64& rate) noexcept
        {
            hn::projective_blend(hn::ProjectiveBlendSpan<f64>{&from_position, &from_velocity, &position, &velocity,
                                                              &acceleration, &elapsed, &width},
                                 1U, &at, &rate);
        };
        f64 at = 0.0;
        f64 rate = 0.0;
        blend_at(0.0, span, at, rate);
        CHECK(at == from_position);
        // Past the span it is the updated path alone, position and velocity.
        const f64 later = span * (1.0 + unit_value(words));
        blend_at(later, span, at, rate);
        const f64 target = position + (velocity * later) + (0.5 * acceleration * later * later);
        CHECK(at == Approx(target).margin(1e-11));
        CHECK(rate == velocity + (acceleration * later));
        // A span that is not one snaps there at once.
        for (const f64 none : {0.0, -1.0, std::numeric_limits<f64>::quiet_NaN()})
        {
            blend_at(0.25, none, at, rate);
            CHECK(at == Approx(position + (velocity * 0.25) + (0.5 * acceleration * 0.0625)).margin(1e-11));
            CHECK(rate == velocity + (acceleration * 0.25));
        }
        // Inside the span the velocity written is the path's derivative: central differences on a smooth stretch.
        const f64 inside = span * (0.1 + (0.8 * unit_value(words)));
        constexpr f64 step = 1e-5;
        f64 before = 0.0;
        f64 after = 0.0;
        f64 ignored = 0.0;
        blend_at(inside - step, span, before, ignored);
        blend_at(inside + step, span, after, ignored);
        blend_at(inside, span, at, rate);
        CHECK(rate == Approx((after - before) / (2.0 * step)).epsilon(1e-5).margin(1e-5));
    }
}

TEST_CASE("projective blend: a negative or not-a-number time holds the start of the blend, in place too",
          "[dead-reckoning]")
{
    f64 from_position[2] = {4.0, 4.0};
    const f64 from_velocity[2] = {1.0, 1.0};
    const f64 position[2] = {8.0, 8.0};
    const f64 velocity[2] = {3.0, 3.0};
    const f64 elapsed[2] = {-0.5, std::numeric_limits<f64>::quiet_NaN()};
    const f64 span[2] = {1.0, 1.0};
    f64 rate[2] = {0.0, 0.0};
    const hn::ProjectiveBlendSpan<f64> blend{from_position, from_velocity, position, velocity, nullptr, elapsed, span};
    hn::projective_blend(blend, 2U, from_position, rate);
    CHECK(from_position[0] == 3.5); // fraction 0: from_position + from_velocity * elapsed
    CHECK(std::isnan(from_position[1]));
}
