// crd-fuzz-crypto-quic: QUIC packet protection and address-validation tokens (NET.2;
// docs/design/network-foundation.md#quic-protection and #address-validation) on hostile input under the bounded fuzz
// harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a short input ends the case):
//   0  a received packet: a byte choosing the version and the suite, the secret (the suite's hash length), the packet
//      number's offset, the receiver's expected packet number (8 bytes), then the packet;
//   1  a received Retry: a byte choosing the version, the original connection ID (one length byte), then the packet;
//   2  packet numbers: the packet number, the largest acknowledged and the truncated value (8 bytes each), then a
//      byte holding the truncated length;
//   3  a token in a client's Initial: a byte choosing the version (bit 0), an IPv6 address (bit 1) and a replay filter
//      (bit 2), the address, the port (2 bytes), the original, the client's and the Retry's connection IDs (each
//      after a length byte), the server's clock (8 bytes), then the token.
//
// Oracles, none of them the code's verdict on the same call:
//   * header protection refuses exactly the packets too short to sample (restated from RFC 9001 section 5.4.2), and
//     a removal followed by a protection restores every byte;
//   * a packet that opens re-seals and re-protects to exactly the input (the AEAD's own round trip), and one that
//     does not releases no byte;
//   * a Retry is refused by length exactly as restated, and one that verifies carries the tag quic_retry_tag computes;
//   * a decoded packet number keeps the truncated bits, lies in [0, 2^62) and is the candidate closest to the
//     expected one (RFC 9000 appendix A.3 restated over the three candidates), and every length the encoder chooses
//     decodes back to the packet number;
//   * a token's verdict is the restated one: its length, its kind and body, its authenticity under the format of
//     quic.hpp restated over the provider's HKDF and AEAD for exactly this client and Initial, and its lifetime; a
//     valid one validates again, and once only with a replay filter when it is a NEW_TOKEN token; a refused one leaves
//     the claims untouched; and tokens minted for the input's client open as restated, carry its original connection
//     ID, refuse another port when they are Retry tokens and accept one when they are NEW_TOKEN tokens.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/quic.hpp>

#include <cstring>

namespace
{
using crd::u16;
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

// ---- Address validation tokens ----

constexpr u8 kTokenKey[kQuicTokenKeyBytes]      = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB,
                                                   0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
                                                   0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0};
constexpr u8 kTokenPrevious[kQuicTokenKeyBytes] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
                                                   0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                                   0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30};
constexpr u8 kTokenOther[kQuicTokenKeyBytes]    = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B,
                                                   0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
                                                   0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70};

const QuicTokenKeys& token_keys()
{
    static QuicTokenKeys keys;
    static const bool    ready = keys.init(ConstSpan<u8>(kTokenKey), ConstSpan<u8>(kTokenPrevious)) ==
                              CryptoError::None;
    CRD_FUZZ_REQUIRE(ready);
    return keys;
}

// What a token input names: the client's address and port, the connection IDs (the original Destination, the
// client's Source and the Retry's Source, which the next Initial is sent to), the server's clock and the token.
struct TokenInput
{
    QuicVersion   version = QuicVersion::V1;
    bool          replay  = false;
    QuicTokenPeer peer;
    ConstSpan<u8> original;
    ConstSpan<u8> client;
    ConstSpan<u8> retry;
    u64           now = 0U;
    ConstSpan<u8> token;
};

// The token format of quic.hpp restated over the provider's HKDF and AEAD: the key's name, and the AES-256-GCM key and
// nonce a token's salt derives.
bool restated_name(ConstSpan<u8> key, u8 (&name)[kQuicTokenNameBytes])
{
    static constexpr char label[] = "cerid quic token name";
    u8                    prk[32];
    return hkdf_extract(AlgorithmId::HkdfSha256, {}, key, Span<u8>(prk)) == CryptoError::None &&
           hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk),
                       ConstSpan<u8>(reinterpret_cast<const u8*>(label), sizeof(label) - 1U),
                       Span<u8>(name)) == CryptoError::None;
}

bool restated_aead(ConstSpan<u8> key, ConstSpan<u8> salt, AeadKey& aead, u8 (&nonce)[kAeadNonceBytes])
{
    static constexpr char label[] = "cerid quic token seal";
    u8                    prk[32];
    u8                    okm[32U + kAeadNonceBytes];
    if (hkdf_extract(AlgorithmId::HkdfSha256, salt, key, Span<u8>(prk)) != CryptoError::None ||
        hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk),
                    ConstSpan<u8>(reinterpret_cast<const u8*>(label), sizeof(label) - 1U),
                    Span<u8>(okm)) != CryptoError::None ||
        aead.init(AlgorithmId::Aes256Gcm, ConstSpan<u8>(okm, 32U)) != CryptoError::None)
    {
        return false;
    }
    std::memcpy(nonce, okm + 32U, kAeadNonceBytes);
    return true;
}

// The associated data of a token with these clear bytes, for its client's next Initial to `dcid` from `scid`.
usize restated_associated(ConstSpan<u8> header, const TokenInput& in, ConstSpan<u8> dcid, ConstSpan<u8> scid, u8* out)
{
    usize n = 0U;
    std::memcpy(out, header.data(), kQuicTokenHeaderBytes);
    n += kQuicTokenHeaderBytes;
    const u32 wire = static_cast<u32>(in.version);
    for (usize i = 0U; i < 4U; ++i)
    {
        out[n++] = static_cast<u8>(wire >> (24U - 8U * i));
    }
    out[n++] = static_cast<u8>(in.peer.address.size());
    std::memcpy(out + n, in.peer.address.data(), in.peer.address.size());
    n += in.peer.address.size();
    if (header[0] == static_cast<u8>(QuicTokenKind::Retry))
    {
        out[n++] = static_cast<u8>(in.peer.port >> 8U);
        out[n++] = static_cast<u8>(in.peer.port);
        for (const ConstSpan<u8> id : {dcid, scid})
        {
            out[n++] = static_cast<u8>(id.size());
            std::memcpy(out + n, id.data(), id.size());
            n += id.size();
        }
    }
    return n;
}

// Whether `token` opens, as restated, under either of the fuzz keys for its client's next Initial; `plain` receives
// its sealed claims.
bool restated_open(const TokenInput& in, ConstSpan<u8> token, u8* plain)
{
    if (token.size() < kQuicTokenHeaderBytes + kQuicTagBytes)
    {
        return false;
    }
    for (const ConstSpan<u8> key : {ConstSpan<u8>(kTokenKey), ConstSpan<u8>(kTokenPrevious)})
    {
        u8 name[kQuicTokenNameBytes];
        CRD_FUZZ_REQUIRE(restated_name(key, name));
        if (std::memcmp(name, token.data() + 1U, sizeof(name)) != 0)
        {
            continue;
        }
        AeadKey aead;
        u8      nonce[kAeadNonceBytes];
        CRD_FUZZ_REQUIRE(restated_aead(key, token.subspan(1U + kQuicTokenNameBytes, kQuicTokenSaltBytes), aead,
                                       nonce));
        u8          associated[128];
        const usize n = restated_associated(token.first(kQuicTokenHeaderBytes), in, in.retry, in.client, associated);
        const usize body = token.size() - kQuicTokenHeaderBytes - kQuicTagBytes;
        return aead.open(ConstSpan<u8>(nonce), token.subspan(kQuicTokenHeaderBytes), ConstSpan<u8>(associated, n),
                         Span<u8>(plain, body)) == CryptoError::None;
    }
    return false;
}

u64 read_u64(const u8* in)
{
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value = (value << 8U) | in[i];
    }
    return value;
}

// The verdict the token deserves, restated: its length, its kind and the shape of its body, its authenticity under
// either key for exactly this client and Initial, and its lifetime.
CryptoError restated_verdict(const TokenInput& in)
{
    const ConstSpan<u8> token = in.token;
    if (token.size() < kQuicNewTokenBytes || token.size() > kQuicMaxRetryTokenBytes ||
        in.client.size() > kQuicMaxConnectionIdBytes || in.retry.size() > kQuicMaxConnectionIdBytes)
    {
        return CryptoError::BadLength;
    }
    const usize body  = token.size() - kQuicTokenHeaderBytes - kQuicTagBytes;
    const bool  retry = token[0] == static_cast<u8>(QuicTokenKind::Retry);
    u8          plain[64];
    if ((!retry && token[0] != static_cast<u8>(QuicTokenKind::NewToken)) || (!retry && body != 8U) ||
        (retry && body <= 8U) || !restated_open(in, token, plain) ||
        (retry && static_cast<usize>(plain[8]) != body - 9U))
    {
        return CryptoError::Forged;
    }
    const u64 issued   = read_u64(plain);
    const u64 lifetime = retry ? kQuicRetryTokenLifetime : kQuicNewTokenLifetime;
    if ((issued > in.now && issued - in.now > kQuicTokenClockSkew) || (in.now > issued && in.now - issued > lifetime))
    {
        return CryptoError::Expired;
    }
    return CryptoError::None;
}

// True when the input's token validated.
bool token(Reader& reader)
{
    const u8   choice = reader.byte();
    TokenInput in;
    in.version        = kVersions[choice & 1U];
    in.replay         = (choice & 4U) != 0U;
    in.peer.address   = reader.bytes((choice & 2U) != 0U ? 16U : 4U);
    in.peer.port      = static_cast<u16>(reader.byte() << 8U);
    in.peer.port      = static_cast<u16>(in.peer.port | reader.byte());
    in.original       = reader.bytes(reader.byte());
    in.client         = reader.bytes(reader.byte());
    in.retry          = reader.bytes(reader.byte());
    in.now            = reader.u64be();
    in.token          = reader.rest();
    if (!reader.ok())
    {
        return false;
    }
    const QuicTokenKeys& keys = token_keys();

    // The token as given, against its restated verdict.
    QuicTokenReplay replay;
    CRD_FUZZ_REQUIRE(replay.init(32U, kQuicNewTokenLifetime, in.now) == CryptoError::None);
    QuicTokenClaims   claims;
    const CryptoError verdict = keys.validate(in.version, in.token, in.peer, in.retry, in.client, in.now, claims,
                                              in.replay ? &replay : nullptr);
    CRD_FUZZ_REQUIRE(verdict == restated_verdict(in));
    if (verdict == CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(claims.kind == quic_token_kind(in.token));
        CRD_FUZZ_REQUIRE(static_cast<usize>(claims.original_dcid_bytes) ==
                         (claims.kind == QuicTokenKind::Retry ? in.token.size() - kQuicNewTokenBytes - 1U : 0U));
        QuicTokenClaims again;
        CRD_FUZZ_REQUIRE(keys.validate(in.version, in.token, in.peer, in.retry, in.client, in.now, again) ==
                         CryptoError::None);
        CRD_FUZZ_REQUIRE(again.issued == claims.issued && again.original_dcid_bytes == claims.original_dcid_bytes);
        const CryptoError replayed = keys.validate(in.version, in.token, in.peer, in.retry, in.client, in.now, again,
                                                   in.replay ? &replay : nullptr);
        CRD_FUZZ_REQUIRE(replayed == (in.replay && claims.kind == QuicTokenKind::NewToken ? CryptoError::Replayed
                                                                                           : CryptoError::None));
    }
    else
    {
        CRD_FUZZ_REQUIRE(claims.kind == QuicTokenKind::None && claims.issued == 0U &&
                         claims.original_dcid_bytes == u8{0U});
    }

    // Tokens minted for the same client, against the same restatement.
    static u8   minted[kQuicMaxTokenBytes];
    usize       written = 0U;
    const bool  long_id = in.original.size() > kQuicMaxConnectionIdBytes ||
                         in.client.size() > kQuicMaxConnectionIdBytes || in.retry.size() > kQuicMaxConnectionIdBytes;
    const bool  overlap = in.original.size() == in.retry.size() &&
                         (in.retry.empty() || std::memcmp(in.original.data(), in.retry.data(), in.retry.size()) == 0);
    const CryptoError retry = keys.mint_retry(in.version, in.peer, in.original, in.client, in.retry, in.now,
                                              Span<u8>(minted), written);
    CRD_FUZZ_REQUIRE(retry == (long_id ? CryptoError::BadLength : overlap ? CryptoError::Overlap : CryptoError::None));
    if (retry == CryptoError::None)
    {
        TokenInput restated = in;
        restated.token      = ConstSpan<u8>(minted, written);
        CRD_FUZZ_REQUIRE(written == kQuicNewTokenBytes + 1U + in.original.size());
        CRD_FUZZ_REQUIRE(restated_verdict(restated) == CryptoError::None);
        CRD_FUZZ_REQUIRE(keys.validate(in.version, restated.token, in.peer, in.retry, in.client, in.now, claims) ==
                         CryptoError::None);
        CRD_FUZZ_REQUIRE(claims.issued == in.now &&
                         static_cast<usize>(claims.original_dcid_bytes) == in.original.size() &&
                         (in.original.empty() ||
                          std::memcmp(claims.original_dcid, in.original.data(), in.original.size()) == 0));
        QuicTokenPeer moved = in.peer;
        moved.port          = static_cast<u16>(in.peer.port ^ 1U);
        CRD_FUZZ_REQUIRE(keys.validate(in.version, restated.token, moved, in.retry, in.client, in.now, claims) ==
                         CryptoError::Forged);
    }
    CRD_FUZZ_REQUIRE(keys.mint_new_token(in.version, in.peer, in.now, Span<u8>(minted), written) == CryptoError::None);
    QuicTokenPeer moved = in.peer;
    moved.port          = static_cast<u16>(in.peer.port + 1U);
    CRD_FUZZ_REQUIRE(keys.validate(in.version, ConstSpan<u8>(minted, written), moved, {}, {}, in.now, claims) ==
                     CryptoError::None);
    return verdict == CryptoError::None;
}

// A token input sealed as the restatement seals it, with a fixed salt: the seeds are the same on every run. A Retry
// token's length byte may be sealed one too high: a body only a key holder can make, and still not a token.
usize token_seed(u8* out, u8 choice, ConstSpan<u8> address, u16 port, ConstSpan<u8> original, ConstSpan<u8> client,
                 ConstSpan<u8> retry, u64 now, QuicTokenKind kind, ConstSpan<u8> key, u64 issued,
                 bool bad_length = false)
{
    usize n  = 0U;
    out[n++] = 3U;
    out[n++] = choice;
    std::memcpy(out + n, address.data(), address.size());
    n += address.size();
    out[n++] = static_cast<u8>(port >> 8U);
    out[n++] = static_cast<u8>(port);
    for (const ConstSpan<u8> id : {original, client, retry})
    {
        out[n++] = static_cast<u8>(id.size());
        std::memcpy(out + n, id.data(), id.size());
        n += id.size();
    }
    for (usize i = 0U; i < 8U; ++i)
    {
        out[n++] = static_cast<u8>(now >> (56U - 8U * i));
    }
    u8* const token = out + n;
    token[0]        = static_cast<u8>(kind);
    u8 name[kQuicTokenNameBytes];
    CRD_FUZZ_REQUIRE(restated_name(key, name));
    std::memcpy(token + 1U, name, sizeof(name));
    for (usize i = 0U; i < kQuicTokenSaltBytes; ++i)
    {
        token[1U + kQuicTokenNameBytes + i] = static_cast<u8>(0x70U + i);
    }
    u8* const plain = token + kQuicTokenHeaderBytes;
    for (usize i = 0U; i < 8U; ++i)
    {
        plain[i] = static_cast<u8>(issued >> (56U - 8U * i));
    }
    usize body = 8U;
    if (kind == QuicTokenKind::Retry)
    {
        plain[body++] = static_cast<u8>(original.size() + (bad_length ? 1U : 0U));
        std::memcpy(plain + body, original.data(), original.size());
        body += original.size();
    }
    TokenInput in;
    in.version      = kVersions[choice & 1U];
    in.peer.address = address;
    in.peer.port    = port;
    in.client       = client;
    in.retry        = retry;
    AeadKey aead;
    u8      nonce[kAeadNonceBytes];
    CRD_FUZZ_REQUIRE(restated_aead(key, ConstSpan<u8>(token + 1U + kQuicTokenNameBytes, kQuicTokenSaltBytes), aead,
                                   nonce));
    u8          associated[128];
    const usize a = restated_associated(ConstSpan<u8>(token, kQuicTokenHeaderBytes), in, retry, client, associated);
    CRD_FUZZ_REQUIRE(aead.seal(ConstSpan<u8>(nonce), ConstSpan<u8>(plain, body), ConstSpan<u8>(associated, a),
                               Span<u8>(plain, body + kQuicTagBytes)) == CryptoError::None);
    return n + kQuicTokenHeaderBytes + body + kQuicTagBytes;
}

// One input through its mode; true when it was accepted (a packet opened, a Retry verified, packet numbers read, a
// token validated).
bool run(const u8* data, usize size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return false;
    }
    Reader in(data + 1U, size - 1U);
    switch (data[0] % 4U)
    {
    case 0U:
        return packet(in);
    case 1U:
        return retry(in);
    case 2U:
        return packet_numbers(in);
    default:
        return token(in);
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, const u8* data, usize size)
{
    sink.add(name, data, size);
}

constexpr const char* kAccepted[] = {"packet-v1-aes128", "packet-v2-aes128", "packet-v1-aes256", "packet-v2-aes256",
                                     "packet-v1-chacha", "packet-v2-chacha", "retry-v1-rfc9001", "retry-v2-rfc9369",
                                     "packet-numbers-rfc9000", "token-retry-v1", "token-new-v2-ipv6",
                                     "token-new-previous-key-replay"};
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

    // Address-validation tokens sealed as restated, with fixed salts, at 2026-11-02T00:00:00Z: a Retry token, a
    // NEW_TOKEN token for an IPv6 client a minute later at another port, one under the previous key checked against a
    // replay filter; refused: a Retry token past its lifetime, one from another port, one under an unknown key, one
    // cut a byte short of the shortest token and one whose sealed length byte is wrong.
    constexpr u64 now      = 1793577600U;
    const u8      v4[]     = {192, 0, 2, 7};
    const u8      v6[]     = {0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07};
    const u8      client[] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};
    const u8      later[]  = {0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61};
    const ConstSpan<u8> key(kTokenKey);
    usize n = token_seed(input, 0U, v4, 4433U, original, client, later, now, QuicTokenKind::Retry, key, now);
    add(sink, "token-retry-v1", input, n);
    input[7] ^= 0x01U; // the port's low byte
    add(sink, "token-retry-other-port", input, n);
    add(sink, "token-short", input, n - sizeof(original) - 2U);
    n = token_seed(input, 0U, v4, 4433U, original, client, later, now + kQuicRetryTokenLifetime + 1U,
                   QuicTokenKind::Retry, key, now);
    add(sink, "token-retry-expired", input, n);
    n = token_seed(input, 3U, v6, 50000U, {}, client, later, now + 60U, QuicTokenKind::NewToken, key, now);
    add(sink, "token-new-v2-ipv6", input, n);
    n = token_seed(input, 4U, v4, 4433U, original, client, later, now, QuicTokenKind::NewToken,
                   ConstSpan<u8>(kTokenPrevious), now - 3600U);
    add(sink, "token-new-previous-key-replay", input, n);
    n = token_seed(input, 0U, v4, 4433U, original, client, later, now, QuicTokenKind::NewToken,
                   ConstSpan<u8>(kTokenOther), now);
    add(sink, "token-unknown-key", input, n);
    n = token_seed(input, 0U, v4, 4433U, original, client, later, now, QuicTokenKind::Retry, key, now, true);
    add(sink, "token-retry-bad-length", input, n);
}
