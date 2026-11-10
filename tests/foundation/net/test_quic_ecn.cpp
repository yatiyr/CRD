// crd-net -- QUIC's ECN validation (NET-PATH; docs/design/network-foundation.md#ecn): the testing period by packets
// and by time, validation by an acknowledgement, each way it fails (counts missing, falling, above what was sent,
// short of what was acknowledged, every testing packet lost), acknowledgements arriving late left unchecked, CE as a
// signal only on a validated path, ECT(1) for L4S, spaces counted apart and a new path starting again.

#include <crd/net/quic_ecn.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using namespace crd::net;

constexpr u64 kMs = 1000000U;
constexpr u64 kTesting = 300U * kMs;

// An ACK frame with ECN counts, or without when `counts` is false.
QuicAck ack_of(u64 largest, u64 ect0, u64 ect1, u64 ce, bool counts = true)
{
    QuicAck ack;
    ack.largest = largest;
    ack.ecn = counts;
    ack.ect0 = ect0;
    ack.ect1 = ect1;
    ack.ce = ce;
    return ack;
}

// What recovery made of a frame that newly acknowledged `ect0` and `ect1` marked packets.
QuicAckOutcome outcome_of(u64 ect0, u64 ect1 = 0U, bool advanced = true)
{
    QuicAckOutcome outcome;
    outcome.advanced = advanced;
    outcome.acked_packets = ect0 + ect1;
    outcome.ect0_packets = ect0;
    outcome.ect1_packets = ect1;
    return outcome;
}

// A validator that marked `packets` Application packets with `codepoint` at time 0.
QuicEcnValidator sent(u64 packets, Ecn codepoint = Ecn::Ect0)
{
    QuicEcnValidator ecn;
    ecn.start(codepoint);
    for (u64 index = 0U; index < packets; ++index)
    {
        const Ecn mark = ecn.marking(0U, kTesting);
        ecn.on_sent(QuicSpace::Application, mark, 0U);
    }
    return ecn;
}

constexpr QuicSpace kApp = QuicSpace::Application;

} // namespace

TEST_CASE("QUIC ECN: a policy without a codepoint never marks", "[net][net-path][quic-ecn]")
{
    for (const Ecn codepoint : {Ecn::NotEct, Ecn::Ce})
    {
        QuicEcnValidator ecn;
        ecn.start(codepoint);
        CHECK(ecn.state() == QuicEcnState::Off);
        CHECK(ecn.marking(0U, kTesting) == Ecn::NotEct);
        ecn.on_sent(kApp, Ecn::NotEct, 0U);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(0U, 0U, 0U, 1U), outcome_of(0U)));
        CHECK(ecn.state() == QuicEcnState::Off);
    }
}

TEST_CASE("QUIC ECN: testing marks ten packets, then nothing until an acknowledgement validates them",
          "[net][net-path][quic-ecn]")
{
    QuicEcnValidator ecn = sent(kQuicEcnTestingPackets - 1U);
    CHECK(ecn.state() == QuicEcnState::Testing);
    CHECK(ecn.marking(0U, kTesting) == Ecn::Ect0);
    ecn.on_sent(kApp, Ecn::Ect0, 0U);
    CHECK(ecn.state() == QuicEcnState::Unknown);
    CHECK(ecn.marking(0U, kTesting) == Ecn::NotEct);
    ecn.on_sent(kApp, Ecn::NotEct, 0U); // an unmarked packet counts nowhere

    // Three of them acknowledged with their counts: validated, and marking resumes.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(2U, 3U, 0U, 0U), outcome_of(3U)));
    CHECK(ecn.state() == QuicEcnState::Capable);
    CHECK(ecn.failure() == QuicEcnFailure::None);
    CHECK(ecn.marking(0U, kTesting) == Ecn::Ect0);
    CHECK(ecn.reported(kApp).ect0 == 3U);
    // Losses no longer matter once validated.
    for (u64 index = 0U; index < 20U; ++index)
    {
        ecn.on_gone(Ecn::Ect0);
    }
    CHECK(ecn.state() == QuicEcnState::Capable);
}

TEST_CASE("QUIC ECN: the testing period also ends three probe timeouts after the first mark",
          "[net][net-path][quic-ecn]")
{
    QuicEcnValidator ecn;
    ecn.start(Ecn::Ect0);
    CHECK(ecn.marking(10U * kMs, kTesting) == Ecn::Ect0);
    ecn.on_sent(QuicSpace::Initial, Ecn::Ect0, 10U * kMs);
    CHECK(ecn.marking(10U * kMs + kTesting - 1U, kTesting) == Ecn::Ect0);
    CHECK(ecn.marking(10U * kMs + kTesting, kTesting) == Ecn::NotEct);
    CHECK(ecn.state() == QuicEcnState::Unknown);
    // A late validation still counts.
    CHECK_FALSE(ecn.on_ack(QuicSpace::Initial, ack_of(0U, 1U, 0U, 0U), outcome_of(1U)));
    CHECK(ecn.state() == QuicEcnState::Capable);
    // Time before the first mark (a clock that does not rise) ends nothing.
    QuicEcnValidator early;
    early.start(Ecn::Ect0);
    early.on_sent(kApp, Ecn::Ect0, 50U * kMs);
    CHECK(early.marking(40U * kMs, kTesting) == Ecn::Ect0);
}

TEST_CASE("QUIC ECN: marked packets acknowledged without counts fail validation for good", "[net][net-path][quic-ecn]")
{
    QuicEcnValidator ecn = sent(4U);
    // An acknowledgement of unmarked packets alone, without counts, says nothing.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(0U, 0U, 0U, 0U, false), outcome_of(0U)));
    CHECK(ecn.state() == QuicEcnState::Testing);
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(3U, 0U, 0U, 0U, false), outcome_of(2U)));
    CHECK(ecn.state() == QuicEcnState::Failed);
    CHECK(ecn.failure() == QuicEcnFailure::Missing);
    CHECK(ecn.marking(0U, kTesting) == Ecn::NotEct);
    // A valid frame afterwards changes nothing.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(5U, 4U, 0U, 1U), outcome_of(2U)));
    CHECK(ecn.state() == QuicEcnState::Failed);
    CHECK(ecn.failure() == QuicEcnFailure::Missing);
}

TEST_CASE("QUIC ECN: counts that fall, pass what was sent or name a codepoint never sent fail validation",
          "[net][net-path][quic-ecn]")
{
    {
        QuicEcnValidator ecn = sent(6U);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(2U, 3U, 0U, 0U), outcome_of(3U)));
        REQUIRE(ecn.state() == QuicEcnState::Capable);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(4U, 2U, 0U, 0U), outcome_of(0U)));
        CHECK(ecn.failure() == QuicEcnFailure::Decreased);
    }
    {
        QuicEcnValidator ecn = sent(3U);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(2U, 4U, 0U, 0U), outcome_of(3U)));
        CHECK(ecn.failure() == QuicEcnFailure::Exceeded);
    }
    {
        // ECT(1) reported where only ECT(0) was sent: a path that re-marks.
        QuicEcnValidator ecn = sent(3U);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(2U, 0U, 3U, 0U), outcome_of(3U)));
        CHECK(ecn.failure() == QuicEcnFailure::Exceeded);
    }
    {
        // More marks than marked packets, CE included.
        QuicEcnValidator ecn = sent(3U);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(2U, 2U, 0U, 2U), outcome_of(3U)));
        CHECK(ecn.failure() == QuicEcnFailure::Exceeded);
    }
}

TEST_CASE("QUIC ECN: counts that rise by less than the marked packets acknowledged fail validation",
          "[net][net-path][quic-ecn]")
{
    {
        QuicEcnValidator ecn = sent(5U);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(4U, 4U, 0U, 0U), outcome_of(5U)));
        CHECK(ecn.failure() == QuicEcnFailure::Short);
    }
    {
        // A CE mark stands for the ECT(0) packet it was.
        QuicEcnValidator ecn = sent(5U);
        CHECK(ecn.on_ack(kApp, ack_of(4U, 4U, 0U, 1U), outcome_of(5U)));
        CHECK(ecn.state() == QuicEcnState::Capable);
    }
    {
        // ECT(1) packets and their own count (L4S).
        QuicEcnValidator ecn = sent(5U, Ecn::Ect1);
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(4U, 0U, 3U, 0U), outcome_of(0U, 5U)));
        CHECK(ecn.failure() == QuicEcnFailure::Short);
    }
}

TEST_CASE("QUIC ECN: an acknowledgement that does not raise the largest is not checked", "[net][net-path][quic-ecn]")
{
    QuicEcnValidator ecn = sent(8U);
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(7U, 8U, 0U, 0U), outcome_of(8U)));
    REQUIRE(ecn.state() == QuicEcnState::Capable);
    // An older frame arriving late: lower counts and no counts at all, both left alone.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(3U, 4U, 0U, 0U), outcome_of(0U, 0U, false)));
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(3U, 0U, 0U, 0U, false), outcome_of(2U, 0U, false)));
    CHECK(ecn.state() == QuicEcnState::Capable);
    CHECK(ecn.reported(kApp).ect0 == 8U);
}

TEST_CASE("QUIC ECN: a rising CE count is a congestion signal on a validated path only", "[net][net-path][quic-ecn]")
{
    QuicEcnValidator ecn = sent(20U);
    // The frame that validates may carry the first CE.
    CHECK(ecn.on_ack(kApp, ack_of(1U, 1U, 0U, 1U), outcome_of(2U)));
    CHECK(ecn.state() == QuicEcnState::Capable);
    for (u64 index = 0U; index < 18U; ++index)
    {
        ecn.on_sent(kApp, ecn.marking(0U, kTesting), 0U);
    }
    // No new CE: no signal. A rise: one signal per frame however many.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(5U, 4U, 0U, 1U), outcome_of(3U)));
    CHECK(ecn.on_ack(kApp, ack_of(9U, 5U, 0U, 4U), outcome_of(4U)));
    CHECK(ecn.reported(kApp).ce == 4U);
    // A frame that fails validation is no signal even with CE.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(12U, 5U, 0U, 5U), outcome_of(3U)));
    CHECK(ecn.failure() == QuicEcnFailure::Short);
    // And a frame of an unvalidated path that fails carries none either.
    QuicEcnValidator testing = sent(4U);
    CHECK_FALSE(testing.on_ack(kApp, ack_of(1U, 0U, 1U, 1U), outcome_of(2U)));
    CHECK(testing.state() == QuicEcnState::Failed);
}

TEST_CASE("QUIC ECN: losing every packet marked before validation fails it", "[net][net-path][quic-ecn]")
{
    {
        // Lost during testing, then testing ends: the path drops marked packets.
        QuicEcnValidator ecn = sent(3U);
        for (u64 index = 0U; index < 3U; ++index)
        {
            ecn.on_gone(Ecn::Ect0);
        }
        CHECK(ecn.state() == QuicEcnState::Testing);
        CHECK(ecn.marking(kTesting, kTesting) == Ecn::NotEct);
        CHECK(ecn.state() == QuicEcnState::Failed);
        CHECK(ecn.failure() == QuicEcnFailure::Lost);
    }
    {
        // Testing ended by its ten packets, then the last of them goes.
        QuicEcnValidator ecn = sent(kQuicEcnTestingPackets);
        REQUIRE(ecn.state() == QuicEcnState::Unknown);
        for (u64 index = 0U; index + 1U < kQuicEcnTestingPackets; ++index)
        {
            ecn.on_gone(Ecn::Ect0);
        }
        ecn.on_gone(Ecn::NotEct); // an unmarked loss counts nowhere
        CHECK(ecn.state() == QuicEcnState::Unknown);
        ecn.on_gone(Ecn::Ect0);
        CHECK(ecn.failure() == QuicEcnFailure::Lost);
    }
    {
        // One of them acknowledged with its count is enough.
        QuicEcnValidator ecn = sent(kQuicEcnTestingPackets);
        for (u64 index = 0U; index + 1U < kQuicEcnTestingPackets; ++index)
        {
            ecn.on_gone(Ecn::Ect0);
        }
        CHECK_FALSE(ecn.on_ack(kApp, ack_of(9U, 1U, 0U, 0U), outcome_of(1U)));
        CHECK(ecn.state() == QuicEcnState::Capable);
    }
}

TEST_CASE("QUIC ECN: each space's counts stand apart, and a new path starts again", "[net][net-path][quic-ecn]")
{
    QuicEcnValidator ecn;
    ecn.start(Ecn::Ect0);
    ecn.on_sent(QuicSpace::Initial, ecn.marking(0U, kTesting), 0U);
    ecn.on_sent(QuicSpace::Handshake, ecn.marking(0U, kTesting), 0U);
    ecn.on_sent(kApp, ecn.marking(0U, kTesting), 0U);
    // The Handshake space reports its one packet; the Application space's own count starts at zero.
    CHECK_FALSE(ecn.on_ack(QuicSpace::Handshake, ack_of(0U, 1U, 0U, 0U), outcome_of(1U)));
    CHECK(ecn.state() == QuicEcnState::Capable);
    CHECK(ecn.reported(QuicSpace::Handshake).ect0 == 1U);
    CHECK(ecn.reported(kApp).ect0 == 0U);
    // Two in the Application space where it sent one: above what that space sent.
    CHECK_FALSE(ecn.on_ack(kApp, ack_of(0U, 2U, 0U, 0U), outcome_of(1U)));
    CHECK(ecn.failure() == QuicEcnFailure::Exceeded);

    ecn.start(Ecn::Ect1);
    CHECK(ecn.state() == QuicEcnState::Testing);
    CHECK(ecn.failure() == QuicEcnFailure::None);
    CHECK(ecn.marking(0U, kTesting) == Ecn::Ect1);
    CHECK(ecn.reported(QuicSpace::Handshake).ect0 == 0U);
}
