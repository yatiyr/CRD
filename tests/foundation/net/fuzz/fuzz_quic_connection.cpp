// crd-fuzz-net-quic-connection: a QUIC connection of crd-net (4.2a; docs/design/network-foundation.md#quic-connection)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: byte 0 picks the mode (bits 0 and 1) and the version (bit 2); the rest drives it.
//   0  a network script between a client and a server: each byte an operation on the datagrams in flight (deliver,
//      deliver with its ECN codepoint rewritten, drop, duplicate, reorder, corrupt a byte, advance time, 0xFD a Retry
//      in place of the client Initials in flight
//      while the server has none); then a clean path until both confirm or one closes. With bit 3 the client offers
//      the other version too and the server prefers it: a compatible switch (RFC 9368 section 2.3). With bit 4 a
//      first clean connection leaves the client a ticket, and the scripted one resumes with a stream and a datagram
//      written before its first flight: 0-RTT (RFC 9001 section 4.6).
//   1  frames from a raw client after the handshake: each [length][bytes] sealed as one 1-RTT packet to the server.
//   2  frames in Initial packets anyone can seal (their keys come from the public Destination Connection ID): each
//      [length][bytes] to a fresh server.
//   3  frames from a raw peer of the raw UDP path: its hello, then each [length][bytes] sealed as one 1-RTT packet.
// With bit 5 mode 0 runs the raw UDP path (S55): both ends opened from one secret, no handshake on the wire. With
// bit 6 every connection runs the NET-STD drafts: it asks for four receive timestamps an ACK frame
// (draft-ietf-quic-receive-ts-03), so the pair reports them and a raw peer's ACK_RECEIVE_TIMESTAMPS frames are read,
// and offers and asks for address reports (draft-ietf-quic-address-discovery-01), so a raw peer's OBSERVED_ADDRESS
// frames are taken. With bit 7 mode 0 runs over paths (S75): each
// client datagram reaches the server from the client's address when it left, a server datagram reaches the client
// only when sent to its address now, a script byte 0xFC moves the client to the next of four addresses (a NAT
// rebinding, an address change, an IPv6 address, back) and 0xFB delivers a copy of the first client datagram in
// flight from another address first, as an off-path attacker forwarding it would. The client also has sockets 1 and
// 2 at addresses of their own: 0xFA moves it to its next socket (migrate()) and 0xF9 probes that one (probe_path()),
// and 0xF8 steps which of its three sockets its host holds back (send()'s held sockets, every mask in turn; none on
// the clean path). The server offers a preferred address on its socket 1 (RFC 9000 section 9.6), which 0xF7 moves the
// client to (migrate(): once validated). The server holds two of its client's IDs, the fewest allowed, so a move
// may wait for one (section 9.5), or four after a leading 0xF3. In any script 0xF6 sets both ends' handover schedule
// (S76) from the next byte: an event every second from now, its guards from the byte, a refused schedule changing
// nothing; 0xF5 has both learn the phase instead (a period and guards from the next byte), the learned schedule held
// to the learning. With bit 6 (RFC 10069, design S78) 0xF0 has one end abandon a path ID (abandon_path()) from the
// next byte: bit 0 the end, bits 1 to 3 the path ID, bits 4 and 5 the error code (NO_ERROR, 0x3E, the largest
// varint, one past it); and over paths 0xF2 has the client open a further path ID (add_path()) on its socket in the
// next byte's bits 0 and 1 (3 is socket 0), to the server's address or, with bit 2, its preferred one.
//
// Oracles. Always: a server sends at most three times what it received before the address is proven, and on paths
// at most three times what arrived from each address it has not validated since it last knew it; every client
// datagram carrying an Initial has 1200 bytes; a server's datagrams keep to its path's size but for PMTU probes, which
// keep to its ceiling; nothing is allocated after construction; a close names a transport
// error of RFC 9000 section 20.1 (or RFC 9368's) or CRYPTO_ERROR when this side closes. Script: after the clean path,
// both ends are confirmed or one has closed (no handshake hangs), and a confirmed pair agrees on the application
// protocol, and then carries a stream larger than every window whole and unchanged and datagrams (every other one
// redundant, sent once on its one path) each at most once, byte-exact and each reported once (or one side closes).
// Frames: the server, which grants streams and takes datagrams of up to 600 bytes, stays Established (Handshaking
// for Initial frames) or closes; a closed server sends a
// CONNECTION_CLOSE only. Both modes route each end's own IDs through a host (QuicRoutedIds): its table holds exactly
// the IDs its connection holds, after every step; a script byte 0xFE rotates the server's IDs; and a confirmed client
// that a stateless reset of the ID it sends to reaches drains, while a forged token changes nothing. A confirmed pair
// agrees on whether a Retry was taken; the server accepts it only for an Initial to the Retry's ID. A confirmed pair
// runs one version: the other one after a switch, the configured one otherwise. Paths: a confirmed pair that carried
// its stream with neither end closed has the server sending to its active socket's address once what is owed has
// arrived; a move or a probe the client refuses changes its active path in nothing, one it starts moves it or not as
// asked; no client datagram leaves by a held socket, and a send() held back changes no counter or packet number; a
// server on its preferred socket moved there after its client, once it validated the path there, and keeps no path
// on its first socket; a client there sends to that address alone. These path checks read path ID 0's slots while
// it is open; the server's credit holds per slot, a datagram counted by the slot that reads it, at every step and on
// every send. Multipath: no path ID opens in a slot of either end twice (RFC 10069 section 2.4, its nonce); add_path()
// and abandon_path() answer as the limits, the paths still sending and the earlier abandonments say, and nothing
// refused changes a counter; each end's limit rises by one per path ID closed, which its peer learns. A confirmed pair
// keeps the client's lowest open path ID once the stream arrived, abandoning its others, and the server follows the
// client on it (that a server not following saw a validation fail is exact only without further paths: a failure on
// another one counts too).
// 0-RTT: an offer ends Accepted or Rejected on both ends alike; accepted, the server reads the early stream
// whole and unchanged, never marking more bytes replayable than came; rejected, the client's streams were reset and
// the server opened none of them.
#include "../../crypto/pinned_wall_clock.hpp"

#include <crd/core/platform.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/quic_header.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireReader;
using namespace crd::crypto;
using namespace crd::net;

constexpr u8 kClientCid[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kServerCid[16] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                               0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
constexpr u8 kInitialDcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u8 kRetryCid[10] = {0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm};
constexpr StringView kProtocols[] = {"cerid-game/1"};
constexpr const char* kServerName = "game.cerid.test";
constexpr u8 kSeeds[2][kPrfKeyBytes] = {{0x01}, {0x02}};
constexpr u64 kMs = 1000000U;
constexpr usize kDatagram = 1500U;
constexpr usize kFlights = 64U;
constexpr u8 kTicketKey[kTlsTicketKeyBytes] = {0x71};
constexpr u8 kAppContext[] = {0x5C, 0x11, 0xE7, 0x02};
constexpr u64 kEarlyBytes = 800U; // within the stream and connection windows of config_for
constexpr u8 kEarlyPayload[] = {0xDA, 0x7A};
constexpr u8 kKeyedSecret[kQuicKeyedSecretBytes] = {0x4D, 0x11, 0x7E};
constexpr usize kKeyedIdBytes = 8U;
constexpr u64 kMaxPathId = 2U; // RFC 10069: both ends' initial_max_path_id when byte 0 offers the extensions
constexpr u64 kNoPathId = ~u64{0};

bool valid_close(u64 code)
{
    return code <= static_cast<u64>(QuicTransportError::VersionNegotiationError) ||
           (code >= static_cast<u64>(QuicTransportError::CryptoErrorFirst) &&
            code < static_cast<u64>(QuicTransportError::CryptoErrorFirst) + 256U);
}

void check_closed(const QuicConnection& end)
{
    // A peer's close carries whatever code it chose; this side's own closes name RFC 9000's.
    if (end.close_info().local && !end.close_info().application)
    {
        CRD_FUZZ_REQUIRE(valid_close(end.close_info().error_code));
    }
    // While it learns, the schedule in force is none or the learning's period and guards at a phase below the period.
    const HandoverLearning& learning = end.handover_learning();
    const HandoverSchedule& schedule = end.handover();
    if (learning.period_ns != 0U)
    {
        CRD_FUZZ_REQUIRE(schedule == HandoverSchedule{} ||
                         (schedule.period_ns == learning.period_ns && schedule.phase_ns < learning.period_ns &&
                          schedule.before_ns == learning.before_ns && schedule.after_ns == learning.after_ns));
        CRD_FUZZ_REQUIRE(end.handover_learned() == (schedule.period_ns != 0U));
    }
}

struct Identity
{
    crd::crypto::test::PinnedWallClock clock; // first: the tickets read it from the first connection
    SigningKey key;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;
    ReplayFilter filter; // the 0-RTT server's, made fresh for each input
    TlsContext early_client;
    TlsContext early_server;
    bool ready = false;

    Identity()
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) != CryptoError::None ||
            credential.init(key, kServerName) != TlsError::None)
        {
            return;
        }
        const KeyId pin = credential.key_id();
        TlsConfig client_config;
        client_config.role = TlsRole::Client;
        client_config.groups = kGroups;
        client_config.signatures = kSignatures;
        client_config.aeads = kAeads;
        client_config.pinned = ConstSpan<KeyId>(&pin, 1U);
        client_config.server_name = kServerName;
        client_config.application_protocols = kProtocols;
        TlsConfig server_config;
        server_config.role = TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = kServerName;
        server_config.application_protocols = kProtocols;
        ready = client.init(client_config) == TlsError::None && server.init(server_config) == TlsError::None;
        client_config.early_data = true;
        server_config.early_data = true;
        server_config.early_data_filter = &filter;
        server_config.ticket_key = ConstSpan<u8>(kTicketKey);
        ready = ready && fresh_filter() && early_client.init(client_config) == TlsError::None &&
                early_server.init(server_config) == TlsError::None;
    }

    bool fresh_filter()
    {
        filter.clear();
        const u64 period = kTlsDefaultTicketLifetime + kTlsMaxTicketAgeSkew;
        return filter.init(64U, period, crd::crypto::test::kPinnedUnixSeconds) == CryptoError::None;
    }
};

Identity& identity()
{
    static Identity held;
    CRD_FUZZ_REQUIRE(held.ready);
    return held;
}

// An end's host: a table routing its IDs under a reset key, sequence 0 routed first.
struct Host
{
    ConnectionIdTable routes;
    ConnectionIdTable tokens;
    QuicResetKey key;
    QuicRoutedIds ids;

    Host(crd::memory::IAllocator& allocator, u32 index, ConstSpan<u8> sequence0)
        : routes(allocator, 32U, ConstSpan<u8>(kSeeds[index], kPrfKeyBytes)),
          tokens(allocator, kQuicMaxPeerConnectionIds * (kMaxPathId + 1U), ConstSpan<u8>(kSeeds[index], kPrfKeyBytes)),
          ids(routes, key, index, &tokens)
    {
        u8 secret[kQuicResetKeyBytes] = {static_cast<u8>(0xA0U + index)};
        CRD_FUZZ_REQUIRE(key.init(ConstSpan<u8>(secret)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(routes.insert(sequence0, index) == RouteError::None);
    }

    // The host routes exactly the IDs its connection holds for every path ID up to its limit, which rises as path IDs
    // close (sequence 0 is routed until the connection is accepted).
    void check(const QuicConnection& end) const
    {
        usize local = 0U;
        usize peer = 0U;
        const u64 limit = (end.local_max_path_id() > kMaxPathId) ? end.local_max_path_id() : kMaxPathId;
        for (u64 path_id = 0U; path_id <= limit; ++path_id)
        {
            local += end.local_cid_count(path_id);
            peer += end.peer_cid_count(path_id);
        }
        CRD_FUZZ_REQUIRE(end.state() == QuicConnectionState::Idle || routes.size() == local);
        // Its peer tokens stay within the IDs held, and none outlives the connection.
        CRD_FUZZ_REQUIRE(tokens.size() <= peer);
        CRD_FUZZ_REQUIRE(end.state() != QuicConnectionState::Closed || tokens.size() == 0U);
    }
};

constexpr u32 kV1First[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
constexpr u32 kV2First[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};

QuicVersion other_version(QuicVersion version)
{
    return (version == QuicVersion::V1) ? QuicVersion::V2 : QuicVersion::V1;
}

// A client's tickets: the newest one, handed out once.
class Tickets final : public TlsTicketStore
{
public:
    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        if (ticket.size() <= sizeof(m_ticket))
        {
            m_binding = binding;
            std::memcpy(m_ticket, ticket.data(), ticket.size());
            m_bytes = ticket.size();
        }
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        if (m_bytes == 0U || !(m_binding == binding))
        {
            return {};
        }
        std::memcpy(m_handed, m_ticket, m_bytes);
        const usize bytes = m_bytes;
        m_bytes = 0U;
        return ConstSpan<u8>(m_handed, bytes);
    }

    [[nodiscard]] bool holds() const { return m_bytes != 0U; }

private:
    TlsBinding m_binding;
    u8 m_ticket[4096] = {};
    u8 m_handed[4096] = {};
    usize m_bytes = 0U;
};

// `side`'s sequence 0 ID on the raw UDP path of kKeyedSecret.
struct KeyedId
{
    u8 bytes[kKeyedIdBytes] = {};

    explicit KeyedId(QuicSide side)
    {
        CRD_FUZZ_REQUIRE(quic_keyed_connection_id(ConstSpan<u8>(kKeyedSecret), side, Span<u8>(bytes, sizeof(bytes))) ==
                         CryptoError::None);
    }

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, sizeof(bytes)); }
};

const KeyedId& keyed_id(QuicSide side)
{
    static const KeyedId kClient(QuicSide::Client);
    static const KeyedId kServer(QuicSide::Server);
    return (side == QuicSide::Client) ? kClient : kServer;
}

// Opens `end` on kKeyedSecret.
void open_keyed(QuicConnection& end, u64 now)
{
    u8 secret[kQuicKeyedSecretBytes];
    std::memcpy(secret, kKeyedSecret, sizeof(secret));
    QuicKeyedStart start;
    start.secret = Span<u8>(secret, sizeof(secret));
    start.local_id_bytes = kKeyedIdBytes;
    start.peer_id_bytes = kKeyedIdBytes;
    CRD_FUZZ_REQUIRE(end.open_keyed(start, now));
}

// Whether every connection of this input runs the NET-STD drafts (byte 0's bit 6).
bool& extensions()
{
    static bool on = false;
    return on;
}

// Whether this input's script runs over paths (byte 0's bit 7).
bool& over_paths()
{
    static bool paths = false;
    return paths;
}

// The IDs of its client's a server holds over paths: the fewest RFC 9000 allows (section 18.2), so a move of its
// client's may find none unused (section 9.5), the edge S75's kept revert and waiting move serve; or four, as
// elsewhere, for a script opening with 0xF3, so that paths left behind keep IDs to give up.
u64& server_ids()
{
    static u64 ids = 2U;
    return ids;
}

// Each end's active_connection_id_limit: the IDs of its peer's it holds.
u64 id_limit(QuicSide side)
{
    return (over_paths() && side == QuicSide::Server) ? server_ids() : 4U;
}

// With `switching` the client lists its version first and the server the other one; with `tickets` both ends run
// the contexts that take 0-RTT, the client keeping and offering its tickets there; `keyed` is for the raw UDP path.
QuicConnectionConfig config_for(QuicSide side, QuicVersion version, QuicConnectionIds* ids, bool switching = false,
                                Tickets* tickets = nullptr, bool keyed = false)
{
    QuicConnectionConfig config;
    config.side = side;
    if (switching)
    {
        const bool first = (side == QuicSide::Client) == (version == QuicVersion::V1);
        config.versions = first ? ConstSpan<u32>(kV1First, 2U) : ConstSpan<u32>(kV2First, 2U);
    }
    config.ids = ids;
    config.parameters.active_connection_id_limit = id_limit(side);
    config.parameters.max_receive_timestamps_per_ack = extensions() ? 4U : 0U;
    config.parameters.address_discovery = extensions() ? QuicAddressDiscovery::Both : QuicAddressDiscovery::Off;
    // RFC 10069 (design S78): ID spaces for path IDs 1 and 2, whose frames the peer may name, and a server's path
    // slots for them.
    config.parameters.has_initial_max_path_id = extensions();
    config.parameters.initial_max_path_id = extensions() ? kMaxPathId : 0U;
    config.version = version;
    config.tls = (side == QuicSide::Client) ? &identity().client : &identity().server;
    if (keyed)
    {
        config.tls = nullptr;
    }
    if (tickets != nullptr)
    {
        config.tls = (side == QuicSide::Client) ? &identity().early_client : &identity().early_server;
        config.tickets = (side == QuicSide::Client) ? tickets : nullptr;
        config.early_context = (side == QuicSide::Server) ? ConstSpan<u8>(kAppContext) : ConstSpan<u8>();
    }
    config.parameters.max_idle_timeout = 30000U;
    config.sent_packets = 64U;
    config.sent_frames = 128U;
    config.crypto_bytes = 8192U;
    config.streams.peer_bidi = 2U;
    config.streams.peer_uni = 1U;
    config.streams.local_bidi = 2U;
    config.streams.local_uni = 1U;
    config.streams.receive_bytes = 2048U;
    config.streams.send_bytes = 2048U;
    config.streams.stream_window = 1024U;
    config.streams.connection_window = 2048U;
    config.streams.connection_max_window = 8192U;
    config.datagrams.max_frame_bytes = 600U;
    config.datagrams.receive_bytes = 2400U; // send_count of the largest: nothing is dropped between reads
    config.datagrams.receive_count = 4U;
    config.datagrams.send_bytes = 1024U;
    config.datagrams.send_count = 4U;
    config.datagrams.redundant_count = 2U;
    config.datagrams.outcomes = true;
    config.entropy_seed = ConstSpan<u8>(kSeeds[side == QuicSide::Client ? 0U : 1U], kPrfKeyBytes);
    return config;
}

QuicAccept accept_of(ConstSpan<u8> dcid, ConstSpan<u8> scid)
{
    QuicAccept accept;
    accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
    accept.client_dcid = dcid;
    accept.client_scid = scid;
    accept.original_dcid = dcid;
    return accept;
}

// ---- Mode 0: a network script ----

struct Flight
{
    u8 bytes[kDatagram] = {};
    usize size = 0U;
    usize to = 0U;
    Ecn ecn = Ecn::NotEct; // as the sender marked it, or as the path rewrote it
    usize path = 0U;       // the client's address (kClientPaths) a datagram left or was sent to
    u32 server_socket = 0U; // the server's socket it reaches or left: 1 at the preferred address
    u64 path_id = 0U;       // the client's path ID it left on (0 for a datagram of no 1-RTT packet)
    u64 epoch = 0U;         // the client's epoch when it left (Pair::opened_from)
};

// The client's addresses over paths (byte 0's bit 7): its socket 0's (a NAT rebinding of the first, another address,
// an IPv6 one), then its sockets 1 and 2's, as the server sees them; and the server's address as the client sees it.
constexpr u8 kClientV6[16] = {0x20U, 0x01U, 0x0DU, 0xB8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
constexpr usize kSocketZeroPaths = 4U;
constexpr usize kClientAddresses = 6U;
const QuicPath kClientPaths[kClientAddresses] = {
    QuicPath{0U, Endpoint{IpAddress::v4(198, 51, 100, 7), 50000U}},
    QuicPath{0U, Endpoint{IpAddress::v4(198, 51, 100, 7), 50001U}},
    QuicPath{0U, Endpoint{IpAddress::v4(203, 0, 113, 5), 50000U}},
    QuicPath{0U, Endpoint{IpAddress::v6(kClientV6), 443U}},
    QuicPath{0U, Endpoint{IpAddress::v4(192, 0, 2, 40), 51000U}},
    QuicPath{0U, Endpoint{IpAddress::v4(192, 0, 2, 41), 52000U}},
};
const Endpoint kServerAddress{IpAddress::v4(192, 0, 2, 1), 443U};
const Endpoint kPreferredAddress{IpAddress::v4(192, 0, 2, 80), 4433U}; // the server's socket 1 (RFC 9000 section 9.6)

// The server's credit in one path slot (RFC 9000 section 8, RFC 10069 section 7.2): the bytes each 4-tuple (a socket
// and a kClientPaths address) gave the slot since it last did not hold it, and what went there while unproven there.
struct SlotCredit
{
    u64 path_id = kNoPathId;
    u64 received[2][kClientAddresses] = {};
    u64 sent[2][kClientAddresses] = {};
};

// Over paths the client's first path names its server's address, which its own sockets' paths keep, and moves to
// the server's preferred address only when the script says (0xF7); a server with a handshake offers that address on
// its socket 1.
QuicConnectionConfig on_paths(QuicConnectionConfig config)
{
    if (!over_paths())
    {
        return config;
    }
    if (config.side == QuicSide::Client)
    {
        config.path = QuicPath{0U, kServerAddress};
        config.follow_preferred_address = false;
    }
    else if (config.tls != nullptr)
    {
        config.parameters.has_preferred_address = true;
        std::memcpy(config.parameters.preferred_address.ipv4, kPreferredAddress.address.bytes, 4U);
        config.parameters.preferred_address.ipv4_port = kPreferredAddress.port;
        config.preferred_socket = 1U;
    }
    return config;
}

// The slot whose number space is `path_id`'s at `end` (RFC 10069: one per open path ID); kQuicNoPath for none.
usize slot_of(const QuicConnection& end, u64 path_id)
{
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (end.protection().path_open(slot) && end.protection().path_id(slot) == path_id)
        {
            return slot;
        }
    }
    return kQuicNoPath;
}

// The bit of a path ID below 64 in a mask of path IDs (0 above: the scripts name none).
u64 id_bit(u64 path_id)
{
    return (path_id < 64U) ? (u64{1} << path_id) : 0U;
}

// The slots that send at `end` (RFC 10069 section 3.4: the last one is never abandoned).
usize sending_slots(const QuicConnection& end)
{
    usize count = 0U;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        count += (end.path_control(slot) != nullptr) ? 1U : 0U;
    }
    return count;
}

// MSVC C4324 as for crd::crypto::AeadKey: the connections are 16-byte aligned; ARM64 MSVC reports the padding.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct Pair
{
    crd::fuzz::BudgetAllocator allocator;
    Host client_host;
    Host server_host;
    QuicConnection client;
    QuicConnection server;
    Flight flights[kFlights];
    usize count = 0U;
    u64 now = 1000U * kMs;
    QuicVersion version;
    bool keyed = false; // the raw UDP path: no handshake to confirm, so it stands once both ends read the other's hello
    bool retry_sent = false;
    usize client_path = 0U; // socket 0's address now (over paths)
    SlotCredit credits[kQuicMaxPaths]; // the server's, per slot: no slot spends another's
    // The client's path IDs below 64 of which the server opened a packet from each address, sent in the client's
    // epoch: since its NAT last mapped its socket 0 elsewhere and it last moved. An older packet moves nothing (RFC
    // 9000 section 9.3: only the newest non-probing one does).
    u64 opened_from[kClientAddresses] = {};
    u64 epoch = 0U;
    u64 client_moves = 0U;  // the client's migrations counted when its epoch began
    u64 unbound_bytes = 0U; // given to the server's first path before it was bound
    // The client's sockets its host holds back (over paths).
    u32 held = 0U;
    bool server_preferred = false; // the server moved to its preferred socket (over paths)
    // Each end's (the client's first) path IDs below 64 that opened in a slot, that left it, and that were abandoned.
    u64 opened_ids[2] = {};
    u64 closed_ids[2] = {};
    u64 abandoned_ids[2] = {};
    u64 added = 0U; // path IDs the client's add_path() opened

    explicit Pair(QuicVersion run, bool switching = false, Tickets* tickets = nullptr, bool raw_path = false)
        : client_host(allocator, 0U,
                      raw_path ? keyed_id(QuicSide::Client).view() : ConstSpan<u8>(kClientCid, sizeof(kClientCid))),
          server_host(allocator, 1U,
                      raw_path ? keyed_id(QuicSide::Server).view() : ConstSpan<u8>(kServerCid, sizeof(kServerCid))),
          client(allocator,
                 on_paths(config_for(QuicSide::Client, run, &client_host.ids, switching, tickets, raw_path))),
          server(allocator,
                 on_paths(config_for(QuicSide::Server, run, &server_host.ids, switching, tickets, raw_path))),
          version(run), keyed(raw_path)
    {
    }

    [[nodiscard]] bool established() const
    {
        return client.confirmed() && server.confirmed() &&
               (!keyed || (client.peer_parameters_known() && server.peer_parameters_known()));
    }

    // The server answers the client Initials in flight with one Retry, as a stateless ingress does: they are gone.
    void retry()
    {
        if (retry_sent || server.state() != QuicConnectionState::Idle || count == kFlights)
        {
            return;
        }
        for (usize index = 0U; index < count;)
        {
            if (flights[index].to == 1U)
            {
                remove(index);
                continue;
            }
            ++index;
        }
        u8 token[24];
        std::memset(token, 0x6B, sizeof(token));
        Flight& flight = flights[count];
        CRD_FUZZ_REQUIRE(write_retry(version, ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                                     ConstSpan<u8>(kRetryCid, sizeof(kRetryCid)),
                                     ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), ConstSpan<u8>(token), 0U,
                                     Span<u8>(flight.bytes, sizeof(flight.bytes)), flight.size) == CryptoError::None);
        flight.to = 0U;
        ++count;
        retry_sent = true;
    }

    // After every step: both hosts route what their ends hold, and no path ID opens in a slot of either end twice
    // (RFC 10069 section 2.4: the path ID is in the nonce).
    void check_hosts()
    {
        client_host.check(client);
        server_host.check(server);
        const QuicConnection* const ends[2] = {&client, &server};
        for (usize side = 0U; side < 2U; ++side)
        {
            u64 open = 0U;
            for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
            {
                const QuicPacketProtection& protection = ends[side]->protection();
                open |= protection.path_open(slot) ? id_bit(protection.path_id(slot)) : 0U;
            }
            CRD_FUZZ_REQUIRE((open & closed_ids[side]) == 0U);
            closed_ids[side] |= opened_ids[side] & ~open;
            opened_ids[side] |= open;
            for (u64 path_id = 0U; path_id < 64U; ++path_id)
            {
                abandoned_ids[side] |= (ends[side]->abandon_of(path_id) != nullptr) ? id_bit(path_id) : 0U;
            }
        }
    }

    // `slot` is the server's slot the datagram went on.
    void check_sent(usize from, ConstSpan<u8> datagram, usize slot)
    {
        QuicHeader header;
        if (from == 0U && parse_quic_header(datagram, sizeof(kServerCid), header) == QuicHeaderError::None &&
            header.type == QuicPacketType::Initial)
        {
            CRD_FUZZ_REQUIRE(datagram.size() >= kQuicMinInitialDatagram);
        }
        // Before its first path is proven (a later one, and a further path ID's, is held to its own count below).
        if (from == 1U && !server.address_validated() && server.counters().migrations == 0U &&
            server.counters().paths_opened == 0U)
        {
            CRD_FUZZ_REQUIRE(server.counters().bytes_sent <= 3U * server.counters().bytes_received);
        }
        const usize to = path_of(server.sent_path());
        const u32 socket = server.sent_path().socket;
        if (from == 1U && to < kClientAddresses)
        {
            // What a slot sends to a 4-tuple it has not proven is at most three times what that slot was given from
            // there, whatever another slot proved or was given (RFC 10069 section 7.2).
            CRD_FUZZ_REQUIRE(socket < 2U);
            const usize index = server.paths(slot).find(server.sent_path());
            CRD_FUZZ_REQUIRE(index != kQuicNoPath);
            if (!server.paths(slot).validated(index))
            {
                SlotCredit& credit = credit_of(slot);
                credit.sent[socket][to] += datagram.size();
                CRD_FUZZ_REQUIRE(credit.sent[socket][to] <= 3U * credit.received[socket][to]);
            }
            check_credit();
        }
    }

    // Slot `slot`'s credit, started again once the slot holds another path ID.
    SlotCredit& credit_of(usize slot)
    {
        SlotCredit& credit = credits[slot];
        const u64 path_id = server.protection().path_id(slot);
        if (credit.path_id != path_id)
        {
            credit = SlotCredit{};
            credit.path_id = path_id;
        }
        return credit;
    }

    // The server's slot a client datagram counts toward, as it reads one (datagram_slot()): the slot of its first
    // packet's path ID; the leading slot for a long header or an ID the server no longer routes; kQuicNoPath while
    // that path ID is open in no slot (it may open on this datagram).
    [[nodiscard]] usize receiving_slot(const Flight& flight) const
    {
        const usize id_bytes = keyed ? kKeyedIdBytes : sizeof(kServerCid); // the server's IDs, all of one length
        const bool routed = flight.size > id_bytes && (flight.bytes[0] & 0x80U) == 0U &&
                            server_host.routes.find(ConstSpan<u8>(flight.bytes + 1U, id_bytes)) != kNoConnection;
        if (routed)
        {
            return slot_of(server, flight.path_id);
        }
        usize lead = 0U; // slot 0's 4-tuples are always there; a further slot's only while open
        for (usize slot = 1U; slot < kQuicMaxPaths; ++slot)
        {
            lead = (server.protection().path_open(slot) && &server.paths(slot) == &server.paths()) ? slot : lead;
        }
        return lead;
    }

    // Each open slot of the server's holds no more credit for a 4-tuple it has not proven than three times what that
    // slot was given from there, less what it sent there.
    void check_credit()
    {
        for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
        {
            if (!server.protection().path_open(slot))
            {
                continue;
            }
            const QuicPaths& paths = server.paths(slot);
            for (usize index = 0U; index < kQuicMaxPaths; ++index)
            {
                const usize address = paths.used(index) ? path_of(paths.path(index)) : kClientAddresses;
                const u32 socket = paths.path(index).socket;
                if (address < kClientAddresses && socket < 2U && !paths.validated(index))
                {
                    const SlotCredit& credit = credit_of(slot);
                    CRD_FUZZ_REQUIRE(paths.credit(index) + credit.sent[socket][address] <=
                                     3U * credit.received[socket][address]);
                }
            }
        }
    }

    // The index of `path`'s peer among kClientPaths; kClientAddresses for none (the server's unbound first path).
    [[nodiscard]] static usize path_of(const QuicPath& path)
    {
        for (usize index = 0U; index < kClientAddresses; ++index)
        {
            if (path.peer == kClientPaths[index].peer)
            {
                return index;
            }
        }
        return kClientAddresses;
    }

    // The address the server sees the client's socket `socket` at.
    [[nodiscard]] usize address_of(u32 socket) const
    {
        return (socket == 0U) ? client_path : kSocketZeroPaths + socket - 1U;
    }

    // A rebinding or a move of the client's begins an epoch.
    void next_epoch()
    {
        ++epoch;
        for (u64& ids : opened_from)
        {
            ids = 0U;
        }
    }

    void follow_moves()
    {
        if (client.counters().migrations != client_moves)
        {
            client_moves = client.counters().migrations;
            next_epoch();
        }
    }

    void flush()
    {
        QuicConnection* const ends[2] = {&client, &server};
        for (usize from = 0U; from < 2U; ++from)
        {
            // Each end drains within the guard: what it sends at one instant is bounded by its window, pacer and
            // owed frames, so one still sending after 64 datagrams sends packets that free nothing, one after another.
            bool drained = false;
            for (usize guard = 0U; guard < 64U && !drained; ++guard)
            {
                u8 out[kDatagram];
                QuicConnection& end = *ends[from];
                const QuicConnectionCounters before = end.counters();
                u64 numbers[kQuicSpaces] = {};
                for (usize space = 0U; space < kQuicSpaces; ++space)
                {
                    numbers[space] = end.next_packet_number(static_cast<QuicSpace>(space));
                }
                u64 slot_numbers[kQuicMaxPaths] = {}; // each slot's 1-RTT numbers: a datagram is one slot's
                for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
                {
                    slot_numbers[slot] = end.next_packet_number(QuicSpace::Application, slot);
                }
                follow_moves();
                const usize size = end.send(Span<u8>(out, sizeof(out)), now, (from == 0U) ? held : 0U);
                follow_moves();
                usize sent_slot = 0U; // the handshake's, for a datagram of no 1-RTT packet
                u64 sent_id = 0U;
                for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
                {
                    const bool moved = end.next_packet_number(QuicSpace::Application, slot) != slot_numbers[slot];
                    sent_slot = moved ? slot : sent_slot;
                    sent_id = moved ? end.protection().path_id(slot) : sent_id;
                }
                if (size == 0U)
                {
                    if (end.held_back())
                    {
                        CRD_FUZZ_REQUIRE(std::memcmp(&before, &end.counters(), sizeof(before)) == 0);
                        for (usize space = 0U; space < kQuicSpaces; ++space)
                        {
                            CRD_FUZZ_REQUIRE(end.next_packet_number(static_cast<QuicSpace>(space)) == numbers[space]);
                        }
                    }
                    drained = true;
                    continue;
                }
                CRD_FUZZ_REQUIRE(((held >> end.sent_path().socket) & 1U) == 0U || from == 1U);
                check_sent(from, ConstSpan<u8>(out, size), sent_slot);
                // Over paths a server datagram to an address socket 0 left is lost: its NAT mapping is gone. Sockets 1
                // and 2 keep theirs.
                const usize address =
                    (from == 0U) ? address_of(client.sent_path().socket) : path_of(server.sent_path());
                const bool reaches = from == 0U || !over_paths() || address == client_path ||
                                     address >= kSocketZeroPaths;
                if (count < kFlights && reaches)
                {
                    std::memcpy(flights[count].bytes, out, size);
                    flights[count].size = size;
                    flights[count].to = 1U - from;
                    flights[count].ecn = ends[from]->marking();
                    flights[count].path = address;
                    flights[count].path_id = sent_id;
                    flights[count].epoch = epoch;
                    const bool preferred = client.sent_path().peer == kPreferredAddress;
                    flights[count].server_socket =
                        (from == 0U) ? static_cast<u32>(preferred) : server.sent_path().socket;
                    ++count;
                }
            }
            CRD_FUZZ_REQUIRE(drained);
        }
    }

    void deliver(usize index)
    {
        follow_moves();
        Flight& flight = flights[index];
        QuicConnection& to = (flight.to == 0U) ? client : server;
        if (flight.to == 1U && to.state() == QuicConnectionState::Idle)
        {
            QuicHeader header;
            if (parse_quic_header(ConstSpan<u8>(flight.bytes, flight.size), sizeof(kServerCid), header) !=
                    QuicHeaderError::None ||
                header.type != QuicPacketType::Initial)
            {
                return;
            }
            QuicAccept accept = accept_of(header.dcid, header.scid);
            const ConstSpan<u8> retried(kRetryCid, sizeof(kRetryCid));
            accept.retried = retry_sent && header.dcid.size() == retried.size() &&
                             std::memcmp(header.dcid.data(), retried.data(), retried.size()) == 0;
            accept.original_dcid = accept.retried ? ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)) : header.dcid;
            accept.validated = accept.retried;
            if (!to.accept(accept, now))
            {
                return;
            }
        }
        if (flight.to == 1U && over_paths())
        {
            const u32 socket = flight.server_socket;
            const QuicPath from{socket, kClientPaths[flight.path].peer};
            // The slot that reads it counts it; a 4-tuple that slot did not hold starts its count again. The unbound
            // first path counts every datagram until one opens, and then belongs to that one's address (S55).
            const bool unbound = server.path().peer.port == 0U;
            const usize before = receiving_slot(flight);
            const bool held_before = before != kQuicNoPath && server.paths(before).find(from) != kQuicNoPath;
            unbound_bytes += unbound ? flight.size : 0U;
            const u64 opened = server.counters().packets_opened;
            to.receive(from, Span<u8>(flight.bytes, flight.size), flight.ecn, now);
            const bool current = flight.epoch == epoch && server.counters().packets_opened != opened;
            opened_from[flight.path] |= current ? id_bit(flight.path_id) : 0U;
            const usize reading = (before != kQuicNoPath) ? before : slot_of(server, flight.path_id);
            if (!unbound && reading != kQuicNoPath && server.protection().path_open(reading))
            {
                SlotCredit& credit = credit_of(reading);
                if (!held_before)
                {
                    credit.received[socket][flight.path] = 0U;
                    credit.sent[socket][flight.path] = 0U;
                }
                credit.received[socket][flight.path] += flight.size;
            }
            const usize bound = path_of(server.path());
            if (unbound && bound < 4U)
            {
                credit_of(0U).received[server.path().socket][bound] = unbound_bytes;
            }
            check_credit();
            // The server's datagrams move to its preferred socket only once it validated the path there, after its
            // client moved there, and leave every other socket's path for good (section 9.6.2); there it follows its
            // client as anywhere. These are path ID 0's 4-tuples, while it is open (a further path ID keeps its own,
            // RFC 10069 section 3.1.2).
            const usize zero = slot_of(server, 0U);
            if (server.on_preferred_address() && !server_preferred)
            {
                server_preferred = true;
                // The path it moved to is validated: active, or left at once for a rebinding the same datagram
                // showed, and then the fallback.
                if (zero != kQuicNoPath)
                {
                    const QuicPaths& paths = server.paths(zero);
                    const usize back = paths.fallback();
                    CRD_FUZZ_REQUIRE(paths.validated(paths.active()) ||
                                     (back != kQuicNoPath && paths.validated(back) && paths.path(back).socket == 1U));
                }
                CRD_FUZZ_REQUIRE(client.on_preferred_address());
            }
            if (zero != kQuicNoPath)
            {
                const QuicPaths& paths = server.paths(zero);
                for (usize tuple = 0U; server_preferred && tuple < kQuicMaxPaths; ++tuple)
                {
                    CRD_FUZZ_REQUIRE(!paths.used(tuple) || paths.path(tuple).socket == 1U);
                }
                CRD_FUZZ_REQUIRE(paths.path(paths.active()).socket == (server_preferred ? 1U : 0U));
            }
        }
        else if (over_paths())
        {
            const u32 socket = (flight.path >= kSocketZeroPaths && flight.path < kClientAddresses)
                                   ? static_cast<u32>(flight.path - kSocketZeroPaths + 1U)
                                   : 0U;
            const Endpoint& server_at = (flight.server_socket == 1U) ? kPreferredAddress : kServerAddress;
            to.receive(QuicPath{socket, server_at}, Span<u8>(flight.bytes, flight.size), flight.ecn, now);
            const usize zero = slot_of(client, 0U);
            CRD_FUZZ_REQUIRE(!client.on_preferred_address() || zero == kQuicNoPath ||
                             client.paths(zero).path(client.paths(zero).active()).peer == kPreferredAddress);
        }
        else
        {
            to.receive(Span<u8>(flight.bytes, flight.size), flight.ecn, now);
        }
        check_closed(to);
        // Whatever the path does to codepoints, a failed validation stays failed for the path's life: a confirmed new
        // path, where recovery's path floor moves with it, validates again (RFC 9000 section 13.4.2). The floor stays
        // where no packet of the old path is held, so the path's change is read from the moves and validations that
        // restart it. Each slot sending holds its own path ID's validation.
        const u64 changes = to.counters().migrations + to.counters().path_validations;
        for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
        {
            const QuicPathControl* const control = to.path_control(slot);
            const EcnSeen now_seen{(control != nullptr) ? to.protection().path_id(slot) : kNoPathId,
                                   control != nullptr && control->ecn().state() == QuicEcnState::Failed,
                                   to.recovery().path_floor(slot), changes};
            const EcnSeen& seen = ecn_seen[flight.to][slot];
            CRD_FUZZ_REQUIRE(now_seen.failed || !seen.failed || now_seen.path_id != seen.path_id ||
                             now_seen.floor != seen.floor || changes != seen.changes);
            ecn_seen[flight.to][slot] = now_seen;
        }
        check_hosts();
    }

    void remove(usize index) { flights[index] = flights[--count]; }

    // Each end's slots: the path ID sending there, whether its ECN validation failed, the path it failed on
    // (QuicRecovery::path_floor) and the end's moves and validations then.
    struct EcnSeen
    {
        u64 path_id = kNoPathId;
        bool failed = false;
        u64 floor = 0U;
        u64 changes = 0U;
    };
    EcnSeen ecn_seen[2][kQuicMaxPaths] = {};

    void timers()
    {
        for (QuicConnection* end : {&client, &server})
        {
            if (end->next_timer_ns() <= now)
            {
                end->on_timeout(now);
                check_closed(*end);
            }
        }
        check_hosts();
    }

    // A clean path: everything delivered in order, time moving to each timer, for at most `events` timers. The
    // script may have taught both ends an RTT of minutes, so the bound counts events, not time: the idle timeout
    // ends every connection that neither confirms nor closes.
    void settle(usize events)
    {
        settle_until(events, [this] { return established(); });
    }

    // The same until `done` holds after a delivery round, past the handshake: a challenge or frame the script lost
    // is sent again on its timer.
    template <typename Done> void settle_until(usize events, Done done)
    {
        usize stalled = 0U;
        for (usize event = 0U; event < events; ++event)
        {
            flush();
            while (count > 0U)
            {
                deliver(0U);
                remove(0U);
                flush();
            }
            const u64 next =
                (client.next_timer_ns() < server.next_timer_ns()) ? client.next_timer_ns() : server.next_timer_ns();
            if (next == kNeverNs || done())
            {
                return;
            }
            stalled = (next <= now) ? stalled + 1U : 0U;
            CRD_FUZZ_REQUIRE(stalled < 64U); // a timer due that nothing serves would spin the caller's loop
            now = (next > now) ? next : now;
            timers();
        }
    }
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

bool finished(const QuicConnection& end)
{
    return end.state() == QuicConnectionState::Closing || end.state() == QuicConnectionState::Draining ||
           end.state() == QuicConnectionState::Closed;
}

constexpr u64 kTransferBytes = 5000U;        // more than every stream and connection window of config_for
constexpr u64 kTransferWaitNs = 5000U * kMs; // a sixth of config_for's idle timeout

// After the script, a confirmed pair carries a stream of more than every window over the clean path: each byte the
// server reads is the byte written, and all of it arrives with the FIN, or one side has closed.
// Address reports (draft-ietf-quic-address-discovery-01): none arrives unasked, and every report a path holds names an
// address the other end has (a client's addresses, the server's two), as each end saw it.
void check_observations(const Pair& pair)
{
    const auto client_address = [](const Endpoint& at)
    {
        bool known = at.port == 0U;
        for (const QuicPath& path : kClientPaths)
        {
            known = known || at == path.peer;
        }
        return known;
    };
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            if (pair.client.protection().path_open(slot) && pair.client.paths(slot).used(index))
            {
                const Endpoint& seen = pair.client.paths(slot).observed(index);
                CRD_FUZZ_REQUIRE(client_address(seen));
            }
            if (pair.server.protection().path_open(slot) && pair.server.paths(slot).used(index))
            {
                const Endpoint& seen = pair.server.paths(slot).observed(index);
                CRD_FUZZ_REQUIRE(seen.port == 0U || seen == kServerAddress || seen == kPreferredAddress);
            }
        }
    }
    CRD_FUZZ_REQUIRE(extensions() || (pair.client.counters().address_observations == 0U &&
                                      pair.server.counters().address_observations == 0U));
    // Multipath is negotiated exactly when both ends offered it (RFC 10069; design S78). Path IDs open only as the
    // client adds them, the server's on their first packet; each end's limit rises by one per path ID closed (section
    // 3.2.1), and its peer's view of it is never above it. Each path ID's IDs stay within the peer's
    // active_connection_id_limit.
    CRD_FUZZ_REQUIRE(pair.client.counters().paths_opened == pair.added);
    CRD_FUZZ_REQUIRE(pair.server.counters().paths_opened <= pair.added);
    for (const QuicConnection* end : {&pair.client, &pair.server})
    {
        const QuicConnection& other = (end == &pair.client) ? pair.server : pair.client;
        CRD_FUZZ_REQUIRE(end->multipath() == (extensions() && end->peer_parameters_known()));
        const u64 limit = end->multipath() ? kMaxPathId + end->counters().paths_closed : 0U;
        CRD_FUZZ_REQUIRE(end->local_max_path_id() == limit);
        CRD_FUZZ_REQUIRE(end->multipath() ? (end->peer_max_path_id() >= kMaxPathId &&
                                             end->peer_max_path_id() <= other.local_max_path_id())
                                          : end->peer_max_path_id() == 0U);
        for (u64 path_id = 1U; path_id <= end->local_max_path_id() || path_id <= kMaxPathId; ++path_id)
        {
            CRD_FUZZ_REQUIRE(end->multipath() || end->local_cid_count(path_id) == 0U);
            const QuicSide peer = (end == &pair.client) ? QuicSide::Server : QuicSide::Client;
            CRD_FUZZ_REQUIRE(end->local_cid_count(path_id) <= id_limit(peer));
        }
    }
}

// PATH_STATUS (RFC 10069 section 4.3): once what is owed has arrived, each end holds for every path ID the newest
// status the other set (a lost frame sent again while newest, an older one never after it), or none. A path ID either
// end abandoned has no status sent any more (section 3.4), so only the others are compared.
void check_path_statuses(Pair& pair)
{
    const auto agree = [&pair]
    {
        bool same = true;
        const u64 abandoned = pair.abandoned_ids[0] | pair.abandoned_ids[1];
        const u64 client_limit = pair.client.local_max_path_id();
        const u64 limit =
            (client_limit > pair.server.local_max_path_id()) ? client_limit : pair.server.local_max_path_id();
        for (u64 path_id = 0U; path_id <= limit; ++path_id)
        {
            same = same && ((abandoned & id_bit(path_id)) != 0U ||
                            (pair.client.peer_path_status(path_id) == pair.server.path_status(path_id) &&
                             pair.server.peer_path_status(path_id) == pair.client.path_status(path_id)));
        }
        return same;
    };
    const auto ended = [&pair]
    {
        return finished(pair.client) || finished(pair.server);
    };
    pair.settle_until(4096U, [&] { return agree() || ended(); });
    CRD_FUZZ_REQUIRE(agree() || ended());
}

void transfer(Pair& pair)
{
    QuicStreamSet& client = pair.client.streams();
    QuicStreamSet& server = pair.server.streams();
    u64 id = 0U;
    CRD_FUZZ_REQUIRE(client.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    u64 read = 0U;
    bool fin = false;
    for (usize round = 0U; round < 512U && !fin && !finished(pair.client) && !finished(pair.server); ++round)
    {
        u8 chunk[512];
        for (usize at = 0U; at < sizeof(chunk); ++at)
        {
            chunk[at] = static_cast<u8>((written + at) * 7U);
        }
        const u64 left = kTransferBytes - written;
        const usize want = (left < sizeof(chunk)) ? static_cast<usize>(left) : sizeof(chunk);
        usize taken = 0U;
        const bool fin_here = written + want == kTransferBytes;
        if (client.write(id, ConstSpan<u8>(chunk, want), fin_here, taken) == QuicStreamError::None)
        {
            written += taken;
        }
        pair.settle(8U);
        for (QuicStreamRead data = server.peek(id); !data.data.empty() || data.fin; data = server.peek(id))
        {
            for (usize at = 0U; at < data.data.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(data.data[at] == static_cast<u8>((read + at) * 7U));
            }
            read += data.data.size();
            CRD_FUZZ_REQUIRE(server.consume(id, data.data.size()) == QuicStreamError::None);
            if (data.fin)
            {
                fin = true;
                break;
            }
        }
        // Time moves to the next timer (a delayed ACK frees the send buffer), kTransferWaitNs at most: a pair with
        // nothing outstanding has only its idle timeout left, which a writer with more to send never waits out (paths
        // that acknowledge at once leave no timer between writes).
        const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns()) ? pair.client.next_timer_ns()
                                                                                     : pair.server.next_timer_ns();
        const u64 wait = pair.now + kTransferWaitNs;
        const u64 until = (next < wait) ? next : wait;
        pair.now = (until > pair.now) ? until : pair.now;
        pair.timers();
    }
    CRD_FUZZ_REQUIRE((fin && read == kTransferBytes) || finished(pair.client) || finished(pair.server));
}

// Once its stream arrived, the open client keeps its lowest path ID still sending and abandons the others (RFC 10069
// section 3.4), each abandonment taken, so that the checks after it read one path; returns the path ID it kept.
u64 keep_one_path(Pair& pair)
{
    const QuicPacketProtection& protection = pair.client.protection();
    usize kept = kQuicNoPath;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        const bool lower = kept == kQuicNoPath || protection.path_id(slot) < protection.path_id(kept);
        kept = (pair.client.path_control(slot) != nullptr && lower) ? slot : kept;
    }
    CRD_FUZZ_REQUIRE(kept != kQuicNoPath);
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (slot != kept && pair.client.path_control(slot) != nullptr)
        {
            const u64 path_id = protection.path_id(slot);
            CRD_FUZZ_REQUIRE(pair.client.abandon_path(path_id, 0U, pair.now) == QuicAbandon::Abandoned);
            pair.abandoned_ids[0] |= id_bit(path_id);
        }
    }
    CRD_FUZZ_REQUIRE(sending_slots(pair.client) == 1U);
    return protection.path_id(kept);
}

constexpr u64 kDatagrams = 12U;

// Then datagrams of rising sizes from the client, every other one redundant, their ids from `base`: on one path each
// the server reads arrives once and byte-exact, no copy is sent, and every one is reported once, acknowledged or lost
// (or one side closes).
void exchange_datagrams(Pair& pair, u64 base)
{
    QuicDatagrams& out = pair.client.datagrams();
    QuicDatagrams& in = pair.server.datagrams();
    bool arrived[kDatagrams] = {};
    bool settled[kDatagrams] = {};
    u64 queued = 0U;
    u64 reported = 0U;
    for (usize round = 0U; round < 256U && reported < kDatagrams && !finished(pair.client) && !finished(pair.server);
         ++round)
    {
        u8 payload[600];
        u64 id = 0U;
        while (queued < kDatagrams)
        {
            const usize bytes = static_cast<usize>(1U + (queued * 47U) % out.max_payload());
            payload[0] = static_cast<u8>(queued);
            for (usize at = 1U; at < bytes; ++at)
            {
                payload[at] = static_cast<u8>((queued * 31U) + at);
            }
            QuicDatagramSend how;
            how.redundant = (queued & 1U) != 0U;
            if (out.queue(ConstSpan<u8>(payload, bytes), pair.now, id, how) != QuicDatagramError::None)
            {
                break;
            }
            CRD_FUZZ_REQUIRE(id == base + queued);
            ++queued;
        }
        pair.settle(8U);
        ConstSpan<u8> held;
        for (; in.peek(held); in.pop())
        {
            const u64 which = held.empty() ? kDatagrams : held[0];
            CRD_FUZZ_REQUIRE(which < queued && !arrived[which]);
            CRD_FUZZ_REQUIRE(held.size() == static_cast<usize>(1U + (which * 47U) % out.max_payload()));
            for (usize at = 1U; at < held.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(held[at] == static_cast<u8>((which * 31U) + at));
            }
            arrived[which] = true;
        }
        QuicDatagramReport report;
        while (out.poll(report))
        {
            const u64 index = report.id - base;
            CRD_FUZZ_REQUIRE(report.id >= base && index < queued && report.outcome != QuicDatagramOutcome::Expired);
            CRD_FUZZ_REQUIRE((report.outcome == QuicDatagramOutcome::Lost || arrived[index]) && !settled[index]);
            settled[index] = true;
            ++reported;
        }
        const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns()) ? pair.client.next_timer_ns()
                                                                                     : pair.server.next_timer_ns();
        pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
        pair.timers();
    }
    CRD_FUZZ_REQUIRE(in.counters().dropped == 0U && out.counters().copies == 0U);
    CRD_FUZZ_REQUIRE(reported == kDatagrams || finished(pair.client) || finished(pair.server));
}

// A stateless reset reaching a confirmed client: a forged token changes nothing, the token of the ID it sends to
// drains it.
void stateless_reset(Pair& pair)
{
    QuicConnection& client = pair.client;
    if (client.state() != QuicConnectionState::Established)
    {
        return;
    }
    // The ID it sends to is used from its next packet on. A client whose window its silent server left full sends
    // none (over paths a server may have closed without a word: section 9.3.2), and only a used ID's token counts.
    const u64 sent = client.counters().datagrams_sent;
    u64 numbers[kQuicMaxPaths] = {};
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        numbers[slot] = client.next_packet_number(QuicSpace::Application, slot);
    }
    client.ping();
    pair.flush();
    if (client.counters().datagrams_sent == sent)
    {
        return;
    }
    usize slot = 0U; // a path that sent: its ID is used
    while (slot + 1U < kQuicMaxPaths && client.next_packet_number(QuicSpace::Application, slot) == numbers[slot])
    {
        ++slot;
    }
    u8 datagram[48];
    std::memset(datagram, 0x3C, sizeof(datagram));
    datagram[0] = 0x4BU;
    const Span<u8> token(datagram + sizeof(datagram) - kQuicResetTokenBytes, kQuicResetTokenBytes);
    CRD_FUZZ_REQUIRE(client.next_packet_number(QuicSpace::Application, slot) != numbers[slot]);
    CRD_FUZZ_REQUIRE(pair.server_host.key.token(client.peer_cid(slot), token) == CryptoError::None);
    token[0] ^= 0x01U;
    CRD_FUZZ_REQUIRE(!client.stateless_reset(ConstSpan<u8>(datagram), pair.now));
    token[0] ^= 0x01U;
    client.receive(Span<u8>(datagram), Ecn::NotEct, pair.now);
    CRD_FUZZ_REQUIRE(client.state() == QuicConnectionState::Draining && client.close_info().stateless_reset);
}

// Time moves to the next timer of either end (the script may have taught both an RTT of minutes) and the clean path
// runs.
void next_timer(Pair& pair)
{
    const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns()) ? pair.client.next_timer_ns()
                                                                                 : pair.server.next_timer_ns();
    pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
    pair.timers();
    pair.settle(8U);
}

// A first clean connection that leaves `tickets` a ticket for the next one (after the handshake, at the Application
// level).
void take_ticket(QuicVersion version, bool switching, Tickets& tickets)
{
    Pair first(version, switching, &tickets);
    CRD_FUZZ_REQUIRE(first.client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                                          ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), first.now));
    first.settle(4096U);
    CRD_FUZZ_REQUIRE(first.client.confirmed() && first.server.confirmed());
    for (usize round = 0U; round < 64U && !tickets.holds(); ++round)
    {
        next_timer(first);
    }
    CRD_FUZZ_REQUIRE(tickets.holds());
}

// The early datagram (id 0): reported once, Acked only when the server holds it, and marked replayable there when
// 0-RTT was accepted (it can only have come in a 0-RTT packet: it was queued before the first flight).
void settle_early_datagram(Pair& pair, bool accepted)
{
    QuicDatagramReport report;
    bool reported = false;
    for (usize round = 0U; round < 256U && !reported && !finished(pair.client) && !finished(pair.server); ++round)
    {
        reported = pair.client.datagrams().poll(report);
        if (!reported)
        {
            next_timer(pair);
        }
    }
    if (!reported)
    {
        CRD_FUZZ_REQUIRE(finished(pair.client) || finished(pair.server));
        return;
    }
    CRD_FUZZ_REQUIRE(report.id == 0U && report.outcome != QuicDatagramOutcome::Expired);
    CRD_FUZZ_REQUIRE(accepted || report.outcome == QuicDatagramOutcome::Lost);
    ConstSpan<u8> held;
    const bool arrived = pair.server.datagrams().peek(held);
    CRD_FUZZ_REQUIRE(report.outcome == QuicDatagramOutcome::Lost || arrived);
    if (arrived)
    {
        CRD_FUZZ_REQUIRE(accepted && held.size() == sizeof(kEarlyPayload) &&
                         std::memcmp(held.data(), kEarlyPayload, held.size()) == 0 &&
                         pair.server.datagrams().oldest_early());
        pair.server.datagrams().pop();
    }
}

// The server's reads of the early stream after the script: whole and unchanged, its replayable marks never past
// what came early.
void read_early(Pair& pair)
{
    QuicStreamSet& server = pair.server.streams();
    u64 read = 0U;
    bool fin = false;
    for (usize round = 0U; round < 512U && !fin && !finished(pair.client) && !finished(pair.server); ++round)
    {
        for (QuicStreamRead data = server.peek(0U); !data.data.empty() || data.fin; data = server.peek(0U))
        {
            CRD_FUZZ_REQUIRE(data.early_bytes <= data.data.size() && read + data.early_bytes <= kEarlyBytes);
            for (usize at = 0U; at < data.data.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(data.data[at] == static_cast<u8>((read + at) * 13U));
            }
            read += data.data.size();
            CRD_FUZZ_REQUIRE(server.consume(0U, data.data.size()) == QuicStreamError::None);
            fin = data.fin;
            if (fin)
            {
                break;
            }
        }
        if (!fin)
        {
            next_timer(pair);
        }
    }
    CRD_FUZZ_REQUIRE((fin && read == kEarlyBytes) || finished(pair.client) || finished(pair.server));
}

void run_script(QuicVersion version, bool switching, bool early, bool keyed, ConstSpan<u8> script)
{
    Tickets tickets;
    if (early)
    {
        CRD_FUZZ_REQUIRE(identity().fresh_filter());
        take_ticket(version, switching, tickets);
    }
    Pair pair(version, switching, early ? &tickets : nullptr, keyed);
    CRD_FUZZ_REQUIRE(pair.client.ready() && pair.server.ready());
    const usize allocations = pair.allocator.peak_bytes();
    if (keyed)
    {
        open_keyed(pair.client, pair.now);
        open_keyed(pair.server, pair.now);
    }
    else
    {
        CRD_FUZZ_REQUIRE(pair.client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                                             ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), pair.now));
    }
    // A switch runs the first connection in the other version: its ticket is offered by none (RFC 9369 section 5).
    const bool offered = pair.client.handshake().early_data() == TlsEarlyData::Offered;
    CRD_FUZZ_REQUIRE(offered == (early && !switching));
    if (offered)
    {
        u64 id = 0U;
        CRD_FUZZ_REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None && id == 0U);
        u8 bytes[kEarlyBytes];
        for (usize at = 0U; at < sizeof(bytes); ++at)
        {
            bytes[at] = static_cast<u8>(at * 13U);
        }
        usize taken = 0U;
        CRD_FUZZ_REQUIRE(pair.client.streams().write(0U, ConstSpan<u8>(bytes), true, taken) == QuicStreamError::None &&
                         taken == kEarlyBytes);
        u64 datagram = 0U;
        CRD_FUZZ_REQUIRE(pair.client.datagrams().queue(ConstSpan<u8>(kEarlyPayload), pair.now, datagram) ==
                         QuicDatagramError::None);
    }
    for (usize at = 0U; at < script.size(); ++at)
    {
        pair.flush();
        const u8 op = script[at];
        const usize pick = (pair.count == 0U) ? 0U : (op >> 3U) % pair.count;
        if (op == 0xFEU)
        {
            static_cast<void>(pair.server.rotate_connection_ids());
            continue;
        }
        if (op == 0xFDU)
        {
            pair.retry();
            continue;
        }
        if (op == 0xFCU && over_paths())
        {
            pair.client_path = (pair.client_path + 1U) % 4U;
            pair.next_epoch();
            continue;
        }
        if ((op == 0xFAU || op == 0xF9U) && over_paths())
        {
            // The client's next socket of three for path ID 0, whose 4-tuples these move (RFC 10069 section 3.1.2):
            // moved to, or probed. Anything but Started changes nothing, and nothing starts once it is abandoned.
            const usize zero = slot_of(pair.client, 0U);
            const QuicPaths& paths = pair.client.paths((zero != kQuicNoPath) ? zero : 0U);
            const QuicPath before = paths.path(paths.active());
            const QuicPath next{(before.socket + 1U) % 3U, before.peer};
            const QuicMigration result =
                (op == 0xFAU) ? pair.client.migrate(next, pair.now) : pair.client.probe_path(next, pair.now);
            const bool moved = result == QuicMigration::Started && op == 0xFAU;
            CRD_FUZZ_REQUIRE(paths.path(paths.active()) == (moved ? next : before));
            CRD_FUZZ_REQUIRE(result != QuicMigration::Started || paths.find(next) != kQuicNoPath);
            CRD_FUZZ_REQUIRE(result != QuicMigration::Started ||
                             (zero != kQuicNoPath && pair.client.abandon_of(0U) == nullptr));
            continue;
        }
        if (op == 0xF7U && over_paths())
        {
            // The server's preferred address from path ID 0's socket: moved to once validated (section 9.6.2).
            const usize zero = slot_of(pair.client, 0U);
            const QuicPaths& paths = pair.client.paths((zero != kQuicNoPath) ? zero : 0U);
            const QuicPath before = paths.path(paths.active());
            const QuicPath preferred{before.socket, kPreferredAddress};
            const usize known = paths.find(preferred);
            const bool proven = known != kQuicNoPath && paths.proven(known);
            const QuicMigration result = pair.client.migrate(preferred, pair.now);
            const bool moved = result == QuicMigration::Started && proven;
            CRD_FUZZ_REQUIRE(paths.path(paths.active()) == (moved ? preferred : before));
            CRD_FUZZ_REQUIRE(result != QuicMigration::Started || paths.find(preferred) != kQuicNoPath);
            CRD_FUZZ_REQUIRE(result != QuicMigration::Started ||
                             (zero != kQuicNoPath && pair.client.abandon_of(0U) == nullptr));
            CRD_FUZZ_REQUIRE(!moved || pair.client.on_preferred_address());
            continue;
        }
        if (op == 0xF5U && at + 1U < script.size())
        {
            // Both ends learn their handover phase (S76) from the next byte: a period of 1 to 4 s, guards of 20 ms a
            // step each side (past the share at times; 0 learns nothing). A learning refused changes nothing.
            const u8 shape = script[++at];
            HandoverLearning learning;
            if (shape != 0U)
            {
                learning.period_ns = kHandoverMinPeriodNs * (1U + (shape & 3U));
                learning.before_ns = static_cast<u64>((shape >> 2U) & 7U) * 20000000U;
                learning.after_ns = static_cast<u64>(shape >> 5U) * 20000000U;
            }
            const bool valid = handover_learning_valid(learning);
            for (QuicConnection* end : {&pair.client, &pair.server})
            {
                const HandoverLearning before = end->handover_learning();
                const HandoverSchedule schedule = end->handover();
                CRD_FUZZ_REQUIRE(end->set_handover_learning(learning) == valid);
                CRD_FUZZ_REQUIRE(end->handover_learning() == (valid ? learning : before));
                CRD_FUZZ_REQUIRE(end->handover() == (valid ? HandoverSchedule{} : schedule));
            }
            continue;
        }
        if (op == 0xF6U && at + 1U < script.size())
        {
            // Both ends' handover schedule (handover.hpp): an event every second, one now, guards from the next byte
            // (8 ms a step each side, past the share at times; 0 clears it). A schedule refused changes nothing.
            const u8 guards = script[++at];
            HandoverSchedule schedule;
            if (guards != 0U)
            {
                schedule.period_ns = kHandoverMinPeriodNs;
                schedule.phase_ns = pair.now % kHandoverMinPeriodNs;
                schedule.before_ns = static_cast<u64>(guards & 0x0FU) * 8000000U;
                schedule.after_ns = static_cast<u64>(guards >> 4U) * 8000000U;
            }
            const bool valid = handover_schedule_valid(schedule);
            for (QuicConnection* end : {&pair.client, &pair.server})
            {
                const HandoverSchedule before = end->handover();
                const HandoverLearning learning = end->handover_learning();
                CRD_FUZZ_REQUIRE(end->set_handover(schedule) == valid);
                CRD_FUZZ_REQUIRE(end->handover() == (valid ? schedule : before));
                CRD_FUZZ_REQUIRE(end->handover_learning() == (valid ? HandoverLearning{} : learning));
            }
            continue;
        }
        if (op == 0xF4U && at + 1U < script.size())
        {
            // One end's path choice and its status for a path ID from the next byte (RFC 10069 sections 3.3 and
            // 4.3): bit 0 the end, bit 1 backup, bits 2 and 3 the path ID (kMaxPathId + 1 is above both limits), bits
            // 4 and 5 the choice (2 and 3 are none), bits 6 and 7 its RTT refresh (0 kept, 1 none, 2 the shortest,
            // 3 below it: refused). Every ID space up to the end's limit is open but those abandoned, which name no
            // path to use any more. Anything refused changes nothing.
            const u8 shape = script[++at];
            const usize side = shape & 1U;
            QuicConnection& end = (side == 0U) ? pair.client : pair.server;
            const bool backup = (shape & 2U) != 0U;
            const u64 path_id = (shape >> 2U) & 3U;
            const auto choice = static_cast<QuicPathChoice>((shape >> 4U) & 3U);
            const QuicPathChoice was = end.path_choice();
            const bool valid = choice <= QuicPathChoice::Turns;
            CRD_FUZZ_REQUIRE(end.set_path_choice(choice) == valid);
            CRD_FUZZ_REQUIRE(end.path_choice() == (valid ? choice : was));
            const u32 refresh = shape >> 6U;
            if (refresh != 0U)
            {
                const u64 refresh_ns = (refresh == 1U) ? 0U : kQuicGranularityNs - (refresh - 2U);
                const u64 kept = end.path_refresh_ns();
                CRD_FUZZ_REQUIRE(end.set_path_refresh(refresh_ns) == (refresh != 3U));
                CRD_FUZZ_REQUIRE(end.path_refresh_ns() == ((refresh != 3U) ? refresh_ns : kept));
            }
            QuicPathStatusChange expected = QuicPathStatusChange::Set;
            if (end.state() != QuicConnectionState::Established)
            {
                expected = QuicPathStatusChange::NotEstablished;
            }
            else if (!end.multipath())
            {
                expected = QuicPathStatusChange::NoMultipath;
            }
            else if (path_id > end.peer_max_path_id() || path_id > end.local_max_path_id() ||
                     (pair.abandoned_ids[side] & id_bit(path_id)) != 0U || end.abandon_of(path_id) != nullptr)
            {
                expected = QuicPathStatusChange::NotOpen;
            }
            const QuicPathStatus held = end.path_status(path_id);
            const QuicPathStatus wanted = backup ? QuicPathStatus::Backup : QuicPathStatus::Available;
            CRD_FUZZ_REQUIRE(end.set_path_status(path_id, backup) == expected);
            CRD_FUZZ_REQUIRE(end.path_status(path_id) == ((expected == QuicPathStatusChange::Set) ? wanted : held));
            continue;
        }
        if (op == 0xF2U && over_paths() && extensions() && at + 1U < script.size())
        {
            // The client opens a further path ID (RFC 10069 section 3.1) on its socket in the next byte's bits 0 and
            // 1 (3 is socket 0), to the server's address or, with bit 2, its preferred one. Started names a path ID
            // under both limits, never abandoned and never open before, now open in a slot that knows the path;
            // anything else changes no counter. A server opens none.
            const u8 shape = script[++at];
            const QuicPath path{static_cast<u32>((shape & 3U) % 3U),
                                ((shape & 4U) != 0U) ? kPreferredAddress : kServerAddress};
            u64 path_id = 1U;
            CRD_FUZZ_REQUIRE(pair.server.add_path(path, pair.now, path_id) == QuicMigration::NotClient);
            CRD_FUZZ_REQUIRE(path_id == 0U);
            QuicConnection& client = pair.client;
            const QuicConnectionCounters before = client.counters();
            const QuicMigration result = client.add_path(path, pair.now, path_id);
            const bool confirmed = client.state() == QuicConnectionState::Established && client.confirmed();
            CRD_FUZZ_REQUIRE((result == QuicMigration::NotConfirmed) == !confirmed);
            CRD_FUZZ_REQUIRE((result == QuicMigration::NoMultipath) == (confirmed && !client.multipath()));
            if (result == QuicMigration::Started)
            {
                CRD_FUZZ_REQUIRE(path_id != 0U && path_id <= client.local_max_path_id() &&
                                 path_id <= client.peer_max_path_id());
                CRD_FUZZ_REQUIRE(((pair.opened_ids[0] | pair.abandoned_ids[0]) & id_bit(path_id)) == 0U);
                const usize slot = slot_of(client, path_id);
                CRD_FUZZ_REQUIRE(slot != kQuicNoPath && client.paths(slot).find(path) != kQuicNoPath);
                CRD_FUZZ_REQUIRE(client.counters().paths_opened == before.paths_opened + 1U);
                ++pair.added;
                pair.check_hosts();
            }
            else
            {
                CRD_FUZZ_REQUIRE(path_id == 0U);
                CRD_FUZZ_REQUIRE(std::memcmp(&before, &client.counters(), sizeof(before)) == 0);
            }
            continue;
        }
        if (op == 0xF0U && extensions() && at + 1U < script.size())
        {
            // One end abandons a path ID from the next byte (RFC 10069 section 3.4): bit 0 the end, bits 1 to 3 the
            // path ID, bits 4 and 5 the error code. The answer follows from the state, the limits, the paths still
            // sending and the abandonments before (a free entry is left while a space is open and not abandoned);
            // an abandoned path ID's slot sends nothing more, and anything refused changes no counter.
            const u8 shape = script[++at];
            const usize side = shape & 1U;
            QuicConnection& end = (side == 0U) ? pair.client : pair.server;
            const u64 path_id = (shape >> 1U) & 7U;
            const u64 codes[4] = {0U, 0x3EU, kQuicMaxVarint, kQuicMaxVarint + 1U};
            const u64 code = codes[(shape >> 4U) & 3U];
            const usize slot = slot_of(end, path_id);
            const bool above = path_id > end.peer_max_path_id(); // a frame naming it is refused first (section 4)
            QuicAbandon expected = QuicAbandon::Abandoned;
            if (end.state() != QuicConnectionState::Established)
            {
                expected = QuicAbandon::NotEstablished;
            }
            else if (!end.multipath())
            {
                expected = QuicAbandon::NoMultipath;
            }
            else if (code > kQuicMaxVarint)
            {
                expected = QuicAbandon::BadError;
            }
            else if (!above && slot != kQuicNoPath && end.path_control(slot) != nullptr && sending_slots(end) == 1U)
            {
                expected = QuicAbandon::LastPath;
            }
            else if (above || path_id > end.local_max_path_id() || (pair.abandoned_ids[side] & id_bit(path_id)) != 0U ||
                     end.abandon_of(path_id) != nullptr)
            {
                expected = QuicAbandon::NotOpen;
            }
            const QuicConnectionCounters before = end.counters();
            CRD_FUZZ_REQUIRE(end.abandon_path(path_id, code, pair.now) == expected);
            if (expected == QuicAbandon::Abandoned)
            {
                const QuicPathAbandon* const entry = end.abandon_of(path_id);
                CRD_FUZZ_REQUIRE(entry != nullptr && entry->error_code == code);
                CRD_FUZZ_REQUIRE(end.counters().paths_abandoned == before.paths_abandoned + 1U);
                CRD_FUZZ_REQUIRE(slot == kQuicNoPath || end.path_control(slot) == nullptr);
                CRD_FUZZ_REQUIRE(sending_slots(end) != 0U);
                pair.abandoned_ids[side] |= id_bit(path_id);
            }
            else
            {
                CRD_FUZZ_REQUIRE(std::memcmp(&before, &end.counters(), sizeof(before)) == 0);
            }
            continue;
        }
        if (op == 0xF8U && over_paths())
        {
            pair.held = (pair.held + 1U) % 8U;
            continue;
        }
        if (op == 0xFBU && over_paths())
        {
            for (usize index = 0U; index < pair.count; ++index)
            {
                if (pair.flights[index].to == 1U && pair.count < kFlights)
                {
                    // The copy arrives first, from another address; the original follows from the client's.
                    Flight& copy = pair.flights[pair.count++];
                    copy = pair.flights[index];
                    copy.path = (copy.path + 2U) % 4U;
                    pair.deliver(pair.count - 1U);
                    pair.remove(pair.count - 1U);
                    break;
                }
            }
            continue;
        }
        switch (op & 7U)
        {
            case 0U:
                if (pair.count > 0U)
                {
                    pair.deliver(pick);
                    pair.remove(pick);
                }
                break;
            case 1U:
                if (pair.count > 0U)
                {
                    pair.flights[pick].ecn = static_cast<Ecn>((op >> 5U) & 3U); // bleached, re-marked or CE
                    pair.deliver(pick);
                    pair.remove(pick);
                }
                break;
            case 2U:
                if (pair.count > 0U)
                {
                    pair.remove(pick); // lost
                }
                break;
            case 3U:
                if (pair.count > 0U && pair.count < kFlights)
                {
                    pair.flights[pair.count++] = pair.flights[pick]; // duplicated
                }
                break;
            case 4U:
                if (pair.count > 1U)
                {
                    const Flight held = pair.flights[0];
                    pair.flights[0] = pair.flights[pick];
                    pair.flights[pick] = held; // reordered
                }
                break;
            case 5U:
                if (pair.count > 0U && at + 2U < script.size())
                {
                    Flight& flight = pair.flights[pick];
                    flight.bytes[script[at + 1U] % flight.size] ^= static_cast<u8>(script[at + 2U] | 1U);
                    at += 2U;
                }
                break;
            case 6U:
                pair.now += static_cast<u64>(op >> 3U) * 10U * kMs;
                pair.timers();
                break;
            default:
            {
                const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns())
                                     ? pair.client.next_timer_ns()
                                     : pair.server.next_timer_ns();
                pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
                pair.timers();
            }
            break;
        }
    }
    pair.held = 0U;
    pair.settle(4096U);
    CRD_FUZZ_REQUIRE(pair.established() || finished(pair.client) || finished(pair.server));
    if (pair.established())
    {
        CRD_FUZZ_REQUIRE(pair.client.handshake().application_protocol() ==
                         pair.server.handshake().application_protocol());
        CRD_FUZZ_REQUIRE(pair.client.retried() == pair.server.retried());
        const QuicVersion running = switching ? other_version(version) : version;
        CRD_FUZZ_REQUIRE(pair.client.version() == running && pair.server.version() == running);
        if (offered)
        {
            const TlsEarlyData decided = pair.client.handshake().early_data();
            CRD_FUZZ_REQUIRE(decided == TlsEarlyData::Accepted || decided == TlsEarlyData::Rejected);
            CRD_FUZZ_REQUIRE((decided == TlsEarlyData::Accepted) ==
                             (pair.server.handshake().early_data() == TlsEarlyData::Accepted));
            if (decided == TlsEarlyData::Accepted)
            {
                read_early(pair);
            }
            else
            {
                CRD_FUZZ_REQUIRE(pair.client.streams().open_streams() == 0U);
                CRD_FUZZ_REQUIRE(pair.server.streams().recv_state(0U) == QuicRecvState::None);
                CRD_FUZZ_REQUIRE(pair.server.datagrams().held() == 0U);
            }
            settle_early_datagram(pair, decided == TlsEarlyData::Accepted);
        }
        transfer(pair);
        // A pair that carried its stream with neither end closed has the server sending to the client's address once
        // what is owed has arrived (a client that reverted at the end sends from there now). Until then it is
        // validating that address: a validation whose challenge or response the script lost, or whose packets came
        // after their ID's retirement, ends on its next challenge (RFC 9000 section 8.2.1). Once it validated the
        // address it follows; only an end that closed first leaves it unvalidated. Or that validation failed after
        // the client's last packet, its challenges or responses lost before the timer (section 8.2.4): the server
        // reverted to its last validated address (section 9.3.2) and learns the client's again only from its next
        // packet, which moves it back or, with no ID of the client's for it, validates the address while the move
        // waits for one (section 9.5). A packet of the client's it opened never leaves it doing neither.
        // All of it on the one path ID the client keeps (keep_one_path), at both ends.
        const u64 kept = (finished(pair.client) || finished(pair.server)) ? 0U : keep_one_path(pair);
        if (over_paths() && !finished(pair.client) && !finished(pair.server))
        {
            pair.settle(1U);
            const auto tuples = [kept](const QuicConnection& end) -> const QuicPaths*
            {
                const usize slot = slot_of(end, kept);
                return (slot != kQuicNoPath) ? &end.paths(slot) : nullptr;
            };
            const auto client_address = [&]
            {
                const QuicPaths* const paths = tuples(pair.client);
                return pair.address_of((paths != nullptr) ? paths->path(paths->active()).socket : 0U);
            };
            const auto client_at = [&]
            {
                return kClientPaths[client_address()].peer;
            };
            const auto follows = [&]
            {
                const QuicPaths* const paths = tuples(pair.server);
                return paths != nullptr && paths->path(paths->active()).peer == client_at();
            };
            const auto ended = [&pair] { return finished(pair.client) || finished(pair.server); };
            // Whether a path of the server's to the client's address is validating, or validated. The client validating
            // its own address counts as validating: a path ID that carried nothing since it moved there (another one
            // took its data) reaches the server from there first with its next challenge.
            const auto proving = [&](bool validated)
            {
                const QuicPaths* const paths = tuples(pair.server);
                const QuicPaths* const own = tuples(pair.client);
                bool found = !validated && own != nullptr && own->validating(own->active());
                for (usize index = 0U; paths != nullptr && index < kQuicMaxPaths; ++index)
                {
                    const bool state = validated ? paths->validated(index) : paths->validating(index);
                    found = found || (paths->used(index) && paths->path(index).peer == client_at() && state);
                }
                return found;
            };
            if (!follows())
            {
                bool pinged = false;
                u64 failures = 0U;
                u64 opened = 0U;
                pair.settle_until(256U,
                                  [&]
                                  {
                                      if (!follows() && !ended() && !proving(false))
                                      {
                                          // A server that opened none of the kept path ID's packets from there since
                                          // the client's NAT mapped it (a path ID that carried nothing since, its
                                          // packets lost, waiting for its probe timeout) learns it from the next one.
                                          const QuicConnectionCounters& counters = pair.server.counters();
                                          const bool heard = (pair.opened_from[client_address()] & id_bit(kept)) != 0U;
                                          CRD_FUZZ_REQUIRE(!heard || counters.path_failures != 0U);
                                          // Asked for a packet and still unheard only while it cannot record one.
                                          const usize own = slot_of(pair.client, kept);
                                          CRD_FUZZ_REQUIRE(
                                              heard || !pinged || own == kQuicNoPath ||
                                              !pair.client.recovery().can_send(QuicSpace::Application, own));
                                          // Again only after another validation failed, or when nothing opened.
                                          CRD_FUZZ_REQUIRE(!pinged || counters.path_failures > failures ||
                                                           counters.packets_opened == opened);
                                          pinged = true;
                                          failures = counters.path_failures;
                                          opened = counters.packets_opened;
                                          pair.client.ping(); // the client's next packet, from its address
                                      }
                                      return follows() || ended();
                                  });
                CRD_FUZZ_REQUIRE(follows() || (ended() && !proving(true)));
            }
        }
        check_path_statuses(pair);
        check_observations(pair);
        exchange_datagrams(pair, offered ? 1U : 0U);
        stateless_reset(pair);
    }
    CRD_FUZZ_REQUIRE(pair.allocator.peak_bytes() == allocations);
}

// ---- Modes 1 and 2: frames from a raw peer ----

QuicLevel level_of(QuicPacketType type)
{
    if (type == QuicPacketType::Initial)
    {
        return QuicLevel::Initial;
    }
    return (type == QuicPacketType::Handshake) ? QuicLevel::Handshake : QuicLevel::Application;
}

QuicPacketType type_of(QuicLevel level)
{
    if (level == QuicLevel::Initial)
    {
        return QuicPacketType::Initial;
    }
    return (level == QuicLevel::Handshake) ? QuicPacketType::Handshake : QuicPacketType::OneRtt;
}

struct Raw
{
    crd::fuzz::BudgetAllocator allocator;
    Host host;
    QuicConnection server;
    QuicHandshake handshake;
    QuicPacketProtection protection;
    u64 next[3] = {};
    u64 sent[3] = {};
    u64 received[3] = {};
    u8 dcid[20] = {};
    usize dcid_bytes = sizeof(kInitialDcid);
    u64 now = 1000U * kMs;
    bool failed = false;
    bool keyed;
    u32 version;

    explicit Raw(QuicVersion run, bool raw_path = false)
        : host(allocator, 1U,
               raw_path ? keyed_id(QuicSide::Server).view() : ConstSpan<u8>(kServerCid, sizeof(kServerCid))),
          server(allocator, config_for(QuicSide::Server, run, &host.ids, false, nullptr, raw_path)), keyed(raw_path),
          version(static_cast<u32>(run))
    {
        std::memcpy(dcid, kInitialDcid, sizeof(kInitialDcid));
    }

    // The raw UDP path from the client's side: the server opened, its keys held and the client's hello sent.
    void open()
    {
        open_keyed(server, now);
        std::memcpy(dcid, keyed_id(QuicSide::Server).bytes, kKeyedIdBytes);
        dcid_bytes = kKeyedIdBytes;
        protection.start_keyed(QuicSide::Client, static_cast<QuicVersion>(version));
        constexpr QuicSide sides[] = {QuicSide::Client, QuicSide::Server};
        for (const QuicSide side : sides)
        {
            u8 traffic[kQuicMaxSecretBytes];
            usize bytes = 0U;
            CRD_FUZZ_REQUIRE(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, ConstSpan<u8>(kKeyedSecret), side,
                                                       Span<u8>(traffic, sizeof(traffic)), bytes) == CryptoError::None);
            CRD_FUZZ_REQUIRE(protection.install(QuicLevel::Application, side == QuicSide::Client,
                                                AlgorithmId::Aes128Gcm,
                                                ConstSpan<u8>(traffic, bytes)) == CryptoError::None);
        }
        protection.confirm();
        QuicTransportParameters parameters;
        parameters.has_initial_source_connection_id = true;
        static_cast<void>(parameters.initial_source_connection_id.assign(keyed_id(QuicSide::Client).view()));
        parameters.has_initial_max_path_id = extensions();
        parameters.initial_max_path_id = kMaxPathId;
        u8 hello[256];
        const usize bytes = write_quic_transport_parameters(parameters, QuicSide::Client, {3U},
                                                            Span<u8>(hello + 2U, sizeof(hello) - 2U));
        hello[0] = static_cast<u8>(bytes >> 8U);
        hello[1] = static_cast<u8>(bytes);
        QuicFrame frame;
        frame.kind = QuicFrameKind::Crypto;
        frame.data = ConstSpan<u8>(hello, 2U + bytes);
        u8 frames[512];
        const usize written = write_quic_frame(frame, Span<u8>(frames, sizeof(frames)));
        send(QuicPacketType::OneRtt, ConstSpan<u8>(frames, written));
    }

    // One packet of `type` holding `frames` as its payload (PADDING when empty), then the server's answers read.
    void send(QuicPacketType type, ConstSpan<u8> frames)
    {
        u8 datagram[kDatagram] = {};
        QuicPacketWriter writer;
        QuicPacketSpec spec;
        spec.type = type;
        spec.version = version;
        spec.dcid = ConstSpan<u8>(dcid, dcid_bytes);
        spec.scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
        spec.number = next[static_cast<usize>(quic_space_of(type))]++;
        if (!writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)))
        {
            return;
        }
        const usize used = (frames.size() < writer.frames().size()) ? frames.size() : writer.frames().size();
        if (used != 0U)
        {
            std::memcpy(writer.frames().data(), frames.data(), used);
        }
        const usize floor = (type == QuicPacketType::Initial) ? kQuicMinInitialDatagram : 0U;
        usize size = 0U;
        if (protection.seal(writer, used, {floor, 0U}, size) != QuicPacketError::None)
        {
            return;
        }
        server.receive(Span<u8>(datagram, size), Ecn::NotEct, now);
        check_closed(server);
        drain();
    }

    void drain()
    {
        for (usize guard = 0U; guard < 64U; ++guard)
        {
            u8 datagram[kDatagram];
            const u64 probes = server.counters().path_probes;
            const usize size = server.send(Span<u8>(datagram, sizeof(datagram)), now);
            if (size == 0U)
            {
                return;
            }
            // Datagrams keep to the path's size; only a PMTU probe passes it, up to the ceiling.
            CRD_FUZZ_REQUIRE(size <= server.path_mtu().ceiling());
            CRD_FUZZ_REQUIRE(size <= server.path_mtu().size() || server.counters().path_probes == probes + 1U);
            if (!server.address_validated())
            {
                CRD_FUZZ_REQUIRE(server.counters().bytes_sent <= 3U * server.counters().bytes_received);
            }
            read(Span<u8>(datagram, size));
        }
    }

    void read(Span<u8> datagram)
    {
        QuicDatagramPackets packets(datagram, sizeof(kClientCid));
        QuicHeader header;
        Span<u8> packet;
        while (packets.next(header, packet))
        {
            if (header.type != QuicPacketType::OneRtt && dcid_bytes == sizeof(kInitialDcid))
            {
                std::memcpy(dcid, header.scid.data(), header.scid.size());
                dcid_bytes = header.scid.size();
            }
            QuicOpened opened;
            if (protection.open(header, packet, opened) != QuicPacketError::None)
            {
                continue;
            }
            WireReader in(opened.payload);
            while (!in.done())
            {
                QuicFrame frame;
                CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
                CRD_FUZZ_REQUIRE(quic_frame_allowed(frame.kind, frame.application, opened.type, true));
                if (server.state() == QuicConnectionState::Closing)
                {
                    CRD_FUZZ_REQUIRE(frame.kind == QuicFrameKind::ConnectionClose ||
                                     frame.kind == QuicFrameKind::Padding);
                }
                const QuicLevel level = level_of(opened.type);
                const usize index = static_cast<usize>(level);
                if (!keyed && frame.kind == QuicFrameKind::Crypto && frame.offset == received[index])
                {
                    received[index] += frame.data.size();
                    failed = failed || handshake.provide(level, frame.data) != TlsError::None;
                }
            }
            failed = failed || (!keyed && protection.install(handshake) != CryptoError::None);
        }
    }

    void send_crypto(QuicLevel level)
    {
        failed = failed || protection.install(handshake) != CryptoError::None;
        const usize index = static_cast<usize>(level);
        while (!failed && handshake.pending(level) > 0U)
        {
            u8 chunk[900];
            usize taken = 0U;
            failed = handshake.take(level, Span<u8>(chunk, sizeof(chunk)), taken) != TlsError::None;
            QuicFrame frame;
            frame.kind = QuicFrameKind::Crypto;
            frame.offset = sent[index];
            frame.data = ConstSpan<u8>(chunk, taken);
            sent[index] += taken;
            u8 frames[1000];
            const usize bytes = write_quic_frame(frame, Span<u8>(frames, sizeof(frames)));
            send(type_of(level), ConstSpan<u8>(frames, bytes));
        }
    }
};

void run_frames(QuicVersion version, ConstSpan<u8> input, bool initial, bool keyed)
{
    Raw raw(version, keyed);
    CRD_FUZZ_REQUIRE(raw.server.ready());
    const usize allocations = raw.allocator.peak_bytes();
    if (keyed)
    {
        raw.open();
    }
    else
    {
        CRD_FUZZ_REQUIRE(raw.protection.start(QuicSide::Client, version,
                                              ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid))) == CryptoError::None);
        CRD_FUZZ_REQUIRE(raw.server.accept(
            accept_of(ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), ConstSpan<u8>(kClientCid, sizeof(kClientCid))),
            raw.now));
    }
    if (!initial && !keyed)
    {
        QuicTransportParameters parameters;
        parameters.has_initial_source_connection_id = true;
        static_cast<void>(
            parameters.initial_source_connection_id.assign(ConstSpan<u8>(kClientCid, sizeof(kClientCid))));
        parameters.has_initial_max_path_id = extensions();
        parameters.initial_max_path_id = kMaxPathId;
        u8 encoded[256];
        const usize bytes =
            write_quic_transport_parameters(parameters, QuicSide::Client, {3U}, Span<u8>(encoded, sizeof(encoded)));
        CRD_FUZZ_REQUIRE(raw.handshake.init(identity().client, ConstSpan<u8>(encoded, bytes)) == TlsError::None);
        raw.send_crypto(QuicLevel::Initial);
        raw.send_crypto(QuicLevel::Handshake);
        CRD_FUZZ_REQUIRE(!raw.failed && raw.server.state() == QuicConnectionState::Established);
        CRD_FUZZ_REQUIRE(raw.protection.can_send(QuicPacketType::OneRtt));
        raw.protection.confirm();
    }
    const QuicPacketType type = initial ? QuicPacketType::Initial : QuicPacketType::OneRtt;
    bool closing = false; // the server closed or drained: three PTOs on, it is Closed
    for (usize at = 0U; at < input.size();)
    {
        const usize length = input[at++];
        const usize take = (length < input.size() - at) ? length : input.size() - at;
        raw.send(type, input.subspan(at, take));
        at += take;
        raw.host.check(raw.server);
        // Multipath only when offered (byte 0's bit 6), and the peer's limit never below its initial_max_path_id.
        CRD_FUZZ_REQUIRE(extensions() || !raw.server.multipath());
        CRD_FUZZ_REQUIRE(!raw.server.multipath() ||
                         raw.server.peer_max_path_id() >= raw.server.peer_parameters().initial_max_path_id);
        const QuicConnectionState state = raw.server.state();
        const QuicConnectionState open = initial ? QuicConnectionState::Handshaking : QuicConnectionState::Established;
        CRD_FUZZ_REQUIRE(state == open || state == QuicConnectionState::Closing ||
                         state == QuicConnectionState::Draining || (state == QuicConnectionState::Closed && closing));
        closing = state != open;
        raw.now += kMs;
        if (raw.server.next_timer_ns() <= raw.now)
        {
            raw.server.on_timeout(raw.now);
            check_closed(raw.server);
            raw.drain();
        }
    }
    CRD_FUZZ_REQUIRE(raw.allocator.peak_bytes() == allocations);
}

// A seed script over paths with multipath (byte 0's bits 6 and 7) that confirms first; each of steps() moves 250 ms
// and delivers everything in flight, so no step waits out the idle timeout.
struct PathScript
{
    u8 bytes[512] = {0xC0U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x07U, 0x07U};
    usize size = 11U;

    template <usize N> PathScript& ops(const u8 (&more)[N])
    {
        CRD_FUZZ_REQUIRE(size + N <= sizeof(bytes));
        for (const u8 op : more)
        {
            bytes[size++] = op;
        }
        return *this;
    }

    PathScript& steps(usize count)
    {
        for (usize step = 0U; step < count; ++step)
        {
            ops({0xCEU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U});
        }
        return *this;
    }
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 1U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const QuicVersion version = ((data[0] & 4U) != 0U) ? QuicVersion::V2 : QuicVersion::V1;
    const bool keyed = (data[0] & 32U) != 0U;
    extensions() = (data[0] & 64U) != 0U;
    over_paths() = (data[0] & 128U) != 0U;
    const bool four = over_paths() && (data[0] & 3U) == 0U && size > 1U && data[1] == 0xF3U;
    server_ids() = four ? 4U : 2U;
    const usize skip = four ? 2U : 1U;
    const ConstSpan<u8> rest(data + skip, size - skip);
    switch (data[0] & 3U)
    {
        case 0U:
            run_script(version, !keyed && (data[0] & 8U) != 0U, !keyed && (data[0] & 16U) != 0U, keyed, rest);
            break;
        case 1U:
            run_frames(version, rest, false, false);
            break;
        case 2U:
            run_frames(version, rest, true, false);
            break;
        default:
            run_frames(version, rest, false, true);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A script: deliver everything in order with a timer step between flights.
    const u8 clean[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-clean", clean, sizeof(clean));
    // Over paths: confirmed, then a NAT rebinding, an address change and an off-path attacker's copy.
    const u8 moved[] = {0x80U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x07U, 0x07U, 0xFCU, 0x07U,
                        0x00U, 0x07U, 0x00U, 0x07U, 0xFCU, 0x07U, 0x00U, 0xFBU, 0x07U, 0x00U, 0x07U, 0x00U};
    sink.add("script-paths", moved, sizeof(moved));
    const u8 keyed_moved[] = {0xA0U, 0x00U, 0x07U, 0x00U, 0xFCU, 0xFCU, 0x07U, 0x00U, 0xFBU, 0x07U, 0x00U};
    sink.add("script-paths-keyed", keyed_moved, sizeof(keyed_moved));
    // Over paths: confirmed, the client probes its next socket and moves there, socket 0 rebinds, it moves twice more.
    const u8 own[] = {0x80U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x07U, 0x07U, 0xF9U, 0x07U,
                      0x00U, 0x07U, 0xFAU, 0x07U, 0x00U, 0x07U, 0xFCU, 0xFAU, 0x07U, 0x00U, 0x07U, 0xFAU, 0x07U, 0x00U};
    sink.add("script-paths-client", own, sizeof(own));
    // Over paths: confirmed, socket 0 held, the move to socket 1, then socket 1 held and both, then none.
    const u8 held[] = {0x80U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x07U, 0x07U, 0xF8U, 0xFAU,
                       0x07U, 0x00U, 0x07U, 0xF8U, 0x07U, 0x00U, 0xF8U, 0x07U, 0xF8U, 0x07U, 0x00U, 0x07U, 0x00U};
    sink.add("script-paths-held", held, sizeof(held));
    // Over paths: confirmed, the client moves to the server's preferred address, then to its next socket there, and
    // socket 0 rebinds.
    const u8 preferred[] = {0x80U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x07U, 0x07U, 0xF7U,
                            0x07U, 0x00U, 0x07U, 0x00U, 0x07U, 0xFAU, 0x07U, 0x00U, 0xFCU, 0x07U, 0x00U};
    sink.add("script-paths-preferred", preferred, sizeof(preferred));
    // Handover schedules on a lossy, reordering script: guards of 40 and 24 ms, one past the share, then cleared.
    const u8 handover[] = {0x00U, 0xF6U, 0x35U, 0x00U, 0x00U, 0x00U, 0x07U, 0x02U, 0x00U, 0x03U, 0x07U, 0xF6U,
                           0xFFU, 0x00U, 0x02U, 0x07U, 0x00U, 0xF6U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-handover", handover, sizeof(handover));
    // Learning a 1 s period with guards of 40 and 20 ms on a lossy script, a 2 s one past the share refused, an
    // authored schedule ending it, then learning again.
    const u8 learned[] = {0x00U, 0xF5U, 0x28U, 0x00U, 0x00U, 0x00U, 0x07U, 0x02U, 0x00U, 0x03U, 0x07U, 0xF5U, 0xFDU,
                          0x00U, 0x02U, 0x07U, 0xF6U, 0x35U, 0x07U, 0x00U, 0xF5U, 0x28U, 0x07U, 0x00U, 0x00U};
    sink.add("script-handover-learning", learned, sizeof(learned));
    // The same with receive timestamps, and a raw client's ACK_RECEIVE_TIMESTAMPS of the server's first 1-RTT packet.
    const u8 timed[] = {0x40U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-receive-timestamps", timed, sizeof(timed));
    const u8 timed_frame[] = {0x41U, 0x0CU, 0x83U, 0x17U, 0x83U, 0x07U, 0x00U,
                              0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x05U};
    sink.add("frames-receive-timestamps", timed_frame, sizeof(timed_frame));
    // Address reports over paths that rebind and change address, and a raw client's OBSERVED_ADDRESS (IPv4, port 50000).
    const u8 reported[] = {0xC0U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x07U, 0x07U, 0xFCU, 0x07U,
                           0x00U, 0x07U, 0x00U, 0x07U, 0xFCU, 0x07U, 0x00U, 0xFBU, 0x07U, 0x00U, 0x07U, 0x00U};
    sink.add("script-address-discovery", reported, sizeof(reported));
    const u8 observed[] = {0x41U, 0x0BU, 0x80U, 0x9FU, 0x81U, 0xA6U, 0x00U, 0xC6U, 0x33U, 0x64U, 0x07U, 0xC3U, 0x50U};
    sink.add("frames-observed-address", observed, sizeof(observed));
    // Multipath negotiated for path ID 0: MAX_PATH_ID 5, PATH_STATUS_BACKUP, both BLOCKED frames, a PATH_ACK of path ID
    // 0's packet 0, then PATH_ABANDON of path ID 0, the only path.
    const u8 multipath[] = {0x41U, 0x03U, 0x7EU, 0x7AU, 0x05U, 0x04U, 0x7EU, 0x76U, 0x00U, 0x01U, 0x03U,
                            0x7EU, 0x7BU, 0x00U, 0x04U, 0x7EU, 0x7CU, 0x00U, 0x01U, 0x06U, 0x3EU, 0x00U,
                            0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x7EU, 0x75U, 0x00U, 0x00U};
    sink.add("frames-multipath", multipath, sizeof(multipath));
    // Path statuses with multipath: one refused before the handshake, then the client's backup for path ID 1 lost,
    // the server's under the Turns choice, a path ID above both limits and a choice that is none, then path ID 0's.
    const u8 statuses[] = {0x40U, 0xF4U, 0x06U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U,
                           0x07U, 0xF4U, 0x06U, 0x02U, 0xF4U, 0x17U, 0x07U, 0x00U, 0xF4U, 0x0CU,
                           0xF4U, 0x24U, 0x02U, 0x07U, 0xF4U, 0x02U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-path-status", statuses, sizeof(statuses));
    // Path IDs over paths (RFC 10069): opened on sockets 1 and 2, the first abandoned by the client with too large a
    // code and then the largest, and by the server, then path ID 0 by the client while path ID 2 sends; once both
    // closed, socket 1 opens path ID 3, never path ID 1 again.
    PathScript opened;
    opened.ops({0xF2U, 0x01U}).steps(2).ops({0xF2U, 0x02U}).steps(2).ops({0xF0U, 0x32U, 0xF0U, 0x22U}).steps(1);
    opened.ops({0xF0U, 0x03U}).steps(1).ops({0xF0U, 0x00U}).steps(32).ops({0xF2U, 0x01U}).steps(2);
    sink.add("script-path-ids", opened.bytes, opened.size);
    // Path ID 0 abandoned once path ID 1 sends, its slot reclaimed after three PTOs by path ID 2 on socket 0, and
    // path ID 3 to the preferred address from socket 2.
    PathScript reclaimed;
    reclaimed.ops({0xF2U, 0x01U}).steps(2).ops({0xF0U, 0x00U}).steps(32).ops({0xF2U, 0x00U}).steps(2);
    reclaimed.ops({0xF2U, 0x06U}).steps(1);
    sink.add("script-path-ids-reclaimed", reclaimed.bytes, reclaimed.size);
    // Path ID 0 refused as the last path by either end, then path IDs 1 and 2 abandoned unused by the client and the
    // server: no path opens until both close and the limits rise past them, and then path ID 3.
    PathScript unused;
    unused.ops({0xF0U, 0x00U, 0xF0U, 0x01U, 0xF0U, 0x02U, 0xF0U, 0x05U}).steps(2).ops({0xF2U, 0x01U}).steps(32);
    unused.ops({0xF2U, 0x01U}).steps(2);
    sink.add("script-path-ids-unused", unused.bytes, unused.size);
    // A path ID opened on a socket its host holds back, then released; the server's status for it.
    PathScript held_open;
    held_open.ops({0xF8U, 0xF8U, 0xF2U, 0x01U}).steps(1).ops({0xF8U}).steps(1);
    held_open.ops({0xF4U, 0x07U, 0xF8U, 0xF8U, 0xF8U, 0xF8U, 0xF8U}).steps(2);
    sink.add("script-path-ids-held", held_open.bytes, held_open.size);
    // The client's own sockets and the preferred address with reports on: each new path is reported.
    u8 reported_own[sizeof(own)];
    std::memcpy(reported_own, own, sizeof(own));
    reported_own[0] = 0xC0U;
    sink.add("script-address-discovery-client", reported_own, sizeof(reported_own));
    u8 reported_preferred[sizeof(preferred)];
    std::memcpy(reported_preferred, preferred, sizeof(preferred));
    reported_preferred[0] = 0xC0U;
    sink.add("script-address-discovery-preferred", reported_preferred, sizeof(reported_preferred));
    // A Retry in place of the first flight: the client goes again to its ID with its token.
    const u8 retried[] = {0x00U, 0xFDU, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-retry", retried, sizeof(retried));
    // A compatible switch from each version, one after a Retry.
    const u8 switched[] = {0x08U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-switch", switched, sizeof(switched));
    const u8 switched_retry[] = {0x0CU, 0xFDU, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U,
                                 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-switch-retry", switched_retry, sizeof(switched_retry));
    // 0-RTT on a resumed connection, clean, after a Retry and on a hostile path in version 2.
    const u8 early[] = {0x10U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-early", early, sizeof(early));
    const u8 early_retry[] = {0x10U, 0xFDU, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U,
                              0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-early-retry", early_retry, sizeof(early_retry));
    const u8 early_hostile[] = {0x14U, 0x04U, 0x02U, 0x03U, 0x00U, 0x0CU, 0x05U, 0x40U, 0x08U, 0x07U,
                                0x00U, 0x02U, 0x06U, 0x86U, 0x07U, 0x00U, 0x00U, 0x0BU, 0x00U};
    sink.add("script-early-hostile", early_hostile, sizeof(early_hostile));
    // The raw UDP path, clean and on a hostile path in version 2, and frames from a peer holding its secret.
    const u8 keyed[] = {0x20U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U};
    sink.add("script-keyed", keyed, sizeof(keyed));
    const u8 keyed_hostile[] = {0x24U, 0x02U, 0x03U, 0x00U, 0x0CU, 0x05U, 0x40U, 0x08U, 0x07U, 0x00U, 0x02U, 0x06U};
    sink.add("script-keyed-hostile", keyed_hostile, sizeof(keyed_hostile));
    const u8 keyed_frames[] = {0x03U, 0x01U, 0x01U, 0x04U, 0x0AU, 0x00U, 0x01U, 0x41U, 0x03U, 0x06U, 0x00U, 0x01U};
    sink.add("keyed-frames", keyed_frames, sizeof(keyed_frames));
    // Losses, duplicates, a reorder and a corrupted byte in version 2.
    const u8 hostile[] = {0x04U, 0x02U, 0x03U, 0x00U, 0x0CU, 0x05U, 0x40U, 0x08U, 0x07U, 0x00U,
                          0x02U, 0x06U, 0x86U, 0x07U, 0x00U, 0x00U, 0x0BU, 0x00U, 0x00U};
    sink.add("script-hostile", hostile, sizeof(hostile));
    // 1-RTT frames after the handshake: a PING, a STREAM frame, a NEW_CONNECTION_ID, a PATH_CHALLENGE.
    const u8 frames[] = {0x01U, 0x01U, 0x01U, 0x04U, 0x0AU, 0x00U, 0x01U, 0x41U, 0x1CU, 0x18U, 0x01U, 0x00U,
                         0x08U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0xA0U, 0xA1U, 0xA2U,
                         0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U, 0xA8U, 0xA9U, 0xAAU, 0xABU, 0xACU, 0xADU, 0xAEU,
                         0xAFU, 0x09U, 0x1AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U};
    sink.add("one-rtt-frames", frames, sizeof(frames));
    // A clean script that rotates the server's IDs once the handshake has run.
    const u8 rotate[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                         0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFEU, 0x00U, 0x00U, 0x00U,
                         0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-rotate", rotate, sizeof(rotate));
    // RETIRE_CONNECTION_ID of the server's sequence 1, then of a sequence never issued.
    const u8 retire[] = {0x01U, 0x02U, 0x19U, 0x01U, 0x02U, 0x19U, 0x05U};
    sink.add("one-rtt-retire", retire, sizeof(retire));
    // Initial frames anyone can seal: a PING, an ACK of a packet never sent, a CRYPTO frame of garbage.
    const u8 initial[] = {0x02U, 0x01U, 0x01U, 0x05U, 0x02U, 0x05U, 0x00U, 0x00U, 0x00U,
                          0x07U, 0x06U, 0x00U, 0x04U, 0x01U, 0x00U, 0x00U, 0x00U};
    sink.add("initial-frames", initial, sizeof(initial));
}
