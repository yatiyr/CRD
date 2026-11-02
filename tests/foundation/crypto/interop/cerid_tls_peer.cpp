// crd-tls-peer -- one end of a TLS 1.3 connection through crd-crypto's sessions, driven over its standard streams by
// tests/foundation/crypto/interop/tls_interop.py (NET.2; docs/design/network-foundation.md#tls-sessions).
//
// Usage: crd-tls-peer --role client|server --groups g[,g...] --seed HEX [--pin HEX] [--name NAME]
//                     [--key-update-records N] [--pings N] [--update requested|not-requested]
//                     [--tickets FILE] [--ticket-key HEX] [--previous-ticket-key HEX]
//                     [--compression zstd|zlib|both|none] [--alpn LIST] [--quic 1|2] [--parameters HEX]
//                     [--ech-configs HEX] [--ech-retry HEX] [--ech-grease 0|1] [--ech-key SEED:ID]
//                     [--ech-previous SEED:ID]
//        crd-tls-peer --public HEX     (prints the Ed25519 public key of a seed, for the pins)
//        crd-tls-peer --ech-config SEED:ID  (prints the ECHConfigList of a Cerid server's ECH key, as hex)
//   groups  register names: x25519mlkem768, secp256r1mlkem768, secp384r1mlkem1024, x25519, secp256r1, secp384r1
//   seed    the 32-byte Ed25519 private key of this end's credential
//   pin     the peer's 32-byte Ed25519 public key (a client's server; a server that pins its client)
//   name    the server's name (default game.cerid.test)
//   key-update-records  TlsConfig::key_update_records (the session updates its key before that many records)
//   pings   how many times an established client sends its ping, each in its own record (1 to 16, default 1)
//   update  an established client updates its key once after its pings, asking the server for an update or not
//   tickets a client's ticket store: the ticket in FILE (its binding, then its bytes) is offered once when it is of
//           this client's binding, and the newest ticket received replaces it
//   ticket-key, previous-ticket-key  a server's 32-byte ticket keys (TlsConfig::ticket_key, previous_ticket_key)
//   compression  TlsConfig::certificate_compression: zstd or zlib alone, both (the default) or none
//   alpn    TlsConfig::application_protocols, comma-separated
//   quic    a QuicHandshake (RFC 9001) instead of a TlsSession, its probes made for QUIC version 1 or 2
//   parameters  the transport parameters a QUIC end sends (hex, at most 256 bytes)
//   ech-configs  a client's ECHConfigList (TlsConfig::ech_configs, hex); ech-retry the retry configurations one
//           connection offers in its place (TlsSession::init); ech-grease TlsConfig::ech_grease (default 1)
//   ech-key, ech-previous  a server's current and previous ECH keys (TlsConfig::ech_keys, previous_ech_keys): the
//           32-byte private key and the config_id (decimal) of each, every one published under the public name
//           front.cerid.test with a maximum_name_length of 32
//
// Frames on both streams are a 4-byte big-endian length and that many bytes of TLS records. A client writes its first
// flight before reading; after that each end answers every frame it reads with exactly one frame (possibly empty).
// Once established, a client sends "ping from cerid" (--pings times) and a server echoes every byte it reads. At the
// end of input the end prints one JSON line on standard error with what it negotiated, whether it resumed, the key
// updates each way, the algorithm of its compressed Certificate sent or received, what became of ECH (with a rejected
// client's retry configurations) and what it read, and exits 0 (2 on a usage error).
//
// Over QUIC a frame carries CRYPTO data as pieces: a level byte (0 Initial, 1 Handshake, 2 Application), a 24-bit
// big-endian length and that many bytes. No application data flows; the JSON line adds the application protocol, the
// level of the alert sent, the peer's transport parameters and one probe per secret: a fixed packet sealed and a fixed
// sample masked under the keys QuicHandshake::derive_keys makes from it, which crd-openssl-tls-peer computes through
// OpenSSL.

#include <crd/crypto/capability.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <cstdio>
#include <cstdlib>
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
using crd::containers::StringView;
using namespace crd::crypto;

struct GroupName
{
    const char* name;
    AlgorithmId id;
};

constexpr GroupName kGroupNames[] = {
    {"x25519mlkem768", AlgorithmId::X25519Mlkem768}, {"secp256r1mlkem768", AlgorithmId::Secp256r1Mlkem768},
    {"secp384r1mlkem1024", AlgorithmId::Secp384r1Mlkem1024}, {"x25519", AlgorithmId::X25519},
    {"secp256r1", AlgorithmId::Secp256r1}, {"secp384r1", AlgorithmId::Secp384r1},
};
constexpr AlgorithmId kAeads[]      = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr char        kPing[]       = "ping from cerid";
constexpr usize       kPieceHeader  = 4U; // a QUIC frame piece's level byte and 24-bit length
constexpr char        kPublicName[] = "front.cerid.test";
constexpr u8          kMaxNameLength = 32U;

// The probe's packet header (the associated data), payload, packet number and header protection sample: the same in
// crd-openssl-tls-peer.
constexpr u8  kProbeHeader[]  = {0x41U, 0xAAU, 0xBBU, 0xCCU};
constexpr u8  kProbeSample[]  = {0x5AU, 0x5BU, 0x5CU, 0x5DU, 0x5EU, 0x5FU, 0x60U, 0x61U,
                                 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U, 0x69U};
constexpr u64 kProbeNumber    = 0x2AU;
constexpr usize kProbePayload = 16U;

u8 g_frame[kTlsMaxBufferedBytes + kQuicLevels * kPieceHeader];

// A ticket store of one ticket kept in a file across processes: the binding's 32 bytes, then the ticket.
class FileStore final : public TlsTicketStore
{
public:
    explicit FileStore(const char* path) noexcept : m_path(path) {}

    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        std::FILE* file = std::fopen(m_path, "wb");
        if (file != nullptr)
        {
            (void)std::fwrite(binding.bytes, 1U, sizeof(binding.bytes), file);
            (void)std::fwrite(ticket.data(), 1U, ticket.size(), file);
            (void)std::fclose(file);
        }
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        std::FILE* file = std::fopen(m_path, "rb");
        if (file == nullptr)
        {
            return {};
        }
        TlsBinding  kept;
        const usize bound = std::fread(kept.bytes, 1U, sizeof(kept.bytes), file);
        const usize size  = std::fread(m_ticket, 1U, sizeof(m_ticket), file);
        (void)std::fclose(file);
        (void)std::remove(m_path); // a ticket is offered once
        return bound == sizeof(kept.bytes) && kept == binding ? ConstSpan<u8>(m_ticket, size) : ConstSpan<u8>();
    }

private:
    const char* m_path;
    u8          m_ticket[kTlsMaxBufferedBytes] = {};
};

// The TLS 1.3 cipher suite of a negotiated AEAD, by its standard name (RFC 9846 appendix B.4).
const char* suite_name(AlgorithmId aead)
{
    switch (aead)
    {
    case AlgorithmId::Aes128Gcm:
        return "TLS_AES_128_GCM_SHA256";
    case AlgorithmId::Aes256Gcm:
        return "TLS_AES_256_GCM_SHA384";
    case AlgorithmId::Chacha20Poly1305:
        return "TLS_CHACHA20_POLY1305_SHA256";
    default:
        return "";
    }
}

const char* group_name(AlgorithmId id)
{
    for (const GroupName& entry : kGroupNames)
    {
        if (entry.id == id)
        {
            return entry.name;
        }
    }
    return "";
}

bool parse_hex(const char* text, u8* out, usize size)
{
    if (std::strlen(text) != size * 2U)
    {
        return false;
    }
    for (usize i = 0U; i < size * 2U; ++i)
    {
        const char c = text[i];
        int        v = -1;
        if (c >= '0' && c <= '9')
        {
            v = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            v = c - 'a' + 10;
        }
        if (v < 0)
        {
            return false;
        }
        out[i / 2U] = static_cast<u8>(i % 2U == 0U ? v << 4 : out[i / 2U] | v);
    }
    return true;
}

// Hex of any even length up to `capacity` bytes.
bool parse_hex_bytes(const char* text, u8* out, usize capacity, usize& size)
{
    const usize length = std::strlen(text);
    size               = length / 2U;
    return length % 2U == 0U && size <= capacity && parse_hex(text, out, size);
}

// "SEED:ID": an ECH key's 32-byte private key in hex and its config_id in decimal (0 to 255).
bool make_ech_key(const char* text, TlsEchKey& key)
{
    const char* colon = std::strchr(text, ':');
    u8          secret[kTlsEchKeyBytes];
    char        hex[2U * kTlsEchKeyBytes + 1U] = {};
    if (colon == nullptr || static_cast<usize>(colon - text) != sizeof(hex) - 1U)
    {
        return false;
    }
    std::memcpy(hex, text, sizeof(hex) - 1U);
    char*                    end = nullptr;
    const unsigned long long id  = std::strtoull(colon + 1, &end, 10);
    return end != colon + 1 && *end == '\0' && id <= 255U && parse_hex(hex, secret, sizeof(secret)) &&
           key.init(ConstSpan<u8>(secret), static_cast<u8>(id), kPublicName, kMaxNameLength) == TlsError::None;
}

void print_hex(ConstSpan<u8> bytes)
{
    for (const u8 byte : bytes)
    {
        std::fprintf(stderr, "%02x", byte);
    }
}

bool read_frame(usize& size)
{
    u8 header[4];
    if (std::fread(header, 1U, 4U, stdin) != 4U)
    {
        return false;
    }
    size = (static_cast<usize>(header[0]) << 24U) | (static_cast<usize>(header[1]) << 16U) |
           (static_cast<usize>(header[2]) << 8U) | header[3];
    return size <= sizeof(g_frame) && std::fread(g_frame, 1U, size, stdin) == size;
}

bool write_frame(TlsSession& session)
{
    usize written = 0U;
    if (session.take(Span<u8>(g_frame, sizeof(g_frame)), written) != TlsError::None)
    {
        return false;
    }
    const u8 header[4] = {static_cast<u8>(written >> 24U), static_cast<u8>(written >> 16U),
                          static_cast<u8>(written >> 8U), static_cast<u8>(written)};
    return std::fwrite(header, 1U, 4U, stdout) == 4U && std::fwrite(g_frame, 1U, written, stdout) == written &&
           std::fflush(stdout) == 0;
}

void print_alert(const char* key, TlsAlert alert)
{
    std::fprintf(stderr, "\"%s\": %d", key, alert.present ? static_cast<int>(alert.description) : -1);
}

// Writes the CRYPTO data waiting at each level as one frame of pieces.
bool write_frame(QuicHandshake& handshake)
{
    usize size = 0U;
    for (usize level = 0U; level < kQuicLevels; ++level)
    {
        usize written = 0U;
        if (handshake.take(static_cast<QuicLevel>(level),
                           Span<u8>(g_frame + size + kPieceHeader, sizeof(g_frame) - size - kPieceHeader),
                           written) != TlsError::None)
        {
            return false;
        }
        if (written != 0U)
        {
            g_frame[size]      = static_cast<u8>(level);
            g_frame[size + 1U] = static_cast<u8>(written >> 16U);
            g_frame[size + 2U] = static_cast<u8>(written >> 8U);
            g_frame[size + 3U] = static_cast<u8>(written);
            size += kPieceHeader + written;
        }
    }
    const u8 header[4] = {static_cast<u8>(size >> 24U), static_cast<u8>(size >> 16U), static_cast<u8>(size >> 8U),
                          static_cast<u8>(size)};
    return std::fwrite(header, 1U, 4U, stdout) == 4U && std::fwrite(g_frame, 1U, size, stdout) == size &&
           std::fflush(stdout) == 0;
}

// Provides a frame's pieces at their levels while the handshake lives; false for a malformed frame.
bool provide_frame(QuicHandshake& handshake, usize size)
{
    usize at = 0U;
    while (at + kPieceHeader <= size)
    {
        const usize length = (static_cast<usize>(g_frame[at + 1U]) << 16U) |
                             (static_cast<usize>(g_frame[at + 2U]) << 8U) | g_frame[at + 3U];
        if (g_frame[at] >= kQuicLevels || length > size - at - kPieceHeader)
        {
            return false;
        }
        if (handshake.state() == TlsState::Handshaking || handshake.state() == TlsState::Established)
        {
            (void)handshake.provide(static_cast<QuicLevel>(g_frame[at]),
                                    ConstSpan<u8>(g_frame + at + kPieceHeader, length));
        }
        at += kPieceHeader + length;
    }
    return at == size;
}

// The probe of every secret still held, named by level and by whose packets it protects: the probe packet sealed
// and the probe sample's mask under the keys derive_keys makes, as hex.
void print_probes(QuicHandshake& handshake, QuicVersion version)
{
    constexpr QuicLevel   levels[]      = {QuicLevel::Handshake, QuicLevel::Application};
    constexpr const char* level_names[] = {"handshake", "application"};
    constexpr QuicSide    sides[]       = {QuicSide::Client, QuicSide::Server};
    constexpr const char* side_names[]  = {"client", "server"};
    bool                  first         = true;
    std::fputs("\"probes\": {", stderr);
    for (usize i = 0U; i < 2U; ++i)
    {
        for (usize j = 0U; j < 2U; ++j)
        {
            QuicPacketKey packet;
            QuicHeaderKey header;
            u8            bytes[sizeof(kProbeHeader) + kProbePayload + kQuicTagBytes] = {};
            u8            mask[kQuicMaskBytes]                                       = {};
            std::memcpy(bytes, kProbeHeader, sizeof(kProbeHeader));
            for (usize k = 0U; k < kProbePayload; ++k)
            {
                bytes[sizeof(kProbeHeader) + k] = static_cast<u8>(k);
            }
            if (handshake.derive_keys(levels[i], sides[j], version, packet, header) != TlsError::None ||
                packet.seal(kProbeNumber, Span<u8>(bytes), sizeof(kProbeHeader)) != CryptoError::None ||
                header.mask(ConstSpan<u8>(kProbeSample), Span<u8>(mask)) != CryptoError::None)
            {
                continue;
            }
            std::fprintf(stderr, R"(%s"%s-%s": ")", first ? "" : ", ", level_names[i], side_names[j]);
            for (usize k = sizeof(kProbeHeader); k < sizeof(bytes); ++k)
            {
                std::fprintf(stderr, "%02x", bytes[k]);
            }
            for (const u8 byte : mask)
            {
                std::fprintf(stderr, "%02x", byte);
            }
            std::fputc('"', stderr);
            first = false;
        }
    }
    std::fputs("}, ", stderr);
}

// What every report carries, a stream session's or a QUIC handshake's.
void print_common(const detail::TlsHandshakeState& state)
{
    const char*      states[] = {"idle", "handshaking", "established", "closed", "failed"};
    const KeyId      peer     = state.peer_key();
    const StringView protocol = state.application_protocol();
    std::fprintf(stderr, R"({"state": "%s", "group": "%s", "hello_retry": %s, "resumed": %s, "alpn": "%.*s", )",
                 states[static_cast<int>(state.state())], group_name(state.group()),
                 state.hello_retry() ? "true" : "false", state.resumed() ? "true" : "false",
                 static_cast<int>(protocol.size()), protocol.data() != nullptr ? protocol.data() : "");
    std::fputs(R"("peer_key_id": ")", stderr);
    for (const u8 byte : peer.bytes)
    {
        std::fprintf(stderr, "%02x", byte);
    }
    std::fputs("\", ", stderr);
    print_alert("alert_sent", state.alert_sent());
    std::fputs(", ", stderr);
    print_alert("alert_received", state.alert_received());
    std::fprintf(stderr, R"(, "certificate_compression": %u, "suite": "%s", "ech": "%s", "ech_retry": ")",
                 static_cast<unsigned>(state.certificate_compression()), suite_name(state.aead()),
                 tls_ech_name(state.ech()).data());
    print_hex(state.ech_retry_configs());
    // The exporter (RFC 9846 section 7.5) under a label and a context, and the world capability binding (its label,
    // no context): the other end's library must derive the same bytes.
    const u8   context[]                        = {'c', 'e', 'r', 'i', 'd'};
    u8         exported[32]                     = {};
    u8         binding[kCapabilityBindingBytes] = {};
    const bool exports =
        state.export_keying_material("EXPERIMENTAL Cerid interop", context, exported) == TlsError::None &&
        capability_binding(state, ScopeKind::World, binding) == TlsError::None;
    std::fputs(R"(", "exporter": ")", stderr);
    print_hex(exports ? ConstSpan<u8>(exported) : ConstSpan<u8>());
    std::fputs(R"(", "binding": ")", stderr);
    print_hex(exports ? ConstSpan<u8>(binding) : ConstSpan<u8>());
    std::fputs("\", ", stderr);
}

// A QUIC handshake over the frames: a client's first flight unasked, then one frame answering each frame read.
int run_quic(const TlsContext& context, TlsTicketStore* tickets, bool client, QuicVersion version,
             ConstSpan<u8> parameters, ConstSpan<u8> ech_retry)
{
    QuicHandshake handshake;
    if (handshake.init(context, parameters, tickets, ech_retry) != TlsError::None)
    {
        std::fputs("crd-tls-peer: the QUIC handshake could not be made\n", stderr);
        return 1;
    }
    if (client && !write_frame(handshake))
    {
        return 1;
    }
    usize size = 0U;
    while (read_frame(size))
    {
        if (!provide_frame(handshake, size) || !write_frame(handshake))
        {
            return 1;
        }
    }
    print_common(handshake);
    std::fprintf(stderr, R"("alert_level": %d, "peer_parameters": ")",
                 handshake.alert_sent().present ? static_cast<int>(handshake.alert_level()) : -1);
    for (const u8 byte : handshake.peer_transport_parameters())
    {
        std::fprintf(stderr, "%02x", byte);
    }
    std::fputs("\", ", stderr);
    print_probes(handshake, version);
    std::fputs("\"received\": \"\"}\n", stderr);
    return 0;
}

int usage()
{
    std::fputs("usage: crd-tls-peer --role client|server --groups g[,g...] --seed HEX [--pin HEX] [--name NAME] "
               "[--key-update-records N] [--pings N] [--update requested|not-requested] [--tickets FILE] "
               "[--ticket-key HEX] [--previous-ticket-key HEX] [--compression zstd|zlib|both|none] [--alpn LIST] "
               "[--quic 1|2] [--parameters HEX] [--ech-configs HEX] [--ech-retry HEX] [--ech-grease 0|1] "
               "[--ech-key SEED:ID] [--ech-previous SEED:ID]\n",
               stderr);
    return 2;
}

int print_ech_config(const char* text)
{
    TlsEchKey        key;
    u8               list[kTlsMaxEchConfigListBytes];
    usize            written = 0U;
    const TlsEchKey* keys[]  = {&key};
    if (!make_ech_key(text, key) ||
        tls_ech_config_list(ConstSpan<const TlsEchKey*>(keys), Span<u8>(list), written) != TlsError::None)
    {
        return usage();
    }
    for (usize i = 0U; i < written; ++i)
    {
        std::printf("%02x", list[i]);
    }
    std::fputc('\n', stdout);
    return 0;
}

int print_public(const char* seed)
{
    u8         seed_bytes[32];
    u8         public_key[32];
    SigningKey key;
    if (!parse_hex(seed, seed_bytes, sizeof(seed_bytes)) ||
        key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed_bytes, sizeof(seed_bytes))) != CryptoError::None ||
        key.public_key(Span<u8>(public_key, sizeof(public_key))) != CryptoError::None)
    {
        return usage();
    }
    for (const u8 byte : public_key)
    {
        std::printf("%02x", byte);
    }
    std::fputc('\n', stdout);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--public") == 0)
    {
        return print_public(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--ech-config") == 0)
    {
        return print_ech_config(argv[2]);
    }
    const char* role  = nullptr;
    const char* list  = nullptr;
    const char* seed  = nullptr;
    const char* pin   = nullptr;
    const char* name  = "game.cerid.test";
    const char* update             = nullptr;
    const char* tickets            = nullptr;
    const char* ticket_key         = nullptr;
    const char* previous_key       = nullptr;
    u64         key_update_records = 0U;
    u64         pings              = 1U;
    constexpr u16  zstd_only[]     = {iana::kCertCompressionZstd};
    constexpr u16  zlib_only[]     = {iana::kCertCompressionZlib};
    ConstSpan<u16> compression     = kTlsCertificateCompression;
    StringView     protocols[kTlsMaxProtocols];
    usize          protocol_count  = 0U;
    u32            quic            = 0U;
    u8             parameters[256] = {};
    usize          parameter_bytes = 0U;
    static u8        ech_configs[kTlsMaxEchConfigListBytes];
    static u8        ech_retry[kTlsMaxEchConfigListBytes];
    usize            ech_config_bytes = 0U;
    usize            ech_retry_bytes  = 0U;
    bool             ech_grease       = true;
    static TlsEchKey ech_keys[2][kTlsMaxEchKeys];
    const TlsEchKey* ech_lists[2][kTlsMaxEchKeys] = {};
    usize            ech_counts[2]                = {};
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const char* option = argv[i];
        const char* value  = argv[i + 1];
        if (std::strcmp(option, "--role") == 0)
        {
            role = value;
        }
        else if (std::strcmp(option, "--groups") == 0)
        {
            list = value;
        }
        else if (std::strcmp(option, "--seed") == 0)
        {
            seed = value;
        }
        else if (std::strcmp(option, "--pin") == 0)
        {
            pin = value;
        }
        else if (std::strcmp(option, "--name") == 0)
        {
            name = value;
        }
        else if (std::strcmp(option, "--key-update-records") == 0)
        {
            key_update_records = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(option, "--pings") == 0)
        {
            pings = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(option, "--tickets") == 0)
        {
            tickets = value;
        }
        else if (std::strcmp(option, "--ticket-key") == 0)
        {
            ticket_key = value;
        }
        else if (std::strcmp(option, "--previous-ticket-key") == 0)
        {
            previous_key = value;
        }
        else if (std::strcmp(option, "--compression") == 0 && std::strcmp(value, "zstd") == 0)
        {
            compression = zstd_only;
        }
        else if (std::strcmp(option, "--compression") == 0 && std::strcmp(value, "zlib") == 0)
        {
            compression = zlib_only;
        }
        else if (std::strcmp(option, "--compression") == 0 && std::strcmp(value, "none") == 0)
        {
            compression = {};
        }
        else if (std::strcmp(option, "--compression") == 0 && std::strcmp(value, "both") == 0)
        {
            compression = kTlsCertificateCompression;
        }
        else if (std::strcmp(option, "--alpn") == 0)
        {
            protocol_count = 0U; // the last list given counts, as in crd-openssl-tls-peer
            for (const char* at = value; *at != '\0' && protocol_count < kTlsMaxProtocols;)
            {
                const char* end    = std::strchr(at, ',');
                const usize length = end != nullptr ? static_cast<usize>(end - at) : std::strlen(at);
                protocols[protocol_count++] = StringView(at, length);
                at += length + (end != nullptr ? 1U : 0U);
            }
        }
        else if (std::strcmp(option, "--quic") == 0 && (std::strcmp(value, "1") == 0 || std::strcmp(value, "2") == 0))
        {
            quic = static_cast<u32>(value[0] - '0');
        }
        else if (std::strcmp(option, "--parameters") == 0 && std::strlen(value) % 2U == 0U &&
                 std::strlen(value) / 2U <= sizeof(parameters) &&
                 parse_hex(value, parameters, std::strlen(value) / 2U))
        {
            parameter_bytes = std::strlen(value) / 2U;
        }
        else if (std::strcmp(option, "--update") == 0 &&
                 (std::strcmp(value, "requested") == 0 || std::strcmp(value, "not-requested") == 0))
        {
            update = value;
        }
        else if (std::strcmp(option, "--ech-configs") == 0)
        {
            if (!parse_hex_bytes(value, ech_configs, sizeof(ech_configs), ech_config_bytes))
            {
                return usage();
            }
        }
        else if (std::strcmp(option, "--ech-retry") == 0)
        {
            if (!parse_hex_bytes(value, ech_retry, sizeof(ech_retry), ech_retry_bytes))
            {
                return usage();
            }
        }
        else if (std::strcmp(option, "--ech-grease") == 0 &&
                 (std::strcmp(value, "0") == 0 || std::strcmp(value, "1") == 0))
        {
            ech_grease = value[0] == '1';
        }
        else if ((std::strcmp(option, "--ech-key") == 0 || std::strcmp(option, "--ech-previous") == 0) &&
                 ech_counts[0] + ech_counts[1] < kTlsMaxEchKeys)
        {
            const usize which = std::strcmp(option, "--ech-key") == 0 ? 0U : 1U;
            TlsEchKey&  key   = ech_keys[which][ech_counts[which]];
            if (!make_ech_key(value, key))
            {
                return usage();
            }
            ech_lists[which][ech_counts[which]++] = &key;
        }
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || role == nullptr || list == nullptr || seed == nullptr || pings == 0U || pings > 16U)
    {
        return usage();
    }
    const bool  client = std::strcmp(role, "client") == 0;
    AlgorithmId groups[kTlsMaxGroups];
    usize       group_count = 0U;
    for (const char* at = list; *at != '\0';)
    {
        const char* end    = std::strchr(at, ',');
        const usize length = end != nullptr ? static_cast<usize>(end - at) : std::strlen(at);
        bool        known  = false;
        for (const GroupName& entry : kGroupNames)
        {
            if (std::strlen(entry.name) == length && std::strncmp(entry.name, at, length) == 0 &&
                group_count < kTlsMaxGroups)
            {
                groups[group_count++] = entry.id;
                known                 = true;
            }
        }
        if (!known)
        {
            return usage();
        }
        at += length + (end != nullptr ? 1U : 0U);
    }
    u8 seed_bytes[32];
    u8 pin_bytes[32];
    u8 key_bytes[kTlsTicketKeyBytes];
    u8 previous_bytes[kTlsTicketKeyBytes];
    if (!parse_hex(seed, seed_bytes, sizeof(seed_bytes)) ||
        (pin != nullptr && !parse_hex(pin, pin_bytes, sizeof(pin_bytes))) || (client && pin == nullptr) ||
        (ticket_key != nullptr && !parse_hex(ticket_key, key_bytes, sizeof(key_bytes))) ||
        (previous_key != nullptr && !parse_hex(previous_key, previous_bytes, sizeof(previous_bytes))) ||
        (client && (ticket_key != nullptr || previous_key != nullptr)) || (!client && tickets != nullptr))
    {
        return usage();
    }

    SigningKey    key;
    TlsCredential credential;
    if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed_bytes, sizeof(seed_bytes))) != CryptoError::None ||
        credential.init(key, name) != TlsError::None)
    {
        std::fputs("crd-tls-peer: the credential could not be made\n", stderr);
        return 1;
    }
    const KeyId pinned = key_id_of(ConstSpan<u8>(pin_bytes, sizeof(pin_bytes)));
    TlsConfig   config;
    config.role        = client ? TlsRole::Client : TlsRole::Server;
    config.groups      = ConstSpan<AlgorithmId>(groups, group_count);
    config.aeads       = kAeads;
    config.credential  = &credential;
    config.server_name        = name;
    config.key_update_records = key_update_records;
    config.certificate_compression = compression;
    config.application_protocols   = ConstSpan<StringView>(protocols, protocol_count);
    config.ech_configs             = ConstSpan<u8>(ech_configs, ech_config_bytes);
    config.ech_grease              = ech_grease;
    config.ech_keys                = ConstSpan<const TlsEchKey*>(ech_lists[0], ech_counts[0]);
    config.previous_ech_keys       = ConstSpan<const TlsEchKey*>(ech_lists[1], ech_counts[1]);
    if (ticket_key != nullptr)
    {
        config.ticket_key = ConstSpan<u8>(key_bytes, sizeof(key_bytes));
    }
    if (previous_key != nullptr)
    {
        config.previous_ticket_key = ConstSpan<u8>(previous_bytes, sizeof(previous_bytes));
    }
    if (pin != nullptr)
    {
        config.signatures = kSignatures;
        config.pinned     = ConstSpan<KeyId>(&pinned, 1U);
    }
    TlsContext context;
    TlsSession session;
    FileStore  store(tickets != nullptr ? tickets : "");
    if (quic != 0U)
    {
        if (context.init(config) != TlsError::None)
        {
            std::fputs("crd-tls-peer: the context could not be made\n", stderr);
            return 1;
        }
        return run_quic(context, tickets != nullptr ? &store : nullptr, client,
                        quic == 1U ? QuicVersion::V1 : QuicVersion::V2, ConstSpan<u8>(parameters, parameter_bytes),
                        ConstSpan<u8>(ech_retry, ech_retry_bytes));
    }
    if (context.init(config) != TlsError::None ||
        session.init(context, tickets != nullptr ? &store : nullptr, ConstSpan<u8>(ech_retry, ech_retry_bytes)) !=
            TlsError::None)
    {
        std::fputs("crd-tls-peer: the session could not be made\n", stderr);
        return 1;
    }

    char  received[256] = {};
    usize received_size = 0U;
    bool  pinged        = false;
    if (client && !write_frame(session))
    {
        return 1;
    }
    usize size = 0U;
    while (read_frame(size))
    {
        if (session.state() == TlsState::Handshaking || session.state() == TlsState::Established)
        {
            (void)session.receive(ConstSpan<u8>(g_frame, size));
        }
        if (session.state() == TlsState::Established)
        {
            u8    plain[4096];
            usize got = 0U;
            while (session.read(Span<u8>(plain, sizeof(plain)), got) == TlsError::None && got > 0U)
            {
                const usize keep = got < sizeof(received) - 1U - received_size ? got
                                                                               : sizeof(received) - 1U - received_size;
                std::memcpy(received + received_size, plain, keep);
                received_size += keep;
                if (!client)
                {
                    (void)session.send(ConstSpan<u8>(plain, got));
                }
            }
            if (client && !pinged && session.state() == TlsState::Established)
            {
                pinged = true;
                for (u64 i = 0U; i < pings && pinged; ++i)
                {
                    pinged = session.send(ConstSpan<u8>(reinterpret_cast<const u8*>(kPing), sizeof(kPing) - 1U)) ==
                             TlsError::None;
                }
                if (pinged && update != nullptr)
                {
                    pinged = session.update_keys(std::strcmp(update, "requested") == 0) == TlsError::None;
                }
            }
        }
        if (!write_frame(session))
        {
            return 1;
        }
    }

    print_common(session);
    std::fprintf(stderr, R"("key_updates_sent": %llu, "key_updates_received": %llu, "key_update_records": %llu, )",
                 static_cast<unsigned long long>(session.key_updates_sent()),
                 static_cast<unsigned long long>(session.key_updates_received()),
                 static_cast<unsigned long long>(session.key_update_records()));
    std::fputs(R"("received": ")", stderr);
    for (usize i = 0U; i < received_size; ++i)
    {
        const char c = received[i];
        std::fputc(c >= ' ' && c <= '~' && c != '"' && c != '\\' ? c : '?', stderr);
    }
    std::fputs("\"}\n", stderr);
    return 0;
}
