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
//   threads     a context is read-only after init() and serves sessions on any thread; a session is used by one
//               thread at a time. Sessions copy what they need, so a context may be cleared while they live.
//   allocation  contexts, credentials and sessions allocate inside the provider at init() and while records flow.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>
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
inline constexpr u8    kTlsAlertGeneralError = 117U;   // RFC 9846 section 6.2
inline constexpr u8    kTlsAlertUserCanceled = 90U;    // RFC 9846 section 6.1

// The bound on a session's own key updates, 2^48-1 (RFC 9846 section 4.7.3), and the fewest records a sending key may
// be configured for: one of data, its KeyUpdate and one alert.
inline constexpr u64 kTlsMaxKeyUpdates       = (u64{1} << 48U) - 1U;
inline constexpr u64 kTlsMinKeyUpdateRecords = 3U;

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
                     // without a credential, a name that is not a DNS name, a list above its bound
    NotReady,        // the context or session holds nothing, or the call does not fit the session's state
    Full,            // the bytes would exceed kTlsMaxBufferedBytes; nothing was taken
    PeerRefused,     // the peer's key is not pinned or its algorithm not accepted, or it names another server
    Failed,          // the handshake or a record failed; alert_sent() and alert_received() tell which
    Closed,          // the session is closed (close_notify)
    ProviderFailure, // the provider failed (no memory, no randomness)
};

// "none", "bad-config", "not-ready", "full", "peer-refused", "failed", "closed", "provider-failure".
[[nodiscard]] cont::StringView tls_error_name(TlsError error) noexcept;

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

struct ProviderCertificate; // the provider's certificate object, defined only inside crd-crypto

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

    // Shares `key` (which may be cleared afterwards) and issues its certificate: X.509 v3, a random serial number,
    // `name` as subject common name and DNS subjectAltName, valid from the provider's clock with no expiry
    // (99991231235959Z, RFC 5280 section 4.1.2.5: trust and rotation come from the pins, never from the dates).
    [[nodiscard]] TlsError init(const SigningKey& key, cont::StringView name) noexcept;
    void                   clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_certificate != nullptr; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_key.algorithm(); }
    [[nodiscard]] KeyId       key_id() const noexcept { return m_key_id; }

private:
    friend class TlsContext;

    ProviderCertificate*      m_certificate = nullptr;
    detail::ProviderKeyHandle m_key;
    KeyId                     m_key_id;
};

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
};

// The settings a session copies from its context.
struct TlsSettings
{
    TlsRole     role = TlsRole::Client;
    AlgorithmId signatures[kTlsMaxSignatures]{};
    u32         signature_count = 0U;
    KeyId       pinned[kTlsMaxPins];
    u32         pin_count = 0U;
    char        server_name[kTlsMaxNameBytes + 1U] = {};
    u32         name_bytes = 0U;
    u64         key_update_records = 0U;
    u64         max_key_updates    = kTlsMaxKeyUpdates;
};

struct ProviderTlsContext; // the provider's context object, defined only inside crd-crypto
struct ProviderTlsSession; // the provider's connection object, defined only inside crd-crypto

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

private:
    friend class TlsSession;

    ProviderTlsContext* m_context = nullptr;
    TlsSettings         m_settings;
};

// An alert as the session saw it: `description` is its RFC 9846 code when `present`.
struct TlsAlert
{
    bool present     = false;
    u8   description = 0U;
};

class TlsSession
{
public:
    TlsSession() noexcept = default;
    ~TlsSession() noexcept;
    TlsSession(const TlsSession&)            = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    TlsSession(TlsSession&&)                 = delete;
    TlsSession& operator=(TlsSession&&)      = delete;

    // A new session of `context`'s role, replacing any previous one. A client's first flight (its ClientHello) is
    // ready to take() when init() returns.
    [[nodiscard]] TlsError init(const TlsContext& context) noexcept;
    void                   clear() noexcept;

    [[nodiscard]] TlsState state() const noexcept { return m_state; }

    // Takes all of `bytes` (received from the peer) and runs the handshake as far as they allow. While the session
    // handshakes, an error moves it to Failed (PeerRefused or Failed) with the alert waiting in the outgoing bytes.
    // After the handshake the bytes wait for read(). Full when they would exceed kTlsMaxBufferedBytes (nothing taken);
    // NotReady in Idle and Failed; Closed after close_notify either way.
    [[nodiscard]] TlsError receive(cont::ConstSpan<u8> bytes) noexcept;

    // The outgoing bytes waiting (records and alerts); take() moves up to out.size() of them into `out`.
    [[nodiscard]] usize    pending() const noexcept;
    [[nodiscard]] TlsError take(cont::Span<u8> out, usize& written) noexcept;

    // Established only. send() protects all of `plaintext` into the outgoing bytes or nothing (Full when they would
    // exceed the bound); read() returns the application data received so far, up to out.size() bytes (0 when none),
    // and Closed once the peer's close_notify has been read.
    [[nodiscard]] TlsError send(cont::ConstSpan<u8> plaintext) noexcept;
    [[nodiscard]] TlsError read(cont::Span<u8> out, usize& written) noexcept;

    // Queues close_notify once the handshake is done; the session is Closed (Closed again when it was sent before, or
    // when the peer closed it during the handshake). NotReady before the handshake ends and after a failure: such a
    // session is abandoned with clear().
    [[nodiscard]] TlsError close() noexcept;

    // Established only: updates the sending key now (a KeyUpdate in the outgoing bytes) and, with `request_peer`, asks
    // the peer to update its own, unless a request is already outstanding (RFC 9846 section 4.7.3: the peer has not
    // answered it yet; the session's own key still updates). Full when the outgoing bytes have no room for it; Closed
    // when the update would reach max_key_updates, after close_notify was queued instead. send() updates the key by
    // itself before key_update_records() records, and read() answers a peer's request.
    [[nodiscard]] TlsError update_keys(bool request_peer) noexcept;

    // After the handshake: the negotiated group and AEAD, the peer key's algorithm and identifier (a server without
    // pins learns none: AlgorithmId{} and a zero KeyId), and whether the server asked for another key share.
    [[nodiscard]] AlgorithmId group() const noexcept { return m_group; }
    [[nodiscard]] AlgorithmId aead() const noexcept { return m_aead; }
    [[nodiscard]] AlgorithmId peer_algorithm() const noexcept { return m_peer_algorithm; }
    [[nodiscard]] KeyId       peer_key() const noexcept { return m_peer_key; }
    [[nodiscard]] bool        hello_retry() const noexcept { return m_hello_retry; }

    // The key updates sent and received, the records (data and alerts) protected so far under the current sending
    // key and the records after which the session updates it (0 before the handshake ends).
    [[nodiscard]] u64 key_updates_sent() const noexcept { return m_updates_sent; }
    [[nodiscard]] u64 key_updates_received() const noexcept { return m_updates_received; }
    [[nodiscard]] u64 records_under_key() const noexcept;
    [[nodiscard]] u64 key_update_records() const noexcept { return m_key_update_records; }

    [[nodiscard]] TlsAlert alert_sent() const noexcept { return m_alert_sent; }
    [[nodiscard]] TlsAlert alert_received() const noexcept { return m_alert_received; }

private:
    friend struct TlsCallbacks;

    [[nodiscard]] TlsError advance() noexcept;
    [[nodiscard]] TlsError fail(TlsError error) noexcept;
    [[nodiscard]] TlsError rekey(bool request_peer) noexcept;
    [[nodiscard]] TlsError close_at_bound() noexcept;
    [[nodiscard]] bool     flush_key_update() noexcept;

    ProviderTlsSession* m_session = nullptr;
    TlsSettings         m_settings;
    TlsState            m_state = TlsState::Idle;
    AlgorithmId         m_group{};
    AlgorithmId         m_aead{};
    AlgorithmId         m_peer_algorithm{};
    KeyId               m_peer_key;
    u64                 m_key_update_records = 0U;
    u64                 m_updates_sent       = 0U;
    u64                 m_updates_received   = 0U;
    bool                m_hello_retry        = false;
    bool                m_refused            = false; // the verify or name check refused the peer
    bool                m_update_requested   = false; // a KeyUpdate asked the peer for one, not answered yet
    TlsAlert            m_alert_sent;
    TlsAlert            m_alert_received;
};
} // namespace crd::crypto
