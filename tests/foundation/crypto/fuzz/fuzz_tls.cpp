// crd-fuzz-crypto-tls: crd-crypto's TLS 1.3 sessions (NET.2; docs/design/network-foundation.md#tls-sessions) fed
// hostile bytes under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. Byte 0 picks the session (bit 0: 0 a server that has received nothing, 1 a client that has sent its
// ClientHello), byte 1 the size of each delivery (0: all at once), the rest are the bytes the peer "sent". With bit 1
// set the rest is instead the ticket a client's store hands out (a ticket corrupted at rest), and nothing is received.
// The server seals and opens tickets under a fixed key, so a seed can lead its identity with that key's name. With bit
// 2 set the input is instead a compressed Certificate (RFC 8879) for the client's decoder: byte 1 its algorithm, three
// bytes its claimed length, then the compressed bytes.
//
// Oracles, none of them the provider's verdict on one call:
//   * neither session is ever established: a server needs a Finished under keys derived from its own fresh share, and
//     a client a CertificateVerify by its pinned key, which no fixed input can hold;
//   * receive() answers only None, Failed, PeerRefused, Full or Closed while the session handshakes, NotReady once it
//     failed and Closed once the peer closed it; a failed or closed session stays so, and a session closed by its
//     peer (a close_notify, after a user_canceled or not) has sent no alert;
//   * the session never holds more than kTlsMaxBufferedBytes of received bytes, and its outgoing bytes are bounded;
//   * send(), read(), close() and update_keys() are refused outside an established session (Closed once closed);
//   * a client offered any ticket starts its handshake all the same, and keeps no ticket before it is established;
//   * a compressed Certificate decodes only for zstd or zlib, to a claim within [4, kTlsMaxCertificateBytes] that, for
//     zlib, its bytes can reach (restated here: DEFLATE's 1032 bytes per byte between zlib's 6 framing bytes), and a
//     refused claim leaves the output untouched; a zlib decoding carries the Adler-32 of its output (restated) and a
//     decoding is the same twice.
#include <crd/fuzz/harness.hpp>

#include <crd/compression/deflate.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
using crd::u16;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::Secp256r1Mlkem768,
                                   AlgorithmId::Secp384r1Mlkem1024, AlgorithmId::X25519, AlgorithmId::Secp256r1,
                                   AlgorithmId::Secp384r1};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::MlDsa44, AlgorithmId::MlDsa65, AlgorithmId::MlDsa87,
                                       AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256,
                                       AlgorithmId::EcdsaSecp384r1Sha384};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr const char* kName    = "game.cerid.test";

// The fuzz server's ticket key: the bytes 0x40, 0x41, ... 0x5F.
struct TicketKey
{
    u8 bytes[kTlsTicketKeyBytes] = {};

    constexpr TicketKey() noexcept
    {
        for (usize i = 0U; i < sizeof(bytes); ++i)
        {
            bytes[i] = static_cast<u8>(0x40U + i);
        }
    }
};
constexpr TicketKey kTicketKey;

// The outgoing bytes a handshake can leave in a session: a server's first flight with an Ed25519 credential, or a
// client's ClientHello, and an alert; far below the bound.
constexpr usize kMaxFlight = 8192U;

constexpr u8    kUntouched      = 0xA5U; // what a refused decoding leaves in the output
constexpr usize kClaimLimit     = kTlsMaxCertificateBytes + 1U; // claims above the bound are cut to this
constexpr usize kCompressedHead = 5U;    // mode, algorithm and the 24-bit claim

// One server and one client context, built once: an Ed25519 credential from a fixed seed, every group, signature and
// AEAD, the client pinning the server's key.
struct Contexts
{
    SigningKey    key;
    TlsCredential credential;
    TlsContext    server;
    TlsContext    client;
    bool          ready = false;

    Contexts() noexcept
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) != CryptoError::None ||
            credential.init(key, kName) != TlsError::None)
        {
            return;
        }
        TlsConfig config;
        config.role        = TlsRole::Server;
        config.groups      = kGroups;
        config.aeads       = kAeads;
        config.credential  = &credential;
        config.server_name = kName;
        config.ticket_key  = ConstSpan<u8>(kTicketKey.bytes, sizeof(kTicketKey.bytes));
        if (server.init(config) != TlsError::None)
        {
            return;
        }
        const KeyId pin    = credential.key_id();
        config.role        = TlsRole::Client;
        config.credential  = nullptr;
        config.ticket_key  = {};
        config.signatures  = kSignatures;
        config.pinned      = ConstSpan<KeyId>(&pin, 1U);
        ready              = client.init(config) == TlsError::None;
    }
};

// A client's store that hands out the input once as its ticket and counts the tickets it is given.
class InputStore final : public TlsTicketStore
{
public:
    explicit InputStore(ConstSpan<u8> ticket) noexcept : m_ticket(ticket) {}

    void keep(const TlsBinding&, ConstSpan<u8>) noexcept override { ++kept; }

    ConstSpan<u8> take(const TlsBinding&) noexcept override
    {
        ++takes;
        return takes == 1U ? m_ticket : ConstSpan<u8>();
    }

    usize kept  = 0U;
    usize takes = 0U;

private:
    ConstSpan<u8> m_ticket;
};

Contexts& contexts() noexcept
{
    static Contexts instance;
    return instance;
}

void drain(TlsSession& session) noexcept
{
    u8    out[kMaxFlight];
    usize written = 0U;
    CRD_FUZZ_REQUIRE(session.pending() <= kMaxFlight);
    CRD_FUZZ_REQUIRE(session.take(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CRD_FUZZ_REQUIRE(session.pending() == 0U);
}

void feed(TlsSession& session, ConstSpan<u8> bytes, usize chunk) noexcept
{
    for (usize at = 0U; at < bytes.size();)
    {
        const usize    size   = chunk == 0U ? bytes.size() - at : std::min(chunk, bytes.size() - at);
        const TlsState before = session.state();
        const TlsError result = session.receive(bytes.subspan(at, size));
        at += size;
        if (before == TlsState::Failed || before == TlsState::Closed)
        {
            CRD_FUZZ_REQUIRE(result == (before == TlsState::Failed ? TlsError::NotReady : TlsError::Closed));
            CRD_FUZZ_REQUIRE(session.state() == before);
            continue;
        }
        CRD_FUZZ_REQUIRE(result == TlsError::None || result == TlsError::Failed || result == TlsError::PeerRefused ||
                         result == TlsError::Full || result == TlsError::Closed);
        CRD_FUZZ_REQUIRE((result == TlsError::Failed || result == TlsError::PeerRefused) ==
                         (session.state() == TlsState::Failed));
        CRD_FUZZ_REQUIRE((result == TlsError::Closed) == (session.state() == TlsState::Closed));
        CRD_FUZZ_REQUIRE(session.state() != TlsState::Closed || !session.alert_sent().present);
        CRD_FUZZ_REQUIRE(session.state() != TlsState::Established && session.state() != TlsState::Idle);
        drain(session);
    }
    const TlsError refused     = session.state() == TlsState::Closed ? TlsError::Closed : TlsError::NotReady;
    u8             scratch[16] = {};
    usize          written     = 0U;
    CRD_FUZZ_REQUIRE(session.send(ConstSpan<u8>(scratch, sizeof(scratch))) == refused);
    CRD_FUZZ_REQUIRE(session.read(Span<u8>(scratch, sizeof(scratch)), written) == refused);
    CRD_FUZZ_REQUIRE(written == 0U);
    CRD_FUZZ_REQUIRE(session.close() == refused);
    CRD_FUZZ_REQUIRE(session.update_keys(true) == refused);
    CRD_FUZZ_REQUIRE(session.pending() == 0U || session.state() == TlsState::Failed);
    CRD_FUZZ_REQUIRE(session.key_updates_sent() == 0U && session.key_updates_received() == 0U);
    CRD_FUZZ_REQUIRE(session.group() == AlgorithmId{} && session.peer_key() == KeyId{});
}
// Adler-32 (RFC 1950 section 8.2), restated.
crd::u32 adler32_of(ConstSpan<u8> bytes) noexcept
{
    crd::u32 a = 1U;
    crd::u32 b = 0U;
    for (const u8 byte : bytes)
    {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

// Whether `compressed` can decode to `claim` bytes of `algorithm`, restated from RFC 1951 (zstd: the bound alone).
bool claim_reachable(u8 algorithm, usize compressed, usize claim) noexcept
{
    if (claim < 4U || claim > kTlsMaxCertificateBytes)
    {
        return false;
    }
    return algorithm == iana::kCertCompressionZstd ||
           (algorithm == iana::kCertCompressionZlib && compressed > 6U && claim <= (compressed - 6U) * 1032U);
}

void decode_certificate(const u8* data, usize size) noexcept
{
    static u8           out[kClaimLimit];
    static u8           again[kClaimLimit];
    const u8            algorithm = data[1];
    const usize         claimed   = (usize{data[2]} << 16U) | (usize{data[3]} << 8U) | data[4];
    const usize         claim     = std::min(claimed, kClaimLimit);
    const ConstSpan<u8> compressed(data + kCompressedHead, size - kCompressedHead);
    std::memset(out, kUntouched, claim);
    const bool decoded = detail::tls_decompress_certificate(algorithm, compressed, Span<u8>(out, claim));
    if (!claim_reachable(algorithm, compressed.size(), claim))
    {
        CRD_FUZZ_REQUIRE(!decoded);
        CRD_FUZZ_REQUIRE(std::all_of(out, out + claim, [](u8 byte) { return byte == kUntouched; }));
        return;
    }
    if (!decoded)
    {
        return;
    }
    if (algorithm == iana::kCertCompressionZlib)
    {
        const u8* trailer = compressed.data() + compressed.size() - 4U;
        CRD_FUZZ_REQUIRE(adler32_of(ConstSpan<u8>(out, claim)) ==
                         ((crd::u32{trailer[0]} << 24U) | (crd::u32{trailer[1]} << 16U) |
                          (crd::u32{trailer[2]} << 8U) | trailer[3]));
    }
    CRD_FUZZ_REQUIRE(detail::tls_decompress_certificate(algorithm, compressed, Span<u8>(again, claim)));
    CRD_FUZZ_REQUIRE(std::memcmp(out, again, claim) == 0);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    if ((data[0] & 4U) != 0U)
    {
        if (size >= kCompressedHead)
        {
            decode_certificate(data, size);
        }
        return 0;
    }
    Contexts& shared = contexts();
    CRD_FUZZ_REQUIRE(shared.ready);
    const bool client = (data[0] & 1U) != 0U;
    TlsSession session;
    if ((data[0] & 2U) != 0U)
    {
        InputStore store(ConstSpan<u8>(data + 2U, size - 2U));
        CRD_FUZZ_REQUIRE(session.init(shared.client, &store) == TlsError::None);
        CRD_FUZZ_REQUIRE(store.takes == 1U);
        CRD_FUZZ_REQUIRE(session.state() == TlsState::Handshaking && session.pending() > 0U);
        drain(session);
        feed(session, {}, 0U);
        CRD_FUZZ_REQUIRE(store.kept == 0U && !session.resumed());
        return 0;
    }
    CRD_FUZZ_REQUIRE(session.init(client ? shared.client : shared.server) == TlsError::None);
    drain(session);
    feed(session, ConstSpan<u8>(data + 2U, size - 2U), data[1]);
    return 0;
}

namespace
{
// A byte writer for the seeds' TLS messages.
struct Writer
{
    u8    bytes[2048] = {};
    usize size        = 0U;

    void put(u8 value) noexcept { bytes[size++] = value; }
    void put16(usize value) noexcept
    {
        put(static_cast<u8>(value >> 8U));
        put(static_cast<u8>(value));
    }
    void put(const u8* data, usize count) noexcept
    {
        std::memcpy(bytes + size, data, count);
        size += count;
    }
    // Writes the 16-bit (or 24-bit) length of everything written since `start` (its length field) at `start`.
    void patch16(usize start) noexcept
    {
        const usize length = size - start - 2U;
        bytes[start]       = static_cast<u8>(length >> 8U);
        bytes[start + 1U]  = static_cast<u8>(length);
    }
    void patch24(usize start) noexcept
    {
        const usize length = size - start - 3U;
        bytes[start]       = static_cast<u8>(length >> 16U);
        bytes[start + 1U]  = static_cast<u8>(length >> 8U);
        bytes[start + 2U]  = static_cast<u8>(length);
    }
};

// A TLS 1.3 ClientHello for the fuzz server: X25519 with RFC 7748's Alice's public key, Ed25519, the server's name and
// TLS_AES_128_GCM_SHA256, in one handshake record (RFC 9846 section 4.1.2). With `ticket` it offers a ticket last
// (psk_dhe_ke): an identity led by the server's ticket key name, a zero salt and a sealed state no key opens, and a
// binder of the right length (RFC 9846 section 4.2.11).
void client_hello(Writer& out, u8 mode, u8 chunk, bool ticket = false) noexcept
{
    constexpr u8 alice[32] = {0x85U, 0x20U, 0xF0U, 0x09U, 0x89U, 0x30U, 0xA7U, 0x54U, 0x74U, 0x8BU, 0x7DU,
                               0xDCU, 0xB4U, 0x3EU, 0xF7U, 0x5AU, 0x0DU, 0xBFU, 0x3AU, 0x0DU, 0x26U, 0x38U,
                               0x1AU, 0xF4U, 0xEBU, 0xA4U, 0xA9U, 0x8EU, 0xAAU, 0x9BU, 0x4EU, 0x6AU};
    out.put(mode);
    out.put(chunk);
    out.put(22U); // handshake record
    out.put16(0x0301U);
    const usize record = out.size;
    out.put16(0U);
    out.put(1U); // client_hello
    const usize message = out.size;
    out.put(0U);
    out.put16(0U);
    out.put16(0x0303U);
    for (u8 i = 0U; i < 32U; ++i)
    {
        out.put(static_cast<u8>(0xC0U + i));
    }
    out.put(0U);        // legacy_session_id
    out.put16(2U);      // cipher_suites
    out.put16(0x1301U); // TLS_AES_128_GCM_SHA256
    out.put(1U);        // legacy_compression_methods
    out.put(0U);
    const usize extensions = out.size;
    out.put16(0U);
    out.put16(0x0000U); // server_name
    out.put16(std::strlen(kName) + 5U);
    out.put16(std::strlen(kName) + 3U);
    out.put(0U); // host_name
    out.put16(std::strlen(kName));
    out.put(reinterpret_cast<const u8*>(kName), std::strlen(kName));
    out.put16(0x000AU); // supported_groups: x25519
    out.put16(4U);
    out.put16(2U);
    out.put16(0x001DU);
    out.put16(0x000DU); // signature_algorithms: ed25519
    out.put16(4U);
    out.put16(2U);
    out.put16(0x0807U);
    out.put16(0x002BU); // supported_versions: TLS 1.3
    out.put16(3U);
    out.put(2U);
    out.put16(0x0304U);
    out.put16(0x0033U); // key_share: x25519
    out.put16(2U + 4U + 32U);
    out.put16(4U + 32U);
    out.put16(0x001DU);
    out.put16(32U);
    out.put(alice, sizeof(alice));
    if (ticket)
    {
        out.put16(0x002DU); // psk_key_exchange_modes: psk_dhe_ke
        out.put16(2U);
        out.put(1U);
        out.put(1U);
        constexpr char info[] = "cerid tls 1.3 ticket name";
        u8             prk[32];
        u8             name[16];
        if (hkdf_extract(AlgorithmId::HkdfSha256, {}, ConstSpan<u8>(kTicketKey.bytes), Span<u8>(prk)) !=
                CryptoError::None ||
            hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk),
                        ConstSpan<u8>(reinterpret_cast<const u8*>(info), sizeof(info) - 1U),
                        Span<u8>(name)) != CryptoError::None)
        {
            return;
        }
        constexpr usize identity = 16U + 32U + 96U + 16U; // name, salt, a state's worth of bytes and the tag
        out.put16(0x0029U);                                 // pre_shared_key
        out.put16(2U + 2U + identity + 4U + 2U + 1U + 32U);
        out.put16(2U + identity + 4U);
        out.put16(identity);
        out.put(name, sizeof(name));
        for (usize i = sizeof(name); i < identity; ++i)
        {
            out.put(static_cast<u8>(i < 48U ? 0U : i));
        }
        out.put16(0x0001U); // obfuscated_ticket_age
        out.put16(0x0203U);
        out.put16(1U + 32U); // binders: one HMAC-SHA256
        out.put(32U);
        for (u8 i = 0U; i < 32U; ++i)
        {
            out.put(static_cast<u8>(0xB0U + i));
        }
    }
    out.patch16(extensions);
    out.patch24(message);
    out.patch16(record);
}

// A ServerHello for the fuzz client: TLS_AES_128_GCM_SHA256 and X25519 with RFC 7748's Bob's public key.
void server_hello(Writer& out) noexcept
{
    constexpr u8 bob[32] = {0xDEU, 0x9EU, 0xDBU, 0x7DU, 0x7BU, 0x7DU, 0xC1U, 0xB4U, 0xD3U, 0x5BU, 0x61U,
                             0xC2U, 0xECU, 0xE4U, 0x35U, 0x37U, 0x3FU, 0x83U, 0x43U, 0xC8U, 0x5BU, 0x78U,
                             0x67U, 0x4DU, 0xADU, 0xFCU, 0x7EU, 0x14U, 0x6FU, 0x88U, 0x2BU, 0x4FU};
    out.put(1U);
    out.put(0U);
    out.put(22U);
    out.put16(0x0303U);
    const usize record = out.size;
    out.put16(0U);
    out.put(2U); // server_hello
    const usize message = out.size;
    out.put(0U);
    out.put16(0U);
    out.put16(0x0303U);
    for (u8 i = 0U; i < 32U; ++i)
    {
        out.put(static_cast<u8>(0x50U + i));
    }
    out.put(0U); // legacy_session_id_echo
    out.put16(0x1301U);
    out.put(0U);
    const usize extensions = out.size;
    out.put16(0U);
    out.put16(0x002BU); // supported_versions
    out.put16(2U);
    out.put16(0x0304U);
    out.put16(0x0033U); // key_share
    out.put16(4U + 32U);
    out.put16(0x001DU);
    out.put16(32U);
    out.put(bob, sizeof(bob));
    out.patch16(extensions);
    out.patch24(message);
    out.patch16(record);
}
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    Writer hello;
    client_hello(hello, 0U, 0U);
    sink.add("server-client-hello", hello.bytes, hello.size);
    hello.bytes[1] = 7U;
    sink.add("server-client-hello-in-7-byte-pieces", hello.bytes, hello.size);
    Writer resuming;
    client_hello(resuming, 0U, 0U, true);
    sink.add("server-client-hello-with-a-forged-ticket", resuming.bytes, resuming.size);

    // Tickets a client's store hands out: no serialized session, and one that ends early.
    const u8 not_a_session[] = {3U, 0U, 0x30U, 0x82U, 0x01U, 0x00U, 0x02U, 0x01U, 0x01U};
    sink.add("client-ticket-not-a-session", not_a_session, sizeof(not_a_session));
    const u8 empty_ticket[] = {3U, 0U};
    sink.add("client-empty-ticket", empty_ticket, sizeof(empty_ticket));

    Writer answer;
    server_hello(answer);
    sink.add("client-server-hello", answer.bytes, answer.size);

    // A record header that claims more than a record may hold, an SSL 2.0 hello, a TLS 1.2-only hello's start, an
    // alert and nothing at all.
    const u8 oversized[] = {0U, 0U, 22U, 0x03U, 0x03U, 0xFFU, 0xFFU, 1U};
    sink.add("server-oversized-record", oversized, sizeof(oversized));
    const u8 sslv2[] = {0U, 0U, 0x80U, 0x2EU, 0x01U, 0x03U, 0x01U, 0x00U, 0x15U, 0x00U, 0x00U, 0x00U, 0x10U};
    sink.add("server-sslv2-hello", sslv2, sizeof(sslv2));
    const u8 alert[] = {1U, 0U, 21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x02U, 0x28U};
    sink.add("client-handshake-failure-alert", alert, sizeof(alert));
    const u8 nothing[] = {0U, 0U};
    sink.add("server-nothing", nothing, sizeof(nothing));

    // A peer that cancels: user_canceled, which is ignored, then close_notify, which closes the session; five warnings
    // in a row, past what the provider tolerates; and a user_canceled ahead of an honest ClientHello.
    const u8 canceled[] = {1U, 0U, 21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, 0x5AU,
                           21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, 0x00U};
    sink.add("client-user-canceled-close-notify", canceled, sizeof(canceled));
    u8 warnings[2U + 5U * 7U] = {0U, 0U};
    for (usize i = 0U; i < 5U; ++i)
    {
        std::memcpy(warnings + 2U + i * 7U, canceled + 2U, 7U);
    }
    sink.add("server-five-user-canceled", warnings, sizeof(warnings));
    Writer late;
    late.put(0U);
    late.put(0U);
    late.put(canceled + 2U, 7U);
    Writer second;
    client_hello(second, 0U, 0U);
    late.put(second.bytes + 2U, second.size - 2U);
    sink.add("server-user-canceled-then-client-hello", late.bytes, late.size);

    // Compressed Certificates: a Certificate-shaped message (an empty context, one 600-byte entry, no extensions) as
    // zstd and as zlib, each claiming its length, one byte more, and far more than the bound; and a brotli claim.
    u8 certificate[609] = {0U, 0U, 0x02U, 0x5DU, 0U, 0x02U, 0x58U};
    for (usize i = 7U; i + 2U < sizeof(certificate); ++i)
    {
        certificate[i] = static_cast<u8>(i % 61U == 0U ? i : 0x30U + i % 7U);
    }
    const ConstSpan<u8>        plain(certificate, sizeof(certificate));
    crd::containers::Array<u8> zstd_form;
    if (!crd::compression::zstd_compress(plain, crd::compression::kZstdDefaultLevel, zstd_form))
    {
        return;
    }
    const crd::containers::Array<u8> zlib_form =
        crd::compression::zlib_deflate(plain, crd::memory::default_allocator());
    struct Form
    {
        const char*                       name;
        u8                                algorithm;
        const crd::containers::Array<u8>* bytes;
    };
    const Form forms[] = {{"zstd", static_cast<u8>(iana::kCertCompressionZstd), &zstd_form},
                          {"zlib", static_cast<u8>(iana::kCertCompressionZlib), &zlib_form}};
    for (const Form& form : forms)
    {
        const usize claims[] = {sizeof(certificate), sizeof(certificate) + 1U, 0xFFFFFFU};
        const char* labels[] = {"exact", "one-more", "bomb"};
        for (usize i = 0U; i < 3U; ++i)
        {
            Writer seed;
            seed.put(4U);
            seed.put(form.algorithm);
            seed.put(static_cast<u8>(claims[i] >> 16U));
            seed.put16(claims[i] & 0xFFFFU);
            seed.put(form.bytes->data(), form.bytes->size());
            char name[64];
            std::snprintf(name, sizeof(name), "compressed-certificate-%s-%s", form.name, labels[i]);
            sink.add(name, seed.bytes, seed.size);
        }
    }
    const u8 brotli[] = {4U, 2U, 0U, 0U, 16U, 0x0BU, 0x07U, 0x80U, 0x61U, 0x62U, 0x63U, 0x03U};
    sink.add("compressed-certificate-brotli", brotli, sizeof(brotli));
}
