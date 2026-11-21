// crd-fuzz-net-quic-cids: a QUIC connection's connection IDs of crd-net (NET-PATH;
// docs/design/network-foundation.md#multipath) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: a first byte for this side's active_connection_id_limit (2 to 8), then operations of three bytes (the low
// four bits of the first the kind, its high four the path ID: 0 to 13, then 2^32 - 1 and 2^32) on one QuicCids over a
// counting host: spaces opened, abandoned and closed; the peer's NEW_CONNECTION_ID and PATH_NEW_CONNECTION_ID frames with fresh,
// gapped, repeated or old sequences, any Retire Prior To and fresh, reused or crossed IDs and tokens; retirements of
// the peer's IDs, sent and settled; IDs used, moved to and reset tokens matched; this side's IDs issued, topped up,
// rotated, retired by the peer under any packet's ID, sent and settled, reannounced; and the end of the connection.
//
// Oracles, against a model of every space. Each verdict is the restated rule's: a frame for a closed or abandoned
// space changes nothing; abandoning retires the space's peer IDs at once with no retirement owed and keeps this
// side's routed, none issued, rotated or announced there again; one ID or token under two IDs anywhere is PROTOCOL_VIOLATION, a repeat changes nothing; below Retire Prior
// To an ID is retired again; an ID retired here while its retirement is outstanding is never held again (once settled
// it may be, as the 16 latest are remembered); a raised Retire Prior To retires every ID below it; past the limit, or
// past twice the limit of retirements outstanding, CONNECTION_ID_LIMIT_ERROR. A refusal ends the input (the connection
// closes). After every operation the class holds exactly the model's IDs both ways with their states; the host routes
// exactly this side's held IDs and watches exactly the tokens of the peer's used IDs; each own ID is found in its own
// space; owed retirements and frames are the model's.
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_connection_ids.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::net;
namespace cont = crd::containers;

constexpr usize kToken = crd::crypto::kQuicResetTokenBytes;
constexpr usize kEver = 64U;            // individual retirements a model space remembers
constexpr usize kMaxOperations = 2000U; // an input's operations at most

// IDs and tokens of 8 and 16 bytes: the host's carry a 16-bit number and end in 0xA0, the peer's a byte and end in
// 0xB0.
void host_bytes(cont::Span<u8> out, u16 number)
{
    std::memset(out.data(), 0, out.size());
    out[0] = static_cast<u8>(number >> 8U);
    out[1] = static_cast<u8>(number);
    out[out.size() - 1U] = 0xA0U;
}

[[nodiscard]] u16 host_number(cont::ConstSpan<u8> bytes)
{
    return static_cast<u16>((u16{bytes[0]} << 8U) | bytes[1]);
}

struct PeerBytes
{
    explicit PeerBytes(u8 value)
    {
        id[0] = value;
        id[7] = 0xB0U;
    }
    u8 id[8] = {};
};

struct TokenBytes
{
    explicit TokenBytes(u8 value)
    {
        token[0] = value;
        token[kToken - 1U] = 0xB0U;
    }
    u8 token[kToken] = {};
};

constexpr u16 kFirstLocal = 0xFFFFU; // this side's sequence 0, routed before the connection

class Host final : public QuicConnectionIds
{
public:
    Host() { routed[kFirstLocal] = true; }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    ~Host() override = default;

    bool issue(cont::Span<u8> id, cont::Span<u8> token) noexcept override
    {
        if (refuse || next == kFirstLocal)
        {
            return false;
        }
        host_bytes(id, next);
        host_bytes(token, next);
        CRD_FUZZ_REQUIRE(!routed[next]);
        routed[next++] = true;
        ++routed_count;
        return true;
    }
    bool token(cont::ConstSpan<u8>, cont::Span<u8> out) noexcept override
    {
        std::memset(out.data(), 0, out.size());
        return true;
    }
    void retire(cont::ConstSpan<u8> id) noexcept override
    {
        const u16 number = host_number(id);
        CRD_FUZZ_REQUIRE(routed[number]); // never one not routed, never twice
        routed[number] = false;
        --routed_count;
    }
    void watch_reset(cont::ConstSpan<u8> token) noexcept override { ++watched[token[0]]; }
    void unwatch_reset(cont::ConstSpan<u8> token) noexcept override
    {
        CRD_FUZZ_REQUIRE(watched[token[0]] > 0);
        --watched[token[0]];
    }

    bool refuse = false;
    u16 next = 0U;
    bool routed[65536] = {};
    usize routed_count = 1U;
    int watched[256] = {};
};

struct Peer
{
    u64 sequence = 0U;
    u8 id = 0U;
    u8 token = 0U;
    bool has_token = false;
    bool used = false;
};

struct Out
{
    u64 sequence = 0U;
    bool owed = false; // else in flight
};

struct Local
{
    u64 sequence = 0U;
    u16 number = 0U;
    bool owed = false;
    bool acked = false;
};

struct Space
{
    bool open = false;
    bool abandoned = false;
    u64 path = 0U;
    QuicCidSpace* storage = nullptr;
    Peer peers[kQuicMaxPeerConnectionIds];
    usize peer_count = 0U;
    u64 current = kQuicNoSequence;
    u64 rpt = 0U;
    u64 next_peer = 0U; // past the highest sequence the peer sent
    Out out[2U * kQuicMaxPeerConnectionIds];
    usize out_count = 0U;
    u64 ever[kEver] = {};
    usize ever_count = 0U;
    bool ever_full = false;
    Local locals[2U * kQuicMaxLocalConnectionIds];
    usize local_count = 0U;
    u64 next_local = 0U;
    u64 local_rpt = 0U;
};

struct World
{
    Host host;
    QuicCids cids{&host};
    QuicCidSpace pool[kQuicMaxPaths];
    QuicCidSpace* free[kQuicMaxPaths + 1U] = {};
    usize free_count = 0U;
    Space spaces[kQuicMaxPaths];
    u64 limit = 2U;
    u8 next_value = 1U; // the next fresh peer ID value
    bool dead = false;
};

[[nodiscard]] Space* find(World& w, u64 path)
{
    for (Space& space : w.spaces)
    {
        if (space.open && space.path == path)
        {
            return &space;
        }
    }
    return nullptr;
}

[[nodiscard]] usize open_count(const World& w)
{
    usize count = 0U;
    for (const Space& space : w.spaces)
    {
        count += space.open ? 1U : 0U;
    }
    return count;
}

[[nodiscard]] u64 path_of(u8 selector)
{
    if (selector == 14U)
    {
        return 0xFFFFFFFFU;
    }
    return (selector == 15U) ? u64{0x100000000U} : selector;
}

[[nodiscard]] Out* outstanding(Space& space, u64 sequence)
{
    for (usize index = 0U; index < space.out_count; ++index)
    {
        if (space.out[index].sequence == sequence)
        {
            return &space.out[index];
        }
    }
    return nullptr;
}

[[nodiscard]] bool owe(const World& w, Space& space, u64 sequence)
{
    if (outstanding(space, sequence) != nullptr)
    {
        return true;
    }
    if (space.out_count >= 2U * w.limit)
    {
        return false;
    }
    space.out[space.out_count++] = Out{sequence, true};
    return true;
}

[[nodiscard]] bool ever_retired(const Space& space, u64 sequence)
{
    for (usize index = 0U; index < space.ever_count; ++index)
    {
        if (space.ever[index] == sequence)
        {
            return true;
        }
    }
    return space.ever_full;
}

void forget_peer(Space& space, usize index)
{
    space.current = (space.current == space.peers[index].sequence) ? kQuicNoSequence : space.current;
    for (usize at = index + 1U; at < space.peer_count; ++at)
    {
        space.peers[at - 1U] = space.peers[at];
    }
    --space.peer_count;
}

[[nodiscard]] usize active(const Space& space)
{
    usize count = 0U;
    for (usize index = 0U; index < space.local_count; ++index)
    {
        count += (space.locals[index].sequence >= space.local_rpt) ? 1U : 0U;
    }
    return count;
}

// Whether the restated issue() gives `space` one more ID: room in it, a host that routes.
[[nodiscard]] bool issuable(const World& w, const Space& space)
{
    return !space.abandoned && !w.host.refuse && w.host.next != kFirstLocal && space.local_count < 2U * kQuicMaxLocalConnectionIds;
}

// The model's record of an ID the class issued, its number read from the class (the host numbers IDs in the class's
// order of spaces).
void issued(World& w, Space& space, bool announced)
{
    const u64 sequence = space.next_local++;
    const QuicLocalCid* got = w.cids.local(space.path, sequence);
    CRD_FUZZ_REQUIRE(got != nullptr);
    space.locals[space.local_count++] = Local{sequence, host_number(got->id.view()), !announced, announced};
}

[[nodiscard]] usize wanted(u64 limit)
{
    return (limit < kQuicMaxLocalConnectionIds) ? static_cast<usize>(limit) : kQuicMaxLocalConnectionIds;
}

void check(World& w)
{
    int watched[256] = {};
    usize routed = 0U;
    bool retirement_owed = false;
    bool local_owed = false;
    for (Space& space : w.spaces)
    {
        if (!space.open)
        {
            continue;
        }
        CRD_FUZZ_REQUIRE(w.cids.is_open(space.path));
        CRD_FUZZ_REQUIRE(w.cids.abandoned(space.path) == space.abandoned);
        CRD_FUZZ_REQUIRE(w.cids.peer_count(space.path) == space.peer_count);
        CRD_FUZZ_REQUIRE(w.cids.current(space.path) == space.current);
        for (usize index = 0U; index < space.peer_count; ++index)
        {
            const Peer& want = space.peers[index];
            const QuicPeerCid* got = w.cids.peer(space.path, want.sequence);
            CRD_FUZZ_REQUIRE(got != nullptr && got->id.bytes == 8U && got->id.id[0] == want.id);
            CRD_FUZZ_REQUIRE(got->has_token == want.has_token && got->used == want.used);
            CRD_FUZZ_REQUIRE(!want.has_token || got->reset_token[0] == want.token);
            watched[want.token] += (want.has_token && want.used) ? 1 : 0;
        }
        for (usize index = 0U; index < space.out_count; ++index)
        {
            retirement_owed = retirement_owed || space.out[index].owed;
        }
        CRD_FUZZ_REQUIRE(w.cids.local_count(space.path) == space.local_count);
        CRD_FUZZ_REQUIRE(w.cids.local_retire_prior_to(space.path) == space.local_rpt);
        for (usize index = 0U; index < space.local_count; ++index)
        {
            const Local& want = space.locals[index];
            const QuicLocalCid* got = w.cids.local(space.path, want.sequence);
            CRD_FUZZ_REQUIRE(got != nullptr && got->owed == want.owed && got->acked == want.acked);
            CRD_FUZZ_REQUIRE(host_number(got->id.view()) == want.number && w.host.routed[want.number]);
            const QuicCidRef found = w.cids.find(got->id.view());
            CRD_FUZZ_REQUIRE(found.found() && found.path_id == space.path && found.sequence == want.sequence);
            local_owed = local_owed || want.owed;
            ++routed;
        }
    }
    CRD_FUZZ_REQUIRE(w.host.routed_count == routed);
    for (usize value = 0U; value < 256U; ++value)
    {
        CRD_FUZZ_REQUIRE(w.host.watched[value] == watched[value]);
    }
    CRD_FUZZ_REQUIRE(w.cids.retirement_owed() == retirement_owed);
    CRD_FUZZ_REQUIRE(w.cids.local_owed() == local_owed);
    for (u8 selector = 0U; selector < 16U; ++selector)
    {
        const u64 path = path_of(selector);
        CRD_FUZZ_REQUIRE(w.cids.is_open(path) == (find(w, path) != nullptr));
    }
}

void op_open(World& w, u64 path)
{
    const bool expected = path <= 0xFFFFFFFFU && find(w, path) == nullptr && open_count(w) < kQuicMaxPaths;
    if (w.free_count == 0U)
    {
        return;
    }
    QuicCidSpace* storage = w.free[w.free_count - 1U];
    CRD_FUZZ_REQUIRE(w.cids.open(path, *storage) == expected);
    if (!expected)
    {
        return;
    }
    --w.free_count;
    for (Space& space : w.spaces)
    {
        if (!space.open)
        {
            space = Space{};
            space.open = true;
            space.path = path;
            space.storage = storage;
            return;
        }
    }
}

void op_close(World& w, u64 path)
{
    Space* space = find(w, path);
    QuicCidSpace* closed = w.cids.close(path);
    CRD_FUZZ_REQUIRE((closed != nullptr) == (space != nullptr));
    if (space != nullptr)
    {
        CRD_FUZZ_REQUIRE(space->storage == nullptr || closed == space->storage);
        w.free[w.free_count++] = closed;
        space->open = false;
    }
}

void op_abandon(World& w, u64 path)
{
    Space* space = find(w, path);
    const bool expected = space != nullptr && !space->abandoned;
    CRD_FUZZ_REQUIRE(w.cids.abandon(path) == expected);
    if (!expected)
    {
        return;
    }
    space->abandoned = true;
    space->peer_count = 0U;
    space->current = kQuicNoSequence;
    space->out_count = 0U;
    for (usize index = 0U; index < space->local_count; ++index)
    {
        space->locals[index].owed = false;
    }
}

void op_add(World& w, u64 selected, u8 a, u8 b)
{
    const bool plain = (b & 0x80U) != 0U; // NEW_CONNECTION_ID: path ID 0's
    const u64 path = plain ? 0U : selected;
    Space* space = find(w, path);
    const u64 next = (space == nullptr) ? 0U : space->next_peer;
    u64 sequence = a & 15U;
    switch (a >> 6U)
    {
        case 0U:
            sequence = next;
            break;
        case 1U:
            sequence = next + (a & 3U);
            break;
        case 2U:
            sequence = (next > (a & 7U)) ? next - 1U - (a & 7U) : 0U;
            break;
        default:
            break;
    }
    u64 retire_prior_to = 0U;
    switch ((a >> 4U) & 3U)
    {
        case 1U:
            retire_prior_to = (space == nullptr) ? 0U : space->rpt;
            break;
        case 2U:
            retire_prior_to = sequence;
            break;
        case 3U:
            retire_prior_to = (sequence + 1U) / 2U;
            break;
        default:
            break;
    }
    retire_prior_to = (retire_prior_to > sequence) ? sequence : retire_prior_to;
    const u8 value = ((b & 0x40U) != 0U) ? static_cast<u8>(b & 15U) : w.next_value;
    const u8 token = ((b & 0x20U) != 0U) ? static_cast<u8>((b & 15U) | 0x10U) : value;
    const PeerBytes id(value);
    const TokenBytes reset(token);
    QuicFrame frame;
    frame.kind = plain ? QuicFrameKind::NewConnectionId : QuicFrameKind::PathNewConnectionId;
    frame.path_id = plain ? (a & 3U) : selected; // a NEW_CONNECTION_ID's path field is never read
    frame.sequence = sequence;
    frame.retire_prior_to = retire_prior_to;
    frame.data = cont::ConstSpan<u8>(id.id, sizeof(id.id));
    frame.reset_token = cont::ConstSpan<u8>(reset.token, kToken);
    bool added = false;
    const QuicTransportError error = w.cids.add_peer(frame, w.limit, added);
    if (space == nullptr || space->abandoned)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError && !added);
        return;
    }
    w.next_value = (value == w.next_value) ? static_cast<u8>(w.next_value + 1U) : w.next_value;
    // The conflict scan: one ID or token under two IDs anywhere, a repeat in this space.
    int conflict = 0;
    for (const Space& other : w.spaces)
    {
        for (usize index = 0U; other.open && index < other.peer_count && conflict == 0; ++index)
        {
            const Peer& held = other.peers[index];
            const bool same_id = held.id == value;
            const bool same_token = held.has_token && held.token == token;
            if (&other == space && held.sequence == sequence)
            {
                conflict = (same_id && (!held.has_token || same_token)) ? 1 : 2;
            }
            else if (same_id || same_token)
            {
                conflict = 2;
            }
        }
    }
    if (conflict == 2)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::ProtocolViolation);
        w.dead = true;
        return;
    }
    if (conflict == 1)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError && !added);
        return;
    }
    space->next_peer = (sequence >= space->next_peer) ? sequence + 1U : space->next_peer;
    if (sequence < space->rpt)
    {
        const bool owed = owe(w, *space, sequence);
        CRD_FUZZ_REQUIRE(!added);
        CRD_FUZZ_REQUIRE(error == (owed ? QuicTransportError::NoError : QuicTransportError::ConnectionIdLimitError));
        w.dead = !owed;
        return;
    }
    if (outstanding(*space, sequence) != nullptr)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError && !added); // never held again while retiring
        return;
    }
    if (error == QuicTransportError::NoError && !added)
    {
        CRD_FUZZ_REQUIRE(ever_retired(*space, sequence)); // the ring still names its retirement
        return;
    }
    if (retire_prior_to > space->rpt)
    {
        space->rpt = retire_prior_to;
        usize index = 0U;
        while (index < space->peer_count)
        {
            const u64 swept = space->peers[index].sequence;
            if (swept >= space->rpt)
            {
                ++index;
                continue;
            }
            forget_peer(*space, index);
            if (!owe(w, *space, swept))
            {
                CRD_FUZZ_REQUIRE(error == QuicTransportError::ConnectionIdLimitError);
                w.dead = true;
                return;
            }
        }
    }
    if (space->peer_count >= w.limit)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::ConnectionIdLimitError);
        w.dead = true;
        return;
    }
    CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError && added);
    space->peers[space->peer_count++] = Peer{sequence, value, token, true, false};
    space->current = (space->current == kQuicNoSequence) ? sequence : space->current;
}

// The `k`th of the peer's IDs held in `space`, or null.
[[nodiscard]] Peer* peer_at(Space* space, u8 k)
{
    return (space == nullptr || space->peer_count == 0U) ? nullptr : &space->peers[k % space->peer_count];
}

void op_retire_peer(World& w, u64 path, u8 a)
{
    Space* space = find(w, path);
    Peer* held = peer_at(space, a);
    const u64 sequence = (held == nullptr || (a & 0x80U) != 0U) ? u64{a & 15U} : held->sequence;
    bool expected = true;
    for (usize index = 0U; space != nullptr && index < space->peer_count; ++index)
    {
        if (space->peers[index].sequence == sequence)
        {
            forget_peer(*space, index);
            if (sequence >= space->rpt && space->ever_count < kEver)
            {
                space->ever[space->ever_count++] = sequence;
            }
            space->ever_full = space->ever_full || space->ever_count == kEver;
            expected = owe(w, *space, sequence);
            break;
        }
    }
    CRD_FUZZ_REQUIRE(w.cids.retire_peer(path, sequence, w.limit) == expected);
    w.dead = !expected;
}

void op_retirement(World& w, u8 a)
{
    if ((a & 1U) == 0U)
    {
        // The next owed one goes.
        u64 path = 0U;
        u64 sequence = 0U;
        if (!w.cids.next_retirement(path, sequence))
        {
            for (Space& space : w.spaces)
            {
                for (usize index = 0U; space.open && index < space.out_count; ++index)
                {
                    CRD_FUZZ_REQUIRE(!space.out[index].owed);
                }
            }
            return;
        }
        Space* space = find(w, path);
        CRD_FUZZ_REQUIRE(space != nullptr);
        Out* out = outstanding(*space, sequence);
        CRD_FUZZ_REQUIRE(out != nullptr && out->owed);
        out->owed = false;
        w.cids.retirement_sent(path, sequence);
        return;
    }
    // One in flight (or a stale record) settles.
    const bool acked = (a & 2U) != 0U;
    usize k = a >> 2U;
    for (Space& space : w.spaces)
    {
        for (usize index = 0U; space.open && index < space.out_count; ++index)
        {
            if (space.out[index].owed || k-- != 0U)
            {
                continue;
            }
            const u64 sequence = space.out[index].sequence;
            w.cids.retirement_settled(space.path, sequence, acked);
            if (acked)
            {
                space.out[index] = space.out[--space.out_count];
            }
            else
            {
                space.out[index].owed = true;
            }
            return;
        }
    }
    // A record of any other retirement: it settles only one in flight.
    const u64 path = a & 3U;
    const u64 sequence = a >> 4U;
    w.cids.retirement_settled(path, sequence, acked);
    Space* space = find(w, path);
    Out* out = (space == nullptr) ? nullptr : outstanding(*space, sequence);
    if (out != nullptr && !out->owed)
    {
        if (acked)
        {
            *out = space->out[--space->out_count];
        }
        else
        {
            out->owed = true;
        }
    }
}

void op_use(World& w, u64 path, u8 a)
{
    Space* space = find(w, path);
    Peer* held = peer_at(space, a);
    if ((a & 0x80U) != 0U)
    {
        // A move to another ID (held or not).
        const u64 sequence = (held == nullptr || (a & 0x40U) != 0U) ? u64{a & 15U} : held->sequence;
        w.cids.set_current(path, sequence);
        for (usize index = 0U; space != nullptr && index < space->peer_count; ++index)
        {
            space->current = (space->peers[index].sequence == sequence) ? sequence : space->current;
        }
        return;
    }
    const u64 sequence = (held == nullptr) ? u64{a & 15U} : held->sequence;
    w.cids.use(path, sequence);
    if (held != nullptr)
    {
        held->used = true;
    }
}

void op_token(World& w, u8 a, u8 b)
{
    // A stateless reset matched, or sequence 0's token learnt (only one no other held ID has, as the transport
    // parameters' token is checked by the connection's own frames).
    const u8 token = static_cast<u8>(((b & 0x80U) != 0U) ? (b & 0x1FU) : b);
    bool held = false;
    bool live = false;
    for (const Space& space : w.spaces)
    {
        for (usize index = 0U; space.open && index < space.peer_count; ++index)
        {
            const Peer& peer = space.peers[index];
            held = held || (peer.has_token && peer.token == token);
            live = live || (peer.has_token && peer.used && peer.token == token);
        }
    }
    const TokenBytes reset(token);
    if ((a & 1U) == 0U)
    {
        CRD_FUZZ_REQUIRE(w.cids.reset_by(cont::ConstSpan<u8>(reset.token, kToken)) == live);
        return;
    }
    Space* first = find(w, 0U);
    Peer* zero = nullptr;
    for (usize index = 0U; first != nullptr && index < first->peer_count; ++index)
    {
        zero = (first->peers[index].sequence == 0U) ? &first->peers[index] : zero;
    }
    if (held && !(zero != nullptr && zero->has_token && zero->token == token))
    {
        return;
    }
    w.cids.peer_reset_token(cont::ConstSpan<u8>(reset.token, kToken));
    if (zero != nullptr)
    {
        zero->token = token;
        zero->has_token = true;
    }
}

void op_issue(World& w, u64 path, u8 a)
{
    Space* space = find(w, path);
    const u64 limit = a & 15U;
    switch (a >> 6U)
    {
        case 0U:
        {
            const bool announced = (a & 0x20U) != 0U;
            const bool expected = space != nullptr && issuable(w, *space);
            CRD_FUZZ_REQUIRE(w.cids.issue(path, announced) == expected);
            if (expected)
            {
                issued(w, *space, announced);
            }
            break;
        }
        case 1U:
            w.cids.top_up(limit);
            for (Space& each : w.spaces)
            {
                while (each.open && active(each) < wanted(limit) && issuable(w, each))
                {
                    issued(w, each, false);
                }
            }
            break;
        case 2U:
        {
            // A rotation: refused while an earlier one is not retired in full, or when nothing is issued.
            bool expected = space != nullptr && !space->abandoned;
            for (usize index = 0U; expected && index < space->local_count; ++index)
            {
                expected = space->locals[index].sequence >= space->local_rpt;
            }
            const bool rotated = w.cids.rotate(path, limit);
            if (expected)
            {
                const u64 earlier = space->local_rpt;
                space->local_rpt = space->next_local;
                usize count = 0U;
                while (count < wanted(limit) && issuable(w, *space))
                {
                    issued(w, *space, false);
                    ++count;
                }
                expected = count != 0U;
                space->local_rpt = expected ? space->local_rpt : earlier;
            }
            CRD_FUZZ_REQUIRE(rotated == expected);
            break;
        }
        default:
            w.host.refuse = !w.host.refuse;
            break;
    }
}

void op_retire_local(World& w, u64 path, u8 a, u8 b)
{
    Space* space = find(w, path);
    const Local* held =
        (space == nullptr || space->local_count == 0U) ? nullptr : &space->locals[a % space->local_count];
    const u64 sequence = (held == nullptr || (a & 0x80U) != 0U) ? u64{a & 15U} : held->sequence;
    QuicCidRef packet;
    if ((b & 3U) == 1U && held != nullptr)
    {
        packet = QuicCidRef{path, space->locals[b % space->local_count].sequence};
    }
    else if ((b & 3U) == 2U)
    {
        packet = QuicCidRef{static_cast<u64>((b >> 2U) & 3U), static_cast<u64>(b >> 4U)};
    }
    const QuicTransportError error = w.cids.retire_local(path, sequence, packet);
    if (space == nullptr || space->abandoned)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError);
        return;
    }
    if (sequence >= space->next_local || (packet.found() && packet.path_id == path && packet.sequence == sequence))
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::ProtocolViolation);
        w.dead = true;
        return;
    }
    CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError);
    for (usize index = 0U; index < space->local_count; ++index)
    {
        if (space->locals[index].sequence == sequence)
        {
            for (usize at = index + 1U; at < space->local_count; ++at)
            {
                space->locals[at - 1U] = space->locals[at];
            }
            --space->local_count;
            break;
        }
    }
}

void op_local_frames(World& w, u8 a)
{
    switch (a & 3U)
    {
        case 0U:
        {
            u64 path = 0U;
            const QuicLocalCid* owed = w.cids.next_owed(path);
            CRD_FUZZ_REQUIRE((owed == nullptr) == !w.cids.local_owed());
            if (owed == nullptr)
            {
                return;
            }
            Space* space = find(w, path);
            CRD_FUZZ_REQUIRE(space != nullptr && owed->owed);
            const u64 sequence = owed->sequence;
            for (usize index = 0U; index < space->local_count; ++index)
            {
                Local& local = space->locals[index];
                local.owed = local.owed && local.sequence != sequence;
            }
            w.cids.local_sent(path, sequence);
            return;
        }
        case 1U:
        {
            // A NEW_CONNECTION_ID or PATH_NEW_CONNECTION_ID frame settles (a record of any ID, held or not).
            const bool acked = (a & 4U) != 0U;
            usize k = a >> 3U;
            for (Space& space : w.spaces)
            {
                for (usize index = 0U; space.open && index < space.local_count; ++index)
                {
                    if (k-- != 0U)
                    {
                        continue;
                    }
                    Local& local = space.locals[index];
                    w.cids.local_settled(space.path, local.sequence, acked);
                    local.acked = local.acked || acked;
                    local.owed = !local.acked && !space.abandoned;
                    return;
                }
            }
            w.cids.local_settled(a >> 6U, kQuicNoSequence - 1U, acked); // never issued: nothing changes
            return;
        }
        case 2U:
            w.cids.reannounce();
            for (Space& space : w.spaces)
            {
                for (usize index = 0U; space.open && index < space.local_count; ++index)
                {
                    Local& local = space.locals[index];
                    local.owed = local.owed || (!local.acked && !space.abandoned);
                }
            }
            return;
        default:
        {
            // IDs not this side's are found nowhere.
            u8 stranger[8] = {};
            host_bytes(cont::Span<u8>(stranger, sizeof(stranger)), 0xFFFEU);
            CRD_FUZZ_REQUIRE(!w.cids.find(cont::ConstSpan<u8>(stranger, sizeof(stranger))).found());
            const PeerBytes peer(a);
            CRD_FUZZ_REQUIRE(!w.cids.find(cont::ConstSpan<u8>(peer.id, sizeof(peer.id))).found());
            return;
        }
    }
}

void op_finish(World& w)
{
    w.cids.finish();
    for (Space& space : w.spaces)
    {
        space.local_count = 0U;
        for (usize index = 0U; index < space.peer_count; ++index)
        {
            space.peers[index].used = false;
        }
    }
    check(w);
    w.dead = true; // a closed connection calls nothing more
}

void run(World& w, u8 op, u8 a, u8 b)
{
    const u64 path = path_of(static_cast<u8>(op >> 4U));
    switch (op & 15U)
    {
        case 0U:
            op_open(w, path);
            break;
        case 1U:
            if ((a & 1U) != 0U)
            {
                op_abandon(w, path);
            }
            else
            {
                op_close(w, path);
            }
            break;
        case 2U:
        case 3U:
        case 4U:
            op_add(w, path, a, b);
            break;
        case 5U:
            op_retire_peer(w, path, a);
            break;
        case 6U:
        case 7U:
            op_retirement(w, a);
            break;
        case 8U:
            op_use(w, path, a);
            break;
        case 9U:
            op_token(w, a, b);
            break;
        case 10U:
        case 11U:
            op_issue(w, path, a);
            break;
        case 12U:
            op_retire_local(w, path, a, b);
            break;
        case 13U:
        case 14U:
            op_local_frames(w, a);
            break;
        default:
            op_finish(w);
            break;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    // At most kMaxOperations, so that the host's 16-bit ID numbers never run out (32 IDs an operation at most).
    if (size > crd::fuzz::kMaxInputBytes || size == 0U || size > 1U + 3U * kMaxOperations)
    {
        return 0;
    }
    World w;
    w.limit = 2U + data[0] % 7U;
    for (QuicCidSpace& storage : w.pool)
    {
        w.free[w.free_count++] = &storage;
    }
    u8 first[8] = {};
    host_bytes(cont::Span<u8>(first, sizeof(first)), kFirstLocal);
    w.cids.first_local(cont::ConstSpan<u8>(first, sizeof(first)), {});
    const PeerBytes zero(0U);
    w.cids.first_peer(cont::ConstSpan<u8>(zero.id, sizeof(zero.id)));
    Space& space = w.spaces[0];
    space.open = true;
    space.locals[0] = Local{0U, kFirstLocal, false, true};
    space.local_count = 1U;
    space.next_local = 1U;
    space.peers[0] = Peer{0U, 0U, 0U, false, false};
    space.peer_count = 1U;
    space.current = 0U;
    space.next_peer = 1U;
    check(w);
    for (usize at = 1U; at + 2U < size && !w.dead; at += 3U)
    {
        run(w, data[at], data[at + 1U], data[at + 2U]);
        if (!w.dead)
        {
            check(w);
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Operations: the kind in the low four bits, the path ID in the high four (14: 2^32 - 1, 15: 2^32).
    // Three paths' IDs both ways: frames, use, a top-up sent and settled, a reset matched, Retire Prior To in one
    // space, retirements sent, acknowledged and lost, a forgotten ID's frame repeated, a rotation, a retirement by the
    // peer, a space closed, the end.
    const u8 paths[] = {
        0x02U,                                    // limit 4
        0x10U, 0x00U, 0x00U, 0x20U, 0x00U, 0x00U, // open path IDs 1 and 2
        0x12U, 0x00U, 0x00U, 0x12U, 0x00U, 0x00U, // path 1's sequences 0 and 1
        0x22U, 0x00U, 0x00U, 0x02U, 0x00U, 0x80U, // path 2's sequence 0, path 0's 1 (NEW_CONNECTION_ID)
        0x18U, 0x00U, 0x00U, 0x08U, 0x01U, 0x00U, // used
        0x0AU, 0x43U, 0x00U,                      // top-up to 3
        0x0DU, 0x00U, 0x00U, 0x0DU, 0x00U, 0x00U, 0x0DU, 0x00U,
        0x00U, 0x0DU, 0x05U, 0x00U, 0x09U, 0x00U, 0x01U, // the reset token of path 1's sequence 0
        0x12U, 0x20U, 0x00U,                             // path 1's sequence 2 retires 0 and 1
        0x06U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x06U, 0x03U,
        0x00U, 0x06U, 0x01U, 0x00U, 0x05U, 0x01U, 0x00U, // path 0's sequence 1 forgotten
        0x02U, 0xC1U, 0xC4U,                             // and its frame repeated
        0x2AU, 0x82U, 0x00U,                             // path 2 rotated
        0x0CU, 0x80U, 0x00U,                             // the peer retires path 0's sequence 0
        0x11U, 0x00U, 0x00U, 0x0FU, 0x00U, 0x00U,        // path 1 closed, the end
    };
    sink.add("paths-both-ways", paths, sizeof(paths));
    // One ID under two path IDs: PROTOCOL_VIOLATION.
    const u8 conflict[] = {0x04U, 0x30U, 0x00U, 0x00U, 0x32U, 0x00U, 0x00U, 0x02U, 0x00U, 0xC1U};
    sink.add("id-under-two-paths", conflict, sizeof(conflict));
    // Retire Prior To on every frame until the retirements outstanding pass twice the limit.
    const u8 bound[] = {0x00U, 0x02U, 0x20U, 0x00U, 0x02U, 0x20U, 0x80U, 0x02U, 0x20U, 0x80U, 0x02U,
                        0x20U, 0x80U, 0x02U, 0x20U, 0x80U, 0x02U, 0x20U, 0x80U, 0x02U, 0x20U, 0x80U};
    sink.add("retirements-past-the-bound", bound, sizeof(bound));
    // Spaces refused (past 2^32 - 1, more than kQuicMaxPaths), path 0 closed and its storage reused, IDs issued,
    // refused by the host, reannounced, strangers not found.
    const u8 spaces[] = {
        0x06U, 0xE0U, 0x00U, 0x00U, 0xF0U, 0x00U, 0x00U, 0x10U, 0x00U, 0x00U, 0x20U, 0x00U, 0x00U,
        0x30U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x30U, 0x00U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x3AU,
        0x20U, 0x00U, 0x0AU, 0xC0U, 0x00U, 0xEAU, 0x45U, 0x00U, 0x0AU, 0xC0U, 0x00U, 0xEAU, 0x45U,
        0x00U, 0x0EU, 0x02U, 0x00U, 0x0EU, 0x03U, 0x00U, 0x3CU, 0x00U, 0x01U, 0x0FU, 0x00U, 0x00U,
    };
    sink.add("spaces-refused-and-reused", spaces, sizeof(spaces));
    // Path 1 abandoned with IDs both ways, a retirement owed and frames in flight: its frames ignored, nothing issued
    // there, a lost frame not owed again, then closed and its storage reopened.
    const u8 abandoned[] = {
        0x02U, 0x10U, 0x00U, 0x00U, 0x12U, 0x00U, 0x00U, 0x12U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, // limit 4, path 1
        0x0AU, 0x43U, 0x00U, 0x0DU, 0x00U, 0x00U, 0x15U, 0x01U, 0x00U,                            // top-up, retire
        0x11U, 0x01U, 0x00U, 0x11U, 0x01U, 0x00U, 0x12U, 0x00U, 0x00U, 0x1CU, 0x00U, 0x00U,       // abandon twice
        0x1AU, 0x00U, 0x00U, 0x1AU, 0x82U, 0x00U, 0x0EU, 0x01U, 0x00U, 0x0EU, 0x02U, 0x00U,       // issue, rotate
        0x09U, 0x00U, 0x01U, 0x11U, 0x00U, 0x00U, 0x30U, 0x00U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x0FU, 0x00U, 0x00U,
    };
    sink.add("path-abandoned", abandoned, sizeof(abandoned));
}
