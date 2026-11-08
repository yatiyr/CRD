#pragma once

// crd-net -- a server's ingress: admission stages S1 (sanity), S2 (route) and S3 (admit) for QUIC datagrams, and the
// stateless answers they send (4.2a, threats T01, T02 and T10; contract: docs/design/network-foundation.md#ingress).
//
//   S1   the first packet's header (quic_header.hpp): datagrams the slot truncated, malformed headers, packets too
//        short to unprotect, a Version Negotiation packet (a server never takes one) and a client's Retry are dropped.
//        A long header of a version the server does not run gets Version Negotiation only when its datagram is at
//        least 1200 bytes (RFC 9000 section 6.1), so a smaller one cannot draw a reflection. A client Initial in a
//        datagram under 1200 bytes, or with a Destination Connection ID under 8 bytes (section 7.2), is dropped.
//   S2   the Destination Connection ID through the keyed table (connection_table.hpp); a coalesced datagram goes
//        where its first packet goes. A short header for no connection is answered with a stateless reset when the
//        server holds a QuicResetKey (RFC 9000 section 10.3), else dropped; a 0-RTT or Handshake packet for no
//        connection is dropped (a reset is not effective before the handshake gave the client its token).
//   S3   a client Initial for no connection: a Retry token that validates proves the address (QuicTokenKeys), one
//        that does not is dropped (INVALID_TOKEN, section 8.1.2); a NEW_TOKEN token that validates proves it, one that
//        does not counts as none (section 8.1.3). Unproven addresses pass a global and a per-prefix rate (/64 IPv6,
//        /32 or a configured prefix IPv4), shared by every loop of the server, and are sent a Retry when either is
//        closed or when every address must be proven; proven ones pass a global rate. Prefixes share a fixed array of
//        buckets chosen by a keyed hash, so a server keeps no per-source state before validation and a collision only
//        tightens.
//   answers  Retry, Version Negotiation and stateless resets pass one global rate. Retry and Version Negotiation are
//        far below three times the datagram they answer (RFC 9000 section 8): at least 1200 bytes in, at most
//        kQuicMaxStatelessAnswerBytes out. A stateless reset is shorter than its trigger, so a loop of resets
//        between two endpoints shrinks to nothing (section 10.3.3): one byte shorter for a trigger of 43 bytes or
//        fewer, else a random length from 43 to kQuicMaxResetAnswerBytes; never under kQuicMinResetBytes.
//
// Threads: HandshakeBuckets is shared by the loops of one server and checked by all of them at once (its gates are
// SharedRateGates: two loops may each let one handshake past a closing gate, and both are charged). An Ingress belongs
// to one loop.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/quota.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

// The rates of S3. NET.4 measures and publishes the defaults; these are the starting configuration.
struct HandshakeRules
{
    GateRule unvalidated{20000U, kNanosPerSecond}; // handshakes from unproven addresses, all prefixes together
    GateRule per_prefix{64U, kNanosPerSecond};     // the same from one prefix bucket
    GateRule validated{50000U, kNanosPerSecond};   // handshakes from proven addresses, all together
    GateRule answers{50000U, kNanosPerSecond};     // Retry, Version Negotiation and reset packets, all together
    u32 ipv4_prefix_bits = 32U;
    u32 ipv6_prefix_bits = 64U;
    u32 prefix_buckets = 4096U; // a power of two, 1 to 2^24
    bool always_retry = false;  // prove every address before a handshake
};

enum class HandshakeVerdict : u8
{
    Admit,
    Retry,  // prove the address first
    Refuse, // the proven-address rate is spent
};

class HandshakeBuckets
{
public:
    // `key` is kPrfKeyBytes (shared by servers that must bucket alike), or empty for a fresh one.
    HandshakeBuckets(memory::IAllocator& allocator, const HandshakeRules& rules,
                     cont::ConstSpan<u8> key = {}) noexcept;
    ~HandshakeBuckets();
    HandshakeBuckets(const HandshakeBuckets&) = delete;
    HandshakeBuckets& operator=(const HandshakeBuckets&) = delete;
    HandshakeBuckets(HandshakeBuckets&&) = delete;
    HandshakeBuckets& operator=(HandshakeBuckets&&) = delete;

    // False when the rules are out of range, the allocator refused or no key could be made.
    [[nodiscard]] bool ready() const noexcept { return m_prefixes != nullptr; }
    [[nodiscard]] const HandshakeRules& rules() const noexcept { return m_rules; }

    // Whether a handshake from `peer` may start at `now_ns`, charging what it spends: a proven address its global
    // rate and its prefix bucket (so a busy prefix's unproven addresses are sent to Retry sooner), an unproven one
    // both of its rates when they are open.
    [[nodiscard]] HandshakeVerdict admit(const IpAddress& peer, bool validated, u64 now_ns) noexcept;
    // Charges one stateless answer; false, charging nothing, when the answer rate is spent.
    [[nodiscard]] bool answer(u64 now_ns) noexcept;

    // The bucket `peer`'s prefix falls in.
    [[nodiscard]] u32 bucket_of(const IpAddress& peer) const noexcept;

private:
    memory::IAllocator& m_allocator;
    HandshakeRules m_rules;
    crypto::PrfKey m_key;
    SharedRateGate m_unvalidated;
    SharedRateGate m_validated;
    SharedRateGate m_answers;
    SharedRateGate* m_prefixes = nullptr;
};

// What a server runs: the versions it accepts in its order of preference, which Version Negotiation lists, and the
// length of the connection IDs it issues (8 to 20 bytes; 16 by default, 128 random bits).
struct IngressConfig
{
    u32 versions[kQuicMaxNegotiationVersions] = {static_cast<u32>(crypto::QuicVersion::V1),
                                                 static_cast<u32>(crypto::QuicVersion::V2)};
    usize version_count = 2U;
    usize local_cid_bytes = 16U;
};

inline constexpr usize kQuicMinClientDcidBytes = 8U;    // RFC 9000 section 7.2
inline constexpr usize kQuicMaxResetAnswerBytes = 128U; // as long as a small 1-RTT packet, never longer
inline constexpr usize kQuicMaxStatelessAnswerBytes =
    (kQuicMaxVersionNegotiationBytes > quic_retry_bytes(crypto::kQuicMaxConnectionIdBytes,
                                                        crypto::kQuicMaxConnectionIdBytes,
                                                        crypto::kQuicMaxRetryTokenBytes))
        ? kQuicMaxVersionNegotiationBytes
        : quic_retry_bytes(crypto::kQuicMaxConnectionIdBytes, crypto::kQuicMaxConnectionIdBytes,
                           crypto::kQuicMaxRetryTokenBytes);

enum class IngressAction : u8
{
    Drop,
    Route,            // to `connection`
    Admit,            // a new connection: the handler creates it
    Retry,            // answer with answer()
    NegotiateVersion, // answer with answer()
    StatelessReset,   // answer with answer()
};

enum class DropReason : u8
{
    Truncated,          // longer than the receive slot (the path's largest datagram)
    Malformed,          // a header the datagram does not hold
    ConnectionIdTooLong,
    TooShortToProtect,
    VersionNegotiation, // a server never takes one
    ClientRetry,        // a client never sends one
    UnsupportedSmall,   // an unsupported version in fewer than 1200 bytes: no answer
    SmallInitial,       // a client Initial in fewer than 1200 bytes
    ShortClientDcid,    // a client Initial's Destination Connection ID under 8 bytes
    UnknownConnection,  // a short header for no connection
    EarlyPacket,        // 0-RTT or Handshake for no connection
    InvalidRetryToken,
    Overloaded,         // S3 refused and no answer was possible
    Count,
};

inline constexpr usize kDropReasons = static_cast<usize>(DropReason::Count);

struct IngressDecision
{
    IngressAction action = IngressAction::Drop;
    DropReason reason = DropReason::Malformed; // for Drop
    QuicHeader header;
    u32 connection = kNoConnection; // for Route
    bool validated = false;         // for Admit: the address is proven
    usize datagram_bytes = 0U;      // for StatelessReset: the size of the datagram it answers
    crypto::QuicTokenClaims claims; // for Admit through a Retry token: its original Destination Connection ID
};

struct IngressCounters
{
    u64 drops[kDropReasons] = {};
    u64 routed = 0U;
    u64 admitted = 0U;
    u64 admitted_validated = 0U;
    u64 retries = 0U;
    u64 negotiations = 0U;
    u64 resets = 0U;
    u64 answer_failures = 0U; // answers that could not be built (no randomness)
};

class Ingress
{
public:
    // `tokens` mints and checks address validation tokens (null: no Retry, and tokens are not read); `new_tokens`
    // makes NEW_TOKEN tokens single use (null: they are not accepted); `reset` derives the stateless reset tokens of
    // the IDs this server issues (null: no stateless reset). All must outlive the ingress.
    Ingress(const IngressConfig& config, const ConnectionIdTable& routes, HandshakeBuckets& buckets,
            const crypto::QuicTokenKeys* tokens, crypto::ReplayFilter* new_tokens,
            const crypto::QuicResetKey* reset = nullptr) noexcept;

    // False when the configuration is out of range (no version, a version this build cannot run or a reserved one,
    // a connection ID length outside 8 to 20).
    [[nodiscard]] bool ready() const noexcept { return m_ready; }
    [[nodiscard]] const IngressConfig& config() const noexcept { return m_config; }

    // S1 to S3 for one datagram from `peer`, received at `now_ns` (the loop's monotonic clock) and `unix_seconds` (the
    // fleet's clock, which tokens carry). `truncated`: the datagram was longer than its slot.
    [[nodiscard]] IngressDecision classify(cont::ConstSpan<u8> datagram, bool truncated, const Endpoint& peer,
                                           u64 now_ns, u64 unix_seconds) noexcept;

    // The Retry, Version Negotiation or stateless reset packet a decision asks for, into `out`
    // (kQuicMaxStatelessAnswerBytes); returns its bytes, zero when it could not be built (counted). The decision's
    // header views the classified datagram, which must still be alive.
    [[nodiscard]] usize answer(const IngressDecision& decision, const Endpoint& peer, u64 unix_seconds,
                               cont::Span<u8> out) noexcept;

    [[nodiscard]] const IngressCounters& counters() const noexcept { return m_counters; }

private:
    [[nodiscard]] bool runs(u32 version) const noexcept;
    [[nodiscard]] IngressDecision drop(IngressDecision decision, DropReason reason) noexcept;
    [[nodiscard]] usize stateless_reset(const IngressDecision& decision, u64 bits, cont::Span<u8> out) noexcept;
    [[nodiscard]] IngressDecision admit_initial(IngressDecision decision, cont::ConstSpan<u8> datagram,
                                                const Endpoint& peer, u64 now_ns, u64 unix_seconds) noexcept;

    IngressConfig m_config;
    const ConnectionIdTable& m_routes;
    HandshakeBuckets& m_buckets;
    const crypto::QuicTokenKeys* m_tokens = nullptr;
    crypto::ReplayFilter* m_new_tokens = nullptr;
    const crypto::QuicResetKey* m_reset = nullptr;
    IngressCounters m_counters;
    bool m_ready = false;
};

} // namespace crd::net
