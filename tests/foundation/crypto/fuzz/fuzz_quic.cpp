// crd-fuzz-crypto-quic: QUIC packet protection (NET.2; docs/design/network-foundation.md#quic-protection) on hostile
// input under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a short input ends the case):
//   0  a received packet: a byte choosing the version and the suite, the secret (the suite's hash length), the packet
//      number's offset, the receiver's expected packet number (8 bytes), then the packet;
//   1  a received Retry: a byte choosing the version, the original connection ID (one length byte), then the packet;
//   2  packet numbers: the packet number, the largest acknowledged and the truncated value (8 bytes each), then a
//      byte holding the truncated length.
//
// Oracles, none of them the code's verdict on the same call:
//   * header protection refuses exactly the packets too short to sample (restated from RFC 9001 section 5.4.2), and
//     a removal followed by a protection restores every byte;
//   * a packet that opens re-seals and re-protects to exactly the input (the AEAD's own round trip), and one that
//     does not releases no byte;
//   * a Retry is refused by length exactly as restated, and one that verifies carries the tag quic_retry_tag computes;
//   * a decoded packet number keeps the truncated bits, lies in [0, 2^62) and is the candidate closest to the
//     expected one (RFC 9000 appendix A.3 restated over the three candidates), and every length the encoder chooses
//     decodes back to the packet number.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/quic.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

constexpr QuicVersion kVersions[] = {QuicVersion::V1, QuicVersion::V2};
constexpr AlgorithmId kAeads[]    = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};

class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    u8 byte() noexcept
    {
        if (m_at >= m_size)
        {
            m_ok = false;
            return 0U;
        }
        return m_data[m_at++];
    }

    u64 u64be() noexcept
    {
        u64 value = 0U;
        for (usize i = 0U; i < 8U; ++i)
        {
            value = (value << 8U) | byte();
        }
        return value;
    }

    ConstSpan<u8> bytes(usize n) noexcept
    {
        if (n > m_size - m_at)
        {
            m_ok = false;
            return {};
        }
        const ConstSpan<u8> out(m_data + m_at, n);
        m_at += n;
        return out;
    }

    ConstSpan<u8> rest() noexcept { return bytes(m_size - m_at); }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
    bool      m_ok = true;
};

bool all_zero(ConstSpan<u8> bytes) noexcept
{
    for (const u8 b : bytes)
    {
        if (b != 0U)
        {
            return false;
        }
    }
    return true;
}

// True when the packet opened.
bool packet(Reader& in)
{
    const u8            choice    = in.byte();
    const QuicVersion   version   = kVersions[choice & 1U];
    const AlgorithmId   aead      = kAeads[(choice >> 1U) % 3U];
    const ConstSpan<u8> secret    = in.bytes(aead == AlgorithmId::Aes256Gcm ? 48U : 32U);
    const usize         pn_offset = in.byte();
    const u64           expected  = in.u64be();
    const ConstSpan<u8> received  = in.rest();
    if (!in.ok())
    {
        return false;
    }
    QuicPacketKey key;
    QuicHeaderKey hp;
    CRD_FUZZ_REQUIRE(key.init(version, aead, secret) == CryptoError::None);
    CRD_FUZZ_REQUIRE(hp.init(version, aead, secret) == CryptoError::None);
    static u8 buffer[crd::fuzz::kMaxInputBytes];
    std::memcpy(buffer, received.data(), received.size());
    const Span<u8> bytes(buffer, received.size());
    usize          length    = 0U;
    u64            truncated = 0U;
    const CryptoError removed = hp.unprotect(bytes, pn_offset, length, truncated);
    const bool        sampled = pn_offset != 0U && pn_offset <= received.size() &&
                         received.size() - pn_offset >= 4U + kQuicSampleBytes;
    CRD_FUZZ_REQUIRE((removed == CryptoError::None) == sampled);
    if (!sampled)
    {
        CRD_FUZZ_REQUIRE(removed == CryptoError::BadLength);
        CRD_FUZZ_REQUIRE(std::memcmp(buffer, received.data(), received.size()) == 0);
        return false;
    }
    CRD_FUZZ_REQUIRE(length >= 1U && length <= 4U && truncated < (u64{1} << (8U * length)));
    static u8 again[crd::fuzz::kMaxInputBytes];
    std::memcpy(again, buffer, received.size());
    CRD_FUZZ_REQUIRE(hp.protect(Span<u8>(again, received.size()), pn_offset) == CryptoError::None);
    CRD_FUZZ_REQUIRE(std::memcmp(again, received.data(), received.size()) == 0);

    const usize header = pn_offset + length;
    const u64   number = quic_decode_packet_number(truncated, length, expected);
    const CryptoError opened = key.open(number, bytes, header);
    if (number > kQuicMaxPacketNumber || header > received.size() || received.size() - header < kQuicTagBytes)
    {
        CRD_FUZZ_REQUIRE(opened == CryptoError::BadLength);
        return false;
    }
    if (opened != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(opened == CryptoError::Forged);
        CRD_FUZZ_REQUIRE(all_zero(bytes.subspan(header, received.size() - header - kQuicTagBytes)));
        return false;
    }
    CRD_FUZZ_REQUIRE(key.seal(number, bytes, header) == CryptoError::None);
    CRD_FUZZ_REQUIRE(hp.protect(bytes, pn_offset) == CryptoError::None);
    CRD_FUZZ_REQUIRE(std::memcmp(buffer, received.data(), received.size()) == 0);
    return true;
}

// True when the Retry verified.
bool retry(Reader& in)
{
    const QuicVersion   version  = kVersions[in.byte() & 1U];
    const ConstSpan<u8> original = in.bytes(in.byte());
    const ConstSpan<u8> received = in.rest();
    if (!in.ok())
    {
        return false;
    }
    const CryptoError verdict = quic_retry_verify(version, original, received);
    if (received.size() < kQuicTagBytes || received.size() > kQuicMaxRetryBytes ||
        original.size() > kQuicMaxConnectionIdBytes)
    {
        CRD_FUZZ_REQUIRE(verdict == CryptoError::BadLength);
        return false;
    }
    CRD_FUZZ_REQUIRE(verdict == CryptoError::None || verdict == CryptoError::Forged);
    CRD_FUZZ_REQUIRE(quic_retry_verify(version, original, received) == verdict);
    u8 tag[kQuicTagBytes];
    const usize body = received.size() - kQuicTagBytes;
    CRD_FUZZ_REQUIRE(quic_retry_tag(version, original, received.first(body), tag) == CryptoError::None);
    CRD_FUZZ_REQUIRE((std::memcmp(tag, received.data() + body, kQuicTagBytes) == 0) == (verdict == CryptoError::None));
    return verdict == CryptoError::None;
}

// RFC 9000 appendix A.3 restated: of the three candidates one window apart, the one in [0, 2^62) closest to
// `expected` (at exactly half a window either way the RFC keeps the higher candidate).
u64 closest(u64 truncated, usize length, u64 expected) noexcept
{
    const u64 window = u64{1} << (8U * length);
    const u64 base   = (expected & ~(window - 1U)) | (truncated & (window - 1U));
    u64        best          = base;
    u64        best_distance = base > expected ? base - expected : expected - base;
    const u64  candidates[]  = {base - window, base + window};
    const bool valid[]       = {base >= window, base + window <= kQuicMaxPacketNumber};
    for (usize i = 0U; i < 2U; ++i)
    {
        const u64 candidate = candidates[i];
        const u64 distance  = candidate > expected ? candidate - expected : expected - candidate;
        if (valid[i] && (distance < best_distance || (distance == best_distance && candidate > best)))
        {
            best          = candidate;
            best_distance = distance;
        }
    }
    return best;
}

bool packet_numbers(Reader& in)
{
    const u64   number    = in.u64be() & kQuicMaxPacketNumber;
    const u64   acked     = in.u64be() & kQuicMaxPacketNumber;
    const u64   truncated = in.u64be();
    const usize length    = 1U + in.byte() % 4U;
    if (!in.ok())
    {
        return false;
    }
    const u64 expected = acked + 1U <= kQuicMaxPacketNumber ? acked + 1U : kQuicMaxPacketNumber;
    const u64 window   = u64{1} << (8U * length);
    const u64 decoded  = quic_decode_packet_number(truncated & (window - 1U), length, expected);
    CRD_FUZZ_REQUIRE(decoded <= kQuicMaxPacketNumber);
    CRD_FUZZ_REQUIRE((decoded & (window - 1U)) == (truncated & (window - 1U)));
    CRD_FUZZ_REQUIRE(decoded == closest(truncated, length, expected));

    const usize chosen = quic_packet_number_bytes(number, acked, true);
    if (number <= acked)
    {
        CRD_FUZZ_REQUIRE(chosen == 0U);
        return true;
    }
    const u64 range = number - acked;
    CRD_FUZZ_REQUIRE((chosen == 0U) == (range > (u64{1} << 31U)));
    if (chosen != 0U)
    {
        const u64 mask = (u64{1} << (8U * chosen)) - 1U;
        CRD_FUZZ_REQUIRE(range <= (u64{1} << (8U * chosen - 1U)));
        CRD_FUZZ_REQUIRE(chosen == 1U || range > (u64{1} << (8U * chosen - 9U)));
        CRD_FUZZ_REQUIRE(quic_decode_packet_number(number & mask, chosen, expected) == number);
    }
    return true;
}

// One input through its mode; true when it was accepted (a packet opened, a Retry verified, packet numbers read).
bool run(const u8* data, usize size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return false;
    }
    Reader in(data + 1U, size - 1U);
    switch (data[0] % 3U)
    {
    case 0U:
        return packet(in);
    case 1U:
        return retry(in);
    default:
        return packet_numbers(in);
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, const u8* data, usize size)
{
    sink.add(name, data, size);
}

constexpr const char* kAccepted[] = {"packet-v1-aes128", "packet-v2-aes128", "packet-v1-aes256", "packet-v2-aes256",
                                     "packet-v1-chacha", "packet-v2-chacha", "retry-v1-rfc9001", "retry-v2-rfc9369",
                                     "packet-numbers-rfc9000"};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static_cast<void>(run(data, size));
    return 0;
}

// Every honest packet opens and its changed copy does not; both RFC Retries verify (VerdictSink checks each seed).
void crd_fuzz_seeds(crd::fuzz::SeedSink& next)
{
    crd::fuzz::VerdictSink sink(next, run, kAccepted, sizeof(kAccepted) / sizeof(kAccepted[0]));
    // An honest packet of each version and suite (sealed here: sealing is deterministic), and the same packet changed.
    static u8 input[1600];
    for (u8 choice = 0U; choice < 6U; ++choice)
    {
        const QuicVersion version = kVersions[choice & 1U];
        const AlgorithmId aead    = kAeads[(choice >> 1U) % 3U];
        const usize       secret  = aead == AlgorithmId::Aes256Gcm ? 48U : 32U;
        usize             n       = 0U;
        input[n++]                = 0U;
        input[n++]                = choice;
        for (usize i = 0U; i < secret; ++i)
        {
            input[n++] = static_cast<u8>(0x40U + i);
        }
        const bool  long_header = (choice & 1U) != 0U;
        const usize pn_offset   = long_header ? 18U : 9U;
        input[n++]              = static_cast<u8>(pn_offset);
        const u64 number        = 0x1234U + choice;
        for (usize i = 0U; i < 8U; ++i)
        {
            input[n++] = static_cast<u8>(number >> (8U * (7U - i)));
        }
        u8* const   packet_start = input + n;
        const usize size         = pn_offset + 2U + 40U + kQuicTagBytes;
        for (usize i = 0U; i < size; ++i)
        {
            packet_start[i] = static_cast<u8>(i * 7U);
        }
        packet_start[0]             = long_header ? u8{0xC1U} : u8{0x41U}; // a 2-byte packet number
        packet_start[pn_offset]     = static_cast<u8>(number >> 8U);
        packet_start[pn_offset + 1] = static_cast<u8>(number);
        QuicPacketKey       key;
        QuicHeaderKey       hp;
        const ConstSpan<u8> secret_bytes(input + 2U, secret);
        CRD_FUZZ_REQUIRE(key.init(version, aead, secret_bytes) == CryptoError::None);
        CRD_FUZZ_REQUIRE(hp.init(version, aead, secret_bytes) == CryptoError::None);
        CRD_FUZZ_REQUIRE(key.seal(number, Span<u8>(packet_start, size), pn_offset + 2U) == CryptoError::None);
        CRD_FUZZ_REQUIRE(hp.protect(Span<u8>(packet_start, size), pn_offset) == CryptoError::None);
        static const char* const kHonest[]  = {"packet-v1-aes128", "packet-v2-aes128", "packet-v1-aes256",
                                               "packet-v2-aes256", "packet-v1-chacha", "packet-v2-chacha"};
        static const char* const kChanged[] = {"forged-v1-aes128", "forged-v2-aes128", "forged-v1-aes256",
                                               "forged-v2-aes256", "forged-v1-chacha", "forged-v2-chacha"};
        add(sink, kHonest[choice], input, n + size);
        packet_start[size - 1U] ^= 0x01U;
        add(sink, kChanged[choice], input, n + size);
    }

    // RFC 9001 appendix A.4's Retry and RFC 9369's, each for the original connection ID 8394c8f03e515708.
    const u8 original[] = {0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08};
    const u8 body[]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xF0, 0x67, 0xA5,
                           0x50, 0x2A, 0x42, 0x62, 0xB5, 0x74, 0x6F, 0x6B, 0x65, 0x6E};
    for (u8 v = 0U; v < 2U; ++v)
    {
        usize n    = 0U;
        input[n++] = 1U;
        input[n++] = v;
        input[n++] = static_cast<u8>(sizeof(original));
        std::memcpy(input + n, original, sizeof(original));
        n += sizeof(original);
        std::memcpy(input + n, body, sizeof(body));
        input[n] = v == 0U ? u8{0xFFU} : u8{0xCFU};
        const u32 wire = static_cast<u32>(kVersions[v]);
        for (usize i = 0U; i < 4U; ++i)
        {
            input[n + 1U + i] = static_cast<u8>(wire >> (8U * (3U - i)));
        }
        CRD_FUZZ_REQUIRE(quic_retry_tag(kVersions[v], original, ConstSpan<u8>(input + n, sizeof(body)),
                                        Span<u8>(input + n + sizeof(body), kQuicTagBytes)) == CryptoError::None);
        add(sink, v == 0U ? "retry-v1-rfc9001" : "retry-v2-rfc9369", input, n + sizeof(body) + kQuicTagBytes);
    }

    // RFC 9000's packet number examples: 0xac5c02 after 0xabe8b3 acknowledged, and 0x9b32 after 0xa82f30ea.
    const u8 numbers[] = {2U,
                          0, 0, 0, 0, 0, 0xAC, 0x5C, 0x02,
                          0, 0, 0, 0, 0, 0xAB, 0xE8, 0xB3,
                          0, 0, 0, 0, 0, 0, 0x9B, 0x32,
                          1U};
    add(sink, "packet-numbers-rfc9000", numbers, sizeof(numbers));
}
