// crd-net -- a QUIC connection's connection IDs (NET-PATH; docs/design/network-foundation.md#multipath): one sequence
// space per path ID both ways (RFC 10069 section 3.2): spaces opened, refused and closed; the peer's IDs held per
// space under its own limit and Retire Prior To, unique across every space, retired with a bound and never held again
// once retired here; this side's IDs issued, topped up, rotated and retired per space; the datagram's ID naming its
// space; stateless reset tokens watched while used.

#include <crd/net/quic_connection_ids.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::net;
namespace cont = crd::containers;

constexpr usize kToken = crd::crypto::kQuicResetTokenBytes;
constexpr u64 kLimit = 4U;

// IDs of eight bytes numbered from 1 (byte 0 the number, byte 7 0xA0), tokens alike; every call counted.
class Host final : public QuicConnectionIds
{
public:
    bool issue(cont::Span<u8> id, cont::Span<u8> token) noexcept override
    {
        if (refuse)
        {
            return false;
        }
        ++issued;
        std::memset(id.data(), 0, id.size());
        id[0] = issued;
        id[id.size() - 1U] = 0xA0U;
        std::memset(token.data(), 0, token.size());
        token[0] = issued;
        token[kToken - 1U] = 0xA0U;
        ++routed;
        return true;
    }
    bool token(cont::ConstSpan<u8>, cont::Span<u8> token) noexcept override
    {
        std::memset(token.data(), 0xEEU, token.size());
        return true;
    }
    void retire(cont::ConstSpan<u8>) noexcept override
    {
        ++retired;
        --routed;
    }
    void watch_reset(cont::ConstSpan<u8> token) noexcept override
    {
        ++watched;
        last_watched = token[0];
    }
    void unwatch_reset(cont::ConstSpan<u8> token) noexcept override
    {
        ++unwatched;
        last_unwatched = token[0];
    }

    bool refuse = false;
    u8 issued = 0U;
    int routed = 0;
    int retired = 0;
    int watched = 0;
    int unwatched = 0;
    u8 last_watched = 0U;
    u8 last_unwatched = 0U;
};

// A peer's ID and token named by `n` (distinct from the host's: their last byte is 0xB0).
struct PeerId
{
    explicit PeerId(u8 n)
    {
        id[0] = n;
        id[7] = 0xB0U;
        token[0] = n;
        token[kToken - 1U] = 0xB0U;
    }
    u8 id[8] = {};
    u8 token[kToken] = {};
};

QuicFrame new_id(u64 path_id, u64 sequence, const PeerId& peer, u64 retire_prior_to = 0U)
{
    QuicFrame frame;
    frame.kind = QuicFrameKind::PathNewConnectionId;
    frame.path_id = path_id;
    frame.sequence = sequence;
    frame.retire_prior_to = retire_prior_to;
    frame.data = cont::ConstSpan<u8>(peer.id, sizeof(peer.id));
    frame.reset_token = cont::ConstSpan<u8>(peer.token, kToken);
    return frame;
}

QuicTransportError add(QuicCids& cids, const QuicFrame& frame, bool* added = nullptr)
{
    bool got = false;
    const QuicTransportError error = cids.add_peer(frame, kLimit, got);
    if (added != nullptr)
    {
        *added = got;
    }
    return error;
}

// A connection's IDs after its handshake: this side's sequence 0 (8 bytes) and the peer's.
struct Started
{
    Started()
    {
        const u8 local[8] = {0x10U, 0, 0, 0, 0, 0, 0, 0xA0U};
        cids.first_local(cont::ConstSpan<u8>(local, sizeof(local)), {});
        const PeerId first(0x20U);
        cids.first_peer(cont::ConstSpan<u8>(first.id, sizeof(first.id)));
    }
    Host host;
    QuicCids cids{&host};
    QuicCidSpace spaces[kQuicMaxPaths];
};

u64 owed_count(const QuicCids& cids)
{
    u64 path = 0U;
    const QuicLocalCid* owed = cids.next_owed(path);
    return owed == nullptr ? 0U : 1U;
}

} // namespace

TEST_CASE("connection ID spaces open, refuse and close per path ID", "[quic][cids][multipath]")
{
    Started s;
    CHECK(s.cids.is_open(0U));
    CHECK(s.cids.consumed(0U));
    CHECK_FALSE(s.cids.consumed(1U));
    CHECK_FALSE(s.cids.open(0U, s.spaces[0]));                // open already
    CHECK_FALSE(s.cids.open(u64{0x100000000U}, s.spaces[0])); // past 2^32 - 1
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    REQUIRE(s.cids.open(2U, s.spaces[2]));
    REQUIRE(s.cids.open(4U, s.spaces[0]));     // path ID 3 passed over
    CHECK_FALSE(s.cids.open(5U, s.spaces[3])); // kQuicMaxPaths open
    CHECK_FALSE(s.cids.consumed(5U));
    // The open ones in rising order, whatever storage holds them.
    CHECK(s.cids.open_after(0U) == 1U);
    CHECK(s.cids.open_after(1U) == 2U);
    CHECK(s.cids.open_after(2U) == 4U);
    CHECK(s.cids.open_after(4U) == kQuicNoSequence);
    // A path ID opens once and in rising order (RFC 10069 sections 2.4 and 3.4): closed, it never opens again, nor
    // does one passed over.
    REQUIRE(s.cids.close(4U) == &s.spaces[0]);
    CHECK(s.cids.consumed(4U));
    CHECK(s.cids.consumed(3U));
    CHECK_FALSE(s.cids.open(4U, s.spaces[0]));
    CHECK_FALSE(s.cids.open(3U, s.spaces[0]));
    // A space numbers this side's IDs from 0; path 0's started at 1 after the handshake's.
    REQUIRE(s.cids.issue(1U));
    REQUIRE(s.cids.issue(0U));
    CHECK(s.cids.local(1U, 0U) != nullptr);
    CHECK(s.cids.local(0U, 1U) != nullptr);
    CHECK(s.cids.local(1U, 1U) == nullptr);
    CHECK(s.host.routed == 2);
    // Closing unroutes the space's own IDs and stops its peer tokens counting; its storage comes back.
    REQUIRE(add(s.cids, new_id(1U, 0U, PeerId(1U))) == QuicTransportError::NoError);
    s.cids.use(1U, 0U);
    CHECK(s.host.watched == 1);
    QuicCidSpace* closed = s.cids.close(1U);
    CHECK(closed == &s.spaces[1]);
    CHECK_FALSE(s.cids.is_open(1U));
    CHECK(s.host.routed == 1);
    CHECK(s.host.unwatched == 1);
    CHECK(s.cids.close(1U) == nullptr);
    CHECK(s.cids.local_count(1U) == 0U);
    CHECK(s.cids.peer_count(1U) == 0U);
    // Frames for a closed space are ignored (RFC 10069 section 4).
    bool added = true;
    CHECK(add(s.cids, new_id(1U, 1U, PeerId(2U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK(s.cids.retire_local(1U, 0U, QuicCidRef{}) == QuicTransportError::NoError);
    CHECK_FALSE(s.cids.issue(1U));
    CHECK_FALSE(s.cids.open(1U, *closed));
    // The storage opens again under another path ID, empty.
    REQUIRE(s.cids.open(7U, *closed));
    CHECK(s.cids.peer_count(7U) == 0U);
    CHECK(s.cids.local_count(7U) == 0U);
    REQUIRE(s.cids.issue(7U));
    CHECK(s.cids.local(7U, 0U) != nullptr);
    // Path ID 0 closes too, and its inline storage serves another path ID.
    QuicCidSpace* first = s.cids.close(0U);
    REQUIRE(first != nullptr);
    CHECK(s.cids.current(0U) == kQuicNoSequence);
    REQUIRE(s.cids.open(0xFFFFFFFFU, *first)); // the largest path ID, after which none opens
    CHECK(s.cids.is_open(0xFFFFFFFFU));
    CHECK(s.cids.open_after(0U) == 2U); // path ID 0 is closed
    CHECK(s.cids.open_after(7U) == 0xFFFFFFFFU);
    CHECK(s.cids.open_after(0xFFFFFFFFU) == kQuicNoSequence);
    CHECK(s.cids.consumed(8U));
    REQUIRE(s.cids.close(7U) != nullptr);
    CHECK_FALSE(s.cids.open(8U, *closed));
}

TEST_CASE("abandoning a path ID retires the peer's IDs at once and keeps this side's until it closes",
          "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    REQUIRE(s.cids.open(2U, s.spaces[2]));
    s.cids.top_up(2U);
    REQUIRE(add(s.cids, new_id(1U, 0U, PeerId(1U))) == QuicTransportError::NoError);
    REQUIRE(add(s.cids, new_id(1U, 1U, PeerId(2U))) == QuicTransportError::NoError);
    REQUIRE(add(s.cids, new_id(2U, 0U, PeerId(3U))) == QuicTransportError::NoError);
    s.cids.use(1U, 0U);
    REQUIRE(s.cids.retire_peer(1U, 1U, kLimit)); // a retirement owed there
    u64 path = 0U;
    u64 sequence = 0U;
    REQUIRE(s.cids.next_retirement(path, sequence));
    REQUIRE(path == 1U);
    const QuicLocalCid* kept = s.cids.local(1U, 0U);
    REQUIRE(kept != nullptr);
    const QuicConnectionId straggler = kept->id; // the peer's packets in transit still name it
    const int routed = s.host.routed;
    const int unwatched = s.host.unwatched;

    REQUIRE(s.cids.abandon(1U));
    CHECK(s.cids.abandoned(1U));
    CHECK_FALSE(s.cids.abandoned(2U));
    CHECK_FALSE(s.cids.abandon(1U)); // once
    CHECK_FALSE(s.cids.abandon(5U)); // not open
    // The peer's IDs are retired at once: none held or sent to, the used one's token no longer counts, and no
    // retirement frame is owed for them, the PATH_ABANDON frame retiring them all (RFC 10069 section 3.4).
    CHECK(s.cids.peer_count(1U) == 0U);
    CHECK(s.cids.current(1U) == kQuicNoSequence);
    CHECK(s.host.unwatched == unwatched + 1);
    CHECK_FALSE(s.cids.reset_by(cont::ConstSpan<u8>(PeerId(1U).token, kToken)));
    CHECK_FALSE(s.cids.next_retirement(path, sequence));
    // This side's stay routed and found, so a straggler opens and draws no stateless reset (section 3.4.2), but none
    // is issued, rotated or announced there again.
    CHECK(s.host.routed == routed);
    const QuicCidRef found = s.cids.find(straggler.view());
    CHECK(found.found());
    CHECK(found.path_id == 1U);
    for (u64 owed = 0U; owed < 2U; ++owed)
    {
        s.cids.local_settled(1U, owed, false); // a lost frame is not owed again
    }
    s.cids.reannounce();
    const QuicLocalCid* next = nullptr;
    while ((next = s.cids.next_owed(path)) != nullptr)
    {
        CHECK(path != 1U);
        s.cids.local_sent(path, next->sequence);
    }
    const usize before = s.cids.local_count(1U);
    CHECK_FALSE(s.cids.issue(1U));
    CHECK_FALSE(s.cids.rotate(1U, 2U));
    s.cids.top_up(4U);
    CHECK(s.cids.local_count(1U) == before);
    CHECK(s.cids.local_count(2U) == 4U);
    // The peer's frames for the path ID are ignored from now on (section 4), a repeat of a retired ID's too.
    bool added = true;
    CHECK(add(s.cids, new_id(1U, 2U, PeerId(4U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK(add(s.cids, new_id(1U, 0U, PeerId(1U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK(s.cids.peer_count(1U) == 0U);
    CHECK(s.cids.retire_local(1U, 0U, QuicCidRef{}) == QuicTransportError::NoError);
    CHECK(s.cids.local_count(1U) == before);
    // The other path IDs are untouched.
    CHECK(s.cids.peer_count(2U) == 1U);
    CHECK(s.cids.current(2U) == 0U);
    // Retention over (three PTOs): closing unroutes this side's IDs, and the straggler finds nothing.
    const int open_routes = s.host.routed;
    REQUIRE(s.cids.close(1U) == &s.spaces[1]);
    CHECK(s.host.routed == open_routes - static_cast<int>(before));
    CHECK_FALSE(s.cids.find(straggler.view()).found());
    CHECK_FALSE(s.cids.abandoned(1U));
    // Its storage opens again under a new path ID, not abandoned.
    REQUIRE(s.cids.open(3U, s.spaces[1]));
    CHECK_FALSE(s.cids.abandoned(3U));
    CHECK(s.cids.issue(3U));
}

TEST_CASE("the datagram's connection ID names its path ID's space", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    REQUIRE(s.cids.open(2U, s.spaces[2]));
    s.cids.top_up(2U);
    CHECK(s.cids.local_count(0U) == 2U);
    CHECK(s.cids.local_count(1U) == 2U);
    CHECK(s.cids.local_count(2U) == 2U);
    for (u64 path = 0U; path < 3U; ++path)
    {
        for (u64 sequence = 0U; sequence < 2U; ++sequence)
        {
            const QuicLocalCid* id = s.cids.local(path, sequence);
            REQUIRE(id != nullptr);
            const QuicCidRef found = s.cids.find(id->id.view());
            CHECK(found.found());
            CHECK(found.path_id == path);
            CHECK(found.sequence == sequence);
        }
    }
    const u8 unknown[8] = {0x77U};
    CHECK_FALSE(s.cids.find(cont::ConstSpan<u8>(unknown, sizeof(unknown))).found());
}

TEST_CASE("the peer's IDs are held per path ID under one connection-wide uniqueness", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    // Each space numbers its IDs from 0 and holds up to the limit on its own.
    for (u8 n = 0U; n < kLimit; ++n)
    {
        bool added = false;
        REQUIRE(add(s.cids, new_id(1U, n, PeerId(static_cast<u8>(0x40U + n))), &added) == QuicTransportError::NoError);
        CHECK(added);
    }
    CHECK(s.cids.peer_count(1U) == kLimit);
    CHECK(s.cids.peer_count(0U) == 1U);
    CHECK(s.cids.current(1U) == 0U);
    CHECK(add(s.cids, new_id(1U, 4U, PeerId(0x50U))) == QuicTransportError::ConnectionIdLimitError);
    REQUIRE(add(s.cids, new_id(0U, 1U, PeerId(0x51U))) == QuicTransportError::NoError); // path 0 has room
    // A repeat changes nothing, another ID or token under a held sequence is PROTOCOL_VIOLATION.
    bool added = true;
    CHECK(add(s.cids, new_id(1U, 2U, PeerId(0x42U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK(add(s.cids, new_id(1U, 2U, PeerId(0x60U))) == QuicTransportError::ProtocolViolation);
    const PeerId repeated(0x42U); // a frame's spans point into it
    QuicFrame forged_token = new_id(1U, 2U, repeated);
    const PeerId other(0x61U);
    forged_token.reset_token = cont::ConstSpan<u8>(other.token, kToken);
    CHECK(add(s.cids, forged_token) == QuicTransportError::ProtocolViolation);
    // One ID or token under two path IDs or sequences is PROTOCOL_VIOLATION (RFC 9000 section 10.3.2).
    CHECK(add(s.cids, new_id(0U, 2U, PeerId(0x41U))) == QuicTransportError::ProtocolViolation);
    const PeerId fresh(0x62U);
    QuicFrame shared_token = new_id(0U, 2U, fresh);
    const PeerId holder(0x43U);
    shared_token.reset_token = cont::ConstSpan<u8>(holder.token, kToken);
    CHECK(add(s.cids, shared_token) == QuicTransportError::ProtocolViolation);
    // NEW_CONNECTION_ID is path ID 0's whatever its path field (RFC 10069 section 4.4).
    const PeerId unpathed(0x63U);
    QuicFrame plain = new_id(1U, 2U, unpathed);
    plain.kind = QuicFrameKind::NewConnectionId;
    REQUIRE(add(s.cids, plain, &added) == QuicTransportError::NoError);
    CHECK(added);
    CHECK(s.cids.peer(0U, 2U) != nullptr);
    CHECK(s.cids.peer(1U, 2U)->id.id[0] == 0x42U);
}

TEST_CASE("Retire Prior To retires within its own path ID", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    REQUIRE(add(s.cids, new_id(1U, 0U, PeerId(0x40U))) == QuicTransportError::NoError);
    REQUIRE(add(s.cids, new_id(1U, 1U, PeerId(0x41U))) == QuicTransportError::NoError);
    REQUIRE(add(s.cids, new_id(0U, 1U, PeerId(0x51U))) == QuicTransportError::NoError);
    s.cids.use(1U, 0U);
    // Sequence 2 retires 0 and 1 of path 1 alone; the one sent to is replaced by the frame's.
    bool added = false;
    REQUIRE(add(s.cids, new_id(1U, 2U, PeerId(0x42U), 2U), &added) == QuicTransportError::NoError);
    CHECK(added);
    CHECK(s.cids.peer_count(1U) == 1U);
    CHECK(s.cids.current(1U) == 2U);
    CHECK(s.cids.peer_count(0U) == 2U);
    CHECK(s.host.unwatched == 1);
    CHECK(s.host.last_unwatched == 0x40U);
    // Both retirements are owed in path 1's space.
    u64 path = 9U;
    u64 sequence = 9U;
    REQUIRE(s.cids.next_retirement(path, sequence));
    CHECK(path == 1U);
    s.cids.retirement_sent(path, sequence);
    REQUIRE(s.cids.next_retirement(path, sequence));
    CHECK(path == 1U);
    s.cids.retirement_sent(path, sequence);
    CHECK_FALSE(s.cids.retirement_owed());
    // A frame below Retire Prior To is retired again, once while in flight; then held never.
    REQUIRE(add(s.cids, new_id(1U, 1U, PeerId(0x41U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK_FALSE(s.cids.retirement_owed());
    s.cids.retirement_settled(1U, 1U, true);
    REQUIRE(add(s.cids, new_id(1U, 1U, PeerId(0x41U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    REQUIRE(s.cids.next_retirement(path, sequence));
    CHECK(path == 1U);
    CHECK(sequence == 1U);
    // A lost retirement is owed again; an acknowledged one is done.
    s.cids.retirement_sent(1U, 1U);
    s.cids.retirement_settled(1U, 1U, false);
    CHECK(s.cids.retirement_owed());
    s.cids.retirement_sent(1U, 1U);
    s.cids.retirement_settled(1U, 1U, true);
    s.cids.retirement_settled(1U, 0U, true);
    CHECK_FALSE(s.cids.retirement_owed());
    // Path 0's Retire Prior To is untouched.
    CHECK(s.cids.peer(0U, 0U) != nullptr);
}

TEST_CASE("an ID retired here is not held again when its frame repeats", "[quic][cids]")
{
    Started s;
    REQUIRE(add(s.cids, new_id(0U, 1U, PeerId(0x51U))) == QuicTransportError::NoError);
    REQUIRE(add(s.cids, new_id(0U, 2U, PeerId(0x52U))) == QuicTransportError::NoError);
    s.cids.use(0U, 1U);
    REQUIRE(s.cids.retire_peer(0U, 1U, kLimit));
    CHECK(s.cids.peer(0U, 1U) == nullptr);
    CHECK(s.host.unwatched == 1);
    CHECK(s.cids.retire_peer(0U, 1U, kLimit)); // not held: nothing more owed
    // The peer's retransmission of sequence 1, sent before it read the retirement, is ignored.
    bool added = true;
    CHECK(add(s.cids, new_id(0U, 1U, PeerId(0x51U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK(s.cids.peer(0U, 1U) == nullptr);
    u64 path = 0U;
    u64 sequence = 0U;
    REQUIRE(s.cids.next_retirement(path, sequence));
    CHECK(sequence == 1U);
    s.cids.retirement_sent(0U, 1U);
    s.cids.retirement_settled(0U, 1U, true);
    CHECK(add(s.cids, new_id(0U, 1U, PeerId(0x51U)), &added) == QuicTransportError::NoError);
    CHECK_FALSE(added);
    CHECK_FALSE(s.cids.retirement_owed());
}

TEST_CASE("retirements owed and in flight stay within twice the limit", "[quic][cids]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    // A peer that raises Retire Prior To on every frame makes this side owe one retirement each time.
    u64 failed = 0U;
    QuicTransportError error = QuicTransportError::NoError;
    for (u64 n = 0U; n < 3U * kLimit; ++n)
    {
        error = add(s.cids, new_id(1U, n, PeerId(static_cast<u8>(0x40U + n)), n));
        if (error != QuicTransportError::NoError)
        {
            failed = n;
            break;
        }
    }
    CHECK(error == QuicTransportError::ConnectionIdLimitError);
    CHECK(failed == 2U * kLimit + 1U); // its retirement is the one past twice the limit
    // Settled retirements free room: the same peer behaviour then goes on.
    Started t;
    REQUIRE(t.cids.open(1U, t.spaces[1]));
    for (u64 n = 0U; n < 6U * kLimit; ++n)
    {
        REQUIRE(add(t.cids, new_id(1U, n, PeerId(static_cast<u8>(0x40U + n)), n)) == QuicTransportError::NoError);
        u64 path = 0U;
        u64 owed = 0U;
        while (t.cids.next_retirement(path, owed))
        {
            t.cids.retirement_sent(path, owed);
            t.cids.retirement_settled(path, owed, true);
        }
    }
    CHECK(t.cids.peer_count(1U) == 1U);
}

TEST_CASE("stateless reset tokens count once sent to, in every space", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(2U, s.spaces[2]));
    const PeerId zero(0x20U);
    const PeerId one(0x41U);
    s.cids.peer_reset_token(cont::ConstSpan<u8>(zero.token, kToken));
    CHECK(s.host.watched == 0); // sequence 0 not sent to yet
    REQUIRE(add(s.cids, new_id(2U, 0U, one)) == QuicTransportError::NoError);
    CHECK_FALSE(s.cids.reset_by(cont::ConstSpan<u8>(one.token, kToken)));
    s.cids.use(2U, 0U);
    s.cids.use(2U, 0U); // once
    CHECK(s.host.watched == 1);
    CHECK(s.cids.reset_by(cont::ConstSpan<u8>(one.token, kToken)));
    CHECK_FALSE(s.cids.reset_by(cont::ConstSpan<u8>(zero.token, kToken)));
    s.cids.use(0U, 0U);
    CHECK(s.host.watched == 2);
    CHECK(s.cids.reset_by(cont::ConstSpan<u8>(zero.token, kToken)));
    const PeerId stranger(0x99U);
    CHECK_FALSE(s.cids.reset_by(cont::ConstSpan<u8>(stranger.token, kToken)));
    // finish() stops every token counting and hands every own ID back.
    REQUIRE(s.cids.issue(2U));
    s.cids.finish();
    CHECK(s.host.unwatched == 2);
    CHECK(s.host.retired == 2); // path 0's sequence 0 and path 2's
    CHECK(s.cids.local_count(0U) == 0U);
    CHECK(s.cids.local_count(2U) == 0U);
    CHECK_FALSE(s.cids.reset_by(cont::ConstSpan<u8>(one.token, kToken)));
}

TEST_CASE("this side's IDs are topped up, announced and settled per space", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(3U, s.spaces[3]));
    s.cids.top_up(3U);
    CHECK(s.cids.local_count(0U) == 3U);
    CHECK(s.cids.local_count(3U) == 3U);
    s.cids.top_up(100U); // at most kQuicMaxLocalConnectionIds
    CHECK(s.cids.local_count(0U) == kQuicMaxLocalConnectionIds);
    CHECK(s.cids.local_count(3U) == kQuicMaxLocalConnectionIds);
    // Every one but path 0's sequence 0 is owed, each in its own space.
    usize owed_frames = 0U;
    u64 path = 0U;
    for (const QuicLocalCid* owed = s.cids.next_owed(path); owed != nullptr; owed = s.cids.next_owed(path))
    {
        s.cids.local_sent(path, owed->sequence);
        ++owed_frames;
    }
    CHECK(owed_frames == 2U * kQuicMaxLocalConnectionIds - 1U);
    // A lost one is owed again, an acknowledged one never; a new path reannounces the unacknowledged.
    s.cids.local_settled(3U, 1U, false);
    CHECK(s.cids.local_owed());
    REQUIRE(s.cids.next_owed(path) != nullptr);
    CHECK(path == 3U);
    s.cids.local_settled(3U, 1U, true);
    CHECK(owed_count(s.cids) == 0U);
    s.cids.local_settled(3U, 1U, false); // a late loss of an acknowledged one
    CHECK_FALSE(s.cids.local_owed());
    s.cids.reannounce();
    CHECK(s.cids.local_owed());
    CHECK(s.cids.local(3U, 1U)->owed == false);
    CHECK(s.cids.local(0U, 0U)->owed == false); // the handshake carried it
    // A host that routes no more ID issues none.
    Started t;
    t.host.refuse = true;
    t.cids.top_up(4U);
    CHECK(t.cids.local_count(0U) == 1U);
    CHECK_FALSE(t.cids.issue(0U));
    // Announced IDs (a preferred address's) are acknowledged already.
    Started u;
    REQUIRE(u.cids.issue(0U, true));
    CHECK(u.cids.local(0U, 1U)->acked);
    CHECK_FALSE(u.cids.local_owed());
}

TEST_CASE("the peer retires this side's IDs within their path ID", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    s.cids.top_up(2U);
    REQUIRE(s.cids.local_count(1U) == 2U);
    const QuicCidRef packet{1U, 0U};
    // Never past the issued, never the packet's own ID, which path 0's sequence 0 is not.
    CHECK(s.cids.retire_local(1U, 2U, packet) == QuicTransportError::ProtocolViolation);
    CHECK(s.cids.retire_local(1U, 0U, packet) == QuicTransportError::ProtocolViolation);
    CHECK(s.cids.retire_local(0U, 0U, packet) == QuicTransportError::NoError);
    CHECK(s.cids.local(0U, 0U) == nullptr);
    CHECK(s.cids.retire_local(0U, 0U, packet) == QuicTransportError::NoError); // retired already
    CHECK(s.cids.retire_local(1U, 1U, packet) == QuicTransportError::NoError);
    CHECK(s.host.retired == 2);
    CHECK(s.cids.local_count(1U) == 1U);
    // The next top-up replaces them, each in its own space.
    s.cids.top_up(2U);
    CHECK(s.cids.local_count(0U) == 2U);
    CHECK(s.cids.local(1U, 2U) != nullptr);
    // Zero-length IDs are never retired.
    Host host;
    QuicCids empty(&host);
    empty.first_local({}, {});
    CHECK(empty.retire_local(0U, 0U, QuicCidRef{}) == QuicTransportError::ProtocolViolation);
    CHECK_FALSE(empty.issue(0U));
}

TEST_CASE("a rotation retires one path ID's IDs with Retire Prior To", "[quic][cids][multipath]")
{
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    s.cids.top_up(2U);
    REQUIRE(s.cids.rotate(1U, 2U));
    CHECK(s.cids.local_retire_prior_to(1U) == 2U);
    CHECK(s.cids.local_retire_prior_to(0U) == 0U);
    CHECK(s.cids.local_count(1U) == 4U);
    // Until the earlier ones are retired, no other rotation of that space.
    CHECK_FALSE(s.cids.rotate(1U, 2U));
    REQUIRE(s.cids.rotate(0U, 2U));
    CHECK(s.cids.retire_local(1U, 0U, QuicCidRef{}) == QuicTransportError::NoError);
    CHECK(s.cids.retire_local(1U, 1U, QuicCidRef{}) == QuicTransportError::NoError);
    REQUIRE(s.cids.rotate(1U, 2U));
    CHECK(s.cids.local_retire_prior_to(1U) == 4U);
    // A rotation the host cannot serve changes nothing.
    s.host.refuse = true;
    CHECK(s.cids.retire_local(1U, 2U, QuicCidRef{}) == QuicTransportError::NoError);
    CHECK(s.cids.retire_local(1U, 3U, QuicCidRef{}) == QuicTransportError::NoError);
    CHECK_FALSE(s.cids.rotate(1U, 2U));
    CHECK(s.cids.local_retire_prior_to(1U) == 4U);
    CHECK_FALSE(s.cids.rotate(5U, 2U)); // not open
}

TEST_CASE("a move to another held peer ID", "[quic][cids]")
{
    Started s;
    REQUIRE(add(s.cids, new_id(0U, 1U, PeerId(0x51U))) == QuicTransportError::NoError);
    s.cids.set_current(0U, 1U);
    CHECK(s.cids.current() == 1U);
    s.cids.set_current(0U, 7U); // not held: no move
    CHECK(s.cids.current() == 1U);
    CHECK(s.cids.peer_at(0U, 0U) != nullptr);
    CHECK(s.cids.peer_at(0U, 2U) == nullptr);
    CHECK(s.cids.peer_at(4U, 0U) == nullptr);
    // Retiring the current one leaves the path without an ID until the peer's next.
    REQUIRE(s.cids.retire_peer(0U, 1U, kLimit));
    CHECK(s.cids.current() == kQuicNoSequence);
    REQUIRE(add(s.cids, new_id(0U, 2U, PeerId(0x52U))) == QuicTransportError::NoError);
    CHECK(s.cids.current() == 2U);
}

TEST_CASE("IDs are issued only for path IDs within the peer's limit", "[quic][cids][multipath]")
{
    // RFC 10069 section 3.2.1: none past the peer's Maximum Path Identifier; the spaces above it wait, open.
    Started s;
    REQUIRE(s.cids.open(1U, s.spaces[1]));
    REQUIRE(s.cids.open(2U, s.spaces[2]));
    s.cids.top_up(2U, 1U);
    CHECK(s.cids.local_count(0U) == 2U);
    CHECK(s.cids.local_count(1U) == 2U);
    CHECK(s.cids.local_count(2U) == 0U);
    CHECK(s.cids.local_next(2U) == 0U);
    // The limit rises (MAX_PATH_ID): the next top-up fills path ID 2 as well.
    s.cids.top_up(2U, 2U);
    CHECK(s.cids.local_count(2U) == 2U);
    CHECK(s.cids.local_count(1U) == 2U);
}
