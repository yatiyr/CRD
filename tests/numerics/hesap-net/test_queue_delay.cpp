// crd-hesap-net -- queue delay estimation (HESAP-NET; design S71): RFC 6817's base delay history of ten one-minute
// minima, its rotation, a long gap and a reset; SCReAM v2's queue delay history against the definitions of its mean
// over the newest 50 samples and its population variance over 200, exactly, as it fills and wraps; and the shared
// integer square root.

#include <crd/hesap/net/fixed_point.hpp>
#include <crd/hesap/net/queue_delay.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
namespace hn = crd::hesap::net;

constexpr u64 kSecond = 1000000000U;
constexpr u64 kMinute = 60U * kSecond;

} // namespace

TEST_CASE("BaseDelayHistory: the least of ten one-minute minima, rotating and resetting", "[hesap-net][queue-delay]")
{
    hn::BaseDelayHistory base;
    CHECK_FALSE(base.has_sample());
    CHECK(base.base() == ~u64{0});
    base.update(30U, 0U);
    base.update(20U, kSecond);
    base.update(25U, kMinute - 1U); // the same minute: its minimum stays 20
    CHECK(base.base() == 20U);
    // Nine more minutes, each with a minimum above 20: 20 is still the base.
    for (u64 minute = 1U; minute < hn::BaseDelayHistory::kIntervals; ++minute)
    {
        base.update(40U + minute, minute * kMinute);
        CHECK(base.base() == 20U);
    }
    // The eleventh minute drops the first: the base rises to the next least, 41.
    base.update(60U, 10U * kMinute);
    CHECK(base.base() == 41U);
    // A propagation delay that rose: within ten minutes every interval holds it.
    for (u64 minute = 11U; minute <= 20U; ++minute)
    {
        base.update(100U, minute * kMinute);
    }
    CHECK(base.base() == 100U);
    // A long gap opens one interval (appendix A), the older ones stay until they rotate out.
    base.update(90U, 600U * kMinute);
    CHECK(base.base() == 90U);
    // Time going backwards stays in the newest interval.
    base.update(80U, 0U);
    CHECK(base.base() == 80U);
    base.reset();
    CHECK_FALSE(base.has_sample());
    base.update(500U, 601U * kMinute);
    CHECK(base.base() == 500U);
}

TEST_CASE("QueueDelayHistory: the mean of the newest 50 and the variance of 200, exact as it fills and wraps",
          "[hesap-net][queue-delay]")
{
    hn::QueueDelayHistory history;
    CHECK(history.count() == 0U);
    CHECK(history.mean() == 0U);
    CHECK(history.variance() == 0U);
    u16 samples[1000] = {};
    u64 state = 0x9E3779B97F4A7C15U;
    for (u32 i = 0U; i < 1000U; ++i)
    {
        state = state * 6364136223846793005U + 1442695040888963407U;
        // Mostly small, with bursts near the top of the range, so the sums run large.
        samples[i] = static_cast<u16>(((i / 100U) % 3U == 2U) ? 0xFF00U + (state >> 56U) : (state >> 52U));
        history.push(samples[i]);
        const u32 n = (i + 1U < 200U) ? i + 1U : 200U;
        const u32 short_n = (i + 1U < 50U) ? i + 1U : 50U;
        u64 sum = 0U;
        u64 squares = 0U;
        for (u32 k = 0U; k < n; ++k)
        {
            const u64 x = samples[i - k];
            sum += x;
            squares += x * x;
        }
        u64 short_sum = 0U;
        for (u32 k = 0U; k < short_n; ++k)
        {
            short_sum += samples[i - k];
        }
        CHECK(history.count() == n);
        CHECK(history.mean() == short_sum / short_n);
        const u64 expected = (n < 2U) ? 0U : (n * squares - sum * sum) / (u64{n} * n);
        CHECK(history.variance() == expected);
    }
    history.reset();
    CHECK(history.count() == 0U);
    CHECK(history.mean() == 0U);
    history.push(7U);
    CHECK(history.mean() == 7U);
    CHECK(history.variance() == 0U);
}

TEST_CASE("square_root: floor of the root across the range", "[hesap-net][queue-delay]")
{
    for (u64 r = 0U; r < 5000U; ++r)
    {
        CHECK(hn::square_root(r * r) == r);
        CHECK(hn::square_root(r * r + 2U * r) == r); // (r + 1)^2 - 1
    }
    CHECK(hn::square_root(~u64{0}) == 0xFFFFFFFFU);
    CHECK(hn::square_root(u64{0xFFFFFFFFU} * 0xFFFFFFFFU) == 0xFFFFFFFFU);
    CHECK(hn::square_root(u64{0xFFFFFFFFU} * 0xFFFFFFFFU - 1U) == 0xFFFFFFFEU);
}
