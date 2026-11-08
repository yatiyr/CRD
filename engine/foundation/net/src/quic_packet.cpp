// crd-net -- QUIC packets per encryption level (4.2a; docs/design/network-foundation.md#quic-packets).

#include <crd/crypto/tls.hpp>
#include <crd/net/frame.hpp>
#include <crd/net/quic_packet.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
constexpr u8 kLongForm = 0x80U;
constexpr u8 kFixedBit = 0x40U;
constexpr u8 kSpinBit = 0x20U;
constexpr u8 kKeyPhaseBit = 0x04U;
constexpr u8 kLongReserved = 0x0CU;  // RFC 9000 section 17.2
constexpr u8 kShortReserved = 0x18U; // RFC 9000 section 17.3.1
constexpr usize kTag = crypto::kQuicTagBytes;
constexpr usize kTwoByteLength = (usize{1} << 14U) - 1U; // the largest Length a 2-byte varint holds
constexpr usize kLengthSizes[] = {1U, 2U, 4U};           // the Length encodings seal() chooses among
constexpr bool kDirections[] = {true, false};            // sending, then receiving

constexpr crypto::QuicSide other_side(crypto::QuicSide side) noexcept
{
    return (side == crypto::QuicSide::Client) ? crypto::QuicSide::Server : crypto::QuicSide::Client;
}

// The least payload that leaves header protection its sample: four bytes from the packet number on.
constexpr usize sample_floor(usize number_bytes) noexcept
{
    return (number_bytes < 4U) ? 4U - number_bytes : 0U;
}

bool long_header(QuicPacketType type) noexcept
{
    return type == QuicPacketType::Initial || type == QuicPacketType::ZeroRtt || type == QuicPacketType::Handshake;
}

// `value` as a varint of exactly `bytes` (1, 2 or 4) bytes at `out`; the caller chose a size that holds it.
void put_varint_sized(u8* out, u64 value, usize bytes) noexcept
{
    const auto prefix = static_cast<u8>(((bytes == 4U) ? 2U : bytes - 1U) << 6U); // 0x00, 0x40 or 0x80
    for (usize i = 0U; i < bytes; ++i)
    {
        out[i] = static_cast<u8>(value >> (8U * (bytes - 1U - i)));
    }
    out[0] = static_cast<u8>(out[0] | prefix);
}
} // namespace

// ---- Keys ----

crypto::CryptoError QuicKeys::init(crypto::QuicVersion version, crypto::AlgorithmId aead,
                                   cont::ConstSpan<u8> secret) noexcept
{
    crypto::CryptoError error = packet.init(version, aead, secret);
    if (error == crypto::CryptoError::None)
    {
        error = header.init(version, aead, secret);
    }
    if (error != crypto::CryptoError::None)
    {
        clear();
    }
    return error;
}

crypto::CryptoError quic_initial_keys(crypto::QuicVersion version, crypto::QuicSide side,
                                      cont::ConstSpan<u8> client_dcid, QuicKeys& out) noexcept
{
    u8 secret[crypto::kQuicInitialSecretBytes] = {};
    crypto::CryptoError error = crypto::quic_initial_secret(version, side, client_dcid, secret);
    if (error == crypto::CryptoError::None)
    {
        error = out.init(version, crypto::AlgorithmId::Aes128Gcm, secret);
    }
    crypto::secure_zero(secret);
    return error;
}

// ---- Writing ----

bool QuicPacketWriter::begin(const QuicPacketSpec& spec, cont::Span<u8> room) noexcept
{
    m_open = false;
    const bool is_long = long_header(spec.type);
    if ((!is_long && spec.type != QuicPacketType::OneRtt) || spec.dcid.size() > crypto::kQuicMaxConnectionIdBytes ||
        spec.scid.size() > crypto::kQuicMaxConnectionIdBytes ||
        (!spec.token.empty() && spec.type != QuicPacketType::Initial) || spec.number > crypto::kQuicMaxPacketNumber)
    {
        return false;
    }
    if (is_long && !crypto::quic_version_supported(spec.version))
    {
        return false;
    }
    const usize needed = crypto::quic_packet_number_bytes(spec.number, spec.largest_acked, spec.acknowledged);
    const usize number_bytes = (spec.number_bytes == 0U) ? needed : spec.number_bytes;
    if (needed == 0U || number_bytes < needed || number_bytes > 4U)
    {
        return false;
    }

    u8 first = static_cast<u8>(number_bytes - 1U);
    if (!spec.clear_quic_bit)
    {
        first = static_cast<u8>(first | kFixedBit);
    }
    usize before = 1U + spec.dcid.size();
    if (is_long)
    {
        const auto version = static_cast<crypto::QuicVersion>(spec.version);
        first = static_cast<u8>(first | kLongForm | (quic_long_type_bits(version, spec.type) << 4U));
        before = 1U + 4U + 1U + spec.dcid.size() + 1U + spec.scid.size();
        if (spec.type == QuicPacketType::Initial)
        {
            before += quic_varint_bytes(spec.token.size()) + spec.token.size();
        }
    }
    else if (spec.spin)
    {
        first = static_cast<u8>(first | kSpinBit);
    }
    const usize floor = before + number_bytes + sample_floor(number_bytes) + kTag;
    if (room.size() < floor)
    {
        return false;
    }
    // The Length is reserved at the size the whole room would need and shrunk at seal() to the packet's own.
    usize length_bytes = 0U;
    if (is_long)
    {
        length_bytes = (room.size() - before <= kTwoByteLength) ? 2U : 4U;
    }
    if (room.size() < floor + length_bytes)
    {
        return false;
    }

    cont::WireWriter out(room);
    out.put_u8(first);
    if (is_long)
    {
        out.put_u32(spec.version);
        out.put_u8(static_cast<u8>(spec.dcid.size()));
        out.put_bytes(spec.dcid);
        out.put_u8(static_cast<u8>(spec.scid.size()));
        out.put_bytes(spec.scid);
        if (spec.type == QuicPacketType::Initial)
        {
            put_quic_varint(out, spec.token.size());
            out.put_bytes(spec.token);
        }
    }
    else
    {
        out.put_bytes(spec.dcid);
    }
    m_room = room;
    m_before_length = before;
    m_length_bytes = length_bytes;
    m_number_bytes = number_bytes;
    m_number = spec.number;
    m_type = spec.type;
    m_open = true;
    return true;
}

cont::Span<u8> QuicPacketWriter::frames() const noexcept
{
    if (!m_open)
    {
        return {};
    }
    const usize at = m_before_length + m_length_bytes + m_number_bytes;
    return m_room.subspan(at, m_room.size() - at - kTag);
}

crypto::CryptoError QuicPacketWriter::seal(crypto::QuicPacketKey& packet, const crypto::QuicHeaderKey& header,
                                           bool key_phase, usize used, QuicPadding padding, usize& written) noexcept
{
    if (!m_open)
    {
        return crypto::CryptoError::NotReady;
    }
    if (used > frames().size())
    {
        return crypto::CryptoError::BadLength;
    }
    const usize room = m_room.size();
    const usize payload = (used < sample_floor(m_number_bytes)) ? sample_floor(m_number_bytes) : used;

    // The size for a Length of `length_bytes`: the frames with the sample's floor, the channel's block, the floor.
    const auto target_for = [&](usize length_bytes)
    {
        const usize base = m_before_length + length_bytes + m_number_bytes + payload + kTag;
        const usize padded = padded_size(base, padding.block, room);
        return (padded < padding.min_bytes) ? padding.min_bytes : padded;
    };
    usize length_bytes = m_length_bytes;
    usize total = target_for(length_bytes);
    if (m_length_bytes != 0U)
    {
        // The shortest Length that holds the packet it describes (a shorter one leaves more to pad).
        for (const usize bytes : kLengthSizes)
        {
            if (bytes > m_length_bytes)
            {
                break;
            }
            const usize candidate = target_for(bytes);
            const usize length = candidate - m_before_length - bytes;
            if (quic_varint_bytes(length) <= bytes)
            {
                length_bytes = bytes;
                total = candidate;
                break;
            }
        }
    }
    if (total > room)
    {
        return crypto::CryptoError::BadLength;
    }

    u8* const base = m_room.data();
    const usize number_at = m_before_length + length_bytes;
    const usize frames_at = number_at + m_number_bytes;
    const usize header_end = frames_at;
    if (length_bytes != m_length_bytes)
    {
        std::memmove(base + frames_at, base + m_before_length + m_length_bytes + m_number_bytes, used);
    }
    const usize body = total - frames_at - kTag; // the frames and their PADDING
    std::memset(base + frames_at + used, 0, body - used);
    if (m_length_bytes != 0U)
    {
        put_varint_sized(base + m_before_length, total - number_at, length_bytes);
    }
    else if (key_phase)
    {
        base[0] = static_cast<u8>(base[0] | kKeyPhaseBit);
    }
    for (usize i = 0U; i < m_number_bytes; ++i)
    {
        base[number_at + i] = static_cast<u8>(m_number >> (8U * (m_number_bytes - 1U - i)));
    }

    const cont::Span<u8> sealed = m_room.first(total);
    crypto::CryptoError error = packet.seal(m_number, sealed, header_end);
    if (error == crypto::CryptoError::None)
    {
        error = header.protect(sealed, number_at);
    }
    if (error != crypto::CryptoError::None)
    {
        return error;
    }
    m_open = false;
    written = total;
    return crypto::CryptoError::None;
}

// ---- Reading ----

QuicDatagramPackets::QuicDatagramPackets(cont::Span<u8> datagram, usize short_dcid_bytes) noexcept
    : m_datagram(datagram), m_short_dcid_bytes(short_dcid_bytes)
{
}

bool QuicDatagramPackets::next(QuicHeader& header, cont::Span<u8>& packet) noexcept
{
    if (m_at >= m_datagram.size() || m_foreign || m_error != QuicHeaderError::None)
    {
        return false;
    }
    const cont::Span<u8> rest = m_datagram.subspan(m_at);
    m_error = parse_quic_header(rest, m_short_dcid_bytes, header);
    if (m_error != QuicHeaderError::None)
    {
        return false;
    }
    if (m_at == 0U)
    {
        m_first_dcid = header.dcid;
    }
    else if (header.dcid.size() != m_first_dcid.size() ||
             (!header.dcid.empty() && std::memcmp(header.dcid.data(), m_first_dcid.data(), header.dcid.size()) != 0))
    {
        m_foreign = true;
        return false;
    }
    packet = rest.first(header.packet_bytes);
    m_at += header.packet_bytes;
    return true;
}

QuicTransportError quic_packet_close(QuicPacketError error) noexcept
{
    switch (error)
    {
        case QuicPacketError::ReservedBits:
        case QuicPacketError::Empty:
            return QuicTransportError::ProtocolViolation;
        case QuicPacketError::KeyUpdate:
            return QuicTransportError::KeyUpdateError;
        case QuicPacketError::IntegrityLimit:
        case QuicPacketError::Exhausted:
            return QuicTransportError::AeadLimitReached;
        default:
            return QuicTransportError::NoError;
    }
}

// ---- Protection ----

QuicPacketProtection::~QuicPacketProtection() noexcept = default;

void QuicPacketProtection::clear_all() noexcept
{
    for (QuicKeys& keys : m_initial)
    {
        keys.clear();
    }
    m_initial_receive = 1U;
    for (QuicKeys& keys : m_handshake)
    {
        keys.clear();
    }
    m_early.clear();
    for (crypto::QuicPacketKey& key : m_send)
    {
        key.clear();
    }
    for (crypto::QuicPacketKey& key : m_recv)
    {
        key.clear();
    }
    m_send_header.clear();
    m_recv_header.clear();
    for (crypto::QuicPacketWindow& window : m_windows)
    {
        window = crypto::QuicPacketWindow{};
    }
    m_recv_current = 0U;
    m_phases = Phases{};
    m_forged = 0U;
    m_confirmed = false;
    m_accept_cleared_bit = false;
    m_initial_gone = false;
    m_handshake_gone = false;
    m_early_gone = false;
}

crypto::CryptoError QuicPacketProtection::start(crypto::QuicSide side, crypto::QuicVersion version,
                                                cont::ConstSpan<u8> client_dcid) noexcept
{
    clear_all();
    m_side = side;
    m_started = true;
    m_version = version;
    m_original = version;
    return rekey_initial(client_dcid);
}

crypto::CryptoError QuicPacketProtection::rekey_initial(cont::ConstSpan<u8> client_dcid) noexcept
{
    if (!m_started || m_initial_gone)
    {
        return crypto::CryptoError::NotReady;
    }
    crypto::CryptoError error = quic_initial_keys(m_version, m_side, client_dcid, m_initial[0]);
    if (error == crypto::CryptoError::None)
    {
        error = quic_initial_keys(m_version, other_side(m_side), client_dcid, initial_receive());
    }
    if (error == crypto::CryptoError::None && initial_other().ready())
    {
        error = quic_initial_keys(m_other, other_side(m_side), client_dcid, initial_other());
    }
    if (error != crypto::CryptoError::None)
    {
        for (QuicKeys& keys : m_initial)
        {
            keys.clear();
        }
    }
    return error;
}

crypto::CryptoError QuicPacketProtection::expect_version(crypto::QuicVersion version,
                                                         cont::ConstSpan<u8> client_dcid) noexcept
{
    if (!m_started || m_initial_gone || m_handshake[0].ready() || m_handshake[1].ready())
    {
        return crypto::CryptoError::NotReady;
    }
    if (version == m_version || initial_other().ready())
    {
        return crypto::CryptoError::Overlap;
    }
    m_other = version;
    return quic_initial_keys(version, other_side(m_side), client_dcid, initial_other());
}

crypto::CryptoError QuicPacketProtection::change_version(crypto::QuicVersion version,
                                                         cont::ConstSpan<u8> client_dcid) noexcept
{
    if (!m_started || m_initial_gone || m_handshake[0].ready() || m_handshake[1].ready())
    {
        return crypto::CryptoError::NotReady;
    }
    if (version == m_version)
    {
        return crypto::CryptoError::Overlap;
    }
    // The running version's receive key becomes the other version's (a client's expected key, already derived for
    // `version`, becomes the running one); the send key follows. A failure leaves no Initial key.
    QuicKeys& receive = initial_other();
    crypto::CryptoError error = crypto::CryptoError::None;
    if (!receive.ready() || m_other != version)
    {
        error = quic_initial_keys(version, other_side(m_side), client_dcid, receive);
    }
    if (error == crypto::CryptoError::None)
    {
        error = quic_initial_keys(version, m_side, client_dcid, m_initial[0]);
    }
    if (error != crypto::CryptoError::None)
    {
        for (QuicKeys& keys : m_initial)
        {
            keys.clear();
        }
        return error;
    }
    m_initial_receive = 3U - m_initial_receive;
    m_other = m_version;
    m_version = version;
    return crypto::CryptoError::None;
}

bool QuicPacketProtection::opens_initial(u32 version) const noexcept
{
    return (version == static_cast<u32>(m_version) && initial_receive().ready()) ||
           (version == static_cast<u32>(m_other) && initial_other().ready());
}

crypto::CryptoError QuicPacketProtection::install(crypto::QuicHandshake& handshake) noexcept
{
    constexpr crypto::QuicLevel levels[] = {crypto::QuicLevel::Handshake, crypto::QuicLevel::Application,
                                            crypto::QuicLevel::Early};
    for (const crypto::QuicLevel level : levels)
    {
        for (const bool sending : kDirections)
        {
            const bool client_secret = (m_side == crypto::QuicSide::Client) == sending;
            const auto side = client_secret ? crypto::QuicSide::Client : crypto::QuicSide::Server;
            if (!handshake.has_keys(level, side))
            {
                continue;
            }
            crypto::QuicPacketKey* packet = nullptr;
            crypto::QuicHeaderKey* header = nullptr;
            if (level == crypto::QuicLevel::Handshake && !m_handshake_gone)
            {
                packet = &m_handshake[sending ? 0U : 1U].packet;
                header = &m_handshake[sending ? 0U : 1U].header;
            }
            else if (level == crypto::QuicLevel::Application && m_phases.send == 0U && m_phases.recv == 0U)
            {
                packet = sending ? &m_send[0] : &m_recv[0];
                header = sending ? &m_send_header : &m_recv_header;
            }
            else if (level == crypto::QuicLevel::Early && !m_early_gone && client_secret)
            {
                packet = &m_early.packet;
                header = &m_early.header;
            }
            if (packet == nullptr || packet->ready())
            {
                continue;
            }
            // 0-RTT stays in the original version (RFC 9369 section 4.1).
            const crypto::QuicVersion version = (level == crypto::QuicLevel::Early) ? m_original : m_version;
            if (handshake.derive_keys(level, side, version, *packet, *header) != crypto::TlsError::None)
            {
                return crypto::CryptoError::ProviderFailure;
            }
            const crypto::CryptoError error = finish_install(level, sending);
            if (error != crypto::CryptoError::None)
            {
                return error;
            }
        }
    }
    return crypto::CryptoError::None;
}

crypto::CryptoError QuicPacketProtection::install(crypto::QuicLevel level, bool sending, crypto::AlgorithmId aead,
                                                  cont::ConstSpan<u8> secret) noexcept
{
    if (!m_started)
    {
        return crypto::CryptoError::NotReady;
    }
    QuicKeys* keys = nullptr;
    switch (level)
    {
        case crypto::QuicLevel::Handshake:
            if (m_handshake_gone)
            {
                return crypto::CryptoError::NotReady;
            }
            keys = &m_handshake[sending ? 0U : 1U];
            break;
        case crypto::QuicLevel::Early:
            if ((m_side == crypto::QuicSide::Client) != sending)
            {
                return crypto::CryptoError::Overlap;
            }
            if (m_early_gone)
            {
                return crypto::CryptoError::NotReady;
            }
            keys = &m_early;
            break;
        case crypto::QuicLevel::Application:
        {
            crypto::QuicPacketKey& packet = sending ? m_send[0] : m_recv[0];
            crypto::QuicHeaderKey& header = sending ? m_send_header : m_recv_header;
            if (packet.ready() || header.ready() || m_phases.send != 0U || m_phases.recv != 0U)
            {
                return crypto::CryptoError::Exists;
            }
            crypto::CryptoError error = packet.init(m_version, aead, secret);
            if (error == crypto::CryptoError::None)
            {
                error = header.init(m_version, aead, secret);
            }
            if (error != crypto::CryptoError::None)
            {
                packet.clear();
                header.clear();
                return error;
            }
            return finish_install(level, sending);
        }
        default:
            return crypto::CryptoError::Unsupported;
    }
    if (keys->ready())
    {
        return crypto::CryptoError::Exists;
    }
    return keys->init((level == crypto::QuicLevel::Early) ? m_original : m_version, aead, secret);
}

crypto::CryptoError QuicPacketProtection::finish_install(crypto::QuicLevel level, bool sending) noexcept
{
    if (level != crypto::QuicLevel::Application)
    {
        return crypto::CryptoError::None;
    }
    if (sending)
    {
        m_send[0].set_limit(send_limit());
        // A client discards its 0-RTT keys once it has 1-RTT keys (RFC 9001 section 4.9.3).
        if (m_side == crypto::QuicSide::Client)
        {
            discard(crypto::QuicLevel::Early);
        }
        return crypto::CryptoError::None;
    }
    // The next phase's receive key, ready before any packet needs it (section 6.3).
    m_recv_current = 0U;
    return m_recv[1].update(m_recv[0]);
}

bool QuicPacketProtection::can_send(QuicPacketType type) const noexcept
{
    switch (type)
    {
        case QuicPacketType::Initial:
            return m_initial[0].ready();
        case QuicPacketType::Handshake:
            return m_handshake[0].ready();
        case QuicPacketType::ZeroRtt:
            return m_side == crypto::QuicSide::Client && m_early.ready();
        case QuicPacketType::OneRtt:
            return m_send[m_phases.send & 1U].ready() && m_send_header.ready();
        default:
            return false;
    }
}

bool QuicPacketProtection::can_open(QuicPacketType type) const noexcept
{
    switch (type)
    {
        case QuicPacketType::Initial:
            return initial_receive().ready();
        case QuicPacketType::Handshake:
            return m_handshake[1].ready();
        case QuicPacketType::ZeroRtt:
            return m_side == crypto::QuicSide::Server && m_early.ready();
        case QuicPacketType::OneRtt:
            return m_recv[m_recv_current].ready() && m_recv_header.ready();
        default:
            return false;
    }
}

void QuicPacketProtection::discard(crypto::QuicLevel level) noexcept
{
    switch (level)
    {
        case crypto::QuicLevel::Initial:
            for (QuicKeys& keys : m_initial)
            {
                keys.clear();
            }
            m_initial_gone = true;
            break;
        case crypto::QuicLevel::Handshake:
            m_handshake[0].clear();
            m_handshake[1].clear();
            m_handshake_gone = true;
            break;
        case crypto::QuicLevel::Early:
            m_early.clear();
            m_early_gone = true;
            break;
        default:
            for (crypto::QuicPacketKey& key : m_send)
            {
                key.clear();
            }
            for (crypto::QuicPacketKey& key : m_recv)
            {
                key.clear();
            }
            m_send_header.clear();
            m_recv_header.clear();
            m_phases.previous_held = false;
            break;
    }
}

void QuicPacketProtection::confirm() noexcept
{
    m_confirmed = true;
    discard(crypto::QuicLevel::Handshake);
}

QuicPacketError QuicPacketProtection::seal(QuicPacketWriter& writer, usize used, QuicPadding padding,
                                           usize& written) noexcept
{
    const QuicPacketType type = writer.type();
    if (!can_send(type))
    {
        return QuicPacketError::NoKeys;
    }
    crypto::QuicPacketKey* packet = nullptr;
    const crypto::QuicHeaderKey* header = nullptr;
    switch (type)
    {
        case QuicPacketType::Initial:
            packet = &m_initial[0].packet;
            header = &m_initial[0].header;
            break;
        case QuicPacketType::Handshake:
            packet = &m_handshake[0].packet;
            header = &m_handshake[0].header;
            break;
        case QuicPacketType::ZeroRtt:
            packet = &m_early.packet;
            header = &m_early.header;
            break;
        default:
            if (m_send[m_phases.send & 1U].update_due() && can_update_keys())
            {
                (void)update_keys(); // can_update_keys() held, so only the provider can refuse; the limit then decides
            }
            packet = &m_send[m_phases.send & 1U];
            header = &m_send_header;
            break;
    }
    const crypto::CryptoError error = writer.seal(*packet, *header, (m_phases.send & 1U) != 0U, used, padding, written);
    if (error == crypto::CryptoError::Exhausted)
    {
        return QuicPacketError::Exhausted;
    }
    if (error != crypto::CryptoError::None)
    {
        return QuicPacketError::Refused;
    }
    if (type == QuicPacketType::OneRtt && !m_phases.send_first_known)
    {
        m_phases.send_first = writer.number();
        m_phases.send_first_known = true;
    }
    // A client's first Handshake packet ends its Initial keys (RFC 9001 section 4.9.1).
    if (type == QuicPacketType::Handshake && m_side == crypto::QuicSide::Client)
    {
        discard(crypto::QuicLevel::Initial);
    }
    return QuicPacketError::None;
}

QuicPacketError QuicPacketProtection::forged(crypto::AlgorithmId aead) noexcept
{
    ++m_forged;
    u64 limit = crypto::quic_integrity_limit(aead);
    if (m_failed_limit != 0U && (limit == 0U || m_failed_limit < limit))
    {
        limit = m_failed_limit;
    }
    return (limit != 0U && m_forged >= limit) ? QuicPacketError::IntegrityLimit : QuicPacketError::Forged;
}

QuicPacketError QuicPacketProtection::open(const QuicHeader& header, cont::Span<u8> packet, QuicOpened& out) noexcept
{
    const QuicPacketType type = header.type;
    if (!can_open(type))
    {
        return QuicPacketError::NoKeys;
    }
    if (packet.size() != header.packet_bytes)
    {
        return QuicPacketError::Unprotectable;
    }
    if ((header.first & kFixedBit) == 0U && !m_accept_cleared_bit)
    {
        return QuicPacketError::QuicBit; // RFC 9000 section 17.2: not a packet of this version
    }
    const crypto::QuicHeaderKey* header_key = &m_recv_header;
    const QuicKeys* keys = nullptr;
    if (type == QuicPacketType::Initial)
    {
        // RFC 9369 section 4.1: the peer's Initial packets may still come in the other version.
        if (!opens_initial(header.version))
        {
            return QuicPacketError::NoKeys;
        }
        keys = (header.version == static_cast<u32>(m_version)) ? &initial_receive() : &initial_other();
    }
    else if (type == QuicPacketType::Handshake)
    {
        keys = &m_handshake[1];
    }
    else if (type == QuicPacketType::ZeroRtt)
    {
        keys = &m_early;
    }
    if (keys != nullptr)
    {
        header_key = &keys->header;
    }

    usize number_bytes = 0U;
    u64 truncated = 0U;
    if (header_key->unprotect(packet, header.number_offset, number_bytes, truncated) != crypto::CryptoError::None)
    {
        return QuicPacketError::Unprotectable;
    }
    crypto::QuicPacketWindow& window = m_windows[static_cast<usize>(quic_space_of(type))];
    const u64 expected = window.any() ? window.largest() + 1U : 0U;
    const u64 number = crypto::quic_decode_packet_number(truncated, number_bytes, expected);
    const usize header_bytes = header.number_offset + number_bytes;
    const u8 first = packet[0];
    switch (window.check(number))
    {
        case crypto::ReplayVerdict::Duplicate:
            return QuicPacketError::Duplicate;
        case crypto::ReplayVerdict::Stale:
            return QuicPacketError::Stale;
        default:
            break;
    }

    out = QuicOpened{};
    if (keys == nullptr)
    {
        const QuicPacketError error = open_application(packet, header_bytes, number, (first & kKeyPhaseBit) != 0U, out);
        if (error != QuicPacketError::None)
        {
            return error;
        }
    }
    else if (keys->packet.open(number, packet, header_bytes) != crypto::CryptoError::None)
    {
        return forged(keys->packet.algorithm());
    }

    // Only now is the header authenticated (RFC 9000 sections 17.2 and 17.3.1, 12.4).
    if ((first & ((first & kLongForm) != 0U ? kLongReserved : kShortReserved)) != 0U)
    {
        return QuicPacketError::ReservedBits;
    }
    if (packet.size() == header_bytes + kTag)
    {
        return QuicPacketError::Empty;
    }
    (void)window.accept(number);
    // A server's first Handshake packet opened ends its Initial keys (RFC 9001 section 4.9.1).
    if (type == QuicPacketType::Handshake && m_side == crypto::QuicSide::Server)
    {
        discard(crypto::QuicLevel::Initial);
    }
    out.type = type;
    out.number = number;
    out.payload = packet.subspan(header_bytes, packet.size() - header_bytes - kTag);
    out.spin = type == QuicPacketType::OneRtt && (first & kSpinBit) != 0U;
    return QuicPacketError::None;
}

QuicPacketError QuicPacketProtection::open_application(cont::Span<u8> packet, usize header_bytes, u64 number,
                                                       bool phase, QuicOpened& out) noexcept
{
    const bool current_phase = ((m_phases.recv & 1U) != 0U) == phase;
    usize slot = m_recv_current;
    if (!current_phase)
    {
        const bool older = m_phases.previous_held && m_phases.recv_any && number < m_phases.recv_smallest;
        slot = (m_recv_current + (older ? 2U : 1U)) % 3U;
    }
    const crypto::QuicPacketKey& key = m_recv[slot];
    if (!key.ready())
    {
        return QuicPacketError::NoKeys;
    }
    if (key.open(number, packet, header_bytes) != crypto::CryptoError::None)
    {
        return forged(key.algorithm());
    }
    // Section 6.4: packets of older keys never carry higher packet numbers than packets of newer ones.
    if (current_phase)
    {
        if (m_phases.older_any && number < m_phases.older_largest)
        {
            return QuicPacketError::KeyUpdate;
        }
        m_phases.recv_smallest =
            (!m_phases.recv_any || number < m_phases.recv_smallest) ? number : m_phases.recv_smallest;
        m_phases.recv_largest = (!m_phases.recv_any || number > m_phases.recv_largest) ? number : m_phases.recv_largest;
        m_phases.recv_any = true;
        return QuicPacketError::None;
    }
    if (slot != (m_recv_current + 1U) % 3U)
    {
        m_phases.older_largest =
            (number > m_phases.older_largest) ? number : m_phases.older_largest; // a late packet of the previous phase
        return QuicPacketError::None;
    }
    // A packet of the next phase: the peer's update or its answer to ours. A second update before this side
    // acknowledged the first in the new phase is section 6.2's KEY_UPDATE_ERROR.
    if ((m_phases.recv_any && number < m_phases.recv_largest) || m_phases.answer_owed)
    {
        return QuicPacketError::KeyUpdate;
    }
    rotate_receive(number);
    out.key_update = true;
    if (m_phases.send < m_phases.recv)
    {
        crypto::QuicPacketKey& next = m_send[m_phases.recv & 1U];
        if (next.update(m_send[m_phases.send & 1U]) == crypto::CryptoError::None)
        {
            next.set_limit(send_limit());
            m_send[m_phases.send & 1U].clear();
            m_phases.send = m_phases.recv;
            m_phases.send_first_known = false;
            m_phases.send_acked = false;
            m_phases.answer_owed = true;
        }
    }
    return QuicPacketError::None;
}

void QuicPacketProtection::rotate_receive(u64 number) noexcept
{
    if (m_phases.recv_any)
    {
        m_phases.older_largest = (!m_phases.older_any || m_phases.recv_largest > m_phases.older_largest)
                                     ? m_phases.recv_largest
                                     : m_phases.older_largest;
        m_phases.older_any = true;
    }
    const usize next = (m_recv_current + 1U) % 3U;
    const usize free = (m_recv_current + 2U) % 3U; // the key before the current one, if any, is dropped
    m_recv_current = next;
    (void)m_recv[free].update(m_recv[next]); // a ready key always updates but for a provider failure, which then
                                             // drops the next phase's packets
    m_phases.previous_held = true;
    m_phases.recv = m_phases.recv + 1U;
    m_phases.recv_smallest = number;
    m_phases.recv_largest = number;
    m_phases.recv_any = true;
}

bool QuicPacketProtection::largest_opened(QuicSpace space, u64& out) const noexcept
{
    const crypto::QuicPacketWindow& window = m_windows[static_cast<usize>(space)];
    if (!window.any())
    {
        return false;
    }
    out = window.largest();
    return true;
}

void QuicPacketProtection::acknowledged(u64 largest) noexcept
{
    if (m_phases.send_first_known && largest >= m_phases.send_first)
    {
        m_phases.send_acked = true;
    }
}

void QuicPacketProtection::sent_acknowledgement() noexcept
{
    if (m_phases.send == m_phases.recv)
    {
        m_phases.answer_owed = false;
    }
}

bool QuicPacketProtection::can_update_keys() const noexcept
{
    return m_confirmed && m_phases.send_acked && !m_phases.previous_held && m_phases.send == m_phases.recv &&
           m_send[m_phases.send & 1U].ready();
}

bool QuicPacketProtection::update_keys() noexcept
{
    if (!can_update_keys())
    {
        return false;
    }
    crypto::QuicPacketKey& current = m_send[m_phases.send & 1U];
    if (m_send[(m_phases.send + 1U) & 1U].update(current) != crypto::CryptoError::None)
    {
        return false;
    }
    m_send[(m_phases.send + 1U) & 1U].set_limit(send_limit());
    current.clear();
    ++m_phases.send;
    m_phases.send_first_known = false;
    m_phases.send_acked = false;
    return true;
}

void QuicPacketProtection::set_key_update_interval(u64 packets) noexcept
{
    m_update_interval = packets;
    for (crypto::QuicPacketKey& key : m_send)
    {
        if (key.ready())
        {
            key.set_limit(send_limit());
        }
    }
}

void QuicPacketProtection::discard_previous() noexcept
{
    if (m_phases.previous_held)
    {
        // The slot after the next one: free until the next rotation derives into it.
        m_recv[(m_recv_current + 2U) % 3U].clear();
        m_phases.previous_held = false;
    }
}

} // namespace crd::net
