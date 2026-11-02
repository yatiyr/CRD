// crd-crypto -- anti-replay (NET.2, threat T07; docs/design/network-foundation.md#anti-replay): the sequence number
// window against a restated model over long random walks of in-order, reordered, duplicated, replayed and far-jumping
// numbers in three sizes, its edges (the span's last remembered number and the first Stale one, a jump past the whole
// ring, the extremes of the number range), check never recording, clear; and the single-use filter's periods, slot
// reuse after a long silence, refusals and false positive rate at capacity. A NEW_TOKEN token's single use is
// test_quic_token.cpp's, and QUIC packets through a packet number space's window are test_quic.cpp's.

#include <crd/containers/hash_set.hpp>
#include <crd/crypto/replay.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

constexpr u64 kMax = ~u64{0};

// The window's contract restated over every number accepted so far: a number above the largest (or the first) is
// Fresh, one kSpan or more below it Stale, any other Duplicate exactly when it was accepted.
class Model
{
public:
    explicit Model(u64 span) noexcept : m_span(span) {}

    [[nodiscard]] ReplayVerdict verdict(u64 number) const
    {
        if (m_accepted.empty() || number > m_largest)
        {
            return ReplayVerdict::Fresh;
        }
        if (m_largest - number >= m_span)
        {
            return ReplayVerdict::Stale;
        }
        return m_accepted.contains(number) ? ReplayVerdict::Duplicate : ReplayVerdict::Fresh;
    }

    void accept(u64 number)
    {
        m_largest = m_accepted.empty() || number > m_largest ? number : m_largest;
        static_cast<void>(m_accepted.insert(number));
    }

private:
    u64                           m_span;
    u64                           m_largest = 0U;
    crd::containers::HashSet<u64> m_accepted;
};

// xorshift64*: a fixed stream, so every walk is the same on every run and platform.
class Stream
{
public:
    explicit Stream(u64 seed) noexcept : m_state(seed) {}

    u64 next() noexcept
    {
        m_state ^= m_state >> 12U;
        m_state ^= m_state << 25U;
        m_state ^= m_state >> 27U;
        return m_state * 0x2545F4914F6CDD1DULL;
    }

private:
    u64 m_state;
};

// A walk mostly forward with reordering, duplicates, replays from far back and rare jumps far ahead, from `start`;
// every number is checked and then offered, and both verdicts are the model's.
template <usize Bits>
void walk(u64 seed, u64 start, usize steps)
{
    ReplayWindow<Bits> window;
    Model              model(ReplayWindow<Bits>::kSpan);
    Stream             stream(seed);
    u64                next     = start;
    usize              kinds[3] = {};
    for (usize step = 0U; step < steps; ++step)
    {
        const u64 roll   = stream.next();
        const u64 offset = stream.next() % (2U * Bits);
        u64       number = next;
        switch (roll % 16U)
        {
        case 0U: // far behind, across the span's edge
            number = next > offset ? next - offset : 0U;
            break;
        case 1U: // a jump ahead, now and then past the whole ring
            number = next + offset * ((roll >> 8U) % 4U == 0U ? Bits : 1U);
            break;
        case 2U:
        case 3U: // just behind: reordered, or a duplicate
            number = next > offset % 16U ? next - offset % 16U : 0U;
            break;
        default: // the next one, or skipping a few
            number = next + offset % 4U;
            break;
        }
        const ReplayVerdict expected = model.verdict(number);
        REQUIRE(window.check(number) == expected);
        REQUIRE(window.accept(number) == expected);
        ++kinds[static_cast<usize>(expected)];
        if (expected == ReplayVerdict::Fresh)
        {
            model.accept(number);
            REQUIRE(window.check(number) == ReplayVerdict::Duplicate);
        }
        REQUIRE(window.any());
        next = window.largest() + 1U;
    }
    // Every kind came up many times.
    CAPTURE(seed, kinds[0], kinds[1], kinds[2]);
    CHECK(kinds[static_cast<usize>(ReplayVerdict::Fresh)] > steps / 2U);
    CHECK(kinds[static_cast<usize>(ReplayVerdict::Duplicate)] > steps / 50U);
    CHECK(kinds[static_cast<usize>(ReplayVerdict::Stale)] > steps / 100U);
}
} // namespace

TEST_CASE("Anti-replay: a window agrees with its restated contract over long random walks", "[crypto][net-2][replay]")
{
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        walk<128>(seed, 0U, 4000U);
        walk<256>(seed * 0x9E3779B97F4A7C15ULL, u64{1} << 40U, 4000U);
        walk<1024>(seed + 77U, (u64{1} << 62U) - 3000U, 4000U);
    }
}

TEST_CASE("Anti-replay: a window accepts each number once, in any order within its span", "[crypto][net-2][replay]")
{
    using Window = ReplayWindow<128>;
    static_assert(Window::kSpan == 64U);
    static_assert(sizeof(ReplayWindow<256>) == 48U, "a 256-bit window is 48 bytes");
    Window window;
    CHECK_FALSE(window.any());
    CHECK(window.largest() == 0U);
    CHECK(window.check(0U) == ReplayVerdict::Fresh);
    CHECK(window.check(0U) == ReplayVerdict::Fresh); // a check records nothing
    CHECK(window.accept(0U) == ReplayVerdict::Fresh);
    CHECK(window.any());
    CHECK(window.largest() == 0U);
    CHECK(window.accept(0U) == ReplayVerdict::Duplicate);
    constexpr u64 order[] = {5U, 3U, 4U, 1U, 2U};
    for (const u64 number : order)
    {
        CHECK(window.accept(number) == ReplayVerdict::Fresh);
    }
    CHECK(window.largest() == 5U);
    for (u64 number = 0U; number <= 5U; ++number)
    {
        CHECK(window.accept(number) == ReplayVerdict::Duplicate);
    }

    // The span's edges: 64 numbers end at the largest; the one below them is Stale, accepted or not.
    REQUIRE(window.accept(1000U) == ReplayVerdict::Fresh);
    CHECK(window.check(1000U - 63U) == ReplayVerdict::Fresh);
    CHECK(window.accept(1000U - 63U) == ReplayVerdict::Fresh);
    CHECK(window.accept(1000U - 63U) == ReplayVerdict::Duplicate);
    CHECK(window.check(1000U - 64U) == ReplayVerdict::Stale);
    CHECK(window.accept(1000U - 64U) == ReplayVerdict::Stale);
    CHECK(window.accept(5U) == ReplayVerdict::Stale);
    CHECK(window.largest() == 1000U);

    // Fill the span, then move up one word: the ring positions the old numbers held read as new numbers' (never
    // accepted), and the old numbers are Stale.
    for (u64 number = 1000U - 63U; number <= 1000U; ++number)
    {
        static_cast<void>(window.accept(number));
    }
    REQUIRE(window.accept(1064U) == ReplayVerdict::Fresh);
    for (u64 number = 1001U; number < 1064U; ++number)
    {
        CHECK(window.check(number) == ReplayVerdict::Fresh);
    }
    for (u64 number = 1000U - 63U; number <= 1000U; ++number)
    {
        CHECK(window.check(number) == ReplayVerdict::Stale);
    }

    // A jump past the whole ring forgets everything below the new span.
    REQUIRE(window.accept(1064U + 10U * 128U) == ReplayVerdict::Fresh);
    for (u64 number = 1064U + 10U * 128U - 63U; number < 1064U + 10U * 128U; ++number)
    {
        CHECK(window.check(number) == ReplayVerdict::Fresh);
    }
    CHECK(window.check(1064U) == ReplayVerdict::Stale);

    window.clear();
    CHECK_FALSE(window.any());
    CHECK(window.accept(0U) == ReplayVerdict::Fresh);
}

TEST_CASE("Anti-replay: a window covers the whole number range", "[crypto][net-2][replay]")
{
    ReplayWindow<256> window;
    REQUIRE(window.accept(0U) == ReplayVerdict::Fresh);
    REQUIRE(window.accept(kMax) == ReplayVerdict::Fresh); // a jump of 2^64 - 1 clears the ring once
    CHECK(window.largest() == kMax);
    CHECK(window.accept(kMax) == ReplayVerdict::Duplicate);
    CHECK(window.check(0U) == ReplayVerdict::Stale);
    CHECK(window.check(kMax - 191U) == ReplayVerdict::Fresh);
    CHECK(window.check(kMax - 192U) == ReplayVerdict::Stale);
    CHECK(window.accept(kMax - 191U) == ReplayVerdict::Fresh);
    CHECK(window.accept(kMax - 1U) == ReplayVerdict::Fresh);
    CHECK(window.accept(kMax - 191U) == ReplayVerdict::Duplicate);

    // QUIC's largest packet number, reached from its first.
    ReplayWindow<256> quic;
    constexpr u64     last_packet = (u64{1} << 62U) - 1U;
    REQUIRE(quic.accept(last_packet - 1U) == ReplayVerdict::Fresh);
    CHECK(quic.accept(last_packet) == ReplayVerdict::Fresh);
    CHECK(quic.accept(last_packet - 1U) == ReplayVerdict::Duplicate);
    CHECK(quic.largest() == last_packet);
}

TEST_CASE("Anti-replay: the filter keeps each period apart and reuses a slot only after its identifiers expired",
          "[crypto][net-2][replay]")
{
    constexpr u64 period = 100U;
    u8            id[kReplayIdBytes];
    for (usize i = 0U; i < sizeof(id); ++i)
    {
        id[i] = static_cast<u8>(0x31U * i + 7U);
    }
    ReplayFilter filter;
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 1000U, 1000U)); // not ready
    CHECK(filter.init(0U, period, 1000U) == CryptoError::BadLength);
    CHECK(filter.init(16U, 0U, 1000U) == CryptoError::BadLength);
    CHECK(filter.init((usize{1} << 37U) + 1U, period, 1000U) == CryptoError::BadLength);
    CHECK_FALSE(filter.ready());
    REQUIRE(filter.init(64U, period, 1000U) == CryptoError::None); // period 10
    CHECK(filter.ready());
    CHECK(filter.period() == period);
    CHECK(filter.bytes() == 4U * 2U * 64U);
    CHECK(filter.first_use(ConstSpan<u8>(id), 1000U, 1000U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 1050U, 1060U));
    CHECK(filter.first_use(ConstSpan<u8>(id), 999U, 1000U));  // period 9, its own slot
    CHECK(filter.first_use(ConstSpan<u8>(id), 1100U, 1100U)); // period 11
    CHECK(filter.first_use(ConstSpan<u8>(id), 1205U, 1200U)); // period 12, from an issuer whose clock runs ahead
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 1100U, 1150U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 1205U, 1210U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id, 15U), 1210U, 1210U));

    // Period 13 reuses period 9's slot once period 11 begins (prepared two ahead); period 10's slot belongs to
    // period 14 only from period 12 on.
    CHECK(filter.first_use(ConstSpan<u8>(id), 1300U, 1300U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 1301U, 1302U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 999U, 1300U)); // period 9: too old for any slot
    CHECK(filter.first_use(ConstSpan<u8>(id), 1400U, 1400U));

    // A long silence: the slots hold periods long gone and are cleared on first use.
    CHECK(filter.first_use(ConstSpan<u8>(id), 100000U, 100000U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 100001U, 100001U));
    CHECK(filter.first_use(ConstSpan<u8>(id), 99999U, 100001U));

    filter.clear();
    CHECK_FALSE(filter.ready());
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 1000U, 1000U));
    CHECK(filter.init(1U, period, 0U) == CryptoError::None); // from time 0: no period before the first
    CHECK(filter.bytes() == 4U * 64U);
    CHECK(filter.first_use(ConstSpan<u8>(id), 0U, 0U));
    CHECK_FALSE(filter.first_use(ConstSpan<u8>(id), 0U, 5U));
}

TEST_CASE("Anti-replay: the filter's false positives at its capacity", "[crypto][net-2][replay]")
{
    // Each probe is recorded too, so the filter holds 90% of its capacity before the probes and 110% after them.
    constexpr u64   now      = 1793577600U;
    constexpr usize capacity = usize{1} << 18U;
    constexpr usize filled   = capacity / 10U * 9U;
    constexpr usize probes   = capacity / 5U;
    ReplayFilter    filter;
    REQUIRE(filter.init(capacity, 3600U, now) == CryptoError::None);
    CHECK(filter.bytes() == 4U * capacity * 2U);
    u8 id[kReplayIdBytes];
    for (usize i = 0U; i < filled; ++i)
    {
        REQUIRE(random_bytes(Span<u8>(id)) == CryptoError::None);
        static_cast<void>(filter.first_use(ConstSpan<u8>(id), now, now));
    }
    usize false_positives = 0U;
    for (usize i = 0U; i < probes; ++i)
    {
        REQUIRE(random_bytes(Span<u8>(id)) == CryptoError::None);
        if (!filter.first_use(ConstSpan<u8>(id), now, now))
        {
            ++false_positives;
        }
    }
    // A split-block filter of eight one-bit words at 16 bits an item: about 0.1% at capacity (a standard filter of
    // the same size and eight hashes: 0.06%); the bound leaves room for the load passing capacity.
    CAPTURE(false_positives, probes);
    CHECK(false_positives * 1000U < probes * 4U);
}
