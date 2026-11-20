// crd-net -- a QUIC connection's path table (NET-PATH; docs/design/network-foundation.md#migration): admission into
// free slots and the least recently used spare, the fallback, the amplification limit per path, validation with its
// resends doubling and its deadline, responses matched on any path to the live challenges alone, an unexpanded
// challenge validating the address but not the path, and the responses owed per path.

#include <crd/net/quic_path.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using namespace crd::net;

constexpr u64 kMs = 1000000U;

QuicPath at(u8 last, crd::u16 port, crd::u32 socket = 0U)
{
    return QuicPath{socket, Endpoint{IpAddress::v4(192, 0, 2, last), port}};
}

TEST_CASE("QUIC paths: a port alone changed is a rebinding, any other change is not", "[net][net-path][quic-path]")
{
    CHECK(quic_port_only_change(at(1, 1000), at(1, 1001)));
    CHECK_FALSE(quic_port_only_change(at(1, 1000), at(1, 1000)));
    CHECK_FALSE(quic_port_only_change(at(1, 1000), at(2, 1000)));
    CHECK_FALSE(quic_port_only_change(at(1, 1000), at(2, 1001)));
    CHECK_FALSE(quic_port_only_change(at(1, 1000), at(1, 1001, 1U))); // another socket: a new local address
}

TEST_CASE("QUIC paths: admission takes free slots, a full table names its least recently used spare, the fallback "
          "is the last validated active path",
          "[net][net-path][quic-path]")
{
    QuicPaths paths;
    paths.open(at(1, 1000), true, 0U);
    CHECK(paths.active() == 0U);
    CHECK(paths.fallback() == kQuicNoPath);
    CHECK(paths.find(at(1, 1000)) == 0U);
    CHECK(paths.find(at(1, 1001)) == kQuicNoPath);
    const usize b = paths.admit(at(2, 1000), 10U);
    const usize c = paths.admit(at(3, 1000), 20U);
    const usize d = paths.admit(at(4, 1000), 30U);
    CHECK(paths.admit(at(2, 1000), 40U) == b); // known: the same slot
    CHECK(paths.admit(at(5, 1000), 40U) == kQuicNoPath);
    CHECK(paths.least_recent() == b);
    paths.on_received(b, 100U, 50U);
    CHECK(paths.least_recent() == c);

    // An unvalidated active path leaves no fallback; a validated one does, and never the path now active.
    paths.activate(c);
    CHECK(paths.active() == c);
    CHECK(paths.fallback() == 0U);
    CHECK(paths.least_recent() == d); // neither active nor the fallback
    paths.activate(d);
    CHECK(paths.fallback() == 0U); // c was never validated
    paths.activate(0U);
    CHECK(paths.fallback() == kQuicNoPath);
    paths.set_validated(d);
    paths.activate(d);
    CHECK(paths.fallback() == 0U);
    paths.drop(0U);
    CHECK(paths.fallback() == kQuicNoPath);
    CHECK_FALSE(paths.used(0U));
    paths.drop(d); // the active path stays
    CHECK(paths.used(d));
    CHECK(paths.admit(at(5, 1000), 60U) == 0U);
}

TEST_CASE("QUIC paths: an unvalidated path carries three times what arrived on it, a validated one anything",
          "[net][net-path][quic-path]")
{
    QuicPaths paths;
    paths.open(at(1, 1000), false, 0U);
    CHECK(paths.credit(0U) == 0U);
    paths.on_received(0U, 1200U, 1U);
    CHECK(paths.credit(0U) == 3600U);
    paths.on_sent(0U, 3000U);
    CHECK(paths.credit(0U) == 600U);
    paths.on_sent(0U, 1000U);
    CHECK(paths.credit(0U) == 0U);
    paths.set_validated(0U);
    CHECK(paths.credit(0U) == ~usize{0});
}

TEST_CASE("QUIC paths: challenges are due at once, again after the probe period doubling, until the deadline",
          "[net][net-path][quic-path]")
{
    QuicPaths paths;
    paths.open(at(1, 1000), true, 0U);
    const usize b = paths.admit(at(2, 1000), 0U);
    CHECK_FALSE(paths.validating(b));
    CHECK_FALSE(paths.challenge_due(b, 0U));
    paths.validate(b, 100U * kMs, 3000U * kMs);
    CHECK(paths.validating(b));
    CHECK(paths.deadline_ns(b) == 3100U * kMs);
    CHECK(paths.challenge_due(b, 100U * kMs));
    paths.validate(b, 200U * kMs, 9000U * kMs); // underway: unchanged
    CHECK(paths.deadline_ns(b) == 3100U * kMs);
    const u8 first[kQuicPathDataBytes] = {1U};
    paths.challenge_sent(b, ConstSpan<u8>(first), true, 100U * kMs, 300U * kMs);
    CHECK_FALSE(paths.challenge_due(b, 399U * kMs));
    CHECK(paths.due_ns(b) == 400U * kMs);
    const u8 second[kQuicPathDataBytes] = {2U};
    paths.challenge_sent(b, ConstSpan<u8>(second), true, 400U * kMs, 300U * kMs);
    CHECK(paths.due_ns(b) == 1000U * kMs); // twice the period
    const u8 third[kQuicPathDataBytes] = {3U};
    paths.challenge_sent(b, ConstSpan<u8>(third), true, 1000U * kMs, 300U * kMs);
    CHECK(paths.due_ns(b) == 2200U * kMs);
    CHECK(paths.expired(3099U * kMs) == kQuicNoPath);
    CHECK(paths.expired(3100U * kMs) == b);
    CHECK_FALSE(paths.validating(b));
    CHECK(paths.used(b)); // abandoned, still known: the connection decides
    CHECK(paths.expired(3100U * kMs) == kQuicNoPath);
    // A late response to an abandoned validation validates nothing.
    usize index = 0U;
    CHECK(paths.on_response(ConstSpan<u8>(third), index) == QuicPathResponse::Ignored);
    CHECK(index == kQuicNoPath);
    CHECK_FALSE(paths.validated(b));
}

TEST_CASE("QUIC paths: a response to any of a path's last challenges validates it from whatever path it came, others "
          "are ignored",
          "[net][net-path][quic-path]")
{
    QuicPaths paths;
    paths.open(at(1, 1000), true, 0U);
    const usize b = paths.admit(at(2, 1000), 0U);
    const usize c = paths.admit(at(3, 1000), 0U);
    paths.validate(b, 0U, 3000U * kMs);
    paths.validate(c, 0U, 3000U * kMs);
    u8 data[6][kQuicPathDataBytes] = {};
    for (usize sent = 0U; sent < 6U; ++sent)
    {
        data[sent][0] = static_cast<u8>(0x10U + sent);
        paths.challenge_sent(b, ConstSpan<u8>(data[sent]), true, sent, 1U);
    }
    const u8 c_data[kQuicPathDataBytes] = {0x77U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    paths.challenge_sent(c, ConstSpan<u8>(c_data), true, 0U, 1U);
    usize index = 0U;
    // Only the last kQuicPathChallengesKept of b's are held: the first two are too old.
    CHECK(paths.on_response(ConstSpan<u8>(data[0]), index) == QuicPathResponse::Ignored);
    CHECK(paths.on_response(ConstSpan<u8>(data[1]), index) == QuicPathResponse::Ignored);
    const u8 unknown[kQuicPathDataBytes] = {0xEEU};
    CHECK(paths.on_response(ConstSpan<u8>(unknown), index) == QuicPathResponse::Ignored);
    CHECK(paths.on_response(ConstSpan<u8>(unknown, 4U), index) == QuicPathResponse::Ignored);
    CHECK(paths.on_response(ConstSpan<u8>(c_data), index) == QuicPathResponse::Validated);
    CHECK(index == c);
    CHECK(paths.validated(c));
    CHECK_FALSE(paths.validating(c));
    CHECK(paths.on_response(ConstSpan<u8>(data[3]), index) == QuicPathResponse::Validated);
    CHECK(index == b);
    CHECK(paths.validated(b));
    CHECK(paths.on_response(ConstSpan<u8>(data[5]), index) == QuicPathResponse::Ignored); // answered already
    CHECK(paths.due_ns(b) == crd::net::kNeverNs);
}

TEST_CASE("QUIC paths: an answered challenge under 1200 bytes proves the address and asks for an expanded one",
          "[net][net-path][quic-path]")
{
    QuicPaths paths;
    paths.open(at(1, 1000), true, 0U);
    const usize b = paths.admit(at(2, 1000), 0U);
    paths.validate(b, 0U, 3000U * kMs);
    const u8 small[kQuicPathDataBytes] = {1U};
    paths.challenge_sent(b, ConstSpan<u8>(small), false, 0U, 500U * kMs);
    usize index = 0U;
    REQUIRE(paths.on_response(ConstSpan<u8>(small), index) == QuicPathResponse::Validated);
    CHECK(paths.validated(b)); // the address: the limit is lifted
    CHECK(paths.validating(b));
    CHECK(paths.challenge_due(b, 10U * kMs)); // an expanded challenge goes now
    const u8 expanded[kQuicPathDataBytes] = {2U};
    paths.challenge_sent(b, ConstSpan<u8>(expanded), true, 10U * kMs, 500U * kMs);
    REQUIRE(paths.on_response(ConstSpan<u8>(expanded), index) == QuicPathResponse::Validated);
    CHECK_FALSE(paths.validating(b));
    // A path whose expanded challenge is never answered fails as any.
    const usize c = paths.admit(at(3, 1000), 0U);
    paths.validate(c, 0U, 3000U * kMs);
    paths.challenge_sent(c, ConstSpan<u8>(small), false, 0U, 500U * kMs);
    REQUIRE(paths.on_response(ConstSpan<u8>(small), index) == QuicPathResponse::Validated);
    CHECK(paths.expired(3000U * kMs) == c);
}

TEST_CASE("QUIC paths: the latest challenge received on a path is owed there once", "[net][net-path][quic-path]")
{
    QuicPaths paths;
    paths.open(at(1, 1000), true, 0U);
    const usize b = paths.admit(at(2, 1000), 0U);
    CHECK_FALSE(paths.response_owed(0U));
    CHECK_FALSE(paths.response_owed(b));
    const u8 one[kQuicPathDataBytes] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    const u8 two[kQuicPathDataBytes] = {9U};
    paths.on_challenge(b, ConstSpan<u8>(one));
    paths.on_challenge(b, ConstSpan<u8>(two));
    CHECK_FALSE(paths.response_owed(0U));
    REQUIRE(paths.response_owed(b));
    CHECK(paths.response(b)[0] == 9U);
    paths.response_sent(b);
    CHECK_FALSE(paths.response_owed(b));
    paths.set_peer_sequence(b, 3U);
    paths.set_local_sequence(b, 2U);
    CHECK(paths.peer_sequence(b) == 3U);
    CHECK(paths.local_sequence(b) == 2U);
    paths.drop(b);
    CHECK(paths.find(at(2, 1000)) == kQuicNoPath);
    CHECK_FALSE(paths.response_owed(b));
}

} // namespace
