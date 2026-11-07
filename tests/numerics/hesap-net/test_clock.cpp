// HESAP-NET's clock filters against restated formulas and simulated hostile paths: RFC 5905 §8's exchange, §10's filter
// by hand, the drift filter's line against the normal equations in long double, the tracker through loss, spikes,
// steps, a smear, a frequency jump and a route change, and every refusal. The bit-exact comparisons with the references
// are crd-hesap-clock-ntpd-tests and crd-hesap-estimators-webrtc-tests.

#include "clock_paths.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/clock.hpp>

#include <algorithm>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace hn = crd::hesap::net;
using Catch::Approx;
using crd::i64;
using crd::u32;
using crd::u64;

namespace
{

constexpr i64 kSecond = 1'000'000'000;
constexpr i64 kPoll = 64 * kSecond;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr i64 kI64Max = std::numeric_limits<i64>::max();
constexpr i64 kI64Min = std::numeric_limits<i64>::min();

[[nodiscard]] hn::ClockFilter started_filter(i64 now_ns, const hn::ClockFilterOptions& options = {})
{
    hn::ClockFilter filter;
    REQUIRE(filter.init(options, now_ns));
    return filter;
}

[[nodiscard]] bool same_filter(const hn::ClockFilter& a, const hn::ClockFilter& b)
{
    bool same = std::bit_cast<u64>(a.offset_s()) == std::bit_cast<u64>(b.offset_s()) &&
                std::bit_cast<u64>(a.delay_s()) == std::bit_cast<u64>(b.delay_s()) &&
                std::bit_cast<u64>(a.dispersion_s()) == std::bit_cast<u64>(b.dispersion_s()) &&
                std::bit_cast<u64>(a.jitter_s()) == std::bit_cast<u64>(b.jitter_s()) && a.epoch_ns() == b.epoch_ns();
    for (u32 i = 0U; i < hn::ClockFilter::kStages; ++i)
    {
        same = same && std::bit_cast<u64>(a.stage(i).offset_s) == std::bit_cast<u64>(b.stage(i).offset_s) &&
               std::bit_cast<u64>(a.stage(i).dispersion_s) == std::bit_cast<u64>(b.stage(i).dispersion_s) &&
               a.stage(i).epoch_ns == b.stage(i).epoch_ns;
    }
    return same;
}

} // namespace

static_assert(std::is_trivially_copyable_v<hn::ClockTracker>, "a tracker holds no heap");
static_assert(std::is_trivially_copyable_v<hn::MediaClockMap>, "a map holds no heap");
static_assert(sizeof(hn::ClockTracker) < 3000U, "one peer's whole state");

// Compile-time evaluation refuses any overflow in the arithmetic.
consteval bool extreme_exchanges_are_defined()
{
    const i64 values[] = {kI64Min, kI64Min + 1, -1, 0, 1, kI64Max - 1, kI64Max};
    for (const i64 t1 : values)
    {
        for (const i64 t2 : values)
        {
            for (const i64 t3 : values)
            {
                for (const i64 t4 : values)
                {
                    hn::OnWireSample out;
                    (void)hn::on_wire_sample(t1, t2, t3, t4, 0, out);
                    (void)hn::on_wire_sample(t1, t2, t3, t4, kI64Max, out);
                }
            }
        }
    }
    return true;
}
static_assert(extreme_exchanges_are_defined());

TEST_CASE("clock: an exchange's offset and delay are RFC 5905 section 8's", "[clock]")
{
    hn::OnWireSample out;
    // The peer's clock is 500 ms ahead; 30 ms out, 10 ms held, 20 ms back.
    REQUIRE(hn::on_wire_sample(1'000, 1'000 + 30 + 500, 1'000 + 40 + 500, 1'060, 1, out));
    CHECK(out.offset_ns == 500 + 5); // The asymmetry shows as half its difference.
    CHECK(out.delay_ns == 50);

    // The sum of the legs rounds down whichever side the odd nanoseconds fall.
    REQUIRE(hn::on_wire_sample(0, 3, 3, 0, 0, out));
    CHECK(out.offset_ns == 3);
    REQUIRE(hn::on_wire_sample(0, 3, 4, 0, 1, out));
    CHECK(out.offset_ns == 3); // (3 + 4) / 2
    REQUIRE(hn::on_wire_sample(0, -3, -3, 1, 0, out));
    CHECK(out.offset_ns == -4); // (-3 + -4) / 2 rounds down
    CHECK(out.delay_ns == 1);

    // The delay never reads below the precision.
    REQUIRE(hn::on_wire_sample(0, 10, 10, 2, 100, out));
    CHECK(out.delay_ns == 100);
    // Clocks at opposite ends of the range: the difference is modulo 2^64, the delay exact.
    REQUIRE(hn::on_wire_sample(kI64Max - 10, kI64Min + 5, kI64Min + 6, kI64Max - 2, 1, out));
    CHECK(out.delay_ns == 7);
}

TEST_CASE("clock: an impossible exchange is refused and writes nothing", "[clock]")
{
    hn::OnWireSample out{7, 9};
    CHECK_FALSE(hn::on_wire_sample(10, 0, 0, 9, 1'000, out));   // Our clock ran backwards.
    CHECK_FALSE(hn::on_wire_sample(0, 10, 9, 100, 1'000, out)); // The peer's did.
    CHECK_FALSE(hn::on_wire_sample(0, 0, 200, 100, 99, out));   // It held the packet longer than the exchange took.
    CHECK_FALSE(hn::on_wire_sample(0, 0, 0, 1, -1, out));       // A negative precision.
    CHECK_FALSE(hn::on_wire_sample(0, kI64Min, kI64Max, 1, 0, out)); // A hold past the range reads negative.
    CHECK(out.offset_ns == 7);
    CHECK(out.delay_ns == 9);
    CHECK(hn::on_wire_sample(0, 0, 100, 100, 0,
                             out)); // A hold of exactly the exchange: zero delay, the precision's floor.
    CHECK(hn::on_wire_sample(0, 0, 199, 100, 99, out));
    CHECK(out.delay_ns == 99);
}

TEST_CASE("clock filter: the sample of least delay wins, with RFC 5905's dispersion and jitter", "[clock][filter]")
{
    hn::ClockFilterOptions options;
    options.phi = 0.0; // No aging, so the values are the hand's.
    options.precision_s = 1e-6;
    hn::ClockFilter filter = started_filter(0, options);
    constexpr double offsets[] = {0.10, 0.20, 0.30, 0.25};
    constexpr double delays[] = {0.040, 0.010, 0.030, 0.020};
    constexpr double dispersions[] = {1e-3, 2e-3, 3e-3, 4e-3};
    hn::ClockFilterEvent event = hn::ClockFilterEvent::Refused;
    for (u32 i = 0U; i < 4U; ++i)
    {
        event = filter.update(i64{i + 1U} * kPoll, offsets[i], delays[i], dispersions[i], kPoll);
    }
    // Order by delay: stage 1, 3, 2, 0, then the four empty ones.
    CHECK(filter.order(0U) == 1U);
    CHECK(filter.order(1U) == 3U);
    CHECK(filter.order(2U) == 2U);
    CHECK(filter.order(3U) == 0U);
    CHECK(filter.offset_s() == 0.20);
    CHECK(filter.delay_s() == 0.010);
    CHECK(filter.sample_dispersion_s() == 2e-3);
    CHECK(filter.epoch_ns() == 2 * kPoll);
    CHECK(event == hn::ClockFilterEvent::Stale); // Stage 1 was used at its own arrival.

    // ε = Σ ε_i / 2^(i+1) over the sorted stages, the empty ones at the maximum.
    const double expected_dispersion = (2e-3 / 2.0) + (4e-3 / 4.0) + (3e-3 / 8.0) + (1e-3 / 16.0) +
                                       16.0 * (1.0 / 32 + 1.0 / 64 + 1.0 / 128 + 1.0 / 256);
    CHECK(filter.dispersion_s() == Approx(expected_dispersion));
    // ψ = sqrt(Σ (θ_i - θ_0)² / (n - 1)) over the four filled stages.
    const double expected_jitter = std::sqrt((0.05 * 0.05 + 0.10 * 0.10 + 0.10 * 0.10) / 3.0);
    CHECK(filter.jitter_s() == Approx(expected_jitter));
}

TEST_CASE("clock filter: dispersions grow at phi, and past the Allan intercept they count in the distance",
          "[clock][filter]")
{
    hn::ClockFilterOptions options;
    options.allan_intercept_ns = 1000 * kSecond;
    hn::ClockFilter filter = started_filter(0, options);
    REQUIRE(filter.update(kSecond, 0.1, 0.001, 0.5, kPoll) == hn::ClockFilterEvent::Fresh);
    // Within the intercept the lower delay still wins, however dispersed.
    REQUIRE(filter.update(101 * kSecond, 0.2, 0.005, 0.0, kPoll) == hn::ClockFilterEvent::Stale);
    CHECK(filter.offset_s() == 0.1);
    CHECK(filter.stage(0U).dispersion_s == Approx(0.5 + (15e-6 * 100.0)));
    CHECK(filter.stage(1U).dispersion_s == 0.0); // The newest is not aged by its own update.
    // Past it the old sample's dispersion counts: 0.001 + 0.5 is more than 0.005 + 0.015.
    REQUIRE(filter.update(1101 * kSecond, 0.3, 0.05, 0.0, kPoll) == hn::ClockFilterEvent::Fresh);
    CHECK(filter.offset_s() == 0.2);
    CHECK(filter.epoch_ns() == 101 * kSecond);
}

TEST_CASE("clock filter: lost polls age it to no sample, and a sample restarts it", "[clock][filter]")
{
    hn::ClockFilter filter = started_filter(0);
    REQUIRE(filter.update(kPoll, 0.25, 0.01, 1e-5, kPoll) == hn::ClockFilterEvent::Fresh);
    for (i64 i = 2; i <= 8; ++i)
    {
        CHECK(filter.on_lost(i * kPoll, kPoll) == hn::ClockFilterEvent::Stale);
        CHECK(filter.offset_s() == 0.25);
    }
    // The eighth loss pushes the only sample out.
    CHECK(filter.on_lost(9 * kPoll, kPoll) == hn::ClockFilterEvent::NoSample);
    CHECK(filter.dispersion_s() == Approx(16.0 * (1.0 - (1.0 / 256.0))));
    CHECK(filter.jitter_s() == 0.0);
    CHECK(filter.offset_s() == 0.25);
    CHECK(filter.update(10 * kPoll, -0.5, 0.01, 1e-5, kPoll) == hn::ClockFilterEvent::Fresh);
    CHECK(filter.offset_s() == -0.5);
    CHECK(filter.jitter_s() == filter.jitter_s());
    CHECK(filter.dispersion_s() < 16.0);
    // A dispersion at or past the maximum is no sample either.
    hn::ClockFilter empty = started_filter(0);
    CHECK(empty.update(kPoll, 1.0, 0.0, 16.0, kPoll) == hn::ClockFilterEvent::NoSample);
}

TEST_CASE("clock filter: a popcorn spike is flagged within two polls and not after", "[clock][filter]")
{
    // The first sample, of the least delay, is used at 5 s; seven near 0.1 s wait behind it.
    const auto settled = []
    {
        hn::ClockFilter filter = started_filter(0);
        REQUIRE(filter.update(kSecond, 5.0, 1e-3, 1e-5, kSecond) == hn::ClockFilterEvent::Fresh);
        for (i64 i = 2; i <= 8; ++i)
        {
            REQUIRE(filter.update(i * kSecond, 0.1 + (static_cast<double>(i % 2) * 1e-4),
                                  0.01 + (1e-4 * static_cast<double>(i)), 1e-5,
                                  kSecond) == hn::ClockFilterEvent::Stale);
        }
        return filter;
    };
    // The ninth pushes it out. The offset moves 4.9 s, far past three jitters of the seven that agree, and the sample
    // that would be used is close enough to the last one used: a spike.
    hn::ClockFilter spiked = settled();
    CHECK(spiked.update(9 * kSecond, 0.1, 0.02, 1e-5, 8 * kSecond) == hn::ClockFilterEvent::Spike);
    CHECK(spiked.epoch_ns() == kSecond);
    CHECK(spiked.offset_s() == Approx(0.1)); // As ntpd: the values move, the event says not to use them.
    CHECK(spiked.jitter_s() < 1e-3);

    // With the last one used past the horizon the sample is taken.
    hn::ClockFilter taken = settled();
    CHECK(taken.update(9 * kSecond, 0.1, 0.02, 1e-5, kSecond / 4) == hn::ClockFilterEvent::Fresh);
    CHECK(taken.epoch_ns() == 2 * kSecond);
}

TEST_CASE("clock filter: the maximum distance leaves samples out of the jitter but keeps two", "[clock][filter]")
{
    hn::ClockFilterOptions options;
    options.phi = 0.0;
    options.max_distance_s = 0.015;
    hn::ClockFilter filter = started_filter(0, options);
    (void)filter.update(1 * kSecond, 0.00, 0.010, 0.0, kSecond);
    (void)filter.update(2 * kSecond, 0.01, 0.012, 0.0, kSecond);
    (void)filter.update(3 * kSecond, 0.02, 0.014, 0.0, kSecond);
    (void)filter.update(4 * kSecond, 5.00, 0.020, 0.0, kSecond); // Out: past the distance, and past two.
    CHECK(filter.jitter_s() == Approx(std::sqrt((0.01 * 0.01 + 0.02 * 0.02) / 2.0)));

    hn::ClockFilter two = started_filter(0, options);
    (void)two.update(1 * kSecond, 0.00, 0.020, 0.0, kSecond);
    (void)two.update(2 * kSecond, 0.03, 0.040, 0.0, kSecond); // Past the distance, but one of the two kept.
    CHECK(two.jitter_s() == Approx(0.03));
}

TEST_CASE("clock filter: a refused update changes nothing", "[clock][filter]")
{
    hn::ClockFilter filter = started_filter(5 * kSecond);
    REQUIRE(filter.update(6 * kSecond, 0.1, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Fresh);
    const hn::ClockFilter before = filter;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double over = hn::ClockFilter::kMaxMagnitudeS * 2.0;
    constexpr i64 far_time = hn::ClockFilter::kMaxTimeNs + 1;
    CHECK(filter.update(5 * kSecond, 0.1, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Refused); // Time back.
    CHECK(filter.update(far_time, 0.1, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, nan, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, over, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, kInfinity, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, 0.1, -1e-9, 1e-5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, 0.1, over, 1e-5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, 0.1, 0.01, -1.0, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, 0.1, 0.01, 16.5, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, 0.1, 0.01, 1e-5, 0) == hn::ClockFilterEvent::Refused);
    CHECK(filter.update(7 * kSecond, 0.1, 0.01, 1e-5, far_time) == hn::ClockFilterEvent::Refused);
    CHECK(filter.on_lost(5 * kSecond, kSecond) == hn::ClockFilterEvent::Refused);
    CHECK_FALSE(filter.slew(1.5, 0));
    CHECK_FALSE(filter.slew(nan, 0));
    CHECK_FALSE(filter.slew(0.0, far_time));
    CHECK(same_filter(filter, before));
}

TEST_CASE("clock filter: options out of range are refused, and the defaults stand", "[clock][filter]")
{
    hn::ClockFilter filter;
    hn::ClockFilterOptions bad;
    bad.precision_s = 0.0;
    CHECK_FALSE(filter.init(bad, 0));
    bad = {};
    bad.phi = -1.0;
    CHECK_FALSE(filter.init(bad, 0));
    bad = {};
    bad.max_distance_s = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(filter.init(bad, 0));
    bad = {};
    bad.allan_intercept_ns = -1;
    CHECK_FALSE(filter.init(bad, 0));
    CHECK_FALSE(filter.init({}, hn::ClockFilter::kMaxTimeNs + 1));
    CHECK(filter.update(kSecond, 0.1, 0.01, 1e-5, kSecond) == hn::ClockFilterEvent::Fresh);
}

TEST_CASE("clock filter: slew re-expresses every held offset", "[clock][filter]")
{
    hn::ClockFilter filter = started_filter(0);
    (void)filter.update(10 * kSecond, 1.0, 0.01, 1e-5, kSecond);
    (void)filter.update(20 * kSecond, 2.0, 0.02, 1e-5, kSecond);
    REQUIRE(filter.slew(0.01, 5 * kSecond));
    CHECK(filter.stage(0U).offset_s == Approx(1.0 - (0.01 * 5.0)));
    CHECK(filter.stage(1U).offset_s == Approx(2.0 - (0.01 * 15.0)));
    CHECK(filter.offset_s() == Approx(1.0 - (0.01 * 5.0)));
}

TEST_CASE("clock drift: the line is the weighted normal equations' with the prior", "[clock][drift]")
{
    SeededWords words(11U, 0U);
    for (u32 trial = 0U; trial < 200U; ++trial)
    {
        hn::ClockDriftOptions options;
        options.frequency_sd = 1e-3;
        hn::ClockDriftFilter drift;
        REQUIRE(drift.init(options));
        const u32 count = 1U + (words.next_u32() % hn::ClockDriftFilter::kRunsTestMinimum);
        crd::containers::Array<long double> xs;
        crd::containers::Array<long double> ys;
        crd::containers::Array<long double> ws;
        i64 time = 1'000 * kSecond;
        for (u32 i = 0U; i < count; ++i)
        {
            time += kSecond + (words.next_u32() % kSecond);
            const double offset = 1e-3 * static_cast<double>(words.next_u32() % 1000U) - 0.5;
            const double uncertainty = 1e-4 + (1e-6 * static_cast<double>(words.next_u32() % 1000U));
            REQUIRE(drift.add(time, offset, uncertainty));
            xs.push_back(static_cast<long double>(time) / 1e9L);
            ys.push_back(offset);
            ws.push_back(1.0L / (static_cast<long double>(uncertainty) * uncertainty));
        }
        long double sum_w = 0.0L;
        long double sum_x = 0.0L;
        long double sum_y = 0.0L;
        for (u32 i = 0U; i < count; ++i)
        {
            sum_w += ws[i];
            sum_x += ws[i] * xs[i];
            sum_y += ws[i] * ys[i];
        }
        const long double mean_x = sum_x / sum_w;
        const long double mean_y = sum_y / sum_w;
        long double sxx = 1.0L / 1e-6L;
        long double sxy = 0.0L;
        for (u32 i = 0U; i < count; ++i)
        {
            sxx += ws[i] * (xs[i] - mean_x) * (xs[i] - mean_x);
            sxy += ws[i] * (xs[i] - mean_x) * (ys[i] - mean_y);
        }
        const long double slope = sxy / sxx;
        REQUIRE(drift.count() == count);
        CHECK(drift.frequency() == Approx(static_cast<double>(slope)).epsilon(1e-9).margin(1e-13));
        const long double at = static_cast<long double>(time) / 1e9L + 7.0L;
        const double expected = static_cast<double>(mean_y + (slope * (at - mean_x)));
        CHECK(drift.offset_s_at(time + (7 * kSecond)) == Approx(expected).epsilon(1e-9).margin(1e-11));
    }
}

TEST_CASE("clock drift: one sample is a level line; the prior holds a short baseline back", "[clock][drift]")
{
    hn::ClockDriftFilter drift;
    CHECK(drift.count() == 0U);
    CHECK(drift.frequency() == 0.0);
    CHECK(drift.offset_s_at(0) == 0.0);
    CHECK(drift.frequency_sd() == 500e-6);
    CHECK(drift.offset_sd_s_at(0) == hn::ClockDriftFilter::kMaxMagnitudeS);
    CHECK_FALSE(drift.contradicts(0, 1e30, 0.0));

    REQUIRE(drift.add(kSecond, 0.25, 1e-3));
    CHECK(drift.frequency() == 0.0);
    CHECK(drift.offset_s_at(kSecond * 9) == 0.25);
    CHECK(drift.offset_sd_s_at(kSecond) == Approx(1e-3));
    // 1 ms of noise one second apart says nothing past the 500 PPM prior: 0.1 reads far under it.
    REQUIRE(drift.add(2 * kSecond, 0.251, 1e-3));
    CHECK(std::fabs(drift.frequency()) < 200e-6);
}

TEST_CASE("clock drift: an exact line is recovered through the whole window", "[clock][drift]")
{
    hn::ClockDriftOptions options;
    options.frequency_sd = 1.0; // Practically no prior.
    hn::ClockDriftFilter drift;
    REQUIRE(drift.init(options));
    constexpr double frequency = 87.5e-6;
    for (i64 i = 0; i < 200; ++i)
    {
        const double offset = 3.0 + (frequency * static_cast<double>(i));
        REQUIRE(drift.add(i * kSecond, offset, 1e-6));
        REQUIRE(drift.frequency() == Approx(frequency).epsilon(1e-6).margin(i < 2 ? 1e-3 : 1e-9));
    }
    CHECK(drift.count() == hn::ClockDriftFilter::kCapacity);
    CHECK(drift.offset_s_at(250 * kSecond) == Approx(3.0 + (frequency * 250.0)));
    CHECK(drift.newest_ns() == 199 * kSecond);
    CHECK(drift.offset_sd_s_at(199 * kSecond) < 1e-5);
}

TEST_CASE("clock drift: the runs test cuts a kink out and keeps white noise whole", "[clock][drift]")
{
    SeededWords words(5U, 1U);
    const auto noise = [&words](double scale)
    {
        return scale * ((static_cast<double>(words.next_u32()) / 2147483648.0) - 1.0);
    };

    // White noise: the window stays nearly full across the run.
    hn::ClockDriftFilter steady;
    u32 lowest = hn::ClockDriftFilter::kCapacity;
    u64 total = 0U;
    for (i64 i = 0; i < 4000; ++i)
    {
        REQUIRE(steady.add(i * kSecond, 40e-6 * static_cast<double>(i) + noise(1e-4), 1e-4));
        if (i >= 64)
        {
            lowest = std::min(lowest, steady.count());
            total += steady.count();
        }
    }
    CHECK(static_cast<double>(total) / 3936.0 > 50.0);
    CHECK(lowest >= 8U);

    // A kink: the frequency changes by 100 PPM. Within a window the line has the new frequency again.
    hn::ClockDriftFilter kinked;
    double offset = 0.0;
    for (i64 i = 0; i < 400; ++i)
    {
        offset += (i < 200) ? 20e-6 : 120e-6;
        REQUIRE(kinked.add(i * kSecond, offset + noise(1e-5), 1e-5));
        if (i == 199)
        {
            CHECK(kinked.frequency() == Approx(20e-6).margin(2e-6));
        }
        if (i == 240)
        {
            CHECK(kinked.count() < hn::ClockDriftFilter::kCapacity);
            CHECK(kinked.frequency() == Approx(120e-6).margin(5e-6));
        }
    }
    CHECK(kinked.frequency() == Approx(120e-6).margin(1e-6));
}

TEST_CASE("clock drift: refusals change nothing; residuals past the stated error scale the errors", "[clock][drift]")
{
    hn::ClockDriftFilter drift;
    CHECK_FALSE(drift.init({.frequency_sd = 0.0}));
    REQUIRE(drift.init({}));
    REQUIRE(drift.add(kSecond, 0.0, 1e-3));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(drift.add(kSecond, 0.1, 1e-3)); // Time must advance.
    CHECK_FALSE(drift.add(0, 0.1, 1e-3));
    CHECK_FALSE(drift.add(2 * kSecond, nan, 1e-3));
    CHECK_FALSE(drift.add(2 * kSecond, hn::ClockDriftFilter::kMaxMagnitudeS * 2.0, 1e-3));
    CHECK_FALSE(drift.add(2 * kSecond, 0.1, 0.0));
    CHECK_FALSE(drift.add(2 * kSecond, 0.1, kInfinity));
    CHECK_FALSE(drift.add(hn::ClockFilter::kMaxTimeNs + 1, 0.1, 1e-3));
    CHECK(drift.count() == 1U);
    CHECK(drift.offset_s_at(kSecond) == 0.0);

    hn::ClockDriftFilter stated;
    hn::ClockDriftFilter understated;
    SeededWords words(3U, 2U);
    for (i64 i = 0; i < 12; ++i)
    {
        const double noise = 1e-3 * ((static_cast<double>(words.next_u32()) / 2147483648.0) - 1.0);
        REQUIRE(stated.add(i * kSecond, noise, 1e-3));
        REQUIRE(understated.add(i * kSecond, noise, 1e-6));
    }
    // A stated 1 µs against 1 ms of real noise: the scale reports the noise, as the stated 1 ms does.
    CHECK(understated.offset_sd_s_at(11 * kSecond) > 1e-4);
    CHECK(stated.offset_sd_s_at(11 * kSecond) > 1e-4);
    // The error grows away from the data.
    CHECK(stated.offset_sd_s_at(1000 * kSecond) > 10.0 * stated.offset_sd_s_at(11 * kSecond));
}

TEST_CASE("clock drift: a contradiction needs the line outside the interval by four standard errors", "[clock][drift]")
{
    hn::ClockDriftFilter drift;
    for (i64 i = 0; i < 40; ++i)
    {
        REQUIRE(drift.add(i * kSecond, 1.0, 1e-5));
    }
    const i64 at = 40 * kSecond;
    const double gate = 1e-3 + (4.0 * drift.offset_sd_s_at(at));
    CHECK_FALSE(drift.contradicts(at, 1.0 + gate - 1e-9, 1e-3));
    CHECK(drift.contradicts(at, 1.0 + gate + 1e-9, 1e-3));
    CHECK(drift.contradicts(at, 1.0 - gate - 1e-9, 1e-3));
}

namespace
{

struct PathResult
{
    crd::containers::Array<double> raw_errors;
    crd::containers::Array<double> errors;
    u32 bound_violations = 0U;
    u32 measured = 0U;
    u32 steps = 0U;
    u32 step_poll = 0U;
    double frequency_error = 0.0;
    double frequency_error_at_step = 0.0;
};

// Past the first 200 polls and, with an event, its 120 polls of recovery.
[[nodiscard]] PathResult run_path(const clock_paths::Profile& profile, u64 seed, u32 polls)
{
    constexpr u32 settle = 200U;
    constexpr u32 recovery = 120U;
    PathResult result;
    clock_paths::Path path(profile, seed, 1'234'567'890'123);
    hn::ClockTracker tracker;
    REQUIRE(tracker.init({}));
    for (u32 poll = 1U; poll <= polls; ++poll)
    {
        const clock_paths::Exchange exchange = path.next();
        const hn::ClockTrackerEvent event =
            exchange.lost ? tracker.on_lost(exchange.t4, path.poll_ns())
                          : tracker.on_exchange(exchange.t1, exchange.t2, exchange.t3, exchange.t4, path.poll_ns());
        if (event == hn::ClockTrackerEvent::Step)
        {
            ++result.steps;
            result.step_poll = poll;
            result.frequency_error_at_step = tracker.frequency() - exchange.true_frequency;
        }
        const bool in_event =
            profile.event_poll != 0U && poll >= profile.event_poll && poll < profile.event_poll + recovery;
        if (poll < settle || in_event)
        {
            continue;
        }
        const double error = static_cast<double>(tracker.offset_ns_at(exchange.t4) - exchange.true_offset_ns) * 1e-9;
        result.errors.push_back(std::fabs(error));
        if (!exchange.lost)
        {
            hn::OnWireSample wire;
            REQUIRE(hn::on_wire_sample(exchange.t1, exchange.t2, exchange.t3, exchange.t4, 1, wire));
            result.raw_errors.push_back(
                std::fabs(static_cast<double>(wire.offset_ns - exchange.true_offset_ns) * 1e-9));
        }
        ++result.measured;
        if (std::fabs(error) > tracker.offset_bound_s_at(exchange.t4))
        {
            ++result.bound_violations;
        }
        result.frequency_error = tracker.frequency() - exchange.true_frequency;
    }
    return result;
}

[[nodiscard]] double median(crd::containers::Array<double> values)
{
    REQUIRE_FALSE(values.empty());
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

} // namespace

TEST_CASE("clock tracker: every hostile path stays within its bound and beats the raw exchange", "[clock][tracker]")
{
    for (const clock_paths::Profile& profile : clock_paths::kProfiles)
    {
        for (u64 seed = 1U; seed <= 5U; ++seed)
        {
            INFO(profile.name << " seed " << seed);
            const PathResult result = run_path(profile, seed, 3000U);
            REQUIRE(result.measured > 2000U);
            CHECK(result.bound_violations == 0U);
            if (profile.asymmetry_us == 0U)
            {
                CHECK(median(result.errors) * 3.0 < median(result.raw_errors));
            }
            if (profile.step_us == 0)
            {
                CHECK(result.steps == 0U);
            }
            CHECK(std::fabs(result.frequency_error) < 12e-6);
        }
    }
}

TEST_CASE("clock tracker: a step is confirmed within a few polls, keeps the frequency and is exact again",
          "[clock][tracker]")
{
    for (const clock_paths::Profile& profile : clock_paths::kProfiles)
    {
        if (profile.step_us == 0)
        {
            continue;
        }
        for (u64 seed = 1U; seed <= 5U; ++seed)
        {
            INFO(profile.name << " seed " << seed);
            const PathResult result = run_path(profile, seed, 3000U);
            CHECK(result.steps == 1U);
            CHECK(result.step_poll >= profile.event_poll + hn::ClockTracker::kStepConfirmations - 1U);
            CHECK(result.step_poll <= profile.event_poll + 12U);
            CHECK(std::fabs(result.frequency_error_at_step) < 10e-6);
            CHECK(median(result.errors) < 5e-5);
        }
    }
}

TEST_CASE("clock tracker: free-running clocks through the filter lose nothing to the drift", "[clock][tracker]")
{
    // The frequency removed inside the filter: on a 100 PPM difference with a quiet path the sample's age costs
    // nothing.
    constexpr clock_paths::Profile fast = {"fast", 1'000'000, 100U, 5U, 0U,  100.0, 0.0, 0U,
                                           0U,     0U,        0U,   0,  0.0, 0U,    0.0, 0U};
    const PathResult result = run_path(fast, 3U, 1000U);
    CHECK(median(result.errors) < 2e-6);
    CHECK(std::fabs(result.frequency_error) < 1e-7);
}

TEST_CASE("clock tracker: the asymmetry no exchange can see stays inside the bound", "[clock][tracker]")
{
    const clock_paths::Profile& asymmetric = clock_paths::kProfiles[3];
    REQUIRE(asymmetric.asymmetry_us == 30'000U);
    const PathResult result = run_path(asymmetric, 9U, 2000U);
    CHECK(median(result.errors) == Approx(0.015).margin(0.002));
    CHECK(result.bound_violations == 0U);
}

TEST_CASE("clock tracker: a lone spike is set aside; repeated disagreement steps; refusals change nothing",
          "[clock][tracker]")
{
    hn::ClockTracker tracker;
    CHECK_FALSE(tracker.synchronised());
    CHECK(tracker.offset_bound_s_at(0) == hn::ClockFilter::kMaxDispersionS);
    CHECK(tracker.on_lost(0, kSecond) == hn::ClockTrackerEvent::Refused); // Nothing to age yet.

    // Exchanges of 2 ms on a clock 50 ms ahead.
    const auto exchange = [&tracker](i64 at, i64 offset, i64 delay = 2'000'000)
    {
        return tracker.on_exchange(at - delay, at - (delay / 2) + offset, at - (delay / 2) + offset, at, kSecond);
    };
    constexpr i64 peer_offset = 50'000'000;
    i64 at = 100 * kSecond;
    for (u32 i = 0U; i < 60U; ++i, at += kSecond)
    {
        REQUIRE(exchange(at, peer_offset) != hn::ClockTrackerEvent::Refused);
    }
    CHECK(tracker.synchronised());
    CHECK(tracker.offset_ns_at(at) == peer_offset);
    CHECK(tracker.frequency() == Approx(0.0).margin(1e-9));

    const double bound = tracker.offset_bound_s_at(at);
    CHECK(bound >= 1e-3);
    CHECK(bound < 2e-3);
    CHECK(tracker.offset_bound_s_at(at + 1000 * kSecond) > bound + (15e-6 * 900.0)); // Grows while we hear nothing.

    // Refused: an impossible exchange, time back, a zero poll.
    const u32 count = tracker.drift().count();
    CHECK(tracker.on_exchange(at, at, at - 1, at + 1, kSecond) == hn::ClockTrackerEvent::Refused);
    CHECK(tracker.on_exchange(at - 2 * kSecond, 0, 0, at - kSecond - kSecond, kSecond) != hn::ClockTrackerEvent::Spike);
    CHECK(exchange(at - 5 * kSecond, peer_offset) == hn::ClockTrackerEvent::Refused);
    CHECK(tracker.on_exchange(at - 1000, at, at, at, 0) == hn::ClockTrackerEvent::Refused);
    CHECK(tracker.on_lost(at - 5 * kSecond, kSecond) == hn::ClockTrackerEvent::Refused);
    CHECK(tracker.on_lost(at, 0) == hn::ClockTrackerEvent::Refused);
    CHECK(tracker.drift().count() == count);

    // A lone exchange 200 ms off, its interval far from the line: set aside.
    CHECK(exchange(at, peer_offset + 200'000'000) == hn::ClockTrackerEvent::Spike);
    at += kSecond;
    CHECK(tracker.offset_ns_at(at) == peer_offset);
    REQUIRE(exchange(at, peer_offset) != hn::ClockTrackerEvent::Spike);
    at += kSecond;
    // Two that disagree with each other do not step, however many alternate.
    for (u32 i = 0U; i < 9U; ++i, at += kSecond)
    {
        const i64 away = (i % 2U == 0U) ? 200'000'000 : 400'000'000;
        CHECK(exchange(at, peer_offset + away) == hn::ClockTrackerEvent::Spike);
    }
    CHECK(tracker.steps() == 0U);
    // Three that agree do, and the estimate is at the new offset at once.
    REQUIRE(exchange(at, peer_offset) != hn::ClockTrackerEvent::Spike); // Clears the run.
    at += kSecond;
    CHECK(exchange(at, peer_offset + 300'000'000) == hn::ClockTrackerEvent::Spike);
    at += kSecond;
    CHECK(exchange(at, peer_offset + 300'000'000) == hn::ClockTrackerEvent::Spike);
    at += kSecond;
    CHECK(exchange(at, peer_offset + 300'000'000) == hn::ClockTrackerEvent::Step);
    CHECK(tracker.steps() == 1U);
    CHECK(tracker.offset_ns_at(at) == peer_offset + 300'000'000);

    // Disagreement that never agrees forces a restart rather than a line that is stale for ever.
    at += kSecond;
    for (u32 i = 0U; i < 12U; ++i, at += kSecond)
    {
        REQUIRE(exchange(at, peer_offset + 300'000'000) != hn::ClockTrackerEvent::Spike);
    }
    hn::ClockTrackerEvent last = hn::ClockTrackerEvent::Refused;
    u32 run = 0U;
    while (last != hn::ClockTrackerEvent::Step)
    {
        const i64 away = (run % 2U == 0U) ? kSecond : 2 * kSecond;
        last = exchange(at, away);
        at += kSecond;
        ++run;
        REQUIRE(run <= hn::ClockTracker::kStepForcedAfter);
    }
    CHECK(run == hn::ClockTracker::kStepForcedAfter);
    CHECK(tracker.steps() == 2U);
}

TEST_CASE("clock tracker: options out of range are refused", "[clock][tracker]")
{
    hn::ClockTracker tracker;
    hn::ClockTrackerOptions bad;
    bad.delay_window_ns = 0;
    CHECK_FALSE(tracker.init(bad));
    bad = {};
    bad.filter.precision_s = -1.0;
    CHECK_FALSE(tracker.init(bad));
    bad = {};
    bad.drift.frequency_sd = 0.0;
    CHECK_FALSE(tracker.init(bad));
    CHECK(tracker.on_exchange(0, 0, 0, kSecond, kSecond) == hn::ClockTrackerEvent::Updated);
    CHECK(tracker.init({}));
    CHECK_FALSE(tracker.synchronised());
}

TEST_CASE("clock tracker: a lost poll ages the bound and can bring out an older sample", "[clock][tracker]")
{
    hn::ClockTracker tracker;
    REQUIRE(tracker.init({}));
    i64 at = 10 * kSecond;
    const auto exchange = [&tracker, &at](i64 delay)
    {
        const hn::ClockTrackerEvent event =
            tracker.on_exchange(at - delay, at - (delay / 2), at - (delay / 2), at, kSecond);
        at += kSecond;
        return event;
    };
    CHECK(exchange(9'000'000) == hn::ClockTrackerEvent::Updated);
    CHECK(exchange(3'000'000) == hn::ClockTrackerEvent::Updated);
    CHECK(exchange(5'000'000) == hn::ClockTrackerEvent::Filtered); // A newer sample of more delay waits.
    for (u32 i = 0U; i < 5U; ++i)
    {
        CHECK(tracker.on_lost(at, kSecond) == hn::ClockTrackerEvent::Filtered);
        at += kSecond;
    }
    // The 3 ms sample has aged out of the register, so the 5 ms one is used at last.
    CHECK(tracker.on_lost(at, kSecond) == hn::ClockTrackerEvent::Filtered);
    at += kSecond;
    CHECK(tracker.on_lost(at, kSecond) == hn::ClockTrackerEvent::Updated);
    CHECK(tracker.drift().count() == 3U);
}

TEST_CASE("clock tracker: exchanges at the ends of the clock range stay finite", "[clock][tracker]")
{
    hn::ClockTracker tracker;
    SeededWords words(21U, 3U);
    const i64 ends[] = {kI64Min, kI64Min / 2, -1, 0, 1, kI64Max / 2, kI64Max};
    i64 at = 0;
    for (u32 i = 0U; i < 20000U; ++i)
    {
        at += kSecond / 8;
        const i64 peer = ends[words.next_u32() % 7U] + static_cast<i64>(words.next_u32() % 1000U);
        const i64 rtt = static_cast<i64>(words.next_u32() % 100'000'000U);
        (void)tracker.on_exchange(at - rtt, peer, peer, at, kSecond);
        CHECK(std::isfinite(tracker.frequency()));
        CHECK(std::isfinite(tracker.offset_bound_s_at(at)));
        CHECK(tracker.offset_bound_s_at(at) >= 0.0);
        (void)tracker.offset_ns_at(at);
    }
}

TEST_CASE("media clock: the line through a 90 kHz clock's reports", "[clock][media]")
{
    hn::MediaClockMap map;
    CHECK(map.estimate(0U) == 0U);
    CHECK(map.frequency_khz() == 0.0);
    constexpr u64 ntp_second = u64{1} << 32U;
    u64 ntp = u64{3'900'000'000} << 32U;
    u32 rtp = 0xFFFF0000U; // Wraps soon after the start.
    CHECK(map.update(ntp, rtp) == hn::MediaClockUpdate::New);
    CHECK(map.estimate(rtp) == 0U); // One report is no line.
    for (u32 i = 0U; i < 30U; ++i)
    {
        ntp += ntp_second;
        rtp += 90'000U;
        REQUIRE(map.update(ntp, rtp) == hn::MediaClockUpdate::New);
    }
    CHECK(map.frequency_khz() == Approx(90.0));
    const u64 halfway = map.estimate(rtp + 45'000U);
    CHECK(static_cast<double>(halfway - ntp) == Approx(static_cast<double>(ntp_second) / 2.0).epsilon(1e-4));
}

TEST_CASE("media clock: duplicates, out-of-order reports and jumps; three in a row restart it", "[clock][media]")
{
    constexpr u64 ntp_second = u64{1} << 32U;
    hn::MediaClockMap map;
    const u64 ntp = ntp_second * 100U;
    CHECK(map.update(0U, 5U) == hn::MediaClockUpdate::Invalid); // NTP zero is not a time.
    CHECK(map.update(ntp, 1000U) == hn::MediaClockUpdate::New);
    CHECK(map.update(ntp, 91000U) == hn::MediaClockUpdate::Same);             // The same NTP time.
    CHECK(map.update(ntp + ntp_second, 1000U) == hn::MediaClockUpdate::Same); // The same RTP time.
    CHECK(map.update(ntp + ntp_second, 91000U) == hn::MediaClockUpdate::New);
    const double frequency = map.frequency_khz();
    CHECK(frequency == Approx(90.0));

    CHECK(map.update(ntp - ntp_second, 91500U) == hn::MediaClockUpdate::Invalid);    // NTP back.
    CHECK(map.update(ntp + ntp_second * 2U, 900U) == hn::MediaClockUpdate::Invalid); // RTP back.
    CHECK(map.update(ntp + ntp_second * 3U, 92000U) == hn::MediaClockUpdate::New);   // A good one clears the count.
    CHECK(map.update(ntp + ntp_second * 4000U, 93000U) == hn::MediaClockUpdate::Invalid); // An NTP jump past an hour.
    CHECK(map.update(ntp + ntp_second * 4U, 92500U + (1U << 26U)) == hn::MediaClockUpdate::Invalid); // An RTP jump.
    CHECK(map.frequency_khz() != 0.0);
    // The third in a row restarts: the line is gone until a second report arrives.
    CHECK(map.update(ntp - ntp_second * 50U, 500U) == hn::MediaClockUpdate::New);
    CHECK(map.frequency_khz() == 0.0);
    CHECK(map.estimate(500U) == 0U);
    CHECK(map.update(ntp - ntp_second * 49U, 90500U) == hn::MediaClockUpdate::New);
    CHECK(map.frequency_khz() == Approx(90.0));
}

TEST_CASE("media clock: half the range apart, the larger timestamp is ahead", "[clock][media]")
{
    // One NTP unit per tick, so the estimate reads the unwrapped timestamp.
    constexpr u64 base = u64{1} << 40U;
    constexpr u64 half = u64{1} << 31U;
    hn::MediaClockMap map;
    REQUIRE(map.update(base, 0U) == hn::MediaClockUpdate::New);
    REQUIRE(map.update(base + 1000U, 1000U) == hn::MediaClockUpdate::New);
    CHECK(map.estimate(1000U + 0x80000000U) == base + 1000U + half);
    CHECK(map.estimate(1000U) == base + 1000U);
    CHECK(map.estimate(999U) == base + 999U); // A step back within the half stays behind.
    CHECK(map.estimate(0xFFFFFFFFU) == base - 1U);
}

TEST_CASE("media clock: estimates saturate at both ends", "[clock][media]")
{
    constexpr u64 ntp_second = u64{1} << 32U;
    hn::MediaClockMap map;
    REQUIRE(map.update(1U, 2U) == hn::MediaClockUpdate::New);
    REQUIRE(map.update(1U + ntp_second, 90002U) == hn::MediaClockUpdate::New);
    // A timestamp behind the line's start reads as before NTP zero.
    CHECK(map.estimate(0xFFFF0000U) == 0U);
    hn::MediaClockMap high;
    const u64 top = ~u64{0} - (ntp_second * 3600U);
    REQUIRE(high.update(top, 0U) == hn::MediaClockUpdate::New);
    REQUIRE(high.update(top + (ntp_second * 3599U), 1U << 25U) == hn::MediaClockUpdate::New);
    // Far along the line, past 2^64.
    CHECK(high.estimate(0x7FFFFFFFU) == ~u64{0});
}

TEST_CASE("capture clock: averaging, the 300 ms restart, no future and the least interval", "[clock][capture]")
{
    hn::CaptureClockAligner aligner;
    i64 out = 0;
    // A capturer on another origin: the first frame maps to the system time.
    REQUIRE(aligner.translate(1'000, 500'000, out));
    CHECK(out == 500'000);
    CHECK(aligner.translate(1'000) == 500'000);
    // A later frame read 4 ms late: the offset moves by half the error (2 frames seen), and the result stays below it.
    REQUIRE(aligner.translate(34'000, 537'000, out));
    CHECK(out == 34'000 + 499'000 + 2'000);
    // No frame is ever translated into the future: the bias takes the difference.
    REQUIRE(aligner.translate(67'000, 501'000 + 67'000, out));
    CHECK(out <= 501'000 + 67'000);
    // A frame stamped at the same capture time keeps the least interval.
    const i64 previous = out;
    REQUIRE(aligner.translate(67'000, previous + 900'000 / 3, out));
    CHECK(out >= previous + 1'000);
    // With a system clock that leaves no room, it stops at the system clock.
    const i64 pinned = out;
    REQUIRE(aligner.translate(67'000, pinned + 10, out));
    CHECK(out == pinned + 10);

    // Past the threshold the filter restarts on that frame.
    REQUIRE(aligner.translate(100'000, 10'000'000, out));
    CHECK(out == 10'000'000);
    // Past the window the average moves by a hundredth.
    hn::CaptureClockAligner averaged;
    for (i64 i = 0; i < 200; ++i)
    {
        REQUIRE(averaged.translate(i * 33'000, (i * 33'000) + 1'000'000, out));
    }
    REQUIRE(averaged.translate(200 * 33'000, (200 * 33'000) + 1'010'000, out));
    CHECK(out == (200 * 33'000) + 1'000'000 + 100);

    i64 untouched = 77;
    CHECK_FALSE(averaged.translate(hn::CaptureClockAligner::kMaxTimeUs + 1, 0, untouched));
    CHECK_FALSE(averaged.translate(0, -hn::CaptureClockAligner::kMaxTimeUs - 1, untouched));
    CHECK(untouched == 77);
}
