// crd-tls-peer -- one end of a TLS 1.3 connection through crd-crypto's sessions, driven over its standard streams by
// tests/foundation/crypto/interop/tls_interop.py (NET.2; docs/design/network-foundation.md#tls-sessions).
//
// Usage: crd-tls-peer --role client|server --groups g[,g...] --seed HEX [--pin HEX] [--name NAME]
//                     [--key-update-records N] [--pings N] [--update requested|not-requested]
//                     [--tickets FILE] [--ticket-key HEX] [--previous-ticket-key HEX]
//                     [--compression zstd|zlib|both|none]
//        crd-tls-peer --public HEX     (prints the Ed25519 public key of a seed, for the pins)
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
//
// Frames on both streams are a 4-byte big-endian length and that many bytes of TLS records. A client writes its first
// flight before reading; after that each end answers every frame it reads with exactly one frame (possibly empty).
// Once established, a client sends "ping from cerid" (--pings times) and a server echoes every byte it reads. At the
// end of input the end prints one JSON line on standard error with what it negotiated, whether it resumed, the key
// updates each way, the algorithm of its compressed Certificate sent or received and what it read, and exits 0 (2 on
// a usage error).

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

u8 g_frame[kTlsMaxBufferedBytes];

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

int usage()
{
    std::fputs("usage: crd-tls-peer --role client|server --groups g[,g...] --seed HEX [--pin HEX] [--name NAME] "
               "[--key-update-records N] [--pings N] [--update requested|not-requested] [--tickets FILE] "
               "[--ticket-key HEX] [--previous-ticket-key HEX] [--compression zstd|zlib|both|none]\n",
               stderr);
    return 2;
}

void print_alert(const char* key, TlsAlert alert)
{
    std::fprintf(stderr, "\"%s\": %d", key, alert.present ? static_cast<int>(alert.description) : -1);
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
        else if (std::strcmp(option, "--update") == 0 &&
                 (std::strcmp(value, "requested") == 0 || std::strcmp(value, "not-requested") == 0))
        {
            update = value;
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
    if (context.init(config) != TlsError::None ||
        session.init(context, tickets != nullptr ? &store : nullptr) != TlsError::None)
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

    const char* states[] = {"idle", "handshaking", "established", "closed", "failed"};
    const KeyId peer     = session.peer_key();
    std::fprintf(stderr, R"({"state": "%s", "group": "%s", "hello_retry": %s, "resumed": %s, "peer_key_id": ")",
                 states[static_cast<int>(session.state())], group_name(session.group()),
                 session.hello_retry() ? "true" : "false", session.resumed() ? "true" : "false");
    for (const u8 byte : peer.bytes)
    {
        std::fprintf(stderr, "%02x", byte);
    }
    std::fputs("\", ", stderr);
    std::fprintf(stderr, R"("key_updates_sent": %llu, "key_updates_received": %llu, "key_update_records": %llu, )",
                 static_cast<unsigned long long>(session.key_updates_sent()),
                 static_cast<unsigned long long>(session.key_updates_received()),
                 static_cast<unsigned long long>(session.key_update_records()));
    print_alert("alert_sent", session.alert_sent());
    std::fputs(", ", stderr);
    print_alert("alert_received", session.alert_received());
    std::fprintf(stderr, R"(, "certificate_compression": %u, "received": ")",
                 static_cast<unsigned>(session.certificate_compression()));
    for (usize i = 0U; i < received_size; ++i)
    {
        const char c = received[i];
        std::fputc(c >= ' ' && c <= '~' && c != '"' && c != '\\' ? c : '?', stderr);
    }
    std::fputs("\"}\n", stderr);
    return 0;
}
