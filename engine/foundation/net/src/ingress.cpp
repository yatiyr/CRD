// crd-net -- admission stages S1 to S3 and the stateless answers (4.2a; docs/design/network-foundation.md#ingress).

#include <crd/memory/allocator.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_packet.hpp>

#include <new>

namespace crd::net
{
namespace
{
constexpr u32 kMaxPrefixBuckets = u32{1} << 24U;

bool gate_open(const SharedRateGate& gate, const GateRule& rule, u64 now_ns) noexcept
{
    return gate.open_at(rule) <= now_ns;
}

// An Initial from the server to the client's Source Connection ID, sealed under the keys of the Destination
// Connection ID it chose, holding one CONNECTION_CLOSE with `error` (RFC 9000 sections 8.1.2 and 10.2.3); not
// ack-eliciting, so not padded. Its bytes, zero when it could not be built.
usize close_initial(const QuicHeader& header, QuicTransportError error, cont::Span<u8> out) noexcept
{
    const auto version = static_cast<crypto::QuicVersion>(header.version);
    QuicPacketSpec spec;
    spec.type = QuicPacketType::Initial;
    spec.version = header.version;
    spec.dcid = header.scid;
    spec.scid = header.dcid;
    QuicFrame close;
    close.kind = QuicFrameKind::ConnectionClose;
    close.error_code = static_cast<u64>(error);
    QuicKeys keys;
    QuicPacketWriter writer;
    usize written = 0U;
    if (quic_initial_keys(version, crypto::QuicSide::Server, header.dcid, keys) != crypto::CryptoError::None ||
        !writer.begin(spec, out))
    {
        return 0U;
    }
    const usize used = write_quic_frame(close, writer.frames());
    if (used == 0U ||
        writer.seal(keys.packet, keys.header, false, used, QuicPadding{}, written) != crypto::CryptoError::None)
    {
        return 0U;
    }
    return written;
}
} // namespace

// ---- HandshakeBuckets ----

HandshakeBuckets::HandshakeBuckets(memory::IAllocator& allocator, const HandshakeRules& rules,
                                   cont::ConstSpan<u8> key) noexcept
    : m_allocator(allocator), m_rules(rules)
{
    const u32 buckets = rules.prefix_buckets;
    if (buckets == 0U || buckets > kMaxPrefixBuckets || (buckets & (buckets - 1U)) != 0U ||
        rules.ipv4_prefix_bits == 0U || rules.ipv4_prefix_bits > 32U || rules.ipv6_prefix_bits == 0U ||
        rules.ipv6_prefix_bits > 128U)
    {
        return;
    }
    const crypto::CryptoError keyed = key.empty() ? m_key.generate(crypto::AlgorithmId::Siphash24)
                                                  : m_key.init(crypto::AlgorithmId::Siphash24, key);
    if (keyed != crypto::CryptoError::None)
    {
        return;
    }
    void* const storage = allocator.try_allocate(sizeof(SharedRateGate) * buckets, alignof(SharedRateGate));
    if (storage == nullptr)
    {
        m_key.clear();
        return;
    }
    m_prefixes = static_cast<SharedRateGate*>(storage);
    for (u32 index = 0U; index < buckets; ++index)
    {
        new (&m_prefixes[index]) SharedRateGate{};
    }
}

HandshakeBuckets::~HandshakeBuckets()
{
    if (m_prefixes != nullptr)
    {
        for (u32 index = 0U; index < m_rules.prefix_buckets; ++index)
        {
            m_prefixes[index].~SharedRateGate();
        }
        m_allocator.deallocate(m_prefixes);
    }
}

u32 HandshakeBuckets::bucket_of(const IpAddress& peer) const noexcept
{
    const bool v4 = peer.family == AddressFamily::V4;
    const IpAddress prefix = peer.prefix(v4 ? m_rules.ipv4_prefix_bits : m_rules.ipv6_prefix_bits);
    // The family is hashed with the bytes, so 10.0.0.0/32 and 0a00::/64 never share a bucket by construction.
    u8 key[17] = {};
    key[0] = v4 ? 4U : 6U;
    for (usize index = 0U; index < 16U; ++index)
    {
        key[1U + index] = prefix.bytes[index];
    }
    u64 hash = 0U;
    // A ready table's key always hashes; were it ever to fail, everything shares bucket 0, which only tightens.
    (void)m_key.hash(cont::ConstSpan<u8>(key, sizeof(key)), hash);
    return static_cast<u32>(hash) & (m_rules.prefix_buckets - 1U);
}

HandshakeVerdict HandshakeBuckets::admit(const IpAddress& peer, bool validated, u64 now_ns) noexcept
{
    if (m_prefixes == nullptr)
    {
        return HandshakeVerdict::Refuse;
    }
    SharedRateGate& prefix = m_prefixes[bucket_of(peer)];
    if (validated)
    {
        if (!gate_open(m_validated, m_rules.validated, now_ns))
        {
            return HandshakeVerdict::Refuse;
        }
        m_validated.charge(1U, m_rules.validated, now_ns);
        prefix.charge(1U, m_rules.per_prefix, now_ns);
        return HandshakeVerdict::Admit;
    }
    if (m_rules.always_retry || !gate_open(m_unvalidated, m_rules.unvalidated, now_ns) ||
        !gate_open(prefix, m_rules.per_prefix, now_ns))
    {
        return HandshakeVerdict::Retry;
    }
    m_unvalidated.charge(1U, m_rules.unvalidated, now_ns);
    prefix.charge(1U, m_rules.per_prefix, now_ns);
    return HandshakeVerdict::Admit;
}

bool HandshakeBuckets::answer(u64 now_ns) noexcept
{
    if (m_prefixes == nullptr || !gate_open(m_answers, m_rules.answers, now_ns))
    {
        return false;
    }
    m_answers.charge(1U, m_rules.answers, now_ns);
    return true;
}

// ---- Ingress ----

Ingress::Ingress(const IngressConfig& config, const ConnectionIdTable& routes, HandshakeBuckets& buckets,
                 const crypto::QuicTokenKeys* tokens, crypto::ReplayFilter* new_tokens,
                 const crypto::QuicResetKey* reset) noexcept
    : m_config(config), m_routes(routes), m_buckets(buckets), m_tokens(tokens), m_new_tokens(new_tokens), m_reset(reset)
{
    if (config.version_count == 0U || config.version_count > kQuicMaxNegotiationVersions ||
        config.local_cid_bytes < kQuicMinClientDcidBytes ||
        config.local_cid_bytes > crypto::kQuicMaxConnectionIdBytes)
    {
        return;
    }
    for (usize index = 0U; index < config.version_count; ++index)
    {
        if (!crypto::quic_version_supported(config.versions[index]))
        {
            return;
        }
    }
    m_ready = true;
}

bool Ingress::runs(u32 version) const noexcept
{
    for (usize index = 0U; index < m_config.version_count; ++index)
    {
        if (m_config.versions[index] == version)
        {
            return true;
        }
    }
    return false;
}

IngressDecision Ingress::drop(IngressDecision decision, DropReason reason) noexcept
{
    decision.action = IngressAction::Drop;
    decision.reason = reason;
    ++m_counters.drops[static_cast<usize>(reason)];
    return decision;
}

IngressDecision Ingress::classify(cont::ConstSpan<u8> datagram, bool truncated, const Endpoint& peer, u64 now_ns,
                                  u64 unix_seconds) noexcept
{
    IngressDecision decision;
    if (!m_ready)
    {
        return drop(decision, DropReason::Overloaded);
    }
    if (truncated)
    {
        return drop(decision, DropReason::Truncated);
    }
    switch (parse_quic_header(datagram, m_config.local_cid_bytes, decision.header))
    {
    case QuicHeaderError::None:
        break;
    case QuicHeaderError::ConnectionIdTooLong:
        return drop(decision, DropReason::ConnectionIdTooLong);
    case QuicHeaderError::TooShortToProtect:
        return drop(decision, DropReason::TooShortToProtect);
    case QuicHeaderError::Empty:
    case QuicHeaderError::Truncated:
    case QuicHeaderError::LengthPastDatagram:
        return drop(decision, DropReason::Malformed);
    }

    const QuicHeader& header = decision.header;
    switch (header.type)
    {
    case QuicPacketType::VersionNegotiation:
        return drop(decision, DropReason::VersionNegotiation);
    case QuicPacketType::Retry:
        return drop(decision, DropReason::ClientRetry);
    case QuicPacketType::OneRtt:
    case QuicPacketType::Initial:
    case QuicPacketType::ZeroRtt:
    case QuicPacketType::Handshake:
    case QuicPacketType::OtherVersion:
        break;
    }

    if (header.type == QuicPacketType::OtherVersion || (header.type != QuicPacketType::OneRtt && !runs(header.version)))
    {
        if (datagram.size() < kQuicMinInitialDatagram)
        {
            return drop(decision, DropReason::UnsupportedSmall);
        }
        if (!m_buckets.answer(now_ns))
        {
            return drop(decision, DropReason::Overloaded);
        }
        decision.action = IngressAction::NegotiateVersion;
        return decision;
    }

    decision.connection = m_routes.find(header.dcid);
    if (decision.connection != kNoConnection)
    {
        decision.action = IngressAction::Route;
        ++m_counters.routed;
        return decision;
    }
    switch (header.type)
    {
    case QuicPacketType::OneRtt:
        // A reset shorter than its trigger and at least 21 bytes long (section 10.3.3), within the answer rate.
        if (m_reset == nullptr || datagram.size() <= kQuicMinResetBytes || !m_buckets.answer(now_ns))
        {
            return drop(decision, DropReason::UnknownConnection);
        }
        decision.action = IngressAction::StatelessReset;
        decision.datagram_bytes = datagram.size();
        return decision;
    case QuicPacketType::Initial:
        return admit_initial(decision, datagram, peer, now_ns, unix_seconds);
    case QuicPacketType::ZeroRtt:
        // Ahead of its Initial, under the ID the client chose (section 7.2), the handler may hold it (section 5.2.2).
        if (header.dcid.size() < kQuicMinClientDcidBytes)
        {
            break;
        }
        decision.action = IngressAction::Early;
        ++m_counters.early;
        return decision;
    case QuicPacketType::Handshake:
    case QuicPacketType::Retry:
    case QuicPacketType::VersionNegotiation:
    case QuicPacketType::OtherVersion:
        break;
    }
    return drop(decision, DropReason::EarlyPacket);
}

IngressDecision Ingress::admit_initial(IngressDecision decision, cont::ConstSpan<u8> datagram, const Endpoint& peer,
                                       u64 now_ns, u64 unix_seconds) noexcept
{
    const QuicHeader& header = decision.header;
    if (datagram.size() < kQuicMinInitialDatagram)
    {
        return drop(decision, DropReason::SmallInitial);
    }
    if (header.dcid.size() < kQuicMinClientDcidBytes)
    {
        return drop(decision, DropReason::ShortClientDcid);
    }
    const auto version = static_cast<crypto::QuicVersion>(header.version);
    if (m_tokens != nullptr && !header.token.empty())
    {
        const crypto::QuicTokenKind kind = crypto::quic_token_kind(header.token);
        if (kind == crypto::QuicTokenKind::Retry)
        {
            if (m_tokens->validate(version, header.token, quic_token_peer(peer), header.dcid, header.scid, unix_seconds,
                                   decision.claims) != crypto::CryptoError::None)
            {
                if (!m_buckets.answer(now_ns))
                {
                    return drop(decision, DropReason::InvalidRetryToken);
                }
                decision.action = IngressAction::InvalidToken;
                return decision;
            }
            decision.validated = true;
        }
        else if (kind == crypto::QuicTokenKind::NewToken && m_new_tokens != nullptr)
        {
            crypto::QuicTokenClaims claims;
            decision.validated =
                m_tokens->validate(version, header.token, quic_token_peer(peer), header.dcid, header.scid, unix_seconds,
                                   claims, m_new_tokens) == crypto::CryptoError::None;
        }
    }

    switch (m_buckets.admit(peer.address, decision.validated, now_ns))
    {
    case HandshakeVerdict::Admit:
        decision.action = IngressAction::Admit;
        ++m_counters.admitted;
        if (decision.validated)
        {
            ++m_counters.admitted_validated;
        }
        return decision;
    case HandshakeVerdict::Refuse:
        return drop(decision, DropReason::Overloaded);
    case HandshakeVerdict::Retry:
        break;
    }
    if (m_tokens == nullptr || !m_buckets.answer(now_ns))
    {
        return drop(decision, DropReason::Overloaded);
    }
    decision.action = IngressAction::Retry;
    return decision;
}

usize Ingress::answer(const IngressDecision& decision, const Endpoint& peer, u64 unix_seconds,
                      cont::Span<u8> out) noexcept
{
    const QuicHeader& header = decision.header;
    u8 entropy[8 + crypto::kQuicMaxConnectionIdBytes] = {};
    if (crypto::random_bytes(cont::Span<u8>(entropy, sizeof(entropy))) != crypto::CryptoError::None)
    {
        ++m_counters.answer_failures;
        return 0U;
    }
    u64 bits = 0U;
    for (usize index = 0U; index < 8U; ++index)
    {
        bits = (bits << 8U) | entropy[index];
    }

    if (decision.action == IngressAction::StatelessReset)
    {
        return stateless_reset(decision, bits, out);
    }
    if (decision.action == IngressAction::InvalidToken)
    {
        const usize written = close_initial(header, QuicTransportError::InvalidToken, out);
        if (written == 0U)
        {
            ++m_counters.answer_failures;
            return 0U;
        }
        ++m_counters.token_closes;
        return written;
    }
    if (decision.action == IngressAction::NegotiateVersion)
    {
        const usize written = write_version_negotiation(
            header.dcid, header.scid, cont::ConstSpan<u32>(m_config.versions, m_config.version_count), bits, out);
        if (written == 0U)
        {
            ++m_counters.answer_failures;
            return 0U;
        }
        ++m_counters.negotiations;
        return written;
    }
    if (decision.action != IngressAction::Retry || m_tokens == nullptr)
    {
        return 0U;
    }

    const auto version = static_cast<crypto::QuicVersion>(header.version);
    const cont::ConstSpan<u8> retry_scid(entropy + 8U, m_config.local_cid_bytes);
    u8 token[crypto::kQuicMaxRetryTokenBytes] = {};
    usize token_bytes = 0U;
    usize written = 0U;
    if (m_tokens->mint_retry(version, quic_token_peer(peer), header.dcid, header.scid, retry_scid, unix_seconds,
                             cont::Span<u8>(token, sizeof(token)), token_bytes) != crypto::CryptoError::None ||
        write_retry(version, header.scid, retry_scid, header.dcid, cont::ConstSpan<u8>(token, token_bytes),
                    static_cast<u8>(bits), out, written) != crypto::CryptoError::None)
    {
        ++m_counters.answer_failures;
        return 0U;
    }
    ++m_counters.retries;
    return written;
}

usize Ingress::stateless_reset(const IngressDecision& decision, u64 bits, cont::Span<u8> out) noexcept
{
    const usize trigger = decision.datagram_bytes;
    const usize longest = (trigger - 1U < kQuicMaxResetAnswerBytes) ? trigger - 1U : kQuicMaxResetAnswerBytes;
    constexpr usize short_trigger = 43U; // a trigger this short gets a reset one byte shorter (section 10.3)
    const usize bytes = (trigger <= short_trigger || longest <= short_trigger)
                            ? longest
                            : short_trigger + static_cast<usize>(bits % (longest - short_trigger + 1U));
    if (m_reset == nullptr || trigger <= kQuicMinResetBytes || out.size() < bytes)
    {
        ++m_counters.answer_failures;
        return 0U;
    }
    // The fixed bits 01, then unpredictable bits, then the token of the Destination Connection ID (section 10.3).
    const usize unpredictable = bytes - crypto::kQuicResetTokenBytes;
    if (crypto::random_bytes(out.first(unpredictable)) != crypto::CryptoError::None ||
        m_reset->token(decision.header.dcid, out.subspan(unpredictable, crypto::kQuicResetTokenBytes)) !=
            crypto::CryptoError::None)
    {
        ++m_counters.answer_failures;
        return 0U;
    }
    out[0] = static_cast<u8>((out[0] & 0x3FU) | 0x40U);
    ++m_counters.resets;
    return bytes;
}

usize Ingress::refuse(const IngressDecision& decision, u64 now_ns, cont::Span<u8> out) noexcept
{
    if (decision.action != IngressAction::Admit || !m_buckets.answer(now_ns))
    {
        return 0U;
    }
    const usize written = close_initial(decision.header, QuicTransportError::ConnectionRefused, out);
    if (written == 0U)
    {
        ++m_counters.answer_failures;
        return 0U;
    }
    ++m_counters.refusals;
    return written;
}

} // namespace crd::net
