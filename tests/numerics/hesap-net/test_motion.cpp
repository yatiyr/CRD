// HESAP-NET's replicated-motion sampling: every kernel level bit for bit against restated scalar expressions, on every
// length, with nothing written past a batch (crd-hesap-net-kernels-at-<level> runs the [kernels] cases under each forced
// level); the curves held to what they mean (ends, paths they reproduce, the true arc in long double); the rewind
// history against a linear search over every stamp.

#include "kernel_levels.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/net/motion.hpp>
#include <crd/hesap/net/rewind.hpp>

#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <string_view>

namespace hn = crd::hesap::net;
using Catch::Approx;
using crd::CpuLevel;
using crd::f32;
using crd::f64;
using crd::i64;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using hn::MotionKernels;
using hn::RewindMoved;
using hn::RewindPlace;

namespace
{

constexpr usize kMaxLength = 70U;
constexpr f32 kCanary32 = -123.5F;
constexpr f64 kCanary64 = -1234.5;

[[nodiscard]] u32 bits_of(f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}

[[nodiscard]] u64 bits_of(f64 value) noexcept
{
    return std::bit_cast<u64>(value);
}

template <class T>
[[nodiscard]] T unit_value(SeededWords& words) noexcept
{
    return static_cast<T>(words.next_u32() >> 8U) / static_cast<T>(16777216);
}

// Values a snapshot holds: many magnitudes, signs and exact zeros.
template <class T>
[[nodiscard]] T random_value(SeededWords& words) noexcept
{
    const u32 pick = words.next_u32() % 8U;
    if (pick == 0U)
    {
        return (words.next_u32() & 1U) != 0U ? static_cast<T>(-0.0) : static_cast<T>(0.0);
    }
    const T magnitude = std::ldexp(static_cast<T>(1), static_cast<int>(words.next_u32() % 41U) - 20);
    return ((unit_value<T>(words) * static_cast<T>(2)) - static_cast<T>(1)) * magnitude;
}

template <class T>
[[nodiscard]] T random_amount(SeededWords& words) noexcept
{
    const u32 pick = words.next_u32() % 6U;
    if (pick == 0U)
    {
        return static_cast<T>(0);
    }
    if (pick == 1U)
    {
        return static_cast<T>(1);
    }
    return (unit_value<T>(words) * static_cast<T>(3)) - static_cast<T>(1); // Past both ends too.
}

void random_unit(SeededWords& words, f32* q) noexcept
{
    f64 v[4];
    f64 sum = 0.0;
    for (f64& component : v)
    {
        component = (static_cast<f64>(words.next_u32()) / 2147483648.0) - 1.0;
        sum += component * component;
    }
    const f64 scale = 1.0 / std::sqrt(sum > 1e-12 ? sum : 1.0);
    for (usize i = 0U; i < 4U; ++i)
    {
        q[i] = static_cast<f32>(v[i] * scale);
    }
}

// ---- The restated scalars: the headers' meaning, no lane in sight --------------------------------------------------

template <class T>
[[nodiscard]] T lerp_of(T a, T b, T t) noexcept
{
    return a + ((b - a) * t);
}

template <class T>
[[nodiscard]] T hermite_of(T p0, T v0, T p1, T v1, T t, T span) noexcept
{
    const T t2 = t * t;
    const T t3 = t2 * t;
    const T h1 = (static_cast<T>(2) * t3) - (static_cast<T>(3) * t2) + static_cast<T>(1);
    const T h2 = (static_cast<T>(-2) * t3) + (static_cast<T>(3) * t2);
    const T h3 = t3 - (static_cast<T>(2) * t2) + t;
    const T h4 = t3 - t2;
    return (h1 * p0) + (h2 * p1) + (h3 * (v0 * span)) + (h4 * (v1 * span));
}

template <class T>
[[nodiscard]] T linear_of(T p, T v, T dt) noexcept
{
    return p + (v * dt);
}

template <class T>
[[nodiscard]] T quadratic_of(T p, T v, T a, T dt) noexcept
{
    return p + (v * dt) + (a * (static_cast<T>(0.5) * dt * dt));
}

[[nodiscard]] f32 dot_of(const f32* a, const f32* b) noexcept
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
}

void nlerp_of(const f32* a, const f32* b, f32 t, f32* out) noexcept
{
    const bool same_side = dot_of(a, b) >= 0.0F;
    f32 chord[4];
    for (usize i = 0U; i < 4U; ++i)
    {
        chord[i] = a[i] + (((same_side ? b[i] : -b[i]) - a[i]) * t);
    }
    const f32 length2 = dot_of(chord, chord);
    if (!(length2 > 0.0F && length2 < std::numeric_limits<f32>::infinity()))
    {
        out[0] = 0.0F;
        out[1] = 0.0F;
        out[2] = 0.0F;
        out[3] = 1.0F;
        return;
    }
    const f32 inverse = 1.0F / std::sqrt(length2);
    for (usize i = 0U; i < 4U; ++i)
    {
        out[i] = chord[i] * inverse;
    }
}

// Eberly's ratio restated: factor i is (t^2 - i^2) / (i * (2 * i + 1)) * x, the sixteenth scaled by 1.902.
[[nodiscard]] f32 ratio_of(f32 t, f32 x) noexcept
{
    const f32 square = t * t;
    f32 product = 1.0F;
    for (u32 i = 16U; i > 0U; --i)
    {
        const auto index = static_cast<f32>(i);
        const f32 odd = (2.0F * index) + 1.0F;
        const f32 scale = (i == 16U) ? 1.902F : 1.0F;
        const f32 u = scale / (index * odd);
        const f32 v = (scale * index) / odd;
        product = 1.0F + ((((u * square) - v) * x) * product);
    }
    return t * product;
}

void slerp_of(const f32* a, const f32* b, f32 raw, f32* out) noexcept
{
    f32 t = 0.0F;
    if (raw >= 0.0F)
    {
        t = (raw <= 1.0F) ? raw : 1.0F;
    }
    const f32 cosine = dot_of(a, b);
    const bool same_side = cosine >= 0.0F;
    const f32 magnitude = same_side ? cosine : -cosine;
    const f32 x = ((magnitude <= 1.0F) ? magnitude : 1.0F) - 1.0F;
    const f32 from_a = ratio_of(1.0F - t, x);
    const f32 ratio = ratio_of(t, x);
    const f32 from_b = same_side ? ratio : -ratio;
    for (usize i = 0U; i < 4U; ++i)
    {
        out[i] = (from_a * a[i]) + (from_b * b[i]);
    }
}

// The true arc in long double, on the shorter way round.
void true_slerp(const f32* a, const f32* b, f32 t, long double* out) noexcept
{
    long double cosine = 0.0L;
    for (usize i = 0U; i < 4U; ++i)
    {
        cosine += static_cast<long double>(a[i]) * static_cast<long double>(b[i]);
    }
    const long double sign = (cosine >= 0.0L) ? 1.0L : -1.0L;
    cosine = std::fabs(cosine);
    const auto real_t = static_cast<long double>(t);
    if (cosine >= 1.0L)
    {
        for (usize i = 0U; i < 4U; ++i)
        {
            out[i] = (1.0L - real_t) * static_cast<long double>(a[i]) + real_t * sign * static_cast<long double>(b[i]);
        }
        return;
    }
    const long double angle = std::acos(cosine);
    const long double sine = std::sin(angle);
    const long double from_a = std::sin((1.0L - real_t) * angle) / sine;
    const long double from_b = sign * std::sin(real_t * angle) / sine;
    for (usize i = 0U; i < 4U; ++i)
    {
        out[i] = (from_a * static_cast<long double>(a[i])) + (from_b * static_cast<long double>(b[i]));
    }
}

template <class T>
[[nodiscard]] T canary_of() noexcept
{
    if constexpr (sizeof(T) == sizeof(f32))
    {
        return kCanary32;
    }
    else
    {
        return kCanary64;
    }
}

// Runs one routine at `count` and checks every result's bits and the guards on both sides.
template <class T, class Run, class Want>
void check_batch(usize count, Run run, Want want)
{
    T out[kMaxLength + 2U];
    for (T& value : out)
    {
        value = canary_of<T>();
    }
    run(out + 1U);
    CHECK(bits_of(out[0]) == bits_of(canary_of<T>()));
    CHECK(bits_of(out[count + 1U]) == bits_of(canary_of<T>()));
    bool same = true;
    for (usize i = 0U; i < count; ++i)
    {
        same = same && bits_of(out[i + 1U]) == bits_of(want(i));
    }
    CHECK(same);
}

template <class T>
struct Routines;

template <>
struct Routines<f32>
{
    static constexpr auto kLerp = &MotionKernels::lerp_f32;
    static constexpr auto kHermite = &MotionKernels::hermite_f32;
    static constexpr auto kExtrapolate = &MotionKernels::extrapolate_f32;
};

template <>
struct Routines<f64>
{
    static constexpr auto kLerp = &MotionKernels::lerp_f64;
    static constexpr auto kHermite = &MotionKernels::hermite_f64;
    static constexpr auto kExtrapolate = &MotionKernels::extrapolate_f64;
};

template <class T>
void check_components(const MotionKernels& table, u64 stream)
{
    const auto lerp_at = table.*Routines<T>::kLerp;
    const auto hermite_at = table.*Routines<T>::kHermite;
    const auto extrapolate_at = table.*Routines<T>::kExtrapolate;
    SeededWords words(32U, stream);
    T a[kMaxLength];
    T b[kMaxLength];
    T c[kMaxLength];
    T d[kMaxLength];
    for (usize length = 0U; length <= kMaxLength; ++length)
    {
        for (usize i = 0U; i < kMaxLength; ++i)
        {
            a[i] = random_value<T>(words);
            b[i] = random_value<T>(words);
            c[i] = random_value<T>(words);
            d[i] = random_value<T>(words);
        }
        const T t = random_amount<T>(words);
        const T span = unit_value<T>(words);
        const T dt = (unit_value<T>(words) * static_cast<T>(4)) - static_cast<T>(2);
        check_batch<T>(
            length, [&](T* out) noexcept { lerp_at(a, b, t, length, out); },
            [&](usize i) noexcept { return lerp_of(a[i], b[i], t); });
        check_batch<T>(
            length, [&](T* out) noexcept { hermite_at({a, b, c, d, t, span}, length, out); },
            [&](usize i) noexcept { return hermite_of(a[i], b[i], c[i], d[i], t, span); });
        check_batch<T>(
            length, [&](T* out) noexcept { extrapolate_at(a, b, nullptr, dt, length, out); },
            [&](usize i) noexcept { return linear_of(a[i], b[i], dt); });
        check_batch<T>(
            length, [&](T* out) noexcept { extrapolate_at(a, b, c, dt, length, out); },
            [&](usize i) noexcept { return quadratic_of(a[i], b[i], c[i], dt); });

        // In place, on each input a caller may overwrite.
        T in_place[kMaxLength];
        std::memcpy(in_place, a, sizeof(a));
        lerp_at(in_place, b, t, length, in_place);
        bool same = true;
        for (usize i = 0U; i < length; ++i)
        {
            same = same && bits_of(in_place[i]) == bits_of(lerp_of(a[i], b[i], t));
        }
        std::memcpy(in_place, c, sizeof(c));
        hermite_at({a, b, in_place, d, t, span}, length, in_place);
        for (usize i = 0U; i < length; ++i)
        {
            same = same && bits_of(in_place[i]) == bits_of(hermite_of(a[i], b[i], c[i], d[i], t, span));
        }
        std::memcpy(in_place, a, sizeof(a));
        extrapolate_at(in_place, b, c, dt, length, in_place);
        for (usize i = 0U; i < length; ++i)
        {
            same = same && bits_of(in_place[i]) == bits_of(quadratic_of(a[i], b[i], c[i], dt));
        }
        CHECK(same);
    }
}

// Unit, equal, negated and, one round in eight, not unit at all: a level must agree on those too.
void random_pair(SeededWords& words, f32* a, f32* b) noexcept
{
    random_unit(words, a);
    const u32 pick = words.next_u32() % 8U;
    if (pick == 0U)
    {
        std::memcpy(b, a, 4U * sizeof(f32));
        return;
    }
    if (pick == 1U)
    {
        for (usize i = 0U; i < 4U; ++i)
        {
            b[i] = -a[i];
        }
        return;
    }
    random_unit(words, b);
    if (pick == 2U)
    {
        for (usize i = 0U; i < 4U; ++i)
        {
            b[i] *= 3.0F;
            a[i] *= 0.25F;
        }
    }
}

void check_quaternions(const MotionKernels& table, u64 stream)
{
    SeededWords words(33U, stream);
    f32 a[4U * kMaxLength];
    f32 b[4U * kMaxLength];
    f32 out[(4U * kMaxLength) + 2U];
    f32 want[4U];
    for (usize length = 0U; length <= kMaxLength; ++length)
    {
        for (usize i = 0U; i < kMaxLength; ++i)
        {
            random_pair(words, a + (4U * i), b + (4U * i));
        }
        const f32 t = random_amount<f32>(words);
        for (const bool spherical : {false, true})
        {
            for (f32& value : out)
            {
                value = kCanary32;
            }
            (spherical ? table.slerp : table.nlerp)(a, b, t, length, out + 1U);
            CHECK(bits_of(out[0]) == bits_of(kCanary32));
            CHECK(bits_of(out[(4U * length) + 1U]) == bits_of(kCanary32));
            bool same = true;
            for (usize i = 0U; i < length; ++i)
            {
                if (spherical)
                {
                    slerp_of(a + (4U * i), b + (4U * i), t, want);
                }
                else
                {
                    nlerp_of(a + (4U * i), b + (4U * i), t, want);
                }
                same = same && std::memcmp(want, out + 1U + (4U * i), sizeof(want)) == 0;
            }
            CHECK(same);

            // In place.
            f32 in_place[4U * kMaxLength];
            std::memcpy(in_place, a, sizeof(a));
            (spherical ? table.slerp : table.nlerp)(in_place, b, t, length, in_place);
            CHECK(std::memcmp(in_place, out + 1U, 4U * length * sizeof(f32)) == 0);
        }
    }
}

template <class T>
[[nodiscard]] bool same_or_both_nan(T a, T b) noexcept
{
    return bits_of(a) == bits_of(b) || (std::isnan(a) && std::isnan(b));
}

// Infinities, NaN, subnormals and the largest values: these routines refuse nothing, so each level must round them alike.
template <class T>
[[nodiscard]] T hostile_value(SeededWords& words) noexcept
{
    constexpr T infinity = std::numeric_limits<T>::infinity();
    switch (words.next_u32() % 8U)
    {
        case 0U:
            return infinity;
        case 1U:
            return -infinity;
        case 2U:
            return std::numeric_limits<T>::quiet_NaN();
        case 3U:
            return std::numeric_limits<T>::denorm_min();
        case 4U:
            return std::numeric_limits<T>::max();
        case 5U:
            return std::numeric_limits<T>::lowest();
        default:
            return random_value<T>(words);
    }
}

std::string_view routine_of(const char* routine)
{
    return routine != nullptr ? routine : "";
}

void lerp32_off(const f32*, const f32*, f32, usize count, f32* out) noexcept
{
    std::memset(out, 0, count * sizeof(f32));
}

void lerp64_off(const f64*, const f64*, f64, usize count, f64* out) noexcept
{
    std::memset(out, 0, count * sizeof(f64));
}

void hermite32_off(const hn::HermiteSpan<f32>&, usize count, f32* out) noexcept
{
    std::memset(out, 0, count * sizeof(f32));
}

void hermite64_off(const hn::HermiteSpan<f64>&, usize count, f64* out) noexcept
{
    std::memset(out, 0, count * sizeof(f64));
}

void extrapolate32_off(const f32*, const f32*, const f32*, f32, usize count, f32* out) noexcept
{
    std::memset(out, 0, count * sizeof(f32));
}

void extrapolate64_off(const f64*, const f64*, const f64*, f64, usize count, f64* out) noexcept
{
    std::memset(out, 0, count * sizeof(f64));
}

// Right without an acceleration, wrong with one: the second half of the routine's answers.
void quadratic32_off(const f32* position, const f32* velocity, const f32* acceleration, f32 dt, usize count,
                     f32* out) noexcept
{
    hn::kMotionKernelsPortable.extrapolate_f32(position, velocity, nullptr, acceleration != nullptr ? dt * 2.0F : dt,
                                               count, out);
}

void quaternion_off(const f32*, const f32*, f32, usize count, f32* out) noexcept
{
    std::memset(out, 0, 4U * count * sizeof(f32));
}

// Writes nothing, so the zeros the check starts from are the answer.
void reckon_off(hn::DeadReckoningAlgorithm, const hn::DeadReckoningInput&, usize, const hn::DeadReckoningOutput&) noexcept
{
}

// Right on the first algorithm the check runs, wrong on the second.
void reckon_second_off(hn::DeadReckoningAlgorithm algorithm, const hn::DeadReckoningInput& in, usize count,
                       const hn::DeadReckoningOutput& out) noexcept
{
    if (algorithm == hn::DeadReckoningAlgorithm::Rvb)
    {
        hn::kMotionKernelsPortable.dead_reckon(algorithm, in, count, out);
    }
}

// The right position with a velocity of nothing.
void blend32_off(const hn::ProjectiveBlendSpan<f32>& span, usize count, f32* position, f32* velocity) noexcept
{
    hn::kMotionKernelsPortable.projective_blend_f32(span, count, position, velocity);
    std::memset(velocity, 0, count * sizeof(f32));
}

void blend64_off(const hn::ProjectiveBlendSpan<f64>& span, usize count, f64* position, f64* velocity) noexcept
{
    hn::kMotionKernelsPortable.projective_blend_f64(span, count, position, velocity);
    std::memset(velocity, 0, count * sizeof(f64));
}

} // namespace

TEST_CASE("motion kernels: the archive holds portable and each body level the build compiles", "[motion][kernels]")
{
    const auto levels = hn::motion_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    for (usize i = 0U; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
    }
}

TEST_CASE("motion kernels: the process runs the level its selection allows", "[motion][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const KernelLevel<MotionKernels>& chosen = hn::motion_kernels();
    CHECK(&chosen == &crd::select_kernel_level<MotionKernels>(hn::motion_kernel_levels(), s.selected, s.kernel_features,
                                                              crd::kernel_level_cap()));
    const crd::KernelFamily* family = crd::test::kernel_family("hesap.motion");
    REQUIRE(family != nullptr);
    CHECK(family->result.status == crd::KernelCheckStatus::Passed);
    CHECK(family->result.level == chosen.level);
}

TEST_CASE("motion self-check: every table the host runs passes, and a wrong one fails", "[motion][kernels]")
{
    const crd::test::SelfCheckRun run = crd::test::self_check_levels(hn::motion_kernel_levels(), &hn::motion_self_check);
    INFO("first failure " << crd::cpu_level_name(run.level) << " " << routine_of(run.routine));
    CHECK(run.failed == 0U);
    CHECK(run.ran >= 1U);
    MotionKernels wrong = hn::kMotionKernelsPortable;
    wrong.lerp_f32 = &lerp32_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "lerp_f32");
    wrong = hn::kMotionKernelsPortable;
    wrong.lerp_f64 = &lerp64_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "lerp_f64");
    wrong = hn::kMotionKernelsPortable;
    wrong.hermite_f32 = &hermite32_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "hermite_f32");
    wrong = hn::kMotionKernelsPortable;
    wrong.hermite_f64 = &hermite64_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "hermite_f64");
    wrong = hn::kMotionKernelsPortable;
    wrong.extrapolate_f32 = &extrapolate32_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "extrapolate_f32");
    wrong = hn::kMotionKernelsPortable;
    wrong.extrapolate_f32 = &quadratic32_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "extrapolate_f32");
    wrong = hn::kMotionKernelsPortable;
    wrong.extrapolate_f64 = &extrapolate64_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "extrapolate_f64");
    wrong = hn::kMotionKernelsPortable;
    wrong.nlerp = &quaternion_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "nlerp");
    wrong = hn::kMotionKernelsPortable;
    wrong.slerp = &quaternion_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "slerp");
    wrong = hn::kMotionKernelsPortable;
    wrong.dead_reckon = &reckon_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "dead_reckon");
    wrong = hn::kMotionKernelsPortable;
    wrong.dead_reckon = &reckon_second_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "dead_reckon");
    wrong = hn::kMotionKernelsPortable;
    wrong.projective_blend_f32 = &blend32_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "projective_blend_f32");
    wrong = hn::kMotionKernelsPortable;
    wrong.projective_blend_f64 = &blend64_off;
    CHECK(routine_of(hn::motion_self_check(wrong)) == "projective_blend_f64");
}

TEST_CASE("motion kernels: every level gives the restated bits on every length", "[motion][kernels]")
{
    for (const KernelLevel<MotionKernels>& level : hn::motion_kernel_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        INFO(crd::cpu_level_name(level.level));
        check_components<f32>(*level.table, 1U);
        check_components<f64>(*level.table, 2U);
        check_quaternions(*level.table, 3U);
    }
}

TEST_CASE("motion kernels: every level agrees with portable on values no snapshot holds", "[motion][kernels]")
{
    SeededWords words(34U, 1U);
    f32 a[4U * kMaxLength];
    f32 b[4U * kMaxLength];
    f32 c[kMaxLength];
    f32 d[kMaxLength];
    f64 a64[kMaxLength];
    f64 b64[kMaxLength];
    f64 c64[kMaxLength];
    f64 d64[kMaxLength];
    for (u32 round = 0U; round < 200U; ++round)
    {
        for (usize i = 0U; i < 4U * kMaxLength; ++i)
        {
            a[i] = hostile_value<f32>(words);
            b[i] = hostile_value<f32>(words);
        }
        for (usize i = 0U; i < kMaxLength; ++i)
        {
            c[i] = hostile_value<f32>(words);
            d[i] = hostile_value<f32>(words);
            a64[i] = hostile_value<f64>(words);
            b64[i] = hostile_value<f64>(words);
            c64[i] = hostile_value<f64>(words);
            d64[i] = hostile_value<f64>(words);
        }
        const f32 t = hostile_value<f32>(words);
        const f64 t64 = hostile_value<f64>(words);
        const MotionKernels& portable = hn::kMotionKernelsPortable;
        const auto compare = [&](const MotionKernels& table)
        {
            f32 want32[4U * kMaxLength];
            f32 got32[4U * kMaxLength];
            f64 want64[kMaxLength];
            f64 got64[kMaxLength];
            bool same = true;
            const auto same32 = [&](usize count)
            {
                for (usize i = 0U; i < count; ++i)
                {
                    same = same && same_or_both_nan(want32[i], got32[i]);
                }
            };
            const auto same64 = [&]()
            {
                for (usize i = 0U; i < kMaxLength; ++i)
                {
                    same = same && same_or_both_nan(want64[i], got64[i]);
                }
            };
            portable.lerp_f32(a, b, t, kMaxLength, want32);
            table.lerp_f32(a, b, t, kMaxLength, got32);
            same32(kMaxLength);
            portable.hermite_f32({a, b, c, d, t, t}, kMaxLength, want32);
            table.hermite_f32({a, b, c, d, t, t}, kMaxLength, got32);
            same32(kMaxLength);
            portable.extrapolate_f32(a, b, c, t, kMaxLength, want32);
            table.extrapolate_f32(a, b, c, t, kMaxLength, got32);
            same32(kMaxLength);
            portable.nlerp(a, b, t, kMaxLength, want32);
            table.nlerp(a, b, t, kMaxLength, got32);
            same32(4U * kMaxLength);
            portable.slerp(a, b, t, kMaxLength, want32);
            table.slerp(a, b, t, kMaxLength, got32);
            same32(4U * kMaxLength);
            portable.lerp_f64(a64, b64, t64, kMaxLength, want64);
            table.lerp_f64(a64, b64, t64, kMaxLength, got64);
            same64();
            portable.hermite_f64({a64, b64, c64, d64, t64, t64}, kMaxLength, want64);
            table.hermite_f64({a64, b64, c64, d64, t64, t64}, kMaxLength, got64);
            same64();
            portable.extrapolate_f64(a64, b64, c64, t64, kMaxLength, want64);
            table.extrapolate_f64(a64, b64, c64, t64, kMaxLength, got64);
            same64();
            return same;
        };
        for (const KernelLevel<MotionKernels>& level : hn::motion_kernel_levels())
        {
            if (crd::test::kernel_level_runs_here(level))
            {
                INFO(crd::cpu_level_name(level.level));
                CHECK(compare(*level.table));
            }
        }
    }
}

TEST_CASE("motion batches: the public calls run the process's level", "[motion][kernels]")
{
    const f32 a[3] = {0.0F, 1.0F, -2.0F};
    const f32 b[3] = {4.0F, 1.0F, 2.0F};
    f32 out[3];
    hn::lerp(a, b, 0.25F, 3U, out);
    CHECK(out[0] == 1.0F);
    CHECK(out[1] == 1.0F);
    CHECK(out[2] == -1.0F);
    hn::extrapolate(a, b, nullptr, 0.5F, 3U, out);
    CHECK(out[0] == 2.0F);
    CHECK(out[1] == 1.5F);
    CHECK(out[2] == -1.0F);
    hn::extrapolate(a, b, b, 0.5F, 3U, out);
    CHECK(out[0] == 2.5F);
    const f32 same[3] = {1.0F, 1.0F, 1.0F};
    hn::hermite(hn::HermiteSpan<f32>{same, a, same, a, 0.0F, 1.0F}, 3U, out);
    CHECK(out[0] == 1.0F);
    const f64 a64[2] = {0.0, 2.0};
    const f64 b64[2] = {1.0, 6.0};
    f64 out64[2];
    hn::lerp(a64, b64, 0.5, 2U, out64);
    CHECK(out64[0] == 0.5);
    CHECK(out64[1] == 4.0);
    hn::extrapolate(a64, b64, nullptr, 2.0, 2U, out64);
    CHECK(out64[1] == 14.0);
    hn::hermite(hn::HermiteSpan<f64>{a64, b64, a64, b64, 1.0, 1.0}, 2U, out64);
    CHECK(out64[1] == 2.0);
    const f32 identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    f32 q[4];
    hn::nlerp(identity, identity, 0.5F, 1U, q);
    CHECK(q[3] == 1.0F);
    hn::slerp(identity, identity, 0.5F, 1U, q);
    CHECK(q[3] == Approx(1.0F).margin(1e-6));
}

TEST_CASE("nlerp: unit length, the shorter way, and exact where its inputs say so", "[motion][nlerp]")
{
    SeededWords words(31U, 1U);
    for (u32 i = 0U; i < 20000U; ++i)
    {
        f32 a[4];
        f32 b[4];
        random_unit(words, a);
        random_unit(words, b);
        const f32 t = unit_value<f32>(words);
        f32 q[4];
        hn::nlerp(a, b, t, 1U, q);
        CHECK(dot_of(q, q) == Approx(1.0F).margin(4e-7));
        // On the same side as a, at most as far from it as b or -b.
        CHECK(dot_of(q, a) >= std::fabs(dot_of(a, b)) - 1e-5F);
    }
    const f32 axis_x[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    const f32 minus_x[4] = {-1.0F, 0.0F, 0.0F, 0.0F};
    f32 q[4];
    hn::nlerp(axis_x, minus_x, 0.5F, 1U, q); // The same rotation: the arc has no length.
    CHECK(q[0] == 1.0F);
    // At exactly a right angle the far end keeps its sign, as the reference's dot >= 0 has it.
    const f32 identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    hn::nlerp(identity, axis_x, 0.5F, 1U, q);
    CHECK(q[0] > 0.0F);
    CHECK(q[3] > 0.0F);
    const f32 zero[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    const f32 huge[4] = {3e38F, 3e38F, 3e38F, 3e38F};
    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();
    const f32 nan[4] = {not_a_number, 0.0F, 0.0F, 1.0F};
    // Nothing to normalise, too much or no number at all answers the identity, never a NaN.
    for (const f32* input : {zero, huge, nan})
    {
        hn::nlerp(input, input, 0.5F, 1U, q);
        CHECK(q[0] == 0.0F);
        CHECK(q[1] == 0.0F);
        CHECK(q[2] == 0.0F);
        CHECK(q[3] == 1.0F);
    }
}

TEST_CASE("slerp: within two parts in ten million of the true arc, at even speed", "[motion][slerp]")
{
    // The whole hemisphere of angles between the inputs, ending at the two hard ones: none and a right angle of cos 0.
    long double worst = 0.0L;
    long double worst_length = 0.0L;
    SeededWords words(31U, 2U);
    for (u32 round = 0U; round < 400U; ++round)
    {
        f32 a[4];
        f32 b[4];
        random_unit(words, a);
        random_unit(words, b);
        for (u32 step = 0U; step <= 64U; ++step)
        {
            const f32 t = static_cast<f32>(step) / 64.0F;
            f32 q[4];
            hn::slerp(a, b, t, 1U, q);
            long double want[4];
            true_slerp(a, b, t, want);
            long double length = 0.0L;
            for (usize i = 0U; i < 4U; ++i)
            {
                const long double error = std::fabs(static_cast<long double>(q[i]) - want[i]);
                worst = (error > worst) ? error : worst;
                length += static_cast<long double>(q[i]) * static_cast<long double>(q[i]);
            }
            const long double off = std::fabs(length - 1.0L);
            worst_length = (off > worst_length) ? off : worst_length;
        }
    }
    INFO("worst component error " << static_cast<f64>(worst) << ", worst length error " << static_cast<f64>(worst_length));
    CHECK(worst < 2e-7L);
    CHECK(worst_length < 4e-7L);

    // A right angle in one plane: every quarter of the way is a known point.
    const f32 identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    const f32 half_turn_x[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    f32 q[4];
    const f64 quarter = std::numbers::pi / 2.0;
    for (u32 step = 0U; step <= 16U; ++step)
    {
        const f64 t = static_cast<f64>(step) / 16.0;
        hn::slerp(identity, half_turn_x, static_cast<f32>(t), 1U, q);
        CHECK(static_cast<f64>(q[0]) == Approx(std::sin(t * quarter)).margin(2e-7));
        CHECK(static_cast<f64>(q[3]) == Approx(std::cos(t * quarter)).margin(2e-7));
    }
}

TEST_CASE("slerp: its edges", "[motion][slerp]")
{
    const f32 a[4] = {0.0F, 0.6F, 0.0F, 0.8F};
    const f32 minus_a[4] = {0.0F, -0.6F, 0.0F, -0.8F};
    f32 q[4];
    // The same quaternion and its negative are the same rotation: a stays a.
    for (const f32* other : {a, minus_a})
    {
        for (const f32 t : {0.0F, 0.25F, 0.5F, 1.0F})
        {
            hn::slerp(a, other, t, 1U, q);
            CHECK(static_cast<f64>(q[1]) == Approx(0.6).margin(2e-7));
            CHECK(static_cast<f64>(q[3]) == Approx(0.8).margin(2e-7));
        }
    }
    // t is cut to [0, 1], NaN to 0: both ends exact.
    const f32 b[4] = {0.8F, 0.0F, 0.6F, 0.0F};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    for (const f32 t : {0.0F, -1.0F, -infinity, nan})
    {
        hn::slerp(a, b, t, 1U, q);
        for (usize i = 0U; i < 4U; ++i)
        {
            CHECK(q[i] == a[i]);
        }
    }
    for (const f32 t : {1.0F, 2.0F, infinity})
    {
        hn::slerp(a, b, t, 1U, q);
        for (usize i = 0U; i < 4U; ++i)
        {
            CHECK(q[i] == b[i]);
        }
    }
    // The shorter way round: from a to a negated b goes as a to b, with the sign of the far end.
    const f32 c[4] = {0.0F, 0.8F, 0.0F, 0.6F};
    const f32 minus_c[4] = {0.0F, -0.8F, 0.0F, -0.6F};
    f32 q_plus[4];
    f32 q_minus[4];
    hn::slerp(a, c, 0.5F, 1U, q_plus);
    hn::slerp(a, minus_c, 0.5F, 1U, q_minus);
    for (usize i = 0U; i < 4U; ++i)
    {
        CHECK(q_plus[i] == q_minus[i]);
    }
}

TEST_CASE("hermite: exact at both ends and reproducing a cubic path", "[motion][hermite]")
{
    SeededWords words(31U, 3U);
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const f64 p0 = random_value<f64>(words);
        const f64 p1 = random_value<f64>(words);
        const f64 v0 = random_value<f64>(words);
        const f64 v1 = random_value<f64>(words);
        const f64 span = 1.0 / 60.0;
        f64 out;
        hn::hermite(hn::HermiteSpan<f64>{&p0, &v0, &p1, &v1, 0.0, span}, 1U, &out);
        CHECK(out == p0 + 0.0);
        hn::hermite(hn::HermiteSpan<f64>{&p0, &v0, &p1, &v1, 1.0, span}, 1U, &out);
        CHECK(out == p1 + 0.0);
    }
    // Position c0 + c1 s + c2 s^2 + c3 s^3 over s in [s0, s0 + span]: the sample is on the path, wherever it is.
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const f64 c0 = random_value<f64>(words);
        const f64 c1 = random_value<f64>(words);
        const f64 c2 = random_value<f64>(words);
        const f64 c3 = random_value<f64>(words);
        const f64 span = (unit_value<f64>(words) * 0.5) + 0.01;
        const auto position = [=](f64 s) noexcept { return c0 + (s * (c1 + (s * (c2 + (s * c3))))); };
        const auto velocity = [=](f64 s) noexcept { return c1 + (s * ((2.0 * c2) + (3.0 * s * c3))); };
        const f64 p0 = position(0.0);
        const f64 p1 = position(span);
        const f64 v0 = velocity(0.0);
        const f64 v1 = velocity(span);
        const f64 t = unit_value<f64>(words);
        f64 out;
        hn::hermite(hn::HermiteSpan<f64>{&p0, &v0, &p1, &v1, t, span}, 1U, &out);
        const f64 scale = std::fabs(c0) + std::fabs(c1) + std::fabs(c2) + std::fabs(c3) + 1.0;
        CHECK(std::fabs(out - position(t * span)) <= 8e-15 * scale);
    }
}

TEST_CASE("extrapolate: the constant-acceleration path, forwards and backwards", "[motion][extrapolate]")
{
    SeededWords words(31U, 4U);
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const f64 p = random_value<f64>(words);
        const f64 v = random_value<f64>(words);
        const f64 a = random_value<f64>(words);
        const f64 dt = (unit_value<f64>(words) * 4.0) - 2.0;
        f64 out;
        hn::extrapolate(&p, &v, &a, 0.0, 1U, &out);
        CHECK(out == p + 0.0);
        hn::extrapolate(&p, &v, nullptr, 0.0, 1U, &out);
        CHECK(out == p + 0.0);
        hn::extrapolate(&p, &v, &a, dt, 1U, &out);
        const long double want = static_cast<long double>(p) + (static_cast<long double>(v) * static_cast<long double>(dt)) +
                                 (static_cast<long double>(a) * 0.5L * static_cast<long double>(dt) *
                                  static_cast<long double>(dt));
        const f64 scale = std::fabs(p) + std::fabs(v * dt) + std::fabs(a * dt * dt) + 1e-300;
        CHECK(std::fabs(static_cast<long double>(out) - want) <= 4e-16L * static_cast<long double>(scale));
    }
    // Two steps from two velocities join: the extrapolation of the extrapolated state, with its velocity carried.
    const f32 p = 1.0F;
    const f32 v = 2.0F;
    const f32 a = 4.0F;
    f32 whole;
    hn::extrapolate(&p, &v, &a, 1.0F, 1U, &whole);
    f32 half_way;
    hn::extrapolate(&p, &v, &a, 0.5F, 1U, &half_way);
    const f32 v_half = v + (a * 0.5F);
    f32 joined;
    hn::extrapolate(&half_way, &v_half, &a, 0.5F, 1U, &joined);
    CHECK(whole == joined);
}

TEST_CASE("rewind history: an empty one, one stamp, refusals and slots", "[motion][rewind]")
{
    const hn::RewindLimits limits{1000, 1000};
    hn::RewindHistory<4U> history;
    CHECK(history.size() == 0U);
    CHECK(history.sample(5, limits).place == RewindPlace::Empty);
    u32 slot = 99U;
    REQUIRE(history.push(10, slot));
    CHECK(slot == 0U);
    u32 refused = 99U;
    CHECK_FALSE(history.push(10, refused)); // Not after the newest, changing nothing.
    CHECK_FALSE(history.push(9, refused));
    CHECK(refused == 99U);
    CHECK(history.size() == 1U);

    hn::RewindSample sample = history.sample(10, limits);
    CHECK(sample.place == RewindPlace::Exact);
    CHECK(sample.moved == RewindMoved::No);
    CHECK(sample.older == 0U);
    CHECK(sample.newer == 0U);
    CHECK(sample.time == 10);
    sample = history.sample(11, limits);
    CHECK(sample.moved == RewindMoved::Future);
    CHECK(sample.time == 10);
    CHECK(sample.place == RewindPlace::Exact);
    sample = history.sample(9, limits);
    CHECK(sample.moved == RewindMoved::TooOld);
    CHECK(sample.time == 10);

    // Slots run 0, 1, 2, 3 and then reuse the oldest.
    REQUIRE(history.push(20, slot));
    CHECK(slot == 1U);
    REQUIRE(history.push(30, slot));
    CHECK(slot == 2U);
    REQUIRE(history.push(40, slot));
    CHECK(slot == 3U);
    REQUIRE(history.push(50, slot));
    CHECK(slot == 0U);
    REQUIRE(history.push(60, slot));
    CHECK(slot == 1U);
    CHECK(history.size() == 4U);
    sample = history.sample(35, limits); // Stamps 30, 40, 50, 60 in slots 2, 3, 0, 1.
    CHECK(sample.place == RewindPlace::Between);
    CHECK(sample.older == 2U);
    CHECK(sample.newer == 3U);
    CHECK(sample.t == 0.5);
    sample = history.sample(55, limits); // Across the wrap of the slots.
    CHECK(sample.older == 0U);
    CHECK(sample.newer == 1U);
    CHECK(sample.t == 0.5);
    sample = history.sample(29, limits); // Evicted: moved up to the oldest held.
    CHECK(sample.moved == RewindMoved::TooOld);
    CHECK(sample.time == 30);
    CHECK(sample.place == RewindPlace::Exact);
    CHECK(sample.older == 2U);
}

TEST_CASE("rewind history: the declared window and the widest span", "[motion][rewind]")
{
    hn::RewindHistory<8U> history;
    u32 slot = 0U;
    for (const i64 stamp : {0, 100, 200, 1000, 1100})
    {
        REQUIRE(history.push(stamp, slot));
    }
    // A window ending inside a span interpolates at its bound, and says it moved.
    hn::RewindSample sample = history.sample(10, hn::RewindLimits{1050, 10000});
    CHECK(sample.moved == RewindMoved::TooOld);
    CHECK(sample.time == 50);
    CHECK(sample.place == RewindPlace::Between);
    CHECK(sample.t == 0.5);
    // At the window's edge nothing moves.
    sample = history.sample(50, hn::RewindLimits{1050, 10000});
    CHECK(sample.moved == RewindMoved::No);
    // No rewind stays on the newest, however old the time; a negative one too.
    for (const i64 none : {i64{0}, i64{-5}, std::numeric_limits<i64>::min()})
    {
        sample = history.sample(10, hn::RewindLimits{none, 10000});
        CHECK(sample.moved == RewindMoved::TooOld);
        CHECK(sample.time == 1100);
        CHECK(sample.place == RewindPlace::Exact);
    }
    // A span wider than max_gap has no motion: the older snapshot, with t 0, on either side of the limit.
    sample = history.sample(600, hn::RewindLimits{10000, 799});
    CHECK(sample.place == RewindPlace::Gap);
    CHECK(sample.t == 0.0);
    CHECK(sample.older == 2U);
    CHECK(sample.newer == 3U);
    sample = history.sample(600, hn::RewindLimits{10000, 800});
    CHECK(sample.place == RewindPlace::Between);
    CHECK(sample.t == 0.5);
    // Exact stamps are not spans, whatever the limit.
    sample = history.sample(1000, hn::RewindLimits{10000, -1});
    CHECK(sample.place == RewindPlace::Exact);
    sample = history.sample(150, hn::RewindLimits{10000, -1});
    CHECK(sample.place == RewindPlace::Gap);
}

TEST_CASE("rewind history: stamps at the ends of the range", "[motion][rewind]")
{
    constexpr i64 lowest = std::numeric_limits<i64>::min();
    constexpr i64 highest = std::numeric_limits<i64>::max();
    constexpr hn::RewindLimits every{highest, highest};
    hn::RewindHistory<4U> history;
    u32 slot = 0U;
    REQUIRE(history.push(lowest, slot));
    REQUIRE(history.push(highest, slot));
    CHECK_FALSE(history.push(highest, slot));
    // The whole range is wider than the widest limit: the distance does not wrap.
    hn::RewindSample sample = history.sample(0, every);
    CHECK(sample.moved == RewindMoved::No);
    CHECK(sample.place == RewindPlace::Gap);
    sample = history.sample(-1, every); // highest back by highest is 0, the oldest the window reaches.
    CHECK(sample.moved == RewindMoved::TooOld);
    CHECK(sample.time == 0);
    sample = history.sample(lowest, hn::RewindLimits{lowest, lowest});
    CHECK(sample.time == highest);
    CHECK(sample.place == RewindPlace::Exact);

    // The widest span the limits can allow, a half of it to the sample.
    hn::RewindHistory<4U> wide;
    REQUIRE(wide.push(-(i64{1} << 62), slot));
    REQUIRE(wide.push((i64{1} << 62) - 1, slot));
    sample = wide.sample(-1, every);
    CHECK(sample.place == RewindPlace::Between);
    CHECK(sample.t == 0.5);
    CHECK_FALSE(wide.push(-1, slot));
}

TEST_CASE("rewind history: every sample as a linear search over the stamps finds it", "[motion][rewind]")
{
    SeededWords words(31U, 5U);
    for (u32 round = 0U; round < 300U; ++round)
    {
        hn::RewindHistory<7U> history;
        crd::containers::Array<i64> stamps;
        crd::containers::Array<u32> slots;
        i64 stamp = static_cast<i64>(words.next_u32() % 1000U) - 500;
        const u32 pushes = 1U + (words.next_u32() % 25U);
        for (u32 i = 0U; i < pushes; ++i)
        {
            u32 slot = 0U;
            REQUIRE(history.push(stamp, slot));
            CHECK(slot < 7U);
            stamps.push_back(stamp);
            slots.push_back(slot);
            stamp += 1 + static_cast<i64>(words.next_u32() % 200U);
        }
        const usize held = (stamps.size() < 7U) ? stamps.size() : 7U;
        const usize first = stamps.size() - held;
        CHECK(history.size() == held);
        const i64 newest = stamps[stamps.size() - 1U];
        for (u32 query = 0U; query < 60U; ++query)
        {
            const i64 at = stamps[first] - 50 + static_cast<i64>(words.next_u32() % 1500U);
            const hn::RewindLimits limits{static_cast<i64>(words.next_u32() % 1500U),
                                          static_cast<i64>(words.next_u32() % 300U)};
            const hn::RewindSample sample = history.sample(at, limits);
            i64 floor = stamps[first];
            floor = (newest - limits.max_rewind > floor) ? newest - limits.max_rewind : floor;
            floor = (limits.max_rewind == 0) ? newest : floor;
            i64 want_time = at;
            RewindMoved want_moved = RewindMoved::No;
            if (at > newest)
            {
                want_time = newest;
                want_moved = RewindMoved::Future;
            }
            else if (at < floor)
            {
                want_time = floor;
                want_moved = RewindMoved::TooOld;
            }
            CHECK(sample.time == want_time);
            CHECK(sample.moved == want_moved);
            usize below = first;
            for (usize k = first; k < stamps.size(); ++k)
            {
                if (stamps[k] <= want_time)
                {
                    below = k;
                }
            }
            CHECK(sample.older == slots[below]);
            if (stamps[below] == want_time)
            {
                CHECK(sample.place == RewindPlace::Exact);
                CHECK(sample.newer == slots[below]);
                CHECK(sample.t == 0.0);
                continue;
            }
            CHECK(sample.newer == slots[below + 1U]);
            const i64 width = stamps[below + 1U] - stamps[below];
            if (width > limits.max_gap)
            {
                CHECK(sample.place == RewindPlace::Gap);
                CHECK(sample.t == 0.0);
            }
            else
            {
                CHECK(sample.place == RewindPlace::Between);
                CHECK(sample.t == static_cast<f64>(want_time - stamps[below]) / static_cast<f64>(width));
            }
        }
    }
}

TEST_CASE("lag compensation: a rewound sample of steady motion is where the body was", "[motion][rewind]")
{
    // One moving body per axis, stamped at irregular ticks; rewinding reads it back at the client's time.
    constexpr usize components = 9U;
    constexpr u32 slots = 16U;
    hn::RewindHistory<slots> history;
    f64 snapshots[slots][components];
    const f64 velocity[components] = {3.0, -2.0, 0.5, 100.0, 0.0, -0.25, 7.0, 1e-3, 40.0};
    const auto position = [&velocity](usize component, i64 time) noexcept
    {
        return 1000.0 + (velocity[component] * (static_cast<f64>(time) * 1e-9));
    };
    SeededWords words(31U, 6U);
    i64 time = 5'000'000'000;
    for (u32 tick = 0U; tick < 200U; ++tick)
    {
        time += 8'000'000 + static_cast<i64>(words.next_u32() % 16'000'000U);
        u32 slot = 0U;
        REQUIRE(history.push(time, slot));
        for (usize c = 0U; c < components; ++c)
        {
            snapshots[slot][c] = position(c, time);
        }
        const hn::RewindLimits limits{200'000'000, 100'000'000};
        const i64 client = time - static_cast<i64>(words.next_u32() % 150'000'000U);
        const hn::RewindSample sample = history.sample(client, limits);
        CHECK(sample.moved != RewindMoved::Future);
        CHECK(sample.place != RewindPlace::Gap);
        f64 rewound[components];
        hn::lerp(snapshots[sample.older], snapshots[sample.newer], sample.t, components, rewound);
        for (usize c = 0U; c < components; ++c)
        {
            CHECK(rewound[c] == Approx(position(c, sample.time)).epsilon(1e-12));
        }
    }
}
