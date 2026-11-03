#pragma once

// crd-crypto -- TLS 1.3 sessions over the provider's TLS stack (NET.2; contract:
// docs/design/network-foundation.md#tls-sessions and #tls-13-profile).
//
// The handshake, the key schedule and the record layer run inside the provider (AWS-LC's libssl, decision P1); Cerid
// configures them from register identifiers and never hands them a socket. A session is sans-I/O: the transport feeds
// it the bytes it received and takes the bytes it must send, so one session runs over TCP, inside NET-SIM's virtual
// sockets or against another session in the same process. Rules:
//
//   version     TLS 1.3 only (RFC 9846); a peer that offers nothing else is refused with protocol_version.
//   algorithms  key exchange groups, the peer's signature algorithms and the AEADs are AlgorithmIds the provider
//               implements. The server's group order decides: a client sends key shares for its first group and for
//               its first group of the other kind (post-quantum or not), and a server whose preferred group has no
//               share asks for one (HelloRetryRequest), so a hybrid group both sides hold is never traded for a
//               classical one. The AEAD is the client's first that the server accepts, or ChaCha20-Poly1305 when the
//               server's CPU has no AES instructions (the provider's rule): a client lists its AEADs fastest first
//               (tls_aead_preference()).
//   trust       a peer is trusted by a pinned key, never by a certificate authority: it is accepted when the SHA-256 of
//               its certificate's public key in the provider interface's encoding (key_id_of) is pinned and its key
//               algorithm is accepted. The certificate only carries the key. A client always authenticates the server;
//               a server with pins asks every client for its credential (mutual authentication).
//   names       a client names the server it expects; a server with a name refuses a client that names another.
//   alerts      toward a peer not yet authenticated a server answers general_error where a specific alert would tell
//               whether a name or a key exists (the TLS 1.3 profile); every alert sent or received is reported.
//   bounds      at most kTlsMaxBufferedBytes of received and of outgoing bytes wait in a session; more is Full.
//   key updates a session counts the records protected under its sending key and updates the key (KeyUpdate) before
//               the negotiated AEAD's usage limit (RFC 9846 section 5.5), or sooner when configured; it counts the
//               key updates each way, has at most one update_requested outstanding and never lets its own count pass
//               max_key_updates (at most 2^48-1, RFC 9846 section 4.7.3): an update that would reach the bound closes
//               the session instead. A receiver never enforces its peer's limits.
//   closure     a session never sends user_canceled (the provider has no warning alert to send, and a handshake is
//               abandoned with clear()); a peer's user_canceled is ignored and its close_notify, during the handshake
//               too, closes the session.
//   tickets     a server issues one ticket with every connection, resumed ones included, sealed under its ticket key;
//               a client hands the tickets it receives to the caller's ticket store and offers one, at most once, only
//               to a connection of the context that received it. Resumption is psk_dhe_ke (a fresh key share, the
//               hybrid group included).
//   early data  0-RTT (RFC 9846 sections 2.3 and 8) is off unless both ends configure it. A server that allows it
//               issues tickets that say so and opens each ticket once through its ReplayFilter (a ticket seen before,
//               or a false positive, resumes nothing: the handshake runs in full); the provider refuses early data
//               whose ticket age is off by more than kTlsMaxTicketAgeSkew, after a HelloRetryRequest and on another
//               application protocol, and Cerid refuses it under another early data context (the application state
//               it depends on, the message schema's digest among it, sealed in the ticket). Early bytes reach a
//               server's caller only through read_early(), never read(): they are replayable, and only operations
//               their schema declares idempotent may run on them (capability.hpp's authorize_operation). A client
//               sends them with send_early() up to its ticket's allowance; when its server refuses them they are gone,
//               and the client sends them again after the handshake.
//   certificates a client accepts its server's Certificate compressed (RFC 8879) with zstd or zlib; a server compresses
//               its own once, at init(), with each algorithm it may use, and sends the smallest form its client
//               accepts only when it is smaller than the plain message. A compressed Certificate is decoded into
//               exactly the length it claims, at most kTlsMaxCertificateBytes, the limit of a plain one too, and a
//               claim its compressed bytes cannot reach is refused before anything is reserved for it.
//   protocols   a context may name application protocols (ALPN, RFC 7301): a client offers them in its order, a server
//               selects the first of its own that the client offers, and with protocols configured on either end the
//               two agree on one or the handshake fails with no_application_protocol.
//   ECH         a client given its server's ECHConfigList encrypts its true ClientHello to the server's ECH key (RFC
//               9849): the server name, its tickets and its credential travel inside, the configuration's public name
//               outside. A client given a list never sends a hello without ECH (none of the list's configurations
//               usable is BadConfig before anything is sent); a client without one sends a GREASE extension, so its
//               hellos look alike. A server with ECH keys decrypts with its current and previous keys and answers a
//               hello it cannot decrypt under the key's public name only for the client to authenticate its retry
//               configurations; a rejected client reports them (EchRejected) for one retried connection. Under a
//               policy store the HPKE suites are the generation's too (decision S21): a client offers only
//               configurations whose every usable suite it allows, and a server publishing a refused one is refused.
//   QUIC        a QuicHandshake runs the same handshake over QUIC (RFC 9001): CRYPTO data per encryption level instead
//               of records, each level's secrets turned into QuicPacketKey and QuicHeaderKey, the transport parameters
//               carried both ways and an application protocol required.
//   threads     a context is read-only after init() and serves sessions on any thread; a session is used by one
//               thread at a time. Sessions copy what they need, so a context may be cleared while they live.
//   allocation  contexts, credentials and sessions allocate inside the provider at init() and while records flow.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/register_ids.hpp>

namespace crd::crypto
{
namespace cont = crd::containers;

inline constexpr usize kTlsMaxGroups         = 8U;     // key exchange groups in one configuration
inline constexpr usize kTlsMaxSignatures     = 8U;     // accepted peer signature algorithms
inline constexpr usize kTlsMaxAeads          = 3U;     // record protection algorithms
inline constexpr usize kTlsMaxPins           = 16U;    // pinned peer keys
inline constexpr usize kTlsMaxNameBytes      = 253U;   // a DNS name (RFC 1035), the server_name's longest host name
inline constexpr usize kTlsMaxBufferedBytes  = 65536U; // received or outgoing bytes waiting in one session
inline constexpr usize kTlsMaxCertificateBytes = kTlsMaxBufferedBytes; // a peer's Certificate, compressed or not
inline constexpr usize kTlsMaxProtocols     = 8U;     // application protocols (ALPN) in one configuration
inline constexpr usize kTlsMaxProtocolListBytes = 256U; // their wire list: a length byte and 1 to 255 bytes each
inline constexpr usize kTlsMaxExporterLabelBytes = 249U;  // an exporter label, which "tls13 " leads in HkdfLabel
inline constexpr usize kTlsMaxExporterBytes      = 8160U; // 255 x SHA-256's length: what HKDF gives every suite
inline constexpr u8    kTlsAlertGeneralError = 117U;   // RFC 9846 section 6.2
inline constexpr u8    kTlsAlertUserCanceled = 90U;    // RFC 9846 section 6.1
inline constexpr u8    kTlsAlertNoApplicationProtocol = 120U; // RFC 7301 section 3.2

// Encrypted Client Hello (RFC 9849, decision S14): an ECH key is a DHKEM(X25519, HKDF-SHA256) private key (RFC 9180);
// its ECHConfig is 51 to 59 bytes (one to three cipher suites) and the public name; a client's ECHConfigList may also
// list configurations of other versions and KEMs (with post-quantum public keys), which it skips.
inline constexpr usize kTlsEchKeyBytes           = 32U;
inline constexpr usize kTlsMaxEchKeys            = 4U;    // ECH keys of one server context, the previous ones included
inline constexpr usize kTlsMaxEchAeads           = 3U;    // the HPKE AEADs one ECHConfig lists, each with HKDF-SHA256
inline constexpr usize kTlsMaxEchConfigBytes     = 59U + kTlsMaxNameBytes;
// The AEADs an ECHConfig lists unless told otherwise: AES-128-GCM, and ChaCha20-Poly1305 for a client without AES
// instructions.
inline constexpr AlgorithmId kTlsEchDefaultAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Chacha20Poly1305};
inline constexpr usize kTlsMaxEchConfigListBytes = 4096U;

// The bound on a session's own key updates, 2^48-1 (RFC 9846 section 4.7.3), and the fewest records a sending key may
// be configured for: one of data, its KeyUpdate and one alert.
inline constexpr u64 kTlsMaxKeyUpdates       = (u64{1} << 48U) - 1U;
inline constexpr u64 kTlsMinKeyUpdateRecords = 3U;

// The RFC 8879 certificate compression algorithms Cerid speaks, in a client's default order: zstd compresses
// certificates best, zlib is what peers without zstd decode. Brotli (2) is not offered: the engine has no brotli codec.
inline constexpr u16 kTlsCertificateCompression[] = {iana::kCertCompressionZstd, iana::kCertCompressionZlib};

// A server's ticket protection key, and its tickets' lifetime: two days unless configured, at most seven (RFC 9846
// section 4.6.1).
inline constexpr usize kTlsTicketKeyBytes        = 32U;
inline constexpr u32   kTlsDefaultTicketLifetime = 172800U;
inline constexpr u32   kTlsMaxTicketLifetime     = 604800U;

// Early data (0-RTT): the bytes a Cerid server's tickets allow over records (the provider's 14,336, below one record's
// 2^14, so 0-RTT data never comes near a key's usage limit); over QUIC a ticket allows any amount (0xffffffff, RFC 9001
// section 4.6.1) and the transport's flow control bounds it. A server refuses early data whose client's ticket age is
// more than kTlsMaxTicketAgeSkew seconds from its own (the provider's freshness check, RFC 9846 section 8.3).
inline constexpr u32 kTlsEarlyDataBytes   = 14336U;
inline constexpr u32 kTlsMaxTicketAgeSkew = 60U;
inline constexpr u32 kQuicEarlyDataBytes  = 0xFFFFFFFFU;

// The bytes of a server's early data context: the state 0-RTT depends on besides the ticket (RFC 9000 section 7.4.1
// over QUIC; the application's own over records).
inline constexpr usize kTlsMaxEarlyDataContextBytes = 8192U;

enum class TlsRole : u8
{
    Client,
    Server,
};

enum class TlsState : u8
{
    Idle,        // no session
    Handshaking, // the handshake runs
    Established, // application data flows
    Closed,      // close() sent close_notify, or the peer's arrived
    Failed,      // a fatal alert was sent or received; only the outgoing bytes (the alert) remain
};

enum class TlsError : u8
{
    None,
    BadConfig,       // an algorithm outside its class or the provider, no group, a client without pins, a server
                     // without a credential, a name that is not a DNS name, a list above its bound, a QUIC handshake
                     // without an application protocol or transport parameters
    NotReady,        // the context or session holds nothing, or the call does not fit the session's state
    Full,            // the bytes would exceed kTlsMaxBufferedBytes; nothing was taken
    PeerRefused,     // the peer's key is not pinned or its algorithm not accepted, it names another server, or it
                     // agrees on no application protocol
    Failed,          // the handshake or a record failed; alert_sent() and alert_received() tell which
    Closed,          // the session is closed (close_notify)
    EchRejected,     // a client's ECH offer was rejected and the server authenticated (by a pinned key): the session
                     // never carries data, and ech() tells what the client may do next
    ProviderFailure, // the provider failed (no memory, no randomness)
    PolicyRefused,   // the context's policy generation refuses every configured group, AEAD or peer signature
                     // algorithm, its own credential's, every ECH configuration a client was given or one a server
                     // publishes (init), or, at a key update, an algorithm the session runs on (the session is then
                     // closed with close_notify, decisions S20 and S21)
};

// "none", "bad-config", "not-ready", "full", "peer-refused", "failed", "closed", "ech-rejected", "provider-failure",
// "policy-refused".
[[nodiscard]] cont::StringView tls_error_name(TlsError error) noexcept;

// What became of Encrypted Client Hello in a handshake (RFC 9849).
enum class TlsEch : u8
{
    None,     // not offered (a GREASE extension at most), or a server that decrypted no offer
    Accepted, // the server decrypted the client's inner ClientHello and the handshake ran on it
    Retry,    // a client's offer was rejected and the server sent ECH configurations: retry once with them
              // (TlsSession::init's ech_retry_configs, from ech_retry_configs())
    Disabled, // rejected, and the server sent no configuration of this version: it turned ECH off. Connecting again
              // without ECH reveals the server name, which only the caller may decide (RFC 9849 section 6.1.6)
    Refused,  // rejected on a connection that already retried: the server's configurations disagree; report a failure
};

// "none", "accepted", "retry", "disabled", "refused".
[[nodiscard]] cont::StringView tls_ech_name(TlsEch ech) noexcept;

// What became of early data (0-RTT) on a connection (decision S17).
enum class TlsEarlyData : u8
{
    None,     // not offered: no early data configured, no ticket that allows it, or a server whose client sent none
    Offered,  // a client's, until its server answers: early bytes may be sent (send_early) under the 0-RTT keys
    Accepted, // the server took the early data; on a server, read_early() returns it
    Rejected, // the server refused it (all a client learns): another protocol, a HelloRetryRequest, another early
              // data context, or a server without early data; the client sends the bytes again after the handshake
    Replayed, // a server's: the offered ticket was not opened for the first time (its ReplayFilter), so the handshake
              // ran in full
    Stale,    // a server's: the client's ticket age is more than kTlsMaxTicketAgeSkew seconds off (the provider's
              // freshness check)
};

// "none", "offered", "accepted", "rejected", "replayed", "stale".
[[nodiscard]] cont::StringView tls_early_data_name(TlsEarlyData early) noexcept;

// The AEADs in this CPU's order: AES-128-GCM, AES-256-GCM and ChaCha20-Poly1305 with AES instructions,
// ChaCha20-Poly1305 first without them (the "chosen by CPU capability" default of the provider policy).
[[nodiscard]] cont::ConstSpan<AlgorithmId> tls_aead_preference() noexcept;

// The records one sending key of `aead` may protect (RFC 9846 section 5.5): floor(2^24.5) for AES-GCM, every
// sequence number (2^64-1) for ChaCha20-Poly1305, whose own limit lies beyond them; 0 for any other algorithm.
[[nodiscard]] u64 tls_key_usage_limit(AlgorithmId aead) noexcept;

// The TLS codepoints of a group or a signature algorithm (the register's TLS Supported Groups and TLS
// SignatureScheme values); 0 for any other algorithm.
[[nodiscard]] u16 tls_group_codepoint(AlgorithmId group) noexcept;
[[nodiscard]] u16 tls_signature_codepoint(AlgorithmId signature) noexcept;

class ReplayFilter;         // replay.hpp: a server's record of the tickets it has opened
struct ProviderCertificate; // the provider's buffer of a certificate's DER bytes, defined only inside crd-crypto
class StoredSigner;         // key_store.hpp
class DeviceSigner;         // key_store.hpp

// A key and the self-signed certificate that carries it to the peer.
class TlsCredential
{
public:
    TlsCredential() noexcept = default;
    ~TlsCredential() noexcept;
    TlsCredential(const TlsCredential&)            = delete;
    TlsCredential& operator=(const TlsCredential&) = delete;
    TlsCredential(TlsCredential&&)                 = delete;
    TlsCredential& operator=(TlsCredential&&)      = delete;

    // Shares `key` (which may be cleared afterwards) and issues its certificate: X.509 v3, a serial number from the
    // key's identifier, `name` as DNS subjectAltName (and subject common name when it fits), valid from the epoch with
    // no expiry (99991231235959Z, RFC 5280 section 4.1.2.5: trust and rotation come from the pins, never the dates).
    [[nodiscard]] TlsError init(const SigningKey& key, cont::StringView name) noexcept;
    // The same for a key store's signature key (key_store.hpp): its Current generation as `signer` loaded it. A key a
    // device holds (decision K12) signs its certificate and every handshake through the device, by the provider's
    // private-key method; the device must outlive the credential and every context made with it.
    [[nodiscard]] TlsError init(const StoredSigner& signer, cont::StringView name) noexcept;
    void                   clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_certificate != nullptr; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept;
    [[nodiscard]] KeyId       key_id() const noexcept { return m_key_id; }

private:
    friend class TlsContext;

    ProviderCertificate*      m_certificate = nullptr;
    detail::ProviderKeyHandle m_key;              // a provider key
    DeviceSigner*             m_device = nullptr; // or the credential's own copy of a device key's signer
    KeyId                     m_key_id;
};

struct ProviderEchKey; // the provider's HPKE key, defined only inside crd-crypto

// A server's ECH key and the ECHConfig that publishes it (RFC 9849 section 4, decision S14).
class TlsEchKey
{
public:
    TlsEchKey() noexcept = default;
    ~TlsEchKey() noexcept;
    TlsEchKey(const TlsEchKey&)            = delete;
    TlsEchKey& operator=(const TlsEchKey&) = delete;
    TlsEchKey(TlsEchKey&&)                 = delete;
    TlsEchKey& operator=(TlsEchKey&&)      = delete;

    // The key of `private_key` (kTlsEchKeyBytes drawn with random_bytes; the servers of a fleet share it, as they share
    // a ticket key) published as ECHConfig `config_id`: DHKEM(X25519, HKDF-SHA256) with HKDF-SHA256 and each of
    // `aeads` in their order (1 to kTlsMaxEchAeads of AES-128-GCM, AES-256-GCM and ChaCha20-Poly1305, each once; every
    // one a client may pick, so a policy refusing one refuses the key's servers, decision S21), `max_name_length` (the
    // longest name served behind `public_name`, to which every client pads its inner server name; 0 when not known)
    // and `public_name`, the DNS name of the server a rejected client authenticates, never one an IPv4 parser would
    // read as an address (RFC 9849 section 6.1.7). BadConfig for a key of another length, another name or AEAD list;
    // the private key may be zeroed afterwards.
    [[nodiscard]] TlsError init(cont::ConstSpan<u8> private_key, u8 config_id, cont::StringView public_name,
                                u8                           max_name_length = 0U,
                                cont::ConstSpan<AlgorithmId> aeads           = kTlsEchDefaultAeads) noexcept;
    void                   clear() noexcept;

    [[nodiscard]] bool ready() const noexcept { return m_key != nullptr; }
    [[nodiscard]] u8   config_id() const noexcept { return m_config_id; }
    // The AEADs the ECHConfig lists; empty when not ready.
    [[nodiscard]] cont::ConstSpan<AlgorithmId> aeads() const noexcept
    {
        return cont::ConstSpan<AlgorithmId>(m_aeads, m_aead_count);
    }

    // The ECHConfig (version 0xfe0d); empty when not ready.
    [[nodiscard]] cont::ConstSpan<u8> config() const noexcept { return cont::ConstSpan<u8>(m_config, m_config_bytes); }

    // The public name; empty when not ready.
    [[nodiscard]] cont::StringView public_name() const noexcept { return cont::StringView(m_name, m_name_bytes); }

private:
    friend class TlsContext;

    ProviderEchKey* m_key = nullptr;
    u8              m_config[kTlsMaxEchConfigBytes] = {};
    u32             m_config_bytes                  = 0U;
    char            m_name[kTlsMaxNameBytes + 1U]   = {};
    u32             m_name_bytes                    = 0U;
    u8              m_config_id                     = 0U;
    AlgorithmId     m_aeads[kTlsMaxEchAeads]        = {};
    u32             m_aead_count                    = 0U;
};

// The ECHConfigList of `keys` in their order (RFC 9849 section 4): what a service publishes (the ech parameter of its
// DNS HTTPS record, RFC 9848, or Cerid's service discovery) and its clients take as TlsConfig::ech_configs. BadConfig
// for no key, more than kTlsMaxEchKeys or one not ready; Full when `out` is too small (nothing written).
[[nodiscard]] TlsError tls_ech_config_list(cont::ConstSpan<const TlsEchKey*> keys, cont::Span<u8> out,
                                           usize& written) noexcept;

// What a client's tickets are bound to: the SHA-256 of its role, the server name (in lower case), its own key, its
// pins and the signature algorithms it accepts. A ticket resumes only under the binding that received it.
struct TlsBinding
{
    u8 bytes[32] = {};

    friend bool operator==(const TlsBinding&, const TlsBinding&) = default;
};

// A client's tickets (decision S9), kept by the caller: the session hands it each ticket it receives and takes one
// when it starts. Tickets never leave the store they were kept in, so the caller keeps one store per correlation
// domain and connections meant to be uncorrelated share none (T15). A ticket is a secret (its resumption key): the
// store zeroes the tickets it drops and the bytes it handed out. A QUIC handshake keeps and takes its tickets under
// its own binding, so a ticket of one transport is never offered on the other (the provider would not resume it).
// The bytes are Cerid's ticket record: the early data the ticket allows and, over QUIC, the server's transport
// parameters that 0-RTT keeps to (RFC 9000 section 7.4.1), beside the provider's session; a record that does not parse
// is never offered.
class TlsTicketStore
{
public:
    TlsTicketStore()                                 = default;
    TlsTicketStore(const TlsTicketStore&)            = delete;
    TlsTicketStore& operator=(const TlsTicketStore&) = delete;
    TlsTicketStore(TlsTicketStore&&)                 = delete;
    TlsTicketStore& operator=(TlsTicketStore&&)      = delete;
    virtual ~TlsTicketStore()                        = default;

    // Keeps a copy of `ticket`, received under `binding` (called from TlsSession::read()); a store without room drops a
    // ticket, the oldest or this one.
    virtual void keep(const TlsBinding& binding, cont::ConstSpan<u8> ticket) noexcept = 0;

    // Removes a ticket kept under `binding` and returns its bytes, valid until the store's next call; empty when it
    // holds none. A ticket is taken once: a client never offers one twice (T07).
    [[nodiscard]] virtual cont::ConstSpan<u8> take(const TlsBinding& binding) noexcept = 0;
};

// RFC 9850 key logging (decision S19, threat T22): each TLS 1.3 secret of a session or QUIC handshake as one
// SSLKEYLOGFILE line, "<label> <client random> <secret>" in lower-case hex with no terminator (a sink writing a file
// ends each line itself), so a test can decrypt its own captures. Only for test traffic (RFC 9850 section 6): a
// shipping build compiles it out, a context given a sink there is refused, and no environment variable turns it on.
// The sink runs on the thread driving a session, inside the call that derived the secret, possibly for several
// sessions of one context at once; it must not call into the session and keeps no line beyond the call. A line is a
// key: whoever holds it decrypts and can forge the connection, so it never enters a diagnostic record or log.
using TlsKeyLogSink = void (*)(void* user, cont::StringView line) noexcept;

// Whether this build has RFC 9850 key logging: false in a shipping build.
[[nodiscard]] bool tls_key_log_compiled() noexcept;

struct TlsConfig
{
    TlsRole                      role = TlsRole::Client;
    cont::ConstSpan<AlgorithmId> groups;       // hybrid or classical key exchange, preferred first
    cont::ConstSpan<AlgorithmId> signatures;   // the algorithms the peer's key may use
    cont::ConstSpan<AlgorithmId> aeads;        // record protection (a client's: preferred first)
    const TlsCredential*         credential = nullptr; // a server's, or a client's for mutual authentication
    cont::ConstSpan<KeyId>       pinned;       // the peer keys accepted (a server without pins asks for none)
    cont::StringView             server_name;  // a client's expected server, a server's own name (empty: any)
    u64 key_update_records = 0U; // records under one sending key, its KeyUpdate included, before the session updates
                                 // it: 0 for the negotiated AEAD's usage limit, else at least kTlsMinKeyUpdateRecords
                                 // (lowered to that limit when above it)
    u64 max_key_updates    = 0U; // the session's own key updates: 0 for kTlsMaxKeyUpdates, else 1 to that bound
    cont::ConstSpan<u8> ticket_key;          // a server's ticket key (kTlsTicketKeyBytes): servers sharing it resume
                                             // each other's tickets; empty: init() draws one only this context opens
    cont::ConstSpan<u8> previous_ticket_key; // a server's key before its last rotation: opens tickets, seals none
    u32 ticket_lifetime = 0U; // a server's tickets' lifetime in seconds: 0 for kTlsDefaultTicketLifetime, at most
                              // kTlsMaxTicketLifetime
    // RFC 8879 algorithms of kTlsCertificateCompression, each once: those a client decodes (offered in this order) or
    // a server may compress its Certificate with (it sends the smallest that is smaller than the plain message); empty:
    // none, for a context whose record padding must not depend on the credential (RFC 8879 section 5).
    cont::ConstSpan<u16> certificate_compression = kTlsCertificateCompression;
    // Application protocol identifiers (ALPN), each 1 to 255 bytes and listed once, at most kTlsMaxProtocols and
    // kTlsMaxProtocolListBytes on the wire: a client's offer in its order, a server's in its preference. Empty: none
    // (a stream session then negotiates none; a QUIC handshake needs at least one).
    cont::ConstSpan<cont::StringView> application_protocols;
    // A server's ECH keys (RFC 9849, decision S14), at most kTlsMaxEchKeys in all with distinct config_ids, read at
    // init() (they may be cleared afterwards): the current ones, published and sent to a client whose offer they
    // cannot decrypt (its retry configurations), and those before the last rotation, still decrypted but no longer
    // sent. A server with a name answers their public names too, only for a rejected client to authenticate it: a
    // handshake that would complete under a public name alone is refused (general_error).
    cont::ConstSpan<const TlsEchKey*> ech_keys;
    cont::ConstSpan<const TlsEchKey*> previous_ech_keys;
    // A client's ECHConfigList for its server (the service's published list, at most kTlsMaxEchConfigListBytes): each
    // session offers ECH with the first configuration it supports, and refuses to start without one (BadConfig; under
    // a policy store PolicyRefused when the generation leaves none, decision S21).
    cont::ConstSpan<u8> ech_configs;
    // A client without ech_configs sends a GREASE ECH extension (RFC 9849 section 6.2), so its hellos look like those
    // that offer ECH; false only where no observer could tell the two apart anyway.
    bool ech_grease = true;
    // Early data (0-RTT, decision S17), off unless set: a client sends it on a ticket whose server allows it; a server
    // issues tickets that allow it and accepts it, opening each ticket once through `early_data_filter`, which it
    // requires: a ReplayFilter that is ready, whose period covers the ticket lifetime and kTlsMaxTicketAgeSkew, shared
    // by every session of the context and kept alive while they live (BadConfig otherwise, and for a filter without
    // early data or on a client).
    bool          early_data        = false;
    ReplayFilter* early_data_filter = nullptr;
    // RFC 9850 key logging (TlsKeyLogSink) of every session and QUIC handshake of the context: the traffic secrets
    // (CLIENT_EARLY_TRAFFIC_SECRET, CLIENT_ and SERVER_HANDSHAKE_TRAFFIC_SECRET, CLIENT_ and SERVER_TRAFFIC_SECRET_0)
    // and, with `key_log_exporters`, the exporter secrets (EXPORTER_SECRET; EARLY_EXPORTER_SECRET should the provider
    // log it), which yield every exporter value a channel binding or an authentication relies on (RFC 9850 section 6:
    // their separate authorization). A user or exporters without a sink,
    // and a sink where tls_key_log_compiled() is false, are BadConfig.
    TlsKeyLogSink key_log           = nullptr;
    void*         key_log_user      = nullptr;
    bool          key_log_exporters = false;
    // The installation's crypto policy (decision S20; policy.hpp), outliving the context, or none (the configured lists
    // as given: tests and tools only; a product always names its store). Each session takes a snapshot when it starts:
    // the groups, AEADs and peer signature algorithms the generation refuses at that time in `policy_scope` drop out of
    // its offer and its acceptance, the preferred ones lead in the configured order, and a session whose own credential
    // is refused, or with nothing left of a list, is PolicyRefused. ECH's HPKE suites follow it too (decision S21): a
    // client offers only the configurations whose every suite the provider could pick is allowed, the preferred ones
    // first, and a server whose ECH keys publish a refused algorithm is refused. At each key update the session checks
    // its negotiated group, AEAD, peer key and credential against the store again and closes when one is refused.
    const PolicyStore* policy       = nullptr;
    PolicyScope        policy_scope = PolicyScope::Everywhere; // CeridServices for a Cerid service's sessions
};

// The settings a session copies from its context.
struct TlsSettings
{
    TlsRole     role = TlsRole::Client;
    AlgorithmId signatures[kTlsMaxSignatures]{};
    u32         signature_count = 0U;
    AlgorithmId groups[kTlsMaxGroups]{}; // as configured; a session keeps those its policy snapshot allows
    u32         group_count = 0U;
    AlgorithmId aeads[kTlsMaxAeads]{};
    u32         aead_count = 0U;
    AlgorithmId credential_algorithm{}; // AlgorithmId{} without a credential
    PolicyScope policy_scope = PolicyScope::Everywhere;
    const PolicyStore* policy = nullptr;
    KeyId       pinned[kTlsMaxPins];
    u32         pin_count = 0U;
    char        server_name[kTlsMaxNameBytes + 1U] = {};
    u32         name_bytes = 0U;
    u8          protocols[kTlsMaxProtocolListBytes] = {}; // the application protocols in ALPN's wire form
    u32         protocol_bytes = 0U;
    u64         key_update_records = 0U;
    u64         max_key_updates    = kTlsMaxKeyUpdates;
    TlsBinding  binding;
    u8          ticket_keys[2][kTlsTicketKeyBytes] = {}; // a server's: the sealing key, then the previous one
    u8          ticket_names[2][16]                = {}; // the keys' public names, which lead their tickets
    u32         ticket_key_count                   = 0U;
    ReplayFilter* early_filter                     = nullptr; // a server's, when it accepts early data
    u8          early_context[kSha256Bytes]        = {}; // a records server session's early data context's SHA-256
    bool        early_data                         = false;
    bool        ech_grease                         = false;
    TlsKeyLogSink key_log                          = nullptr;
    void*         key_log_user                     = nullptr;
    bool          key_log_exporters                = false;
};

namespace detail
{
// The decoding the provider runs for a compressed Certificate of RFC 8879 `algorithm` (zstd or zlib): true only when
// `compressed` decodes to exactly certificate.size() bytes, at most kTlsMaxCertificateBytes. A length outside that
// bound, or beyond what DEFLATE's 1032:1 lets a zlib `compressed` decode to, is refused before `certificate` is
// written. Exposed for the tests and the fuzz target, which cannot reach it through a handshake.
[[nodiscard]] bool tls_decompress_certificate(u16 algorithm, cont::ConstSpan<u8> compressed,
                                              cont::Span<u8> certificate) noexcept;
} // namespace detail

struct ProviderTlsContext; // the provider's context object, defined only inside crd-crypto
struct ProviderTlsSession; // the provider's connection object, defined only inside crd-crypto
struct ProviderBuffer;     // the provider's memory buffer, defined only inside crd-crypto
struct TlsCallbacks;       // the provider's callbacks, defined only inside crd-crypto

class TlsContext
{
public:
    TlsContext() noexcept = default;
    ~TlsContext() noexcept;
    TlsContext(const TlsContext&)            = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&&)                 = delete;
    TlsContext& operator=(TlsContext&&)      = delete;

    // Checks `config` (BadConfig, nothing kept) and builds the provider context, replacing any previous one.
    [[nodiscard]] TlsError init(const TlsConfig& config) noexcept;
    void                   clear() noexcept;

    [[nodiscard]] bool    ready() const noexcept { return m_context != nullptr; }
    [[nodiscard]] TlsRole role() const noexcept { return m_settings.role; }
    // Whether it configures early data: a client's sessions offer it, a server's accept it and name an early data
    // context at init().
    [[nodiscard]] bool early_data() const noexcept { return m_settings.early_data; }

private:
    friend struct TlsCallbacks;

    ProviderTlsContext* m_context = nullptr;
    TlsSettings         m_settings;
};

// An alert as the session saw it: `description` is its RFC 9846 code when `present`.
struct TlsAlert
{
    bool present     = false;
    u8   description = 0U;
};

namespace detail
{
// What a stream session and a QUIC handshake share: the provider's connection, the settings copied from the context
// and what the provider's callbacks learn about the handshake (tls.cpp's TlsCallbacks write it).
class TlsHandshakeState
{
public:
    TlsHandshakeState(const TlsHandshakeState&)            = delete;
    TlsHandshakeState& operator=(const TlsHandshakeState&) = delete;
    TlsHandshakeState(TlsHandshakeState&&)                 = delete;
    TlsHandshakeState& operator=(TlsHandshakeState&&)      = delete;

    [[nodiscard]] TlsState state() const noexcept { return m_state; }

    // After the handshake: the negotiated group and AEAD, the peer key's algorithm and identifier (a server without
    // pins learns none: AlgorithmId{} and a zero KeyId), and whether the server asked for another key share.
    [[nodiscard]] AlgorithmId group() const noexcept { return m_group; }
    [[nodiscard]] AlgorithmId aead() const noexcept { return m_aead; }
    [[nodiscard]] AlgorithmId peer_algorithm() const noexcept { return m_peer_algorithm; }
    [[nodiscard]] KeyId       peer_key() const noexcept { return m_peer_key; }
    [[nodiscard]] bool        hello_retry() const noexcept { return m_hello_retry; }

    // After the handshake, and on a server that accepted early data from its ServerHello on: whether it resumed a
    // ticket (the peer key is then the one the ticket's handshake proved).
    [[nodiscard]] bool resumed() const noexcept { return m_resumed; }

    // The RFC 8879 algorithm of the compressed Certificate this session sent or received (a server's own, a client's
    // server's); 0 when the Certificate went uncompressed or none was sent (a resumption).
    [[nodiscard]] u16 certificate_compression() const noexcept { return m_certificate_compression; }

    // The application protocol both ends agreed on (ALPN); empty before the handshake ends and when neither end
    // configured one. Valid while the session lives.
    [[nodiscard]] cont::StringView application_protocol() const noexcept;

    // What became of ECH: Accepted after a handshake on the inner ClientHello, Retry, Disabled or Refused on a client
    // whose handshake ended in EchRejected, else None.
    [[nodiscard]] TlsEch ech() const noexcept { return m_ech; }

    // After TlsEch::Retry: the ECHConfigList the server sent, which the server authenticated by a pinned key; for one
    // retried connection only (RFC 9849 section 6.1.6). Empty otherwise. Valid while the session lives.
    [[nodiscard]] cont::ConstSpan<u8> ech_retry_configs() const noexcept;

    // What became of early data: on a client Offered from init() until its server answers, settled when the handshake
    // ends; on a server settled from its ClientHello on.
    [[nodiscard]] TlsEarlyData early_data() const noexcept { return m_early; }

    [[nodiscard]] TlsAlert alert_sent() const noexcept { return m_alert_sent; }
    [[nodiscard]] TlsAlert alert_received() const noexcept { return m_alert_received; }

    // Established only: `out.size()` bytes of the connection's exporter (RFC 9846 section 7.5; over QUIC the same
    // exporter, RFC 9001 section 4) for `label` and `context`, equal at both ends, different for every connection (a
    // resumed one too) and unchanged by key updates. What binds a capability's presentation to the session it travels
    // in (capability.hpp). BadConfig for a label of 0 or above kTlsMaxExporterLabelBytes bytes and an `out` of 0 or
    // above kTlsMaxExporterBytes; NotReady outside Established; `out` is zeroed on any error.
    [[nodiscard]] TlsError export_keying_material(cont::StringView label, cont::ConstSpan<u8> context,
                                                  cont::Span<u8> out) const noexcept;

    // The policy generation the session started under (0 without a policy or before init()), and under a product
    // layer (decision S25) the base layer's generation composed with it (0 without one).
    [[nodiscard]] u64 policy_generation() const noexcept { return m_policy_generation; }
    [[nodiscard]] u64 base_policy_generation() const noexcept { return m_base_policy_generation; }

    // Established only (NotReady otherwise): whether the context's policy store still accepts, now, in the context's
    // scope, the negotiated group (none on a resumption without a key share) and AEAD, the peer key's algorithm and
    // the session's own credential's; None without a policy, PolicyRefused naming the first refused one. A stream
    // session checks at each of its key updates (decision S20); a QUIC transport calls it before each
    // QuicPacketKey::update and closes the connection on PolicyRefused.
    [[nodiscard]] TlsError check_policy() const noexcept;

protected:
    TlsHandshakeState() noexcept  = default;
    ~TlsHandshakeState() noexcept = default;

    // Frees the provider's connection (its keys zeroed) and resets everything to Idle.
    void release() noexcept;
    [[nodiscard]] TlsError fail(TlsError error) noexcept;

    friend struct crd::crypto::TlsCallbacks;

    ProviderTlsSession* m_session = nullptr;
    TlsTicketStore*     m_tickets = nullptr;
    TlsSettings         m_settings;
    TlsState            m_state = TlsState::Idle;
    AlgorithmId         m_group{};
    AlgorithmId         m_aead{};
    AlgorithmId         m_peer_algorithm{};
    KeyId               m_peer_key;
    u64                 m_updates_sent     = 0U;
    u64                 m_updates_received = 0U;
    u64                 m_policy_generation = 0U;
    u64                 m_base_policy_generation = 0U;
    u32                 m_early_allowance  = 0U; // a client's: the early bytes its offered ticket allows
    u32                 m_early_sent       = 0U; // a client's: the early bytes sent
    u32                 m_ticket_early     = 0U; // a client's: what the last NewSessionTicket allows
    u16                 m_certificate_compression = 0U;
    bool                m_hello_retry       = false;
    bool                m_resumed           = false;
    bool                m_refused           = false; // the verify, name or protocol check refused the peer
    bool                m_update_requested  = false; // a KeyUpdate asked the peer for one, not answered yet
    bool                m_protocol_offered  = false; // a server's client offered application protocols
    bool                m_ech_pending       = false; // a client given ECH configurations has not encrypted a hello
    bool                m_ech_retry         = false; // a client's ECH configurations are a rejection's retry ones
    bool                m_public_name       = false; // a server's client named one of its ECH public names only
    bool                m_early_offered     = false; // a server's client offered early data
    bool                m_ticket_replayed   = false; // a server's ReplayFilter refused the offered ticket
    TlsEch              m_ech               = TlsEch::None;
    TlsEarlyData        m_early             = TlsEarlyData::None;
    TlsAlert            m_alert_sent;
    TlsAlert            m_alert_received;
};
} // namespace detail

// TLS 1.3 over a byte stream (TCP, NET-SIM's virtual sockets, a session in the same process): records in, records out.
class TlsSession : public detail::TlsHandshakeState
{
public:
    TlsSession() noexcept = default;
    ~TlsSession() noexcept;
    TlsSession(const TlsSession&)            = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    TlsSession(TlsSession&&)                 = delete;
    TlsSession& operator=(TlsSession&&)      = delete;

    // A new session of `context`'s role, replacing any previous one. A client's first flight (its ClientHello) is
    // ready to take() when init() returns. A client with `tickets` offers a ticket it takes from them (when one parses
    // and belongs to this context's binding) and keeps the tickets its server issues there; without a store it offers
    // none and ignores NewSessionTicket. A client retrying after TlsEch::Retry passes the rejected session's
    // ech_retry_configs() as `ech_retry_configs` (read before init() returns): this connection alone offers ECH with
    // them in place of the context's, and a second rejection is TlsEch::Refused. A server whose context accepts early
    // data names its `early_data_context` (1 to kTlsMaxEarlyDataContextBytes): the application state its early data
    // depends on, encoded the same way on every connection (its message schema's digest, AuthorizedSchema::digest, and
    // any setting a replayed operation must see unchanged). Its tickets carry the context's SHA-256, sealed, and
    // allow 0-RTT only under the same context: a ticket of another resumes without early data (Rejected). BadConfig
    // for a server with a store or retry configurations, for an early data context missing, oversized or given where
    // early data is not accepted, and for a client with ECH configurations none of which it can use (nothing was
    // sent).
    [[nodiscard]] TlsError init(const TlsContext& context, TlsTicketStore* tickets = nullptr,
                                cont::ConstSpan<u8> ech_retry_configs = {},
                                cont::ConstSpan<u8> early_data_context = {}) noexcept;
    void                   clear() noexcept;

    // Takes all of `bytes` (received from the peer) and runs the handshake as far as they allow. While the session
    // handshakes, an error moves it to Failed (PeerRefused, Failed or a client's EchRejected) with the alert waiting in
    // the outgoing bytes. After the handshake the bytes wait for read(), and on a server that accepted early data, for
    // read_early() until the handshake ends. Full when they would exceed
    // kTlsMaxBufferedBytes (nothing taken); NotReady in Idle and Failed; Closed after close_notify either way.
    [[nodiscard]] TlsError receive(cont::ConstSpan<u8> bytes) noexcept;

    // The outgoing bytes waiting (records and alerts); take() moves up to out.size() of them into `out`.
    [[nodiscard]] usize    pending() const noexcept;
    [[nodiscard]] TlsError take(cont::Span<u8> out, usize& written) noexcept;

    // Established only. send() protects all of `plaintext` into the outgoing bytes or nothing (Full when they would
    // exceed the bound); read() returns the application data received so far, up to out.size() bytes (0 when none),
    // and Closed once the peer's close_notify has been read. Where a key update falls due (send() before its records,
    // read() after the peer's KeyUpdate) and check_policy() refuses, the session queues close_notify and is Closed:
    // PolicyRefused, with nothing of `plaintext` sent, or with the bytes read before it in `out`.
    [[nodiscard]] TlsError send(cont::ConstSpan<u8> plaintext) noexcept;
    [[nodiscard]] TlsError read(cont::Span<u8> out, usize& written) noexcept;

    // A client while early_data() is Offered: protects all of `plaintext` as early data behind its ClientHello, or
    // nothing (Full when it would pass early_data_room() or the outgoing bound). The bytes are replayable: only
    // operations their schema declares idempotent go here. NotReady in any other state.
    [[nodiscard]] TlsError send_early(cont::ConstSpan<u8> plaintext) noexcept;

    // The early bytes a client may still send: its ticket's allowance less what send_early() took; 0 unless Offered.
    [[nodiscard]] u32 early_data_room() const noexcept;

    // A server that accepted early data, until the handshake ends: the early bytes received so far, up to out.size()
    // (0 when none waits). Early bytes come only from here, never from read(), and the handshake goes on only through
    // here: once the client's early data has ended, a call completes the handshake (Established; what follows is
    // read()'s), so the server calls it after each receive() until the session is Established. An error fails the
    // session as receive() does. NotReady for a client and for a server that accepted no early data.
    [[nodiscard]] TlsError read_early(cont::Span<u8> out, usize& written) noexcept;

    // Queues close_notify once the handshake is done; the session is Closed (Closed again when it was sent before, or
    // when the peer closed it during the handshake). NotReady before the handshake ends and after a failure: such a
    // session is abandoned with clear().
    [[nodiscard]] TlsError close() noexcept;

    // Established only: updates the sending key now (a KeyUpdate in the outgoing bytes) and, with `request_peer`, asks
    // the peer to update its own, unless a request is already outstanding (RFC 9846 section 4.7.3: the peer has not
    // answered it yet; the session's own key still updates). Full when the outgoing bytes have no room for it; Closed
    // when the update would reach max_key_updates, and PolicyRefused when check_policy() refuses, after close_notify
    // was queued instead. send() updates the key by itself before key_update_records() records, and read() answers a
    // peer's request.
    [[nodiscard]] TlsError update_keys(bool request_peer) noexcept;

    // The key updates sent and received, the records (data and alerts) protected so far under the current sending
    // key and the records after which the session updates it (0 before the handshake ends).
    [[nodiscard]] u64 key_updates_sent() const noexcept { return m_updates_sent; }
    [[nodiscard]] u64 key_updates_received() const noexcept { return m_updates_received; }
    [[nodiscard]] u64 records_under_key() const noexcept;
    [[nodiscard]] u64 key_update_records() const noexcept { return m_key_update_records; }

private:
    friend struct TlsCallbacks;

    [[nodiscard]] TlsError advance() noexcept;
    [[nodiscard]] TlsError established() noexcept;
    [[nodiscard]] TlsError rekey(bool request_peer) noexcept;
    [[nodiscard]] TlsError close_at_bound() noexcept;
    [[nodiscard]] TlsError close_refused() noexcept;
    void                   flush() noexcept;
    [[nodiscard]] bool     flush_key_update() noexcept;

    u64 m_key_update_records = 0U;
};

// The encryption levels of a QUIC connection (RFC 9001 section 4.1.3). The first kQuicLevels carry CRYPTO data:
// Initial packets are protected by quic_initial_secret's keys, the others by the handshake's own secrets. Early (0-RTT)
// packets carry none (RFC 9000 section 12.4): the level has keys only, the client's (decision S17).
enum class QuicLevel : u8
{
    Initial,
    Handshake,
    Application,
    Early,
};

inline constexpr usize kQuicLevels = 3U;

// The bytes of transport parameters a QUIC handshake sends (RFC 9000 section 18): far above any real set, whose
// parameters encode in a few hundred bytes. Their encoding and checks are the transport's (crd-net, 4.2a); the
// handshake carries them, authenticated, as the quic_transport_parameters extension (57).
inline constexpr usize kQuicMaxTransportParameterBytes = 4096U;

// The QUIC error code of a TLS alert sent during a QUIC handshake (RFC 9001 section 4.8): CRYPTO_ERROR, 0x0100 plus the
// alert, which the transport sends in a CONNECTION_CLOSE frame at the alert's level.
[[nodiscard]] constexpr u64 quic_crypto_error(u8 alert) noexcept
{
    return 0x0100U + alert;
}

// The TLS 1.3 handshake over QUIC (RFC 9001; contract: docs/design/network-foundation.md#quic-handshake), sans-I/O
// like TlsSession: the transport provides the CRYPTO data it received at each level, in order, and takes the CRYPTO
// data to send at each level; it turns each level's secrets into packet and header keys with derive_keys(). Over the
// same contexts as TlsSession (pins, names, groups, tickets, certificate compression), with these differences: no
// records, no close_notify and no TLS KeyUpdate (a QUIC connection closes with CONNECTION_CLOSE and updates its keys
// with QuicPacketKey::update; a peer's KeyUpdate fails the handshake), an application protocol is required, and the
// transport parameters travel in the hellos. A server's NewSessionTicket goes out at the Application level with its
// last flight; a client provides it there after the handshake.
class QuicHandshake : public detail::TlsHandshakeState
{
public:
    QuicHandshake() noexcept = default;
    ~QuicHandshake() noexcept;
    QuicHandshake(const QuicHandshake&)            = delete;
    QuicHandshake& operator=(const QuicHandshake&) = delete;
    QuicHandshake(QuicHandshake&&)                 = delete;
    QuicHandshake& operator=(QuicHandshake&&)      = delete;

    // A new handshake of `context`'s role sending `transport_parameters` (1 to kQuicMaxTransportParameterBytes, the
    // encoded parameters of this connection: a server's include the connection IDs RFC 9000 section 7.3
    // authenticates), replacing any previous one. A client's ClientHello is pending at the Initial level when init()
    // returns. Tickets and ECH as TlsSession::init, tickets kept under this transport's binding, each with the server's
    // transport parameters. A server whose context accepts early data names its `early_data_context` (1 to
    // kTlsMaxEarlyDataContextBytes): the transport parameters a client remembers and the application state, the
    // message schema's digest among it, encoded the same way on every connection; its tickets accept 0-RTT only under
    // the same context (RFC 9000 section 7.4.1; the provider compares it with the one its ticket recorded).
    // BadConfig for a context without an application protocol, missing or oversized parameters, an early data context
    // missing, oversized or given where early data is not accepted, and as TlsSession::init.
    [[nodiscard]] TlsError init(const TlsContext& context, cont::ConstSpan<u8> transport_parameters,
                                TlsTicketStore* tickets = nullptr, cont::ConstSpan<u8> ech_retry_configs = {},
                                cont::ConstSpan<u8> early_data_context = {}) noexcept;
    void                   clear() noexcept;

    // Takes all of `bytes`, the CRYPTO data received at `level` continuing what was provided there before (the
    // transport reassembles CRYPTO frames by offset), and runs the handshake, or after it the post-handshake messages
    // (a client's tickets), as far as they allow. Full when the level's unprocessed bytes would exceed receive_limit()
    // (nothing taken; the transport closes with CRYPTO_BUFFER_EXCEEDED). An error moves the handshake to Failed:
    // PeerRefused, Failed or a client's EchRejected, with alert_sent() and alert_level() telling the CRYPTO_ERROR to
    // close with when an alert was sent, and none when the data came at a level the handshake did not expect (a
    // PROTOCOL_VIOLATION). NotReady in Idle and Failed.
    [[nodiscard]] TlsError provide(QuicLevel level, cont::ConstSpan<u8> bytes) noexcept;

    // The CRYPTO data waiting to be sent at `level` (at most kTlsMaxBufferedBytes across the levels; a flight beyond it
    // fails the handshake); take() moves up to out.size() bytes of it into `out`. The Early level has none: provide()
    // and take() refuse it with BadConfig, pending() and receive_limit() are 0.
    [[nodiscard]] usize    pending(QuicLevel level) const noexcept;
    [[nodiscard]] TlsError take(QuicLevel level, cont::Span<u8> out, usize& written) noexcept;

    // The most CRYPTO data the handshake buffers unprocessed at `level` (RFC 9000 section 7.5): at least 4096 bytes,
    // more where the peer's Certificate may be long. 0 before init().
    [[nodiscard]] usize receive_limit(QuicLevel level) const noexcept;

    // Whether the secret of `side`'s packets at `level` (Handshake, Application or Early) is ready. A level's secret
    // for writing comes before the peer's for reading, so a packet can always be acknowledged at its own level; a
    // server's Application secret for writing comes before the client's Finished (half-RTT data: the client is not yet
    // authenticated). The Early secret is the client's only: a client's from init() while early_data() is Offered, a
    // server's once it accepted; a client's is zeroed when its server refuses early data.
    [[nodiscard]] bool has_keys(QuicLevel level, QuicSide side) const noexcept;

    // Derives `side`'s packet and header protection keys at `level` for `version` (the connection's negotiated
    // version) from that secret, and zeroes the secret: each is handed out once, and QuicPacketKey::update derives
    // every later key phase (0-RTT has no key update: RFC 9001 section 6). NotReady when it is not ready (or was
    // handed out), BadConfig for the Initial level (whose keys quic_initial_secret gives) and an unknown version.
    [[nodiscard]] TlsError derive_keys(QuicLevel level, QuicSide side, QuicVersion version, QuicPacketKey& packet,
                                       QuicHeaderKey& header) noexcept;

    // The peer's transport parameters as received (the transport decodes and checks them); empty until they arrive
    // (a server's with the ClientHello, a client's with EncryptedExtensions). Valid while the handshake lives.
    [[nodiscard]] cont::ConstSpan<u8> peer_transport_parameters() const noexcept;

    // A client offering early data: the server's transport parameters from the connection that issued the ticket,
    // which its 0-RTT packets keep to (RFC 9000 section 7.4.1); once the handshake accepts, the transport checks that
    // peer_transport_parameters() lowers none of them. Empty when the handshake offers no ticket. Valid while the
    // handshake lives.
    [[nodiscard]] cont::ConstSpan<u8> remembered_transport_parameters() const noexcept;

    // The level alert_sent() was written at (the handshake's write level then): the transport's CONNECTION_CLOSE goes
    // at it (RFC 9001 section 4.8) and, before the handshake is confirmed, also at the lower levels its peer may still
    // read only (RFC 9000 section 10.2.3).
    [[nodiscard]] QuicLevel alert_level() const noexcept { return m_alert_level; }

private:
    friend struct TlsCallbacks;

    [[nodiscard]] TlsError advance() noexcept;

    struct Secret
    {
        u8          bytes[kQuicMaxSecretBytes] = {};
        u8          size                       = 0U;
        AlgorithmId aead{};
    };

    static constexpr usize kEarlySecrets = 2U; // m_secrets' Early row, whose secret is the client's only
    static constexpr usize kNoSecret     = 3U;

    // The row of m_secrets that holds `side`'s secret at `level`; kNoSecret for the Initial level and a server's Early.
    [[nodiscard]] static usize secret_level(QuicLevel level, QuicSide side) noexcept;
    static void                forget(Secret& secret) noexcept;

    ProviderBuffer* m_outgoing[kQuicLevels] = {};
    Secret          m_secrets[3][2]; // [Handshake, Application, Early][Client, Server]
    cont::Array<u8> m_remembered;    // a client's: its ticket's server transport parameters
    QuicLevel       m_alert_level = QuicLevel::Initial;
};
} // namespace crd::crypto
