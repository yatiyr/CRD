#pragma once

// crd-net -- QUIC transport parameters and compatible version negotiation (4.2a; contract:
// docs/design/network-foundation.md#quic-transport-parameters). Sources: RFC 9000 sections 7.3, 7.4 and 18, RFC 9221
// section 3 (max_datagram_frame_size), RFC 9287 section 3 (grease_quic_bit) and RFC 9368 sections 2 to 4 and 8
// (version_information). The bytes travel in the TLS handshake's quic_transport_parameters extension
// (crypto::QuicHandshake), authenticated; their encoding and every check is here.
//
//   reading   read_quic_transport_parameters takes a peer's bytes for the side that sent them and fills every
//             parameter, the defaults for those absent; unknown and reserved parameters are skipped (section 7.4.2).
//             TRANSPORT_PARAMETER_ERROR for a malformed or repeated parameter, a value outside its section 18.2
//             bounds, a server-only parameter from a client, and a version_information RFC 9368 calls a parsing
//             failure. Connection IDs are copied; nothing views the input after the call.
//   writing   only what differs from a default, in identifier order, then always one reserved parameter (31 * N + 27,
//             section 18.1) chosen from caller entropy, so a peer that does not ignore unknown parameters fails in
//             testing; version_information lists a reserved version too (RFC 9368 section 3).
//   checking  after the handshake: the connection ID authentication of section 7.3 and the downgrade prevention of
//             RFC 9368 section 4, each returning the error the connection closes with.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/quic_frame.hpp>

namespace crd::net
{
// The transport parameter identifiers of RFC 9000 section 18.2 (the IANA registry's first entries). RFC 9221's,
// RFC 9287's and RFC 9368's come from the standards register (register_ids.hpp, iana::kQuicTp...).
// NOLINTNEXTLINE(performance-enum-size): a 62-bit varint codepoint of its IANA registry, as the wire carries it
enum class QuicTransportParameterId : u64
{
    OriginalDestinationConnectionId = 0x00U,
    MaxIdleTimeout                  = 0x01U,
    StatelessResetToken             = 0x02U,
    MaxUdpPayloadSize               = 0x03U,
    InitialMaxData                  = 0x04U,
    InitialMaxStreamDataBidiLocal   = 0x05U,
    InitialMaxStreamDataBidiRemote  = 0x06U,
    InitialMaxStreamDataUni         = 0x07U,
    InitialMaxStreamsBidi           = 0x08U,
    InitialMaxStreamsUni            = 0x09U,
    AckDelayExponent                = 0x0AU,
    MaxAckDelay                     = 0x0BU,
    DisableActiveMigration          = 0x0CU,
    PreferredAddress                = 0x0DU,
    ActiveConnectionIdLimit         = 0x0EU,
    InitialSourceConnectionId       = 0x0FU,
    RetrySourceConnectionId         = 0x10U,
};

inline constexpr u64   kQuicDefaultMaxUdpPayload      = 65527U; // section 18.2
inline constexpr u64   kQuicMinMaxUdpPayload          = 1200U;
inline constexpr u64   kQuicDefaultAckDelayExponent   = 3U;
inline constexpr u64   kQuicMaxAckDelayExponent       = 20U;
inline constexpr u64   kQuicDefaultMaxAckDelay        = 25U;              // milliseconds
inline constexpr u64   kQuicMaxAckDelayLimit          = u64{1} << 14U;    // max_ack_delay must stay below it
inline constexpr u64   kQuicDefaultActiveIdLimit      = 2U;
inline constexpr usize kQuicMaxAvailableVersions      = 16U; // version_information's list this module keeps
inline constexpr usize kQuicMaxGreaseParameterBytes   = 8U;

// A connection ID carried in a transport parameter, held inline.
struct QuicConnectionId
{
    u8 bytes                                     = 0U;
    u8 id[crypto::kQuicMaxConnectionIdBytes]     = {};

    [[nodiscard]] cont::ConstSpan<u8> view() const noexcept { return cont::ConstSpan<u8>(id, bytes); }
    // Copies `source`; false, changing nothing, past kQuicMaxConnectionIdBytes.
    [[nodiscard]] bool assign(cont::ConstSpan<u8> source) noexcept;
};

// The server's preferred address (section 18.2, section 9.6): either family may be all zeros (absent).
struct QuicPreferredAddress
{
    u8               ipv4[4]                          = {};
    u16              ipv4_port                        = 0U;
    u8               ipv6[16]                         = {};
    u16              ipv6_port                        = 0U;
    QuicConnectionId connection_id;                   // never empty
    u8               reset_token[kQuicResetTokenBytes] = {};
};

// RFC 9368's version_information: the version the sender chose and the versions it would accept, in its
// preference order (a reserved version among them, which never selects anything).
struct QuicVersionInformation
{
    u32   chosen                             = 0U;
    usize available_count                    = 0U;
    u32   available[kQuicMaxAvailableVersions] = {};

    [[nodiscard]] cont::ConstSpan<u32> available_view() const noexcept
    {
        return cont::ConstSpan<u32>(available, available_count);
    }
};

// One side's transport parameters, every field at its section 18.2 default until set.
struct QuicTransportParameters
{
    bool                   has_original_destination_connection_id = false; // server only
    QuicConnectionId       original_destination_connection_id;
    u64                    max_idle_timeout                       = 0U; // milliseconds, 0 for none
    bool                   has_stateless_reset_token              = false; // server only
    u8                     stateless_reset_token[kQuicResetTokenBytes] = {};
    u64                    max_udp_payload_size                   = kQuicDefaultMaxUdpPayload;
    u64                    initial_max_data                       = 0U;
    u64                    initial_max_stream_data_bidi_local     = 0U;
    u64                    initial_max_stream_data_bidi_remote    = 0U;
    u64                    initial_max_stream_data_uni            = 0U;
    u64                    initial_max_streams_bidi               = 0U;
    u64                    initial_max_streams_uni                = 0U;
    u64                    ack_delay_exponent                     = kQuicDefaultAckDelayExponent;
    u64                    max_ack_delay                          = kQuicDefaultMaxAckDelay; // milliseconds
    bool                   disable_active_migration               = false;
    bool                   has_preferred_address                  = false; // server only
    QuicPreferredAddress   preferred_address;
    u64                    active_connection_id_limit             = kQuicDefaultActiveIdLimit;
    bool                   has_initial_source_connection_id       = false;
    QuicConnectionId       initial_source_connection_id;
    bool                   has_retry_source_connection_id         = false; // server only
    QuicConnectionId       retry_source_connection_id;
    u64                    max_datagram_frame_size                = 0U; // RFC 9221: 0, no DATAGRAM frames
    bool                   grease_quic_bit                        = false; // RFC 9287
    bool                   has_version_information                = false; // RFC 9368
    QuicVersionInformation version_information;
};

// What a writer adds that no peer may depend on: one reserved parameter, identifier 31 * N + 27 with N from
// `entropy`, and its value bytes (0 to kQuicMaxGreaseParameterBytes of `entropy`'s bytes, the count from `entropy`
// too), and with version_information one reserved version (crd::net::quic_grease_version of `entropy`'s high half)
// at the end of the available versions.
struct QuicTransportParameterGrease
{
    u64 entropy = 0U;
};

// The bytes write_quic_transport_parameters writes; 0 for parameters it refuses: a server-only parameter on a
// client's (`side` is the sender), a value outside its section 18.2 bounds, a preferred address with an empty
// connection ID, version_information with no versions, a zero or reserved version among them (the writer adds the
// reserved one), more than kQuicMaxAvailableVersions - 1 or, from a client, without its chosen version.
[[nodiscard]] usize quic_transport_parameters_bytes(const QuicTransportParameters& parameters, crypto::QuicSide side,
                                                    QuicTransportParameterGrease grease) noexcept;

// Writes them at the start of `out` and returns the bytes; 0, writing nothing, when refused or too large.
[[nodiscard]] usize write_quic_transport_parameters(const QuicTransportParameters& parameters, crypto::QuicSide side,
                                                    QuicTransportParameterGrease grease, cont::Span<u8> out) noexcept;

// Reads the parameters `side` sent: NoError with `out` filled, or TransportParameterError with `out` unchanged.
// A version_information listing more than kQuicMaxAvailableVersions versions is refused as well: no deployed version
// set comes near it, and keeping a peer's whole list would make the reader allocate.
[[nodiscard]] QuicTransportError read_quic_transport_parameters(cont::ConstSpan<u8> bytes, crypto::QuicSide side,
                                                                QuicTransportParameters& out) noexcept;

// ---- Connection ID authentication (section 7.3) ----

// What one end knows of the connection IDs its peer must confirm.
struct QuicConnectionIdExpectation
{
    cont::ConstSpan<u8> peer_source;         // the Source Connection ID of the peer's first Initial
    cont::ConstSpan<u8> original_destination; // client: the Destination Connection ID of its first Initial
    bool                retried = false;     // client: whether a Retry was accepted
    cont::ConstSpan<u8> retry_source;        // client: that Retry's Source Connection ID
};

// Checks the peer's parameters (`peer` is the side that sent them) against what this end saw: a missing
// initial_source_connection_id, a server's missing original_destination_connection_id, or a server's preferred
// address while it uses zero-length connection IDs (section 18.2), is TRANSPORT_PARAMETER_ERROR; a value that
// differs, a retry_source_connection_id missing after a Retry or present without one, PROTOCOL_VIOLATION.
[[nodiscard]] QuicTransportError quic_check_connection_ids(const QuicTransportParameters& parameters,
                                                           crypto::QuicSide peer,
                                                           const QuicConnectionIdExpectation& expected) noexcept;

// ---- Compatible version negotiation (RFC 9368) ----

// Whether a first flight of version `from` can be converted to `to` (RFC 9368 section 2.2): any version to itself,
// and versions 1 and 2 to each other (RFC 9369 section 5).
[[nodiscard]] bool quic_versions_compatible(u32 from, u32 to) noexcept;

// The first of `preference` that `offered` lists, never a reserved version; 0 when there is none. A client choosing
// after a Version Negotiation packet passes its own preference and the packet's versions.
[[nodiscard]] u32 quic_select_version(cont::ConstSpan<u32> preference, cont::ConstSpan<u32> offered) noexcept;

// The version a server runs a connection in whose client's first flight came in `original` (RFC 9368 section 2.3):
// the first of the server's `preference` that the client's available versions list and that `original` converts to,
// or `original` itself when the client sent no version_information or nothing else qualifies. 0 when `original` is
// not in `preference` either (the server answers with Version Negotiation instead).
[[nodiscard]] u32 quic_server_select_version(const QuicTransportParameters& client, u32 original,
                                             cont::ConstSpan<u32> preference) noexcept;

// A server's check of its client's version_information (RFC 9368 section 4): its chosen version must be the version
// of the client's first flight (`original`), else VERSION_NEGOTIATION_ERROR. A client that sent none passes: servers
// may complete without it.
[[nodiscard]] QuicTransportError quic_server_check_version_information(const QuicTransportParameters& client,
                                                                       u32 original) noexcept;

// A client's check of its server's version_information (RFC 9368 sections 4 and 8) for a connection that started
// in `attempted` and runs in `negotiated` (the same unless the server switched compatibly): the server's chosen
// version must be `negotiated`. After a Version Negotiation packet (`after_version_negotiation`) the server must
// also have sent the parameter (a version 1 server that did not is read as having chosen and listed only version 1),
// its list must not be empty, and the client, choosing by its `preference` among that list and `negotiated` as from
// a Version Negotiation packet, must arrive at `attempted`: otherwise VERSION_NEGOTIATION_ERROR.
[[nodiscard]] QuicTransportError quic_client_check_version_information(const QuicTransportParameters& server,
                                                                       u32 negotiated, u32 attempted,
                                                                       bool after_version_negotiation,
                                                                       cont::ConstSpan<u32> preference) noexcept;

} // namespace crd::net
