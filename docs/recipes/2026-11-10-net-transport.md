# Recipe — The crd-net transport layer: from a UDP socket to an application channel

<!-- doc-role: reference -->
> Technique reference. Live owner: [4.2a](../ROADMAP.md#slice-4.2a) (Done 2026-11-10); path behaviour:
> [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Current work: [ROADMAP](../ROADMAP.md); current rules:
> [AGENTS](../../AGENTS.md).

This recipe teaches the transport layer of `crd-net`, roadmap row 4.2a, end to end: how bytes leave and enter a socket,
how QUIC turns an unreliable network into secure streams and datagrams, how the same streams ride over TCP and TLS
when UDP is blocked, how browsers reach the engine through HTTP/3, WebTransport, HTTP/2 and WebSocket, and how all of
it ends in one place, the **channel**, that the game or the editor actually uses.

It assumes a careful engineer who is not a networking specialist. Every term is explained the first time it appears.
The contract behind every statement is the design document's S-sections
([network-foundation.md, S33 to S65](../design/network-foundation.md#transport)) and the dated session logs; each part
names its source. Where something could not be confirmed from a file, the recipe says so (section 8).

Path behaviour (congestion control, ECN, path MTU discovery) belongs to the next row,
[NET-PATH](../ROADMAP.md#slice-net-path), which is still **Partial**. Its knobs sit in the same configuration structs,
so they appear here, always marked "NET-PATH".

---

## 1. Parameters first

### 1.0 Words the tables use

Read these eight first; the rest of the recipe builds on them.

- **Datagram.** One self-contained message on the network. UDP (the User Datagram Protocol) sends datagrams: each
  arrives whole or not at all, possibly late, twice, or out of order. Nothing is resent.
- **Byte stream.** An ordered, reliable flow of bytes. TCP (the Transmission Control Protocol) gives one per
  connection; QUIC gives many per connection.
- **QUIC.** A transport protocol (RFC 9000) built on UDP. It adds encryption, many independent streams, resending of
  lost data, and unreliable datagrams (RFC 9221). Versions 1 (RFC 9000) and 2 (RFC 9369) differ only in constants.
- **Packet.** QUIC's unit inside a datagram. One datagram may hold several packets ("coalesced"); each packet holds
  **frames**, small typed records such as STREAM (stream bytes), ACK (what arrived) or DATAGRAM.
- **RTT (round-trip time).** How long a message takes to go to the peer and an answer to come back.
- **Window / credit.** How many bytes the receiver allows the sender to send ahead (flow control), or how many the
  network is believed to carry at once (the congestion window). Both stop a fast sender from flooding.
- **Sans-I/O.** A component that never touches a socket or a clock. You hand it bytes and the time; it hands you bytes
  to send and the time it next needs to run. Every protocol object in `crd-net` is sans-I/O, so tests and the
  simulator (NET-SIM) drive the same code with virtual sockets and virtual time.
- **ns.** Nanoseconds. All times in the configuration are `u64` nanoseconds unless the table says otherwise.

A field marked "set by the connection" must stay at its default: the code fills it. "No bound stated" means the
header states no limit; it does not mean any value works.

### 1.1 The socket and the I/O loop

`UdpOptions` ([udp.hpp](../../engine/foundation/net/include/crd/net/udp.hpp)): one UDP socket.

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `bind` | The local address and port. Port 0 asks the operating system for a free one; the address picks IPv4 or IPv6 | endpoint | none | any local address |
| `dual_stack` | An IPv6 socket also carries IPv4; IPv4 peers are reported as IPv4, never as `::ffff:a.b.c.d` | bool | true | |
| `receive_buffer_bytes` | The kernel's receive buffer | bytes | 0 (the host's default) | no bound stated |
| `send_buffer_bytes` | The kernel's send buffer | bytes | 0 (the host's default) | no bound stated |

`IoLoopConfig` ([io_loop.hpp](../../engine/foundation/net/include/crd/net/io_loop.hpp)): the loop that reads the socket.

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `batch` | Datagrams read by one receive call (Linux reads them with one `recvmmsg`) | datagrams | 32 | no bound stated |
| `slot_bytes` | Size of each receive slot; a longer datagram is marked truncated and dropped | bytes | 1500 | no bound stated |
| `batches_per_poll` | Receive calls in one turn of `poll()`, so timers still run under a flood | calls | 4 | no bound stated |
| `queued_answers` | Stateless answers (Retry, Version Negotiation, reset) waiting while the socket is busy | datagrams | 64 | no bound stated |
| `max_wait_ns` | The longest `run()` sleeps without a turn | ns | 100 ms | no bound stated |

### 1.2 Admission at the door (the ingress)

`IngressConfig` ([ingress.hpp](../../engine/foundation/net/include/crd/net/ingress.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `versions`, `version_count` | QUIC versions the server runs, in preference order; Version Negotiation lists them | version numbers | {1, 2}, 2 | 1 to 8 (`kQuicMaxNegotiationVersions`) |
| `local_cid_bytes` | Length of the connection IDs this side issues (an ID names a connection independent of the address) | bytes | 16 (128 random bits) | 8 to 20 |

`HandshakeRules` (same header): how many new handshakes may start. Each rate is a `GateRule`
([quota.hpp](../../engine/foundation/net/include/crd/net/quota.hpp)): `per_second` events on average plus
`tolerance_ns` of credit, so an idle gate lets `per_second × tolerance` events through at once (the generic cell rate
algorithm of ITU-T I.371: one integer, no floating point, so a simulated clock reproduces it exactly). The header says
NET.4 measures and publishes the final defaults; these are the starting configuration.

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `unvalidated` | Handshakes from addresses not yet proven, all together | per second, ns credit | 20,000/s, 1 s | `per_second` 0 means none |
| `per_prefix` | The same from one address prefix bucket | per second, ns credit | 64/s, 1 s | |
| `validated` | Handshakes from proven addresses, all together | per second, ns credit | 50,000/s, 1 s | |
| `answers` | Retry, Version Negotiation and reset packets, all together | per second, ns credit | 50,000/s, 1 s | |
| `ipv4_prefix_bits`, `ipv6_prefix_bits` | How much of an address names one "source" | bits | 32, 64 | |
| `prefix_buckets` | Shared rate gates the prefixes hash into | count | 4096 | a power of two, 1 to 2^24 |
| `always_retry` | Prove every address with a Retry before any handshake | bool | false | |

### 1.3 The QUIC connection

`QuicConnectionConfig` ([quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `side` | Client or server | enum | Client | |
| `version` | The QUIC version this connection runs | enum | V1 | V1, V2 |
| `tls` | The TLS context with the application protocol name (ALPN, RFC 7301). Null: the connection can only be opened keyed (the raw UDP path) | pointer | null | |
| `tickets` | Where resumption tickets are kept | pointer | null | |
| `ids` | Where this side's extra connection IDs come from; null gives sequence 0 alone and no stateless reset token | pointer | null | |
| `parameters` | This side's transport parameters (table 1.4) | struct | RFC 9000 18.2 defaults | |
| `streams` | Stream limits and buffers (table 1.5) | struct | all zero: no streams | |
| `datagrams` | Datagram limits (table 1.6) | struct | all zero: none | |
| `versions` | Versions accepted in preference order (`version_information`, RFC 9368) | list | empty: `version` alone | must contain `version` |
| `after_version_negotiation` | A client restarted by a Version Negotiation packet (turns on the downgrade check) | bool | false | |
| `token` | A NEW_TOKEN token from an earlier connection, proving the client's address | bytes | empty | at most 256 (`kQuicMaxCarriedTokenBytes`) |
| `early_context` | A server's part of its 0-RTT context (its settings and schema digest) | bytes | empty | at most 8095 (`kQuicMaxEarlyContextBytes` = 8192 − 97: the TLS context limit less 1 + 12 × 8 bytes of remembered limits) |
| `held_packets` | Packets kept while the keys that open them may still arrive (RFC 9001 section 4.1.4) | packets | 8 | 0 to 64 |
| `grease_quic_bit` | Send `grease_quic_bit` and accept a cleared QUIC bit (RFC 9287) | bool | true | |
| `sent_packets` | Packets per packet number space held in flight (waiting for acknowledgement) | packets | 128 | no bound stated |
| `sent_frames` | Frame records settled on acknowledgement or loss (the "ledger"), eight per packet | records | 1024 | no bound stated |
| `crypto_bytes` | Handshake (CRYPTO) buffer per encryption level and direction | bytes | 16,384 | multiple of 64, at least 4096 |
| `max_datagram_bytes` | The largest datagram sent; also the ceiling of the MTU search (NET-PATH) | bytes | 1472 | no bound stated |
| `base_datagram_bytes` | What the path is known to carry (NET-PATH); `QuicHost` sets 1232 toward IPv6 | bytes | 1200 | 1200 to `max_datagram_bytes` |
| `path_mtu_discovery` | Probe for larger datagrams (NET-PATH, RFC 8899) | bool | true | the host clears it on a socket that cannot forbid fragmentation |
| `padding_block` | Packets pad to the next multiple of this (NET.3's `padded_size()`, hides message sizes) | bytes | 0 (none) | 0 or a channel's block |
| `initial_rtt_ns` | The RTT assumed before any is measured (RFC 9002 section 6.2.2) | ns | 333 ms | |
| `congestion` | The congestion controller and pacing (NET-PATH, table 1.8) | struct | CUBIC | |
| `ecn` | The ECN codepoint datagrams are marked with (NET-PATH) | enum | ECT(0) | ECT(0), ECT(1), Not-ECT; CE refused |
| `key_update_interval` | 1-RTT packets one key seals before an update starts | packets | 0 (the AEAD's own limit alone) | |
| `entropy_seed` | A fixed key for the connection's randomness (grease, skipped numbers), for deterministic tests | bytes | empty: fresh from the provider | |

### 1.4 Transport parameters (what each side tells the other in the handshake)

`QuicTransportParameters` ([quic_transport_parameters.hpp](../../engine/foundation/net/include/crd/net/quic_transport_parameters.hpp)),
RFC 9000 section 18. Bounds are section 18.2's; a value outside them is TRANSPORT_PARAMETER_ERROR (S35).

| Name | What it is | Units | Default | Who sets it |
|---|---|---|---|---|
| `max_idle_timeout` | Close the connection after this long with nothing received | ms | 0 (none) | you |
| `max_udp_payload_size` | The largest datagram this side will read | bytes | 65,527 | you (at least 1200) |
| `ack_delay_exponent` | Scale of the delay field in ACK frames | power of 2 | 3 | you (at most 20) |
| `max_ack_delay` | The longest this side waits before acknowledging | ms | 25 | you (below 2^14) |
| `disable_active_migration` | The peer must not move the connection to another address | bool | false | the server sets it (migration is NET-PATH's) |
| `preferred_address` | A server's other address | struct | none | you (server) |
| `active_connection_id_limit` | How many of the peer's connection IDs this side keeps | count | 2 | you (2 to 8) |
| IDs (`original_destination_`, `initial_source_`, `retry_source_connection_id`), `stateless_reset_token`, `version_information`, `grease_quic_bit`, the `initial_max_*` stream and data limits, `max_datagram_frame_size`, `reset_stream_at` | | | | set by the connection from `streams`, `datagrams`, `ids` and `versions`: leave them |

Every write also carries one reserved parameter (identifier 31 × N + 27) and one reserved version, so a peer that does
not ignore unknown values fails in testing (S35).

### 1.5 Streams and flow control

`QuicStreamLimits` ([quic_stream_set.hpp](../../engine/foundation/net/include/crd/net/quic_stream_set.hpp)). All
zero is a connection without streams. A **bidirectional** stream carries bytes both ways, a **unidirectional** one
from its opener only.

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `peer_bidi`, `peer_uni` | Streams the peer may have open at once (sent as `initial_max_streams_*`); one slot each | streams | 0 | no bound stated |
| `local_bidi`, `local_uni` | Streams this side may open at once (also within the peer's grant) | streams | 0 | no bound stated |
| `receive_bytes` | Per-stream receive buffer, which is also that stream's largest window | bytes | 0 | multiple of 64, at most 16 MiB |
| `send_bytes` | Per-stream send buffer (data stays until acknowledged) | bytes | 0 | multiple of 64, at most 16 MiB |
| `stream_window` | The window first granted on each stream | bytes | 0 | 1 to `receive_bytes` |
| `connection_window` | The window first granted for all streams together (`initial_max_data`) | bytes | 0 | no bound stated |
| `connection_max_window` | The largest the connection window may auto-tune to | bytes | 0 | at least `connection_window` |
| `in_order` | QMux only: the path delivers in order, so a STREAM frame must continue its stream exactly | bool | false | |
| `reset_stream_at` | Advertise and accept RESET_STREAM_AT (a reset that keeps a prefix) | bool | false | not with `in_order` |

### 1.6 Unreliable datagrams

`QuicDatagramLimits` ([quic_datagram.hpp](../../engine/foundation/net/include/crd/net/quic_datagram.hpp)), RFC 9221.

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `max_frame_bytes` | `max_datagram_frame_size` advertised: the largest DATAGRAM frame accepted (type, length and payload) | bytes | 0 (none) | 0 to 65,535 |
| `receive_bytes` | Payload bytes held for the application | bytes | 0 | at least the largest payload accepted |
| `receive_count` | Datagrams held | datagrams | 0 | |
| `send_bytes` | Payload bytes queued to send | bytes | 0 (send none) | |
| `send_count` | Datagrams queued, including those whose outcome is not yet read | datagrams | 0 | |
| `max_age_ns` | A queued datagram older than this is dropped unsent | ns | 0 (no limit) | |
| `outcomes` | Report each sent datagram as Acked, Lost or Expired | bool | false | |

### 1.7 Loss recovery constants

Not configuration: fixed by RFC 9002 and `quic_recovery.hpp`
([header](../../engine/foundation/net/include/crd/net/quic_recovery.hpp)). Listed because every number in section 3.3
uses them. (`QuicRecoveryConfig` exists too, but the connection builds it from its own fields.)

| Constant | Value | Source |
|---|---|---|
| Packet threshold | 3 packets (raised to at most 32 after spurious losses) | RFC 9002 section 6.1.1 |
| Time threshold | 9/8 of an RTT | section 6.1.2 |
| Timer granularity | 1 ms | section 6.1.2 |
| Initial RTT | 333 ms | section 6.2.2 |
| Persistent congestion | 3 probe periods | section 7.6.1 |
| ACK after | 2 ack-eliciting packets | RFC 9000 section 13.2.2 |
| ACK ranges remembered | 32 per space | S37 |
| Clean detections before thresholds reset | 16 | RFC 8985 section 6.2 |

### 1.8 Congestion control and pacing (NET-PATH)

`CongestionPolicy` ([congestion.hpp](../../engine/foundation/net/include/crd/net/congestion.hpp)), interface version 1.

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `algorithm` | The controller | enum | CUBIC (RFC 9438) | NewReno (RFC 9002 appendix B), CUBIC |
| `initial_packets` | The first window, by RFC 9002 section 7.2's formula with 10 | datagrams | 10 | at most 64 |
| `minimum_packets` | The window's floor | datagrams | 2 | at least 2 |
| `hystart` | RFC 9406's slow start (leave slow start before loss) | bool | true | |
| `fast_convergence` | RFC 9438 section 4.7 (CUBIC only) | bool | true | |
| `pacing` | Spread a window over an RTT (RFC 9002 section 7.7) | bool | true | |
| `pacing_gain` | Pacing rate as a percentage of window per smoothed RTT | percent | 125 | 100 to 400 |

Path MTU constants ([quic_pmtu.hpp](../../engine/foundation/net/include/crd/net/quic_pmtu.hpp)): floor 1200 bytes,
IPv6 base 1232, default ceiling 1472, 3 probe losses fail a size (RFC 8899's MAX_PROBES), 600 s between searches
(PMTU_RAISE_TIMER), 3 suspect detections declare a black hole.

### 1.9 The hosts (many connections behind one socket)

`QuicServerConfig` ([quic_server.hpp](../../engine/foundation/net/include/crd/net/quic_server.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `connection` | Every connection's `QuicConnectionConfig` (side Server, `versions` = every version the ingress runs) | struct | | the server sets `version`, `ids` and `disable_active_migration` |
| `connections` | Live connections at most (a fixed pool) | connections | 256 | 1 to 2^20 |
| `early_datagrams` | 0-RTT datagrams held before their Initial arrives | datagrams | 16 | 0 for none |
| `early_per_client` | Of them per connection ID and address | datagrams | 4 | |
| `early_bytes` | The largest held | bytes | 1500 | |
| `early_lifetime_ns` | How long one is held | ns | 100 ms | |
| `send_slots` | Datagrams waiting for the socket | datagrams | 64 | |
| `send_retry_ns` | A connection held back by a busy socket is served again within this | ns | 1 ms | |
| `new_tokens` | Send each client a NEW_TOKEN token | bool | true | |

`QuicClientConfig` ([quic_client.hpp](../../engine/foundation/net/include/crd/net/quic_client.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `connection` | Every connection's config (side Client; `connect()` may name another TLS context) | struct | | |
| `connections` | Live connections at most | connections | 16 | 1 to 2^20 |
| `tokens` | Servers whose NEW_TOKEN token is remembered | servers | 16 | 0 to 4096 |
| `send_slots`, `send_retry_ns` | As on the server | datagrams, ns | 64, 1 ms | |
| `tickets` | The client's own ticket cache, used unless `connection.tickets` names one | struct | below | |

`TicketCacheConfig` ([ticket_cache.hpp](../../engine/foundation/net/include/crd/net/ticket_cache.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `entries` | Tickets kept | tickets | 16 | 0 (keep none) to 4096 |
| `per_binding` | Of them under one binding (server name, pins, QUIC version) | tickets | 4 | 1 to `entries` |
| `ticket_bytes` | The largest ticket kept | bytes | 8192 | 1 to 65,536 |
| `max_age_ns` | No ticket outlives this | ns | 7 days | 1 ns to 7 days (RFC 9846 section 4.6.1) |

`QuicHostLimits` ([quic_host.hpp](../../engine/foundation/net/include/crd/net/quic_host.hpp)) is what both hosts
derive from the configs above; you do not fill it yourself.

### 1.10 The TCP and TLS path (QMux) and its host

`QmuxConfig` ([qmux.hpp](../../engine/foundation/net/include/crd/net/qmux.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `tls` | Client or server TLS context with ALPN; null only for a tunnel | pointer | null | |
| `tickets` | A client's tickets; null offers and keeps none | pointer | null | |
| `early_context` | A server's 0-RTT context (as on QUIC) | bytes | empty | at most 8095 |
| `websocket` | Take connections whose TLS agrees on `http/1.1` as WebSocket | pointer | null (none) | |
| `tunnel` | Set by the owner of a WebSocket tunnel (QMux without its own TLS) | enum | None | |
| `http2` | Take connections whose TLS agrees on `h2` as HTTP/2 bearing WebSocket tunnels | pointer | null (none) | needs `websocket` |
| `http2_tunnels` | Tunnels one HTTP/2 connection carries at once | tunnels | 4 | 1 to 256 |
| `streams`, `datagrams` | The same structs as QUIC (tables 1.5 and 1.6) | | | |
| `idle_timeout_ns` | Idle timeout offered | ns | 30 s | 0 for none |
| `handshake_timeout_ns` | From `start()` to the end of the TLS handshake (covers the TCP connect) | ns | 10 s | |
| `close_wait_ns` | How long a closed side waits for the peer's end | ns | 1 s | |
| `keepalive_ns` | A QX_PING after this long with no record | ns | 0 (none) | |
| `padding_block` | Records pad to this multiple with PADDING frames | bytes | 0 | |

A QMux record holds at most 16,382 bytes (`kQmuxMaxRecord`).

`QmuxHostConfig` ([qmux_host.hpp](../../engine/foundation/net/include/crd/net/qmux_host.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `connection` | Every connection's `QmuxConfig` | struct | | |
| `connections` | Live connections and tunnels at most | count | 256 | 1 to 2^20 |
| `ready_per_turn` | Readiness reports one turn takes | reports | 256 | at least 1 |
| `accepts_per_turn` | Connections one turn accepts | connections | 32 | at least 1 |
| `accept_retry_ns` | Accepting pauses this long when out of file descriptors | ns | 100 ms | |
| `write_bytes_per_turn` | Bytes written for one connection in one turn | bytes | 256 KiB | at least one chunk |
| `max_wait_ns` | The longest `run()` waits without a turn | ns | 100 ms | |

`TcpListenOptions` ([stream_socket.hpp](../../engine/foundation/net/include/crd/net/stream_socket.hpp)): `bind`,
`dual_stack` (true), `backlog` (1024 queued connections, within the host's limit) and `connection` (`TcpOptions`:
receive and send buffer bytes, 0 for the host's default).

### 1.11 HTTP/3 and QPACK

`Http3Config` ([http3.hpp](../../engine/foundation/net/include/crd/net/http3.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `side` | Client or server | enum | Client | |
| `field_bytes` | The largest header section taken or sent (sent as SETTINGS_MAX_FIELD_SECTION_SIZE) | bytes | 16,384 | 1024 to 1 MiB |
| `decoder` | QPACK decoder (below) | struct | | |
| `encoder` | QPACK encoder (below) | struct | | |
| `datagram_bytes` | The largest HTTP datagram sent | bytes | 1500 | |
| `connect_protocol` | A server's SETTINGS_ENABLE_CONNECT_PROTOCOL (extended CONNECT, RFC 9220) | bool | false | |
| `datagrams` | HTTP datagrams (RFC 9297) | bool | false | |
| `webtransport` | The WebTransport offer (below) | struct | off | needs `datagrams` and `streams.reset_stream_at`; a server needs `connect_protocol` |
| `remembered` | A resuming client's stored server settings, used until the server's arrive | pointer | null | |
| `grease` | Varies the reserved values sent | number | 0 | |

`QpackDecoderConfig` and `QpackEncoderConfig` ([qpack.hpp](../../engine/foundation/net/include/crd/net/qpack.hpp)).

| Name | What it physically is | Units | Default |
|---|---|---|---|
| decoder `table_capacity` | The dynamic table the peer may fill (SETTINGS_QPACK_MAX_TABLE_CAPACITY) | bytes | 4096 |
| decoder `blocked_streams` | Streams that may wait for table inserts (SETTINGS_QPACK_BLOCKED_STREAMS) | streams | 16 |
| decoder `instruction_bytes` | Decoder-stream bytes waiting at most | bytes | 1024 |
| encoder `table_capacity` | The most of the peer's table this side uses (0 uses none) | bytes | 4096 |
| encoder `sections` | Header sections with outstanding dynamic references | sections | 64 |
| encoder `instruction_bytes` | Encoder-stream bytes waiting at most | bytes | 8192 |

`Http3Settings` is not a knob: it is what the peer said, with RFC 9114's defaults.

### 1.12 WebTransport

`Http3WebTransportSettings` in `Http3Config` (the offer) and `WebTransportConfig`
([webtransport.hpp](../../engine/foundation/net/include/crd/net/webtransport.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| offer `enabled` | SETTINGS_WT_ENABLED: take WebTransport at all | bool | false | |
| offer `max_streams_uni`, `max_streams_bidi`, `max_data` | Initial session flow-control limits; all non-zero on both ends turns flow control on | streams, bytes | 0 | stream limits at most 2^60 |
| offer `legacy_sessions` | Sessions offered under the legacy `webtransport` spelling | sessions | 0 (not offered) | |
| `sessions` | Sessions alive at once | sessions | 4 | also bounded by SETTINGS |
| `waiting_streams` | Streams held for a session not known yet | streams | 16 | |
| `waiting_datagrams` | Datagrams held for a session not known yet | datagrams | 8 | |
| `datagram_bytes` | The largest datagram that waits, or arrives as a capsule | bytes | 1200 | |
| `origins` | Origins a browser session may come from, compared exactly; empty refuses every browser | list | empty | |
| `any_origin` | Take every Origin: a deliberate choice, never a default | bool | false | |

### 1.13 HTTP/2 and HPACK

`Http2Config` ([http2.hpp](../../engine/foundation/net/include/crd/net/http2.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `side` | Client or server | enum | Client | |
| `field_bytes` | The largest header section taken or sent (section 6.5.2's count) | bytes | 16,384 | 1024 to 1 MiB |
| `streams` | Open streams at most (SETTINGS_MAX_CONCURRENT_STREAMS) | streams | 100 | at most 4096 |
| `stream_window` | Per-stream window (SETTINGS_INITIAL_WINDOW_SIZE) | bytes | 1 MiB | at most 2^31 − 1 |
| `connection_window` | Connection window | bytes | 16 MiB | at least 65,535 |
| `frame_bytes` | The largest frame taken | bytes | 16,384 | 16,384 to 16,777,215 |
| `table_bytes` | HPACK decoder table (SETTINGS_HEADER_TABLE_SIZE) | bytes | 4096 | |
| `encoder_table_bytes` | The most of the peer's table the encoder uses | bytes | 4096 | |
| `output_bytes` | The one output buffer for every frame written | bytes | 64 KiB | at least 1024 |
| `connect_protocol` | A server's SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 8441) | bool | false | |
| `settings_timeout_ns` | SETTINGS_TIMEOUT if the peer has not acknowledged by then | ns | 10 s | |
| `resets` | Flood gate for resets the peer causes (rapid reset and the provoked kinds) | GateRule | 64/s, 4 s credit (burst 256) | |
| `control` | Flood gate for SETTINGS, PING, PRIORITY, empty frames, tiny WINDOW_UPDATEs | GateRule | 256/s, 4 s credit (burst 1024) | |

`Http2Settings` is what the peer said (RFC 9113's defaults: table 4096, window 65,535, frame 16,384).

### 1.14 WebSocket and WebSocket tunnels

`WebSocketConfig` ([websocket.hpp](../../engine/foundation/net/include/crd/net/websocket.hpp)) and `WsTunnelConfig`
([websocket_tunnel.hpp](../../engine/foundation/net/include/crd/net/websocket_tunnel.hpp)).

| Name | What it physically is | Units | Default | Range |
|---|---|---|---|---|
| `protocols` | Subprotocols: a client offers them in order, a server takes the first of its own the client offers | tokens | none | 1 to 8, each at most 255 bytes |
| `path` | The resource, starting with `/`; a server compares it exactly, query ignored | text | none | |
| `host`, `origin` | A client's Host value and the Origin it claims | text | empty | |
| `origins` | A server's allowed Origins; empty refuses every browser | list | empty | |
| `any_origin` | Take every Origin | bool | false | |
| tunnel `connection` | Every tunnel's `QmuxConfig` (`tls` null, `tunnel` None) | struct | | |
| tunnel `tunnels` | Tunnels open at once on one HTTP connection | tunnels | 4 | 1 to 256 |

Fixed limits: an opening handshake head of 8 KiB and 64 fields, a control frame of 125 bytes, a frame header of at
most 14 bytes.

### 1.15 Channels (what the application sees)

A channel's declaration is NET.3's `ChannelDesc` ([wire.hpp](../../engine/foundation/net/include/crd/net/wire.hpp)),
authored in a schema. 4.2a maps it onto a path.

| Name | What it physically is | Units | Default |
|---|---|---|---|
| `channel_class` | Control, Input, State, Event, Transaction, Bulk or Media | enum | |
| `reliable`, `ordered` | Delivery promise: reliable ordered (one stream), reliable unordered (a stream per message), or unreliable (datagrams) | bool | false, false |
| `idempotent` | A second delivery changes nothing, so 0-RTT may carry it | bool | false |
| `priority` | 0 sheds last; becomes the stream's RFC 9218 urgency | 0 to 255 | 0 |
| `max_message_bytes`, `messages_per_s`, `bytes_per_s` | Limits the admission stage enforces | bytes, per second | 0 |
| `padding_block` | Traffic pads to a multiple of this (0, or at least 16) | bytes | 0 |

`ChannelLinkConfig` ([channel_link.hpp](../../engine/foundation/net/include/crd/net/channel_link.hpp)): `streams`
(16 peer streams read at once; past it a stream is refused), `fragment_slots` (4 messages per unreliable channel held
in pieces) and `fragment_timeout_ns` (1 s). A principal's ceilings are NET.3's `QuotaLimits`
([quota.hpp](../../engine/foundation/net/include/crd/net/quota.hpp)): 256 streams, 16,384 datagrams and messages per
second, 64 MiB/s, 8 MiB in reassembly, 4096 pending messages; NET.4 publishes the measured defaults.

---

## 2. What it is, and why the naive approach fails

`crd-net`'s Layer 1 (ADR-0035, [Layer 1](../decisions/0035-networking-architecture.md)) must give a game, a
simulation and an editor two promises over any network: **reliable ordered** delivery (session control, edits,
assets) and **unreliable unordered** delivery (player input, state snapshots, voice), with no dependency on the scene
or the job system, and the same behaviour on every platform and in the simulator.

**Naive approach 1: raw UDP, one datagram per message.** It is fast and simple, and it fails on every other count:

- A message larger than the path's MTU (maximum transmission unit, about 1500 bytes on Ethernet) is either fragmented
  by IP, where losing one fragment loses the whole message, or dropped.
- Nothing is resent, nothing is ordered, and nothing stops a sender from flooding a slow link: you must invent
  acknowledgements, retransmission timers, flow control and congestion control yourself, and get every corner right.
- It is not encrypted or authenticated. Anyone on the path reads it, forges it, or replays it.
- A server answering an unauthenticated datagram with a larger one becomes an **amplifier**: an attacker spoofs a
  victim's address and the server floods the victim. A server that allocates state per datagram is exhausted by a
  spoofed flood.
- Middleboxes (firewalls, NATs) and peers **ossify**: they learn to expect exact bytes, so the protocol can never change.

**Naive approach 2: plain TCP, one connection, messages in order.** TCP resends and orders, but:

- **Head-of-line blocking.** One lost segment stalls every message behind it, including the player input that was
  sent later and is already useless if late. A game needs latest-wins state, not a queue.
- Opening it costs a round trip, TLS another one or two; there are no unreliable messages at all.
- Connections are named by address and port, so a phone that changes network loses the connection.

**What 4.2a does instead.** QUIC (RFC 9000) is primary: encrypted and authenticated from the first packet by TLS 1.3,
many independent streams (a loss stalls only its own stream), unreliable datagrams beside them (RFC 9221), a three
times amplification limit and Retry against spoofing, connection IDs instead of addresses, and every field designed
to be greased. Where UDP is blocked, the very same stream and datagram code rides over TCP and TLS as **QMux**
(draft-ietf-quic-qmux-02). Browsers, which cannot open raw sockets, reach the engine through HTTP/3 with WebTransport,
or WebSocket over HTTP/1.1, HTTP/2 or HTTP/3. A **raw UDP path** for LAN peers and headless bots is still a QUIC
connection, keyed from a secret the session already holds. And all of them end in the same **channel link**, so the
application never knows which path it is on ([Transport](../design/network-foundation.md#transport)).

Two design rules run through everything:

- **Fixed memory, no allocation in steady state.** Every pool, ring and table is sized by configuration and taken
  once at construction; receiving and sending allocate nothing. Under attack a full table refuses; it never grows.
- **Bounded work before trust.** Admission runs in stages; each stage's cost is bounded, and expensive work (key
  derivation, decryption, a handshake) waits for the stage that justifies it
  ([Bounded admission](../design/network-foundation.md#admission)).

---

## 3. The protocols and the mathematics

### 3.1 The standards, precisely

The register ([cmake/standards-register.json](../../cmake/standards-register.json)) pins each one; its state is quoted
as read on 2026-11-10.

| What | Standard | Register entry, state | Where in this recipe |
|---|---|---|---|
| QUIC invariants (what every version keeps) | RFC 8999 | `quic.invariants`, implemented | 3.2 |
| QUIC transport | RFC 9000 | `quic.transport`, planned (its paths and qualification continue in NET-PATH and NET.5) | 3.2 to 3.8 |
| QUIC with TLS | RFC 9001 | `quic.tls`, implemented | 3.6, 3.9 |
| Loss detection and congestion control | RFC 9002 | `quic.recovery`, planned (NET-PATH) | 3.3, 3.13 |
| Unreliable datagrams | RFC 9221 | `quic.datagram`, implemented | 3.5 |
| Greasing the QUIC bit | RFC 9287 | `quic.grease-bit`, implemented | 3.8 |
| Compatible version negotiation | RFC 9368 | `quic.version-negotiation`, implemented | 3.8 |
| QUIC version 2 | RFC 9369 | `quic.v2`, implemented | 3.8 |
| Reliable reset (RESET_STREAM_AT) | draft-ietf-quic-reliable-stream-reset-11 | `quic.reliable-reset`, implemented | 3.4 |
| QMux over TLS | draft-ietf-quic-qmux-02 | `quic.qmux`, implemented | 3.10 |
| TLS 1.3 | RFC 9846, which obsoletes RFC 8446 and keeps version 0x0304 and the wire format | `tls.v13`, implemented (NET.2) | 3.9 |
| ALPN | RFC 7301 | `tls.alpn`, implemented | 4 |
| HTTP/3 | RFC 9114 | `http.h3`, implemented | 3.11 |
| QPACK | RFC 9204 | `http.qpack`, implemented | 3.11 |
| HTTP datagrams and capsules | RFC 9297 | `http.datagrams`, implemented | 3.11 |
| HTTP/2 | RFC 9113 | `http.h2`, implemented | 3.12 |
| HPACK | RFC 7541 | `http.hpack`, implemented | 3.11, 3.12 |
| WebSocket | RFC 6455 (opening head per RFC 9112) | `websocket`, implemented | 3.12 |
| WebSocket over HTTP/2 | RFC 8441 | `websocket.h2`, implemented | 3.12 |
| WebSocket over HTTP/3 | RFC 9220 | `websocket.h3`, implemented | 3.12 |
| WebTransport over HTTP/3 | draft-ietf-webtrans-http3-17 (and -16's codepoint) | `webtransport.h3`, planned | 3.11 |
| WebTransport over HTTP/2 | draft-ietf-webtrans-http2-15 | `webtransport.h2`, watch (review 2026-12-15) | not built: waits for the draft to settle |
| Extensible priorities | RFC 9218 (urgency 0 to 7) | | 4.7 |
| Too Early (status 425) | RFC 8470 | | 3.12 |
| Structured field strings | RFC 9651 | | 3.11 |
| Address text | RFC 4291 (reading), RFC 5952 (printing) | | 4.1 |
| Multiplexing with other UDP protocols | RFC 7983 (the fixed bit) | | 3.8 |
| Raw UDP path | Cerid design S55 | `cerid.raw-path`, implemented | 3.10 |
| CUBIC | RFC 9438 | `cc.cubic`, implemented (NET-PATH) | 3.13 |
| HyStart++ | RFC 9406 | `cc.hystart`, implemented (NET-PATH) | 3.13 |
| Reordering adaptation (RACK) | RFC 8985 section 6.2 | | 3.3 |
| ECN | RFC 3168 section 5; RFC 9000 sections 13.4 and A.4; RFC 9002 section B.7 | inside `quic.transport` (NET-PATH) | 3.13 |
| Path MTU discovery | RFC 8899; RFC 9000 sections 14.2 to 14.4 | `path.dplpmtud`, planned (NET-PATH) | 3.13 |
| GREASE practice | RFC 8701 (TLS), RFC 9287 (QUIC) | | 3.8 |

Other sources the design names: the flow-control auto-tuning of Chromium's QUIC flow controller (S38; no paper or
version is cited), ITU-T I.371's generic cell rate algorithm (`quota.hpp`), and the HTTP/2 flood advisories
CVE-2019-9511 (data dribble), CVE-2019-9517 (a peer that never reads) and CVE-2019-9518 (empty frames), plus the
"rapid reset" and "MadeYouReset" attack families (S63).

### 3.2 QUIC's shape: packets, levels, spaces and varints

**Encryption levels.** A QUIC connection protects its packets under four sets of keys as the TLS handshake
progresses (RFC 9001 section 4): **Initial** (keys anyone can derive from the client's first connection ID, so they
hide nothing from an attacker and only stop accidental corruption), **0-RTT** (early data from a resumed client),
**Handshake**, and **1-RTT** (the final keys). Initial and Handshake packets have a *long header* that carries the
version and both connection IDs; 1-RTT packets have a *short header* with only the destination connection ID.

**Packet number spaces.** Initial, Handshake and Application (0-RTT and 1-RTT together) each count their packets
from 0 and are acknowledged separately (RFC 9000 section 12.3). A packet number is never reused, so the AEAD nonce
(below) never repeats.

**AEAD.** Authenticated encryption with associated data: one operation both encrypts a packet and attaches a 16-byte
tag that proves nobody changed it. If the tag fails, the packet is dropped as noise. Nothing inside a packet is
trusted before its tag verifies (S36).

**Header protection.** The packet number itself is hidden by XOR with bytes derived from a 16-byte *sample* of the
ciphertext (RFC 9001 section 5.4). That is why a packet needs at least 20 bytes from its packet number onward, and why
the writer pads short packets: four bytes of packet number room plus the 16-byte sample.

**Variable-length integers** (RFC 9000 section 16). Most numbers on the wire use the top two bits of the first byte
as a length: `00` one byte (values to 63), `01` two bytes (to 16,383), `10` four bytes (to about 1.07 × 10^9), `11`
eight bytes (to 2^62 − 1). A writer must use the shortest form for frame types; a longer one is PROTOCOL_VIOLATION
(section 12.4), because two spellings of one value are a place for parsers to disagree.

**Connection IDs.** Each end chooses the IDs the *other* end puts in its packets (0 to 20 bytes). A server routes
packets by them, so a client that changes address (NAT rebinding, Wi-Fi to cellular) keeps its connection. The table
that maps IDs to connections is keyed with SipHash-2-4 under a secret key, so an attacker cannot choose IDs that all
land in one bucket and turn lookups linear (threat T10, S34).

### 3.3 Loss recovery: the RTT estimator, the thresholds and the probe timeout

All of this is RFC 9002 appendix A in code ([S37](../design/network-foundation.md#quic-recovery)).

**Measuring RTT** (RFC 9002 section 5). When an ACK newly acknowledges the largest packet it names, and an
ack-eliciting packet is among those it acknowledges, the sender takes `latest_rtt = now − send_time`. Then:

```
min_rtt      = the smallest latest_rtt seen (ignores the peer's delay)
adjusted     = latest_rtt − ack_delay   if that stays >= min_rtt, else latest_rtt
               (ack_delay is capped at max_ack_delay only after the handshake is confirmed, ignored in Initial)
first sample:  smoothed_rtt = latest_rtt;   rttvar = latest_rtt / 2
afterwards:    rttvar       = 3/4 · rttvar + 1/4 · |smoothed_rtt − adjusted|
               smoothed_rtt = 7/8 · smoothed_rtt + 1/8 · adjusted
before any sample: smoothed_rtt = 333 ms (initial_rtt_ns), rttvar = 166.5 ms
```

`smoothed_rtt` is a running average; `rttvar` is how much it jitters. Both are exponentially weighted moving averages
with weights 1/8 and 1/4.

**Declaring a packet lost** (section 6.1). A packet is lost when either:

```
packet threshold:  a packet 3 or more numbers later is acknowledged
time threshold:    it was sent more than  max(9/8 · max(smoothed_rtt, latest_rtt), 1 ms)  before a later
                   acknowledged packet
```

The leftover time to the time threshold arms the **loss timer**.

**The probe timeout (PTO)** (section 6.2). When nothing is acknowledged for a while, the sender does not declare loss;
it sends a probe to provoke an ACK:

```
PTO = smoothed_rtt + max(4 · rttvar, 1 ms) + max_ack_delay (Application space only)
each expiry doubles it:  PTO · 2^pto_count;  an ACK resets pto_count (once the peer validated the address)
```

A probe resends unacknowledged CRYPTO data if there is any, else a PING (section 6.2.4). Before the handshake is
confirmed the Application space arms no probe. A client with nothing in flight before the server validated its
address sends an **anti-deadlock** probe, because a server held by the amplification limit (3.6) cannot send until it
hears from the client.

**Persistent congestion** (section 7.6). If every ack-eliciting packet sent over a span longer than

```
3 · (smoothed_rtt + max(4 · rttvar, 1 ms) + max_ack_delay)
```

is lost, with none acknowledged in between and all sent after the first RTT sample, the path is badly congested and
the congestion window drops to its minimum (NET-PATH).

**Spurious losses** (section 6.1 by RFC 8985 section 6.2's mechanism; S37, 2026-11-08). On a path that *reorders*,
the fixed thresholds declare packets lost that were only late. Cerid keeps a lost packet's slot for one probe period;
if its acknowledgement arrives late, the loss was **spurious**. Then:

```
packet threshold := (largest acknowledged before this frame) − (the late packet) + 1,  capped at 32
time window      += min_rtt / 4 once per round trip after a time-declared loss,        capped at smoothed_rtt
after 16 loss detections with no spurious loss: both return to RFC 9002's values
```

A peer that times its ACKs to fake reordering (threat T09) gains at most the caps: a loss then waits for 17/8 of an
RTT or the probe timeout.

**Unsent numbers.** The sender skips one 1-RTT packet number in 64, chosen from entropy. An ACK naming a number never
sent is PROTOCOL_VIOLATION (RFC 9000 section 21.4): a peer that "optimistically" acknowledges data it never received,
to make the sender go faster, is caught.

**When to acknowledge** (RFC 9000 section 13.2). At once for an ack-eliciting Initial or Handshake packet, a packet out
of order or after a gap, a CE mark, or the second ack-eliciting packet; otherwise within `max_ack_delay`. A packet
that holds only ACK and PADDING owes nothing.

### 3.4 Streams and flow control

**Reassembly** ([S38](../design/network-foundation.md#quic-streams)). Each stream's receive buffer is a window of its
capacity starting at the first unread byte, with **one bit per byte** recording what arrived. Memory is fixed and exact
whatever the peer overlaps, repeats or leaves out. The bitmap costs 1/8 of the receive capacity (and 1/4 of the send
capacity: one bit for acknowledged, one for lost). Refused: a final size that changes (FINAL_SIZE_ERROR), data past
the window (FLOW_CONTROL_ERROR), and different bytes at an offset already held (PROTOCOL_VIOLATION).

**Sending.** Data stays in the send buffer until acknowledged. The next chunk is the lowest *lost* run first, then
fresh data within the flow-control limit. The FIN (end of stream) rides with the last byte or alone.

**Flow control** (RFC 9000 sections 4.1 and 4.2) works at two levels, each stream and the whole connection. The
receiver grants a byte limit; the sender may not pass it. A **window update** (MAX_STREAM_DATA, MAX_DATA) goes once
half the window has been read, when the peer says it is blocked, or when the last update was lost.

**Auto-tuning.** How large must a window be? To keep a path full, the window must hold the **bandwidth-delay
product**: `bandwidth × RTT`. For example (arithmetic, not a measurement), 100 Mbit/s over 50 ms is
`12.5 MB/s × 0.05 s = 625 KB` in flight. A fixed window is either too small (the sender stalls each RTT) or wastes
memory. Cerid doubles a window, up to its buffer, whenever updates come within two smoothed RTTs (the reader is fast);
the design credits Chromium's QUIC flow controller for the idea. An update resent only because the last one was lost
goes again unchanged and tunes nothing (a lost packet says nothing about the reader).

**Stream limits** ([S40](../design/network-foundation.md#quic-stream-set)). The peer may open as many streams of a kind
as this side has slots, so its implicit opens (a frame for stream 8 opens 0 to 8) always find one. MAX_STREAMS grants
more only as slots free, and a slot frees only when both directions are finished *and the application has read its
events*: a peer can never outrun an application that does not read.

**Reliable reset** (draft-ietf-quic-reliable-stream-reset-11, frame 0x24, parameter 0x1d). A normal RESET_STREAM throws
away everything unsent. RESET_STREAM_AT keeps the first *reliable size* bytes, which WebTransport needs so a stream's
header (which names its session) always arrives. In Cerid's machinery it is a FIN at a smaller offset: the kept bytes
are sent and resent like data, the frame waits until they have all gone once, and a second call can only lower the
size.

**Priority.** Streams are scheduled by RFC 9218 urgency (0 first, 3 by default), round robin within one urgency.

### 3.5 Datagrams

([S41](../design/network-foundation.md#quic-datagrams)) A DATAGRAM frame (RFC 9221) is ack-eliciting and congestion
controlled, but never resent and never flow controlled. The largest payload is fixed when the peer's parameters
arrive, for the **worst-case** short header (a 20-byte connection ID and a 4-byte packet number) and the 16-byte tag:

```
largest payload = the largest p such that a DATAGRAM frame of p bytes with its Length fits
                  min(peer's max_datagram_frame_size,
                      min(this side's max_datagram_bytes, peer's max_udp_payload_size) − worst short header − tag)
```

so a later, longer connection ID can never strand a queued datagram. Inside a 1-RTT packet the order is the
connection's own frames, then datagrams, then stream data: latency first. An outcome (Acked, Lost, Expired) is loss
detection's verdict: on a path that reorders, a datagram reported Lost may still arrive.

### 3.6 The amplification limit, Retry and address tokens

**Amplification limit** (RFC 9000 section 8). Until a server knows the client's address is real, it sends at most
**three times** the bytes it received from it. A client's first datagram must be at least 1200 bytes (section 14.1),
so the server can answer it. Stateless answers (Retry, Version Negotiation) go only to datagrams of at least 1200
bytes and are at most 553 and 129 bytes (S34), well inside the limit.

**Retry** (section 17.2.5). Under load, the server answers a first Initial with a Retry packet carrying a token, and
keeps no state. A client that really is at that address sends its Initial again with the token, proving it received
the Retry. The token is sealed under a rotating fleet key and bound to the version, address, port and IDs (NET.2,
S13). An invalid token is answered with one Initial closing with INVALID_TOKEN, so the client fails in one round trip
instead of a handshake timeout ([S43](../design/network-foundation.md#quic-retry)).

**NEW_TOKEN.** After a connection, the server gives the client a single-use token for its next connection, which then
skips the Retry.

**Admission stages** S0 to S3 ([S34](../design/network-foundation.md#ingress)): S0 receives a batch into fixed slots;
S1 reads the header without trusting anything protected; S2 looks up the connection ID; S3 admits a new handshake by
the rates of table 1.2, sending unproven addresses over their rate to Retry. Prefixes share a fixed array of gates
chosen by a keyed hash: no state exists per source before validation, and a collision only makes the limit stricter.

### 3.7 Stateless reset

([S42](../design/network-foundation.md#quic-connection-ids)) A server that lost a connection's state (it restarted)
cannot decrypt the client's packets. It answers with a **stateless reset**: a packet that looks like any short-header
packet and ends in a 16-byte token the client was given earlier. The token is
`first 16 bytes of HMAC-SHA256(fleet key, connection ID)`, so any server of the fleet can derive it from a packet alone.

Sizes are chosen so that two servers resetting each other cannot loop:

```
a reset is at least 21 bytes (5 bytes of header room + the 16-byte token)
a trigger of 43 bytes or fewer gets a reset one byte shorter; a longer one gets a random length from 43 to 128,
always shorter than the trigger
every datagram this side sends is at least (connection ID length + 22) bytes, so a reset of it can look like a packet
```

Detection compares in constant time against the tokens of every peer ID that is used and not retired, only when the
datagram's first packet does not open.

### 3.8 Versions, negotiation and GREASE

**GREASE** ("Generate Random Extensions And Sustain Extensibility", RFC 8701's practice) sends reserved, meaningless
values in every build, so a peer or middlebox that chokes on unknown values breaks *in testing*, not after a protocol
update. In 4.2a: a reserved transport parameter (`31 · N + 27`) and a reserved version in every parameter write, a
reserved version at a random place in Version Negotiation, reserved SETTINGS, frame and stream types in HTTP/3, and
the **QUIC bit** cleared at random toward a peer that sent `grease_quic_bit` (RFC 9287). The QUIC bit is set by
default so QUIC can share a port with other UDP protocols (RFC 7983); greasing it keeps that from ossifying.

**Version Negotiation** (RFC 9000 section 6). A server that does not run the client's version answers with the
versions it runs. The client starts again in one of them; RFC 9368's downgrade check then confirms, inside the
authenticated handshake, that an attacker did not forge the list.

**Compatible version negotiation** (RFC 9368, [S44](../design/network-foundation.md#quic-version-switch)). Versions 1
and 2 can convert each other's first flight, so a server can switch versions without a round trip. The choice must be
made while the TLS stack reads the ClientHello, before any CRYPTO frame is sent, because RFC 9369 section 4.1 requires
every CRYPTO frame in the negotiated version. A ticket resumes only in the version it was issued under (RFC 9369
section 5).

### 3.9 TLS 1.3, keys and key updates

TLS 1.3 (RFC 9846, the successor of RFC 8446) is NET.2's: hybrid post-quantum key exchange (X25519MLKEM768 by default),
tickets, 0-RTT. QUIC carries TLS's handshake messages in CRYPTO frames (RFC 9001) and takes its secrets for packet
protection; 4.2a does not reimplement TLS.

**Keys are discarded on time** (RFC 9001 section 4.9): a client's Initial keys with its first Handshake packet, a
server's with the first Handshake packet it opens, Handshake keys at confirmation.

**Key updates** (RFC 9001 section 6). An AEAD key may protect only so many packets before its security weakens (the
limits per AEAD are in section 6.6). So the 1-RTT key rotates: the *key phase* bit flips and both ends derive the next
key. Cerid derives the next receive key ahead, so an update costs no timing difference; starts an update itself at
three quarters of the key's limit or of `key_update_interval`; and refuses to send once a key reaches its limit
without updating (AEAD_LIMIT_REACHED). An update can only start after a packet of the current phase was acknowledged,
which a real peer may delay (trap 5.4).

**0-RTT** (RFC 9001 section 4.6, RFC 9000 section 7.4.1; [S45](../design/network-foundation.md#quic-early)). A resumed
client sends data before the handshake finishes, under limits its ticket remembers. Such data can be **replayed** by an
attacker, so every early byte is marked and the application may run only what its schema declares idempotent.
A server that lowers a remembered limit refuses the 0-RTT; the client then resets its streams and reports its early
datagrams Lost.

### 3.10 QMux and the raw UDP path: one stream machine, three carriers

**QMux** (draft-ietf-quic-qmux-02, [S52](../design/network-foundation.md#qmux)). The same `QuicStreamSet` and
`QuicDatagrams` ride as QUIC version 1 frames inside **records** inside TLS 1.3 over TCP. A record is a size and
whole frames, at most 16,382 bytes. No ACK frames, no loss detection: TCP already resends, so a frame counts as
delivered when TLS takes it. What the draft does not say, S52 decides: a record of no frames is PROTOCOL_VIOLATION, a
repeated STREAM range is refused (`in_order`), tickets are per path.

**The raw UDP path** ([S55](../design/network-foundation.md#raw-path)). For LAN peers and headless processes that
already share a session: a 32-byte secret (a TLS exporter of the session, or a launcher's random bytes) is expanded
with HKDF into each side's traffic secret, and the connection starts directly in 1-RTT packets with no certificate and
no Initial or Handshake packet. Everything after the start (recovery, streams, key updates, close) is the ordinary
connection's code. Holding the secret is the authentication; there is no plaintext mode in any build.

### 3.11 HTTP/3, QPACK and WebTransport

**HTTP/3** (RFC 9114, [S57](../design/network-foundation.md#http3)) maps HTTP onto QUIC streams: each request is one
bidirectional stream of frames (HEADERS, DATA); each side also opens a control stream (SETTINGS, GOAWAY) and two QPACK
streams. **Extended CONNECT** (RFC 9220) turns a request stream into a tunnel with a `:protocol` such as
`websocket` or `webtransport-h3`. **HTTP datagrams** (RFC 9297) carry a *quarter stream ID* (the request's stream ID
divided by 4) to say which tunnel they belong to. **Capsules** (RFC 9297 section 3) are type-length-value records
inside a tunnel's DATA.

**Prefix integers** (RFC 7541 section 5.1, shared by HPACK and QPACK). An integer starts in the low N bits of a byte.
If it is below `2^N − 1` it fits; otherwise those bits are all ones and the rest follows in 7-bit groups, low first,
the top bit of each byte saying "more follows". Cerid reads up to 2^62 − 1 in at most ten continuation bytes.

**Huffman coding** (RFC 7541 appendix B). Header strings may be coded with a fixed canonical Huffman code over 257
symbols (256 bytes and an end-of-string). Cerid builds the code at compile time from the 257 code lengths, uses it
only when strictly shorter, and refuses padding longer than seven bits, not all ones, or an EOS inside a string.

**The dynamic table.** Both HPACK and QPACK remember recent header fields so a repeat costs an index instead of the
text. An entry's size is `name length + value length + 32` (RFC 7541 section 4.1). Adding an entry past the capacity
evicts the oldest.

**QPACK's twist** (RFC 9204). HTTP/3 streams arrive in any order, so a header section may reference a table entry
whose insert (on the encoder stream) has not arrived: that stream is **blocked** until it does, up to the promised
number of blocked streams. An entry may be evicted only once its insert is acknowledged and no outstanding section
references it (section 2.1.1). Cerid's encoder inserts a field the *second* time it sees it (a 64-slot filter), and
duplicates a field still in use that is in the oldest quarter of the table
([S56](../design/network-foundation.md#qpack)).

**Sensitive fields.** `authorization`, `proxy-authorization`, `cookie` and `set-cookie` are never put in a table and
carry the never-indexed flag, so an attacker who can make the client send requests cannot guess a cookie by watching
whether the compressed size shrinks (RFC 9204 section 7.1, RFC 7541 section 7.1).

**WebTransport** (draft-ietf-webtrans-http3-17, [S59](../design/network-foundation.md#webtransport)). A browser API for
streams and datagrams to a server. A session is an extended CONNECT; its streams start with a header: unidirectional
type 0x54, or 0x41 as the first value of a bidirectional stream, followed by the session ID. Three spellings exist:
`webtransport-h3` (draft -17, SETTINGS_WT_ENABLED 0x20b8933b, with flow control), the same wire under draft -16's
codepoint 0x2c7cf000 (Cerid sends and reads both, as section 7.1 allows), and the legacy `webtransport` (SETTINGS
0xc671706a, no flow control) that browsers still send. Application error codes (32 bits) map onto HTTP/3 codes while
skipping HTTP/3's reserved points `0x1f · N + 0x21`:

```
wire code = 0x52e4a40fa8db + a + floor(a / 0x1e)        (a = the application's code; http3.hpp wt_http_code)
```

Session keys for the application come from the TLS exporter with the label `EXPORTER-WebTransport` (section 4.8).
Origins are checked before the application hears of a request; an empty list refuses every browser.

### 3.12 HTTP/2, HPACK and WebSocket

**HTTP/2** (RFC 9113, [S63](../design/network-foundation.md#http2)) multiplexes streams over one TCP connection with
9-byte frame headers and its own flow control: windows start at 65,535 bytes, grow by WINDOW_UPDATE, never pass
2^31 − 1, and every byte of a DATA frame counts, padding included. Its known attacks are why the two flood gates of
table 1.13 exist: **rapid reset** (open and reset streams faster than a server can clean up), resets the server is
tricked into sending ("MadeYouReset"), floods of SETTINGS, PING, PRIORITY or empty frames (CVE-2019-9518), tiny
WINDOW_UPDATEs that dribble data (CVE-2019-9511), and a peer that sends PINGs and never reads the answers
(CVE-2019-9517, which is why Cerid stops reading when its output has no room for an answer).

**HPACK** (RFC 7541, [S62](../design/network-foundation.md#hpack)) has no acknowledgements: blocks are decoded in
connection order, so every line changes the table at once. A table size limit binds only once the peer acknowledged
the SETTINGS that set it.

**WebSocket** (RFC 6455, [S61](../design/network-foundation.md#websocket)) starts with an HTTP/1.1 GET with
`Upgrade: websocket` and a random key; the server proves it understood by answering
`Sec-WebSocket-Accept = base64(SHA-1(key ++ "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))` (section 4.2.2; SHA-1 protects
nothing here, TLS beneath does). Messages are frames with a 2 to 14 byte header; every client frame is XOR-masked with
a fresh 4-byte key (section 10.3, against cache poisoning of intermediaries). In Cerid a WebSocket is a QMux path:
**one QMux record per Binary message**, the framing the `qmux` crate's WebSocket transport uses (the draft names no
WebSocket binding).

**RFC 8441 and RFC 9220** open a WebSocket inside one stream of an HTTP/2 or HTTP/3 connection with an extended
CONNECT whose `:protocol` is `websocket`. A refused version is 400 with `sec-websocket-version: 13`; a request in 0-RTT
is 425 Too Early (RFC 8470). ([S64](../design/network-foundation.md#websocket-tunnels))

### 3.13 Path behaviour: congestion, pacing, ECN and MTU (NET-PATH, still open)

These landed on 2026-11-10 under NET-PATH and are in every connection; the row is Partial (BBRv3, PMTU against real
peers and migration remain). Sources: [S66](../design/network-foundation.md#congestion),
[S67](../design/network-foundation.md#ecn), [S68](../design/network-foundation.md#pmtu).

**Initial window** (RFC 9002 section 7.2): `min(10 · max_datagram, max(14720, 2 · max_datagram))` bytes.

**CUBIC** (RFC 9438). After a loss the window is cut to `β · W` with `β = 0.7` and the window before the cut is
remembered as `W_max`. Growth then follows a cubic curve in the time `t` since the cut:

```
W_cubic(t) = C · (t − K)^3 + W_max,      C = 0.4,     K = cube_root(W_max · (1 − β) / C)
```

It grows fast while far below `W_max`, slows to a plateau near it (probing gently where loss happened before), then
accelerates past it. A "Reno-friendly" estimate with `α = 3(1 − β)/(1 + β) = 9/17` keeps CUBIC from being slower than
classic Reno on short paths; fast convergence sets `W_max` to `(1 + β)/2 = 0.85` of the window when losses repeat
(section 4.7). Cerid computes all of it **in integers**: bytes, time in 2^−10 s steps, an exact integer cube root and
128-bit products, so every platform computes the same window and NET-SIM replays match. Within one byte it agrees with
the floating-point formula (session [net-path-congestion](../sessions/2026-11-10-net-path-congestion.md)). A period
whose losses all prove spurious is undone (section 4.9); an ECN mark or persistent congestion is never undone.

**HyStart++** (RFC 9406) leaves slow start before the first loss when the RTT rises: rounds counted by Application
packet numbers, eight RTT samples per round, a threshold of `clamp(min_rtt / 8, 4 ms, 16 ms)`, a conservative phase
growing at a quarter of slow start's rate, and congestion avoidance after five such rounds (constants in
[congestion.cpp](../../engine/foundation/net/src/congestion.cpp)).

**Pacing** (RFC 9002 section 7.7). Sending a whole window at once makes bursts that overflow router queues. The pacer
spreads it: `rate = pacing_gain · cwnd / smoothed_rtt` (125%: 1.25 windows per RTT), as exact credit in
byte-nanoseconds per second, refilled up to a burst of the initial window.

**ECN** (Explicit Congestion Notification, RFC 3168). Two bits of the IP header: Not-ECT `00`, ECT(1) `01`, ECT(0)
`10`, CE `11` (congestion experienced). A router about to drop a packet may instead mark it CE; the receiver counts
marks per space in its ACK frames, and the sender treats a rise in CE as a loss without losing anything. Because many
middleboxes erase or rewrite the bits, the sender first **validates** the path (RFC 9000 section 13.4.2 and A.4):
marks its first ten packets (or three probe timeouts), then checks that the peer's counts exist, never fall, never
exceed what was sent and rise by at least what was newly acknowledged. A failure is for the path's life.

**Path MTU discovery** (DPLPMTUD, RFC 8899). Every path starts at QUIC's 1200 bytes. After confirmation the sender
sends probes (a PING padded to exactly the size probed), first the ceiling (1472 bytes by default), then bisects
between the largest size acknowledged and the smallest that failed: for 1200 to 1472, at most `ceil(log2(272)) = 9`
steps (arithmetic). A size fails after three probe losses. The socket forbids fragmentation, so a probe that is too
large is simply lost rather than silently split. A **black hole** (a path that stops carrying large datagrams without
any report) is suspected when a loss detection's lost packets are all over 1200 bytes and all sent after the newest
large packet acknowledged; three suspect detections fall back to 1200.

### 3.14 Padding: what a size says

NET.3's `padded_size(size, block, room)` ([frame.hpp](../../engine/foundation/net/include/crd/net/frame.hpp),
[S30](../design/network-foundation.md#padding)) rounds a carrier up to the next multiple of the channel's block:

```
padded = size                                   if block == 0 or size is a multiple of block
       = size + block − (size mod block)         if that is below room
       = room                                    otherwise (never shrinks: at most size if size >= room)
```

A path that controls its packets (QUIC PADDING frames, QMux records, the raw path) pads each packet with the largest
block among the channels whose frames it holds. WebTransport cannot (the browser builds the packets), so there a
channel pads in band with a padding route on datagrams and message 0 on streams.

---

## 4. The full assembly

### 4.1 The map

```
                        Application: game session, project session, editor tools (4.2d and above)
                                               |
                        +----------------------+-----------------------+
                        |        ChannelLink (S60)  one per session    |   NET.3 frames: Route, FrameHeader,
                        |  StreamAssembler / DatagramAssembler,        |   messages, admission (Admission, quotas)
                        |  Admission, padding, urgency                 |
                        +----------+------------------------+----------+
                                   |                        |
                 ChannelCarrier: a QuicStreamSet +          | ChannelCarrier: one WebTransport session
                 QuicDatagrams (QUIC, QMux, raw path,       |
                 WebSocket, WebSocket tunnels)              |
                                   |                        |
   +-------------------------------+-----+       +----------+-------------+
   | QuicStreamSet (S40) QuicDatagrams   |       | WebTransport (S59)      |
   | (S41): the ONE stream and datagram  |<------| Http3Connection (S57)   |
   | machine                             |       | + QPACK (S56)           |
   +----+-------------------+------------+       +----------+-------------+
        |                   |                               |
        |                   |  QmuxConnection (S52)         | (ALPN "h3")
        |                   |  records | WebSocket messages |
        |                   |  | HTTP/2 bearer (S65)        |
        |                   |  inside TLS 1.3 (crd-crypto)  |
        |                   |           |                   |
   QuicConnection (S39)     |     QmuxHost (S53)            |
   packets S36, recovery    |     TcpPoller (epoll/WSAPoll) |
   S37, buffers S38, IDs    |     TcpSocket / TcpListener   |
   S42, Retry S43, version  |     (S51)                     |
   S44, 0-RTT S45, keyed    |                               |
   start S55 ---------------+-------------------------------+
        |
   QuicServer (S46) / QuicClient (S47) on QuicHost: pool, handles, deadline heap, SendQueue, reset index (S48)
        |
   IoLoop (S34): poll() turns, TransportClock      Ingress S1-S3 + ConnectionIdTable + HandshakeBuckets
        |
   DatagramSocket -> UdpSocket (S33): recvmmsg/sendmmsg batches, per-datagram ECN, no fragmentation
```

`WebSocketTunnels` (S64) sits beside WebTransport: over an `Http2Connection` or an `Http3Connection`, each tunnel is a
`QmuxConnection` with no TLS of its own, so it ends in the same `QuicStreamSet`.

### 4.2 The receive path, step by step (QUIC)

1. **Socket (S0).** `UdpSocket::receive` fills caller-owned slots (`batch` of `slot_bytes`), marks truncated
   datagrams, reports each one's ECN codepoint. No allocation. Data: `ReceivedDatagram` (bytes, peer `Endpoint`,
   codepoint).
2. **Ingress (S1 to S3).** `Ingress` reads form, version, IDs, token and length without decrypting. Unknown version:
   Version Negotiation (if the datagram is at least 1200 bytes and the answer rate allows). Known ID: route. New
   Initial: admit, Retry or refuse by `HandshakeRules`. 0-RTT for no connection: `Early`. A short header for no
   connection: offered to the host as a possible stateless reset, else answered with one. Data: `IngressDecision`.
3. **Loop.** `IoLoop::poll()` calls the `IngressHandler` (the host): `on_route`, `on_admit`, `on_early`, `on_unrouted`,
   then `on_timers`. It returns when it next needs to run.
4. **Host.** `QuicServer`/`QuicClient` (both on `QuicHost`) finds the connection slot, checks the datagram comes from
   the connection's own address (another address is migration, NET-PATH's, dropped for now), and calls the
   connection's `receive`.
5. **Packets (S36).** `QuicDatagramPackets` walks coalesced packets, stopping at a foreign destination ID.
   `QuicPacketProtection::open` removes header protection, decodes the packet number, drops duplicates (a 256-bit
   window per space) and only then runs the AEAD. A packet whose keys may still come goes to the held ring.
6. **Frames (S35).** `QuicFrame` views point into the decrypted payload. Each frame is checked for placement (table 3
   of RFC 9000) and dispatched: ACK to recovery, CRYPTO to the level's receive buffer and then to TLS, STREAM and
   credit frames to the stream set, DATAGRAM to the datagram ring, connection ID frames to S42's code.
7. **Recovery (S37).** The ACK updates RTT, declares losses (losses first), and emits per-packet events. The frame
   ledger settles them: acknowledged records are done, lost CRYPTO, stream and control records are resent. NET-PATH's
   controller reads the same events.
8. **Streams and datagrams (S40, S41).** Stream bytes land by offset in their buffer; the application reads them
   zero-copy with `peek()` and gives them back with `consume()`. Events (opened, readable, writable, reset, stop asked,
   all sent) wait in a per-slot queue that cannot overflow.
9. **Application.** The host calls `on_service(handle, connection, now)`; a `ChannelLink` reads NET.3 frames off the
   streams and datagrams and hands out messages.

### 4.3 The send path

1. The host serves due connections from a binary min-heap of deadlines (`DeadlineHeap`), once per turn.
2. `QuicConnection::send(out, now)` is called until it returns 0. It builds one datagram that may coalesce Initial,
   Handshake and 1-RTT packets, each with the ACK its space owes. Inside a 1-RTT packet: an owed ACK, the
   connection's own frames and CRYPTO, then datagrams, then stream frames by urgency (`QuicStreamSet::next_frame()`
   hands one frame and the record that will settle it).
3. `QuicPacketWriter` pads to the largest of the sample floor, the 1200-byte Initial floor and `padded_size()`; writes
   the Length in its shortest form; seals with the AEAD and protects the header. NET-PATH's congestion window and
   pacer decide whether data may go now; ACK-only packets, probes and handshake CRYPTO data are never held by them.
4. The host queues the datagram in its `SendQueue`, stamped with the connection's ECN codepoint. A busy socket keeps it
   queued; the connection is tried again within `send_retry_ns`.
5. `UdpSocket::send` writes the batch (`sendmmsg` on Linux), stopping at the first datagram that cannot leave and
   saying why (`Busy` is back-pressure, not an error).

### 4.4 The stream path (TCP and TLS)

`TcpListener` accepts; `TcpPoller` reports readiness (level-triggered: what is left is reported again). `QmuxHost`
charges each accepted connection to the same `HandshakeBuckets` as a validated address (TCP's handshake proved it),
takes a slot, and runs a `QmuxConnection` over its TLS session. After the TLS handshake, ALPN decides what the
connection is: a QMux identifier (records), `http/1.1` with a `WebSocketConfig` (an opening handshake, then one record
per Binary message), or `h2` with an `Http2Config` (an HTTP/2 bearer whose WebSocket tunnels each become a host
connection with its own handle). One port and one server context serve all three. Writes show TLS's output in place
(`outgoing()`) and remove only what the socket took (`take()`), so a partial write needs no extra buffer.

### 4.5 The browser paths

A QUIC connection whose ALPN is `h3` goes to an `Http3Connection`; Cerid's own ALPNs go to the session layer, on the
same socket and host. `WebTransport` takes over `Http3Connection::poll()`, keeps its sessions' events and passes on the
rest; `WebSocketTunnels` does the same for RFC 9220 tunnels on HTTP/3 and RFC 8441 tunnels on HTTP/2. Browser
constraints are listed in [Browser constraints](../design/network-foundation.md#browsers): no raw sockets, no ECN or
socket options, Origin checked on every session; each browser's exact settings are confirmed under WEB.5.

### 4.6 The raw path

`QuicClient::open_keyed` and `QuicServer::open_keyed` start a `QuicConnection` from a `QuicKeyedStart` (the secret,
IDs derived from it). It shares the socket, the ID table and the host with TLS connections. A server opened with no
peer takes the address of the first packet that opens and sends nothing before it.

### 4.7 Channels on a path

`ChannelLink` ([S60](../design/network-foundation.md#channel-link)) is the one reader and writer of NET.3's frames:

| Channel kind | Carrier | Why |
|---|---|---|
| Reliable ordered | One unidirectional stream, opened by its first write | Order inside the channel, independent of other channels |
| Reliable unordered | One stream per message, ended | No message waits behind another's loss |
| Unreliable | Datagrams the caller assembled (Whole, Listed or Fragment) | Late data is useless; nothing is resent |

A channel's `priority` becomes its stream's urgency (the lowest priorities share urgency 7), so control traffic goes
first on every path. A reader that the rate gate or quota stops (`Wait`) leaves bytes unread, so flow control holds the
sender back instead of a buffer growing. Broken framing fails the link once; a refused stream stops alone.

### 4.8 Who owns what

| Concern | One owner |
|---|---|
| Addresses and datagram sockets | `endpoint.hpp`, `udp.hpp` |
| Admission S1 to S3 and stateless answers | `ingress.hpp` |
| Every QUIC frame and transport parameter on the wire | `quic_frame.hpp`, `quic_transport_parameters.hpp` |
| Packet protection, key discards and key phases | `quic_packet.hpp` |
| ACKs, RTT, loss and probes | `quic_recovery.hpp` |
| Ordered bytes, credit and reassembly (streams and CRYPTO data) | `quic_stream.hpp` |
| Stream states, limits and scheduling, on every path | `quic_stream_set.hpp` |
| Datagrams, on every path | `quic_datagram.hpp` |
| A connection's lifetime | `quic_connection.hpp` |
| Pools, handles, deadlines, the send queue | `quic_host.hpp`, `deadline_heap.hpp`, `host_handle.hpp` |
| Huffman, prefix integers and the field table | `http_fields.hpp` |
| HTTP message rules for HTTP/2 and HTTP/3 | `http_message.hpp` |
| WebSocket framing and handshake policy for every HTTP version | `websocket.hpp` |

---

## 5. The traps

Each scar names its symptom, why it happened and what holds it now. Sources are the session logs and fix commits.

### 5.1 Sockets, the loop and the ingress

- **`::` after eight groups was accepted as an IPv6 address.** Symptom: the first fuzz run of `crd-fuzz-net-endpoint`
  produced an address with nine groups' worth of text. Why: gap position 8 doubled as the "no gap" sentinel. Now a
  flag, with a unit case and a named seed ([session](../sessions/2026-11-08-net-4-order-and-udp-sockets.md)).
- **A Windows timed wait ended early.** Symptom: win-arm64 failed "a UDP socket waits until a datagram arrives or its
  time passes": a 30 ms `WSAPoll` returned after 22.7 ms. Why: Windows may end a timed wait a tick early, and a signal
  restarted the whole wait. Now `wait_native` measures what is left and waits again (0351c2d3,
  [session](../sessions/2026-11-09-4-2a-qmux-early.md)).
- **Tests answered decisions whose datagram had died.** Symptom: ARM64 ASan lanes reported stack-use-after-scope in
  three ingress tests, and UBSan a null pointer passed to `memcpy` from an empty span. Why: `answer()` reads the
  classified datagram, which the tests held as a temporary. Now the contract says so and the tests keep it alive
  (f3cd695a, [session](../sessions/2026-11-08-4-2a-retry-tokens.md)).
- **An IPv6 peer's reset was never found.** Why: 5ca261ef added the IPv6 zone to the peer key, which made it 22
  bytes, over the connection table's 20-byte keys, so an IPv6 peer was never inserted and no IPv6 stateless reset
  matched. Found while building the token index; resets are now found by their token, not the address (S48,
  [session](../sessions/2026-11-09-4-2a-reset-index-and-tickets.md)). Lesson: a table with fixed-size keys must refuse
  a key that does not fit loudly, not skip it.
- **A forgery granted a send to port 0.** Symptom: the first raw-path test run sent to an unbound slot. Why: `QuicHost`
  tried to send for a slot with no peer yet. Now it never does
  ([session](../sessions/2026-11-09-4-2a-raw-path.md)).

### 5.2 Packets, frames and the connection

- **GCC's link-time null warning.** Symptom: linux-gcc-release, -relwithdebinfo and -shipping refused to link
  `crd-net-tests` with `-Wnull-dereference` on `frames()[0]`, `connection(handle)->close`, `connection->ping()`,
  `last()->` in `test_http2.cpp` and a site in `fuzz_http3.cpp`. Why: under link-time optimisation GCC inlines across
  files, sees that a pointer-returning helper *can* return null, and warns at each dereference; debug lanes never
  inline that far, so it showed only on the release lanes, often first in hosted CI. Now the tests go through checked
  helpers that require and return references (d3e225d2, 9b2b9de2, 813a6d45; sessions
  [packets](../sessions/2026-11-08-4-2a-quic-packets.md), [tunnels](../sessions/2026-11-10-4-2a-websocket-tunnels.md)).
  Recognise it: a release-only link failure naming a test file; relink the release lane before handover.
- **Timers reported due that nothing served.** Symptom: two source mutations hung the tests. Why: an ACK was owed in
  a space with no keys or no storage, so `next_timer_ns()` said "now" forever and the caller's loop spun. Now an owed
  ACK counts only where `send()` can write it, and the test network fails any timer that stays due
  ([session](../sessions/2026-11-08-4-2a-quic-connection.md)).
- **ACK-only packets filling storage.** Symptom: a peer that acknowledges only ack-eliciting packets made the sender's
  storage fill with ACK-only packets nobody would ever acknowledge. Now every 16th ACK-only packet carries a PING
  (RFC 9000 section 13.2.4).
- **The sent-packet storage stall.** Symptom: Cerid against Cerid, 8 WebTransport streams of 32 KiB over the loopback,
  both ends stalled until the idle timeout. Why: both filled `sent_packets` with data before any ACK arrived, and a
  full space refused ACK-only packets and probes too, so neither end could free the other's entries. Quinn's pacing
  had hidden it. Now eight entries past `sent_packets` are reserved for ACK-only packets and probes
  (`reserved_entries`, `can_send_data`; 62c5fe79,
  [session](../sessions/2026-11-09-4-2a-webtransport-interop.md)).
- **C4324 on ARM64 MSVC.** Symptom: win-arm64 failed on "structure was padded due to alignment" for
  `QuicPacketProtection`, later for a test peer and the fuzz harness's `Pair`. Why: an over-aligned key member. Now
  accepted where it occurs (dc0200fc, 954d098a, 0213dc7b).
- **A packet test sealed version 2 keys under version 1 headers**; found and fixed while building the version switch
  ([session](../sessions/2026-11-08-4-2a-version-switch.md)). A test fixture can be wrong in the same way as the code.
- **The client's downgrade check compared the wrong version.** It compared with the negotiated version; RFC 9368's
  figure 1 compares with the version attempted after Version Negotiation. Before compatible negotiation the two were
  always equal, so no test saw it (same session).

### 5.3 Recovery and flow control

- **A lost window update doubled the window.** Found while building the stream set. Why: a MAX_DATA
  resent because the last one was lost was taken as a new update arriving within two RTTs, which the auto-tuner reads
  as a fast reader. Now it goes again unchanged and tunes nothing (f105a339,
  [session](../sessions/2026-11-08-4-2a-quic-stream-set.md)).
- **A stale STREAMS_BLOCKED.** An `open()` before the handshake met limit 0 and owed STREAMS_BLOCKED(0); the peer's
  parameters then lifted the limit, but the frame still went in the first 1-RTT packet. Now the parameters clear it
  as MAX_STREAMS does (91cd9bc6).
- **Most "lost" datagrams were only late.** Symptom: on a hostile path (10% loss, 20 ms jitter) 1,812 datagrams were
  reported Acked and 5,388 Lost. Why: jitter reorders past RFC 9002's fixed thresholds. Fix: spurious-loss detection
  raising the thresholds (3.3); the same seeds then gave 5,640 Acked and 1,560 Lost
  ([datagrams](../sessions/2026-11-08-4-2a-quic-datagrams.md), [spurious](../sessions/2026-11-08-4-2a-spurious-losses.md)).
  A related fix: a full space dropped every settled entry at its head, losing the late acknowledgements that prove a
  loss spurious; it now frees only the slot it needs (e7ad8110).
- **A reliable reset could pin a stream for ever.** Symptom: a reliable size that stops inside a frame left the
  receiver waiting for bytes that would complete a frame it could never use: a cheap way for a hostile peer to hold
  slots. Now `reset_coming` tells the framing layer, which gives the bytes up; HTTP/3 takes the reset at once. Also:
  a reliable reset's bytes waiting on credit owed no STREAM_DATA_BLOCKED (a deadlock against a peer that raises
  credit only on that signal), and QMux's `on_all_acked()` named the whole stream in a 32-bit length, so a stream past
  4 GiB stopped being acknowledged; a 4 GiB case holds it (48b6021c,
  [session](../sessions/2026-11-09-4-2a-reliable-reset.md)).
- **The "flow-control windows" scar of RFC 8441 interop.** With the `h2` crate's default 65,535-byte windows, a reset
  stream's data sat behind 256 KiB of other data until its RESET_STREAM went alone; the `qmux` crate opens a stream
  whose first frame is RESET_STREAM without handing it to `accept_uni()`, so its reader waited for ever. That is
  third-party behaviour (TP-14, left be); the crate peer now runs with hyper's windows (2 MiB per stream, 5 MiB per
  connection) and `crd-quic-peer`'s tunnel stream with 2 MiB buffers
  ([h2](../sessions/2026-11-10-4-2a-websocket-h2-interop.md), [h3](../sessions/2026-11-10-4-2a-websocket-h3-interop.md)).
  On the Cerid side, the HTTP/2 interop's teeth show why the connection window matters: leaving it uncharged makes
  nghttp2 stop at its window and the exchange never completes
  ([session](../sessions/2026-11-09-4-2a-http2-interop.md)).

### 5.4 Keys, Retry, versions and 0-RTT

- **`key_update_interval` closed real connections.** Symptom: against ngtcp2, the four "Cerid updates" cases closed
  with code 15 (AEAD_LIMIT_REACHED) after about 140 packets. Why: the interval was also the key's hard limit, and an
  update can start only once a packet of the current phase is acknowledged; a real peer's flights exceed a quarter of
  an interval in flight, the virtual-path tests never did. Now the interval only starts updates; the hard cap is
  `limit_sealed_packets` (f6dc75d4, [session](../sessions/2026-11-09-4-2a-ngtcp2-interop.md)). Lesson: a soft trigger
  and a hard limit must be two numbers.
- **A Retry stranded 0-RTT stream frames**, and **a 0-RTT packet behind a full Initial left a datagram under 1200
  bytes** (which the server must drop). Now discarded stream frames are resent like lost ones, and an earlier packet
  pads when the last leaves too little room ([session](../sessions/2026-11-08-4-2a-early-data.md)).
- **The raw path's reordered hello.** Symptom: libFuzzer at once. Why: a keyed connection's transport parameters are a
  packet like any other, so a reordered NEW_CONNECTION_ID could arrive first and retire sequence 0, and the hello's
  reset token then landed on whatever sat at index 0. Now it goes to sequence 0 or nowhere
  ([session](../sessions/2026-11-09-4-2a-raw-path.md)).

### 5.5 QMux and the stream host

- **Bytes that arrived with the close were lost.** Symptom: the first run against the `qmux` crate: the application
  never read a peer's last stream bytes. Why: they came in the same read as its CONNECTION_CLOSE, the connection went
  Closed, and `QmuxHost` did not serve a Closed connection. Now a connection that ended on what it read is served once
  more (56f1ad08). QUIC has no such gap: a peer's close leaves it Draining, still served
  ([session](../sessions/2026-11-09-4-2a-qmux-interop.md)).
- **A failed write left received bytes unread.** Symptom: a peer that reset behind its last bytes read as a plain
  Reset. Now `read_left` reads what the socket still holds first, a bounded number of reads (36911598,
  [session](../sessions/2026-11-09-4-2a-websocket.md)).
- **QMux 0-RTT stopped for good.** A client whose nothing-to-send turn was taken for a full early-data allowance never
  sent early again; found by source mutation ([session](../sessions/2026-11-09-4-2a-qmux-early.md)).
- **A test that restarted its pattern after a partial send** failed `[tcp]` intermittently; the test now continues
  from the offset ([session](../sessions/2026-11-09-4-2a-qmux.md)).
- **`padding_block` never reached QUIC.** `QuicConnection` passed a block of zero to every packet, so NET.3's padding
  never applied on QUIC; found while building QMux. Now `QuicConnectionConfig::padding_block` reaches the writer.

### 5.6 HPACK, QPACK and HTTP/3

- **QPACK evicted an entry before its acknowledgement.** Random traffic showed it; a later section could then exceed
  what the decoder's wrap can read (RFC 9204 section 2.1.1). Also found: a Huffman value after a plain name decoded
  past the scratch buffer (libFuzzer with UBSan, a real overflow), and the first duplicate design left a section
  referencing the original it had just evicted ([session](../sessions/2026-11-09-4-2a-qpack.md)).
- **A narrowing only one lane sees.** linux-gcc-asan refused a `usize` to `u32` narrowing in `QpackTable::init` that no
  other lane flagged ([session](../sessions/2026-11-09-4-2a-nghttp3-interop.md)).
- **nghttp3 deletes a blocked section.** A QUIC-level stream close by ngtcp2 deletes nghttp3's stream with its
  QPACK-blocked section, so a response arriving ahead of its table insert vanished. Third-party (TP-13); the C peer
  holds the close until the message ends (same session).
- **BoringSSL's default verification set.** A quiche client refused Cerid's Ed25519 identity with handshake_failure
  until the peer named Ed25519: BoringSSL leaves it out by default
  ([session](../sessions/2026-11-09-4-2a-quiche-interop.md)).
- **A 1 MiB stack overflow.** win-arm64-debug overflowed its 1 MiB stack in a WebTransport test: `H3Wire` held 16 KiB
  inline and the test made dozens as temporaries. Now its buffer is on the heap
  ([session](../sessions/2026-11-09-4-2a-websocket.md)).

### 5.7 WebTransport

- **A datagram before its request's HEADERS aborted the request** with H3_DATAGRAM_ERROR; the refusal now waits until
  the request was read. **A reset of the direction the peer opened** asked for a reliable size naming a header that
  direction never carried, and QUIC refused it silently; only the opener's direction keeps a header
  ([classifier](../sessions/2026-11-09-4-2a-webtransport-classifier.md)).
- **STOP_SENDING overtakes what it travels with.** It ended a session with no status when it arrived before the
  response or the close capsule; it lowered a reliable reset to nothing without telling anyone; and a pool slot reused
  by a later session was charged by an old stream's late events
  ([sessions](../sessions/2026-11-09-4-2a-webtransport-sessions.md)).
- **Two codepoints for one setting.** webtransport-go 0.13 speaks only draft -16's SETTINGS_WT_ENABLED (0x2c7cf000);
  -17 changed only that codepoint. Without sending both, a Go client refuses the server and a Cerid client never opens
  a session on a Go server. quic-go's HTTP/3 layer also keeps only 32 datagrams for a session nobody reads yet, so the
  datagram case is capped at 32 against Go ([session](../sessions/2026-11-09-4-2a-webtransport-go-interop.md)).

### 5.8 HTTP/2 and WebSocket tunnels

- **Frames on closed HTTP/2 streams.** Symptom: h2spec 5.1/12 failed. Why: a frame other than WINDOW_UPDATE,
  RST_STREAM or PRIORITY on a stream both ends had finished was answered as a stream error; RFC 9113 section 5.1 asks
  for a connection error STREAM_CLOSED. Now the closed ring remembers *how* each stream closed: this side's reset
  (drop), the peer's reset (stream error), both finished (connection error) (0a90f827). And a stream above this side's
  GOAWAY is remembered as reset, so what follows on it is dropped (a73a7ba1)
  ([session](../sessions/2026-11-09-4-2a-http2-interop.md)).
- **The PRIORITY race.** Symptom: h2spec 6.3/2 failed in the third of the first runs only. Why: a PRIORITY of the wrong
  size arriving after the response had finished was dropped as "closed stream"; sections 6.3 and 6.9 make a bad
  PRIORITY, a self-dependency and a zero WINDOW_UPDATE stream errors in any state. Now a closed stream answers
  RST_STREAM, charged to the `resets` gate. Recognise it: a conformance case that fails only sometimes is a timing
  race between your response finishing and the test's frame (same session).
- **STOP_SENDING ended a whole tunnel.** Symptom: against the `h3` crate over Quinn, a 400 refusal read as "refused with
  no status", and intermittently an open tunnel whose peer stopped reading after its Close ended Reset with that Close
  still in flight. Why: `WebSocketTunnels` took STOP_SENDING as the end of both directions, and Quinn sends the frame
  ahead of stream data it still owes. RFC 9000 section 3.5: STOP_SENDING ends only this side's sending. Now the peer's
  direction is read to its end (5d74b727, [session](../sessions/2026-11-10-4-2a-websocket-h3-interop.md)).
- **Five defects of HTTP/2 over TLS** (fixed with 126eebb0, [session](../sessions/2026-11-10-4-2a-h2-tls-interop.md)): a GOAWAY and
  close_notify arriving together ended the connection as TlsFailed (HTTP/2 output was flushed after this side had
  stopped sending); a tunnel was not served when its bytes left and its streams had room; a tunnel event waiting for
  `poll()` did not make `next_deadline_ns()` 0, so the host slept on a timer; `QmuxHost::wake()` did not reach a
  tunnel; and an HTTP/2 writer that blocked again inside its Writable event was told Writable again, for ever (100%
  CPU). Lesson: an event-driven object must turn every reason to run into an event or a zero deadline.
- **The bearer's last service came before its tunnels' reports** (the host fuzz oracle found it); tunnels are now
  served first ([session](../sessions/2026-11-10-4-2a-h2-bearer.md)).

### 5.9 NET-PATH scars that changed 4.2a's connection

- **128 ACK-only datagrams in one instant.** Capping a datagram to the window's spare room let only ACK frames through,
  packet after packet. Data now waits until a whole datagram fits.
- **Handshake data behind 1-RTT data.** A peer still handshaking reads no 1-RTT packet, so 1-RTT data filling the
  window was never acknowledged or declared lost, and a lost Handshake flight held behind it wedged both ends until
  idle. The handshake's CRYPTO data is no longer held by the window or the pacer
  ([session](../sessions/2026-11-10-net-path-congestion.md)).
- **A probe ahead of an owed ACK** stalled a keyed server's address validation, and a probe size settled by another
  acknowledgement was probed again ([session](../sessions/2026-11-10-net-path-pmtu.md)). A spuriously lost large
  packet now clears black hole suspicion (c5a06d38).
- **Windows refuses a sender's CE mark**: that datagram goes unmarked and marking stays on
  ([session](../sessions/2026-11-10-net-path-ecn.md)).

### 5.10 The test harnesses' own traps

- `SSL_set_quic_tls_early_data_enabled` fails silently before the QUIC callbacks are set, and `SSL_free` without
  `SSL_SENT_SHUTDOWN` marks a kept ticket unresumable: the OpenSSL peer's 0-RTT cases failed until both were fixed
  ([session](../sessions/2026-11-09-4-2a-interop-cases.md)).
- An ngtcp2 server drops a first flight it receives out of order (`NGTCP2_ERR_RETRY`) by design; the peer program
  takes the next Initial. ngtcp2 buffers only four packets ahead of the handshake's end, the likely sink of Cerid's
  0-RTT datagrams on the relay (same session).
- The echo application must reset a stream only after its bytes have left (QMux) or been acknowledged (QUIC); waiting
  one turn was not enough ([session](../sessions/2026-11-10-4-2a-h2-tls-interop.md)).
- A static `Http3Tunnels` outlived its connection ("pure virtual method called" at exit)
  ([session](../sessions/2026-11-10-4-2a-websocket-h3-interop.md)).
- Fuzz harness oracles were wrong before the code was: a peer's close code held to this side's codes, an RTT of
  minutes taught by a script, an honest HTTP/2 mode moving one byte per 50 ms into the real SETTINGS timeout, a
  WebSocket tunnel already Closed when reported Opened. Fix the oracle, then trust it.
- **Not code defects: this host's misexecution.** Many libFuzzer runs on the local Linux fuzz lane stopped on reports
  that never replay (wild jumps, a `this` of 0x6c, corrupted allocator metadata, inside AWS-LC). The same target built
  from an older HEAD failed alike. They are the host fault on record, and hosted Host check legs are the arbiter: a
  hosted event is a defect ([MEMORY](../../MEMORY.md), sessions
  [spurious](../sessions/2026-11-08-4-2a-spurious-losses.md), [hpack](../sessions/2026-11-09-4-2a-hpack.md)).

---

## 6. Measured numbers

**Compression boards** (deterministic sizes; the machine does not matter):

- [QPACK against six encoders on the qifs corpus](../bench/2026-11-09-4-2a-qpack-compression.md): overall 1.003 of
  ls-qpack (0.3% above the best) and smaller than f5 (0.958), proxygen (0.951), qthingey (0.969), quinn (0.939) and
  nghttp3 (0.892). The board explains each loss.
- [HPACK against the hpack-test-case encoders](../bench/2026-11-09-4-2a-hpack-compression.md): 1.0111 of nghttp2 with
  cookies never indexed (the shipped rule), 0.9953 with them indexed (an experiment not kept).

**There is no throughput, latency or CPU board for the transport yet.** NET.4 owns those measurements (legitimate
client latency under attack, bucket rates, pool sizes); do not quote any.

**Figures recorded in the session logs** (each with its source):

| Figure | Value | Source |
|---|---|---|
| Hostile datagram path (10% loss, 20 ms jitter), Acked / Lost reports | 1,812 / 5,388 before spurious-loss detection; 5,640 / 1,560 after | [spurious losses](../sessions/2026-11-08-4-2a-spurious-losses.md) |
| The same as the row states it | Acked share 25% to 78% | [4.2a row](../ROADMAP.md#slice-4.2a) |
| Cerid client's 0-RTT datagrams on ngtcp2's relay | 73 to 86% arrive, 12 to 18% reported Acked; the case holds delivery at 50% | [interop cases](../sessions/2026-11-09-4-2a-interop-cases.md) |
| 8 MiB transfer with ECN against ngtcp2 on the loopback | ngtcp2 marked 6,582 datagrams and read 7,316 marked ECT(0) | [ECN with ngtcp2](../sessions/2026-11-10-net-path-ecn-ngtcp2.md) |
| CE relay against ngtcp2 | Cerid read 976 CE in ngtcp2's counts and made 470 reductions | same |

**Interoperability records** (both ways round unless stated; every byte checked by both ends):

| Peer | What | Cases | Source |
|---|---|---|---|
| ngtcp2 1.26.0 over OpenSSL 4.0.3 | QUIC v1 and v2, real UDP | 58 in 69 s, then 102 in 103 and 104 s (two runs), then 114 with ECN (NET-PATH) | [ngtcp2](../sessions/2026-11-09-4-2a-ngtcp2-interop.md), [cases](../sessions/2026-11-09-4-2a-interop-cases.md), [ECN](../sessions/2026-11-10-net-path-ecn-ngtcp2.md) |
| nghttp3 1.19.0 over ngtcp2 | HTTP/3 | 66 of 66 | [nghttp3](../sessions/2026-11-09-4-2a-nghttp3-interop.md) |
| quiche 0.30.0 over BoringSSL | HTTP/3 (v1 only, no dynamic table) | 54 of 54 | [quiche](../sessions/2026-11-09-4-2a-quiche-interop.md) |
| `qmux` crate 0.6.6 over rustls 0.23.46 | QMux over TLS, then WebSocket | 52; then 36 WebSocket cases added | [QMux](../sessions/2026-11-09-4-2a-qmux-interop.md), [WebSocket](../sessions/2026-11-09-4-2a-websocket.md) |
| web-transport-quinn 0.13.6 over Quinn 0.11.12 | WebTransport, legacy spelling (and Cerid against itself) | 82 of 82 in 86 s | [quinn](../sessions/2026-11-09-4-2a-webtransport-interop.md) |
| webtransport-go 0.13.0 over quic-go 0.62.0 | WebTransport `webtransport-h3`, flow control, RESET_STREAM_AT | 30 of 30 in 60 s | [go](../sessions/2026-11-09-4-2a-webtransport-go-interop.md) |
| h2spec 2.6.0 `--strict` | HTTP/2 conformance | 147 of 147, eight runs in a row | [HTTP/2 interop](../sessions/2026-11-09-4-2a-http2-interop.md) |
| nghttp2 1.71.0 | HTTP/2 exchange | 58 of 58 | same |
| `h2` crate 0.4.20 with tokio-tungstenite | RFC 8441, prior knowledge | 42 of 42 (130 in the whole QMux run) | [RFC 8441](../sessions/2026-11-10-4-2a-websocket-h2-interop.md) |
| `h3` crate (archive 43745ad8) over Quinn 0.11.12 | RFC 9220, also through a lossy relay | 40 of 40 (170 in the whole run) | [RFC 9220](../sessions/2026-11-10-4-2a-websocket-h3-interop.md) |
| `h2` crate over rustls, ALPN `h2` | RFC 8441 over TLS on the stream host | 44 (214 of 214 in the whole run) | [h2 over TLS](../sessions/2026-11-10-4-2a-h2-tls-interop.md) |
| qifs corpus (six encoders) | QPACK decoding | all 528 outputs | [QPACK](../sessions/2026-11-09-4-2a-qpack.md) |
| hpack-test-case (fourteen encoder runs) | HPACK decoding | all 446 stories | [HPACK](../sessions/2026-11-09-4-2a-hpack.md) |

---

## 7. Where the code lives

**Headers and sources**: [engine/foundation/net/include/crd/net/](../../engine/foundation/net/include/crd/net/) and
[engine/foundation/net/src/](../../engine/foundation/net/src/), one `.cpp` per header.

| Layer | Headers |
|---|---|
| Addresses and sockets | `endpoint.hpp`, `udp.hpp`, `stream_socket.hpp`, `stream_poller.hpp`, `src/native_socket.hpp` |
| Loop and admission | `io_loop.hpp`, `ingress.hpp`, `connection_table.hpp`, `quic_header.hpp`, `quota.hpp` (NET.3) |
| QUIC wire | `quic_frame.hpp`, `quic_transport_parameters.hpp`, `quic_packet.hpp`, `register_ids.hpp` |
| QUIC state | `quic_recovery.hpp`, `quic_stream.hpp`, `quic_stream_set.hpp`, `quic_datagram.hpp`, `quic_connection.hpp` |
| Hosts | `host_handle.hpp`, `deadline_heap.hpp`, `quic_host.hpp`, `quic_server.hpp`, `quic_client.hpp`, `ticket_cache.hpp` |
| Stream path | `qmux.hpp`, `qmux_host.hpp` (with `src/qmux_http2.cpp`) |
| HTTP | `http_fields.hpp`, `qpack.hpp`, `hpack.hpp`, `http_message.hpp`, `http_capsule.hpp`, `http3.hpp`, `http2.hpp` |
| Browser sessions | `webtransport.hpp`, `websocket.hpp`, `websocket_tunnel.hpp` |
| Channels | `channel_link.hpp` (on NET.3's `frame.hpp`, `wire.hpp`, `admission.hpp`) |
| NET-PATH | `congestion.hpp`, `ecn.hpp`, `quic_ecn.hpp`, `quic_pmtu.hpp` |

Cryptography lives in crd-crypto: `quic.hpp` (packet keys, tokens, reset keys, keyed secrets) and `tls.hpp`.

**Unit tests** ([tests/foundation/net/](../../tests/foundation/net/), the `crd-net-tests` executable, Catch2 tags in
brackets): `test_endpoint.cpp`, `test_udp.cpp` `[udp]`, `test_io_loop.cpp`, `test_ingress.cpp`,
`test_quic_frame.cpp`, `test_quic_transport_parameters.cpp`, `test_quic_packet.cpp` `[quic-packet]`,
`test_quic_recovery.cpp`, `test_quic_stream.cpp`, `test_quic_stream_set.cpp`, `test_quic_datagram.cpp`,
`test_quic_connection.cpp` `[quic-connection]`, `test_quic_keyed.cpp` `[raw-path]`, `test_quic_server.cpp`,
`test_quic_client.cpp`, `test_ticket_cache.cpp`, `test_stream_socket.cpp` `[tcp]`, `test_stream_poller.cpp`
`[poller]`, `test_qmux.cpp` `[qmux]` `[qmux-early]`, `test_qmux_host.cpp` `[qmux-host]`, `test_qmux_http2.cpp`
`[qmux-h2]`, `test_http_fields.cpp`, `test_qpack.cpp` `[qpack]`, `test_hpack.cpp` `[hpack]`, `test_http_message.cpp`,
`test_http3.cpp` `[http3]`, `test_http3_webtransport.cpp` and `test_webtransport.cpp` `[webtransport]`,
`test_http2.cpp` `[http2]`, `test_websocket.cpp` `[websocket]`, `test_websocket_tunnel.cpp` `[ws-tunnel]`,
`test_channel_link.cpp` `[channels]`; NET-PATH: `test_congestion.cpp`, `test_quic_congestion.cpp`,
`test_quic_ecn.cpp`, `test_quic_pmtu.cpp`. Fixtures: `quic_connection_fixture.hpp` (the seeded hostile path),
`quic_host_fixture.hpp`, `loop_fixture.hpp`, `http3_fixture.hpp`, `stream_pipe_fixture.hpp`.

**Fuzz targets** ([tests/foundation/net/fuzz/](../../tests/foundation/net/fuzz/), each `crd-fuzz-net-<name>` with a
committed corpus): `endpoint`, `ingress`, `quic_codec`, `quic_packet`, `quic_recovery`, `quic_stream`,
`quic_stream_set`, `quic_datagram`, `quic_connection`, `ticket_cache`, `qmux`, `qmux_early`, `qmux_host`, `qpack`,
`hpack`, `http3`, `http2`, `webtransport`, `websocket`, `websocket_tunnel`, `channel_link`; NET-PATH: `congestion`,
`quic_ecn`, `quic_pmtu`.

**Interop peers and drivers** ([tests/foundation/net/interop/](../../tests/foundation/net/interop/)):

| Program | Over | Driver, CTest |
|---|---|---|
| `crd-quic-peer` (`cerid_quic_peer.cpp`) | crd-net's QUIC, HTTP/3, WebTransport, RFC 9220 tunnels | all UDP drivers |
| `crd-ngtcp2-quic-peer` (`ngtcp2_quic_peer.c`, `ngtcp2_http3.c`) | ngtcp2, nghttp3, OpenSSL 4.0 | `quic_interop.py` (`crd-net-quic-interop-ngtcp2`), `http3_interop.py` (`crd-net-http3-interop-nghttp3`) |
| `crd-quiche-peer` (`quiche_peer/`) | quiche, BoringSSL | `http3_interop.py --quiche` (`crd-net-http3-interop-quiche`) |
| `crd-webtransport-quinn-peer` (`webtransport_peer/`) | web-transport-quinn | `webtransport_interop.py` (`crd-net-webtransport-interop-quinn`) |
| `crd-webtransport-go-peer` (`webtransport_go_peer/`) | webtransport-go | `webtransport_interop.py --go` (`crd-net-webtransport-interop-go`) |
| `crd-qmux-peer` (`cerid_qmux_peer.cpp`) and `crd-qmux-crate-peer` (`qmux_crate_peer/`) | crd-net's stream host; the `qmux`, `h2`, `h3` crates | `qmux_interop.py` (`crd-net-qmux-interop-crate`) |
| `crd-http2-peer` (`cerid_http2_peer.cpp`), `crd-nghttp2-peer` (`nghttp2_http2_peer.c`), h2spec | HTTP/2 | `http2_interop.py` (`crd-net-http2-interop`) |
| `cerid_qpack_replay.cpp`, `cerid_hpack_replay.cpp` | the corpora | `qpack_interop.py` (`crd-net-qpack-interop-qifs`), `hpack_interop.py` (`crd-net-hpack-interop-corpus`) |

Shared pieces: `interop_common.py` (report reading, the UDP `Relay` with seeded loss, duplication, delay, jitter and
ECN marking, the TCP `Chopper`), `interop_echo.hpp`, `http_exchange.h`, `interop_http3.hpp`, `interop_qmux.hpp`,
`interop_webtransport.hpp`. All are built only on the `linux-gcc-public-checks` preset
([CMakePresets.json](../../CMakePresets.json)); the pins are in [cmake/pins.json](../../cmake/pins.json).

**Contract and evidence**: [network-foundation.md S33 to S65](../design/network-foundation.md#sockets) and
[S66 to S68](../design/network-foundation.md#congestion); [ADR-0035](../decisions/0035-networking-architecture.md);
the session logs linked from the [4.2a row](../ROADMAP.md#slice-4.2a).

---

## 8. What this recipe could not confirm from a file

- **TLS 1.3's RFC number.** The register pins RFC 9846, which obsoletes RFC 8446 and keeps its version and wire format;
  this recipe cites RFC 9846 and mentions RFC 8446 as the obsoleted one.
- **Register states.** As read on 2026-11-10, `quic.transport`, `quic.recovery`, `path.dplpmtud` and `webtransport.h3`
  are `planned` although their 4.2a parts are built and proven; their notes say the remaining work is NET-PATH's and
  NET.5's. The recipe quotes the states as read.
- **Chromium's flow-control auto-tuning** is credited in S38 without a paper, version or link.
- **Browsers' exact WebTransport settings** (which codepoints Chrome and Safari send) are confirmed only under WEB.5.
- **No transport performance board exists**; NET.4 will publish one.
- **HyStart++'s RTT threshold** formula above is RFC 9406's, stated from its constants in `congestion.cpp`
  (4 ms, 16 ms, divisor 8); the code itself was not traced line by line.
- **NET-PATH** is Partial: PMTU discovery against real peers, `max_udp_payload_size` advertised as the loop's real
  receive slot (it advertises 65,527 while the loop reads 1500), ICMP hints, BBRv3, Prague and migration are open.

---

## 9. How to learn this

Read in the order data flows, one header with its test at a time. The test is the best explanation of the header: it
shows each rule being kept and each refusal being made.

1. `endpoint.hpp` and `udp.hpp`, with `test_endpoint.cpp` and `test_udp.cpp`: what an address and a datagram are.
2. `io_loop.hpp` and `ingress.hpp`, with `test_io_loop.cpp` and `test_ingress.cpp`: one turn of the loop, and what a
   server says to a stranger before it trusts anything. Read RFC 9000 sections 8, 14 and 17 beside it.
3. `quic_frame.hpp` and `quic_transport_parameters.hpp`: the vocabulary. RFC 9000 sections 16, 18 and 19.
4. `quic_packet.hpp` with `test_quic_packet.cpp`: RFC 9001's appendix A packets rebuilt byte for byte.
5. `quic_recovery.hpp` with `test_quic_recovery.cpp`: work section 3.3's formulas by hand against the estimator test.
   RFC 9002 sections 5 to 7 and appendix A.
6. `quic_stream.hpp`, then `quic_stream_set.hpp` and `quic_datagram.hpp`: bytes, credit, streams, datagrams.
7. `quic_connection.hpp` with `test_quic_connection.cpp` and `quic_connection_fixture.hpp`: everything carried over
   seeded hostile paths.
8. `quic_host.hpp`, `quic_server.hpp`, `quic_client.hpp`: many connections behind one socket.
9. `qmux.hpp` and `qmux_host.hpp`: the same streams over TLS and TCP.
10. `http_fields.hpp`, `qpack.hpp`, `http3.hpp`, `webtransport.hpp`; then `hpack.hpp`, `http2.hpp`, `websocket.hpp`,
    `websocket_tunnel.hpp`: the browser paths.
11. `channel_link.hpp` with `test_channel_link.cpp`: where every path ends.
12. NET-PATH's `congestion.hpp`, `quic_ecn.hpp`, `quic_pmtu.hpp` last, since they change while the row is open.

**Watch it work, first.** Build the Linux interop lane and run the QUIC interop against ngtcp2: two separate processes,
two independent implementations, real UDP on the loopback, every byte checked by both ends. Through the WSL source
mirror ([lane script](../../scripts/wsl-lane.sh)):

```
bash /mnt/d/Dev/cerid/scripts/wsl-lane.sh configure linux-gcc-public-checks
bash /mnt/d/Dev/cerid/scripts/wsl-lane.sh build linux-gcc-public-checks crd-quic-peer crd-ngtcp2-quic-peer
bash /mnt/d/Dev/cerid/scripts/wsl-lane.sh test linux-gcc-public-checks -R crd-net-quic-interop-ngtcp2 -V
```

`-V` prints the driver's command line. To watch one case, run `quic_interop.py` with that command line and
`--only "<text>"`, which keeps the cases whose description contains the text. Descriptions read like
`cerid client v1 x25519mlkem768 aes128 1x4096 client closes 0`, with ` retry`, ` compat`, ` path ...` or
` stateless reset` appended, so `--only retry` shows a Retry counted off the wire, and `--only "stateless reset"` a
server killed mid-transfer whose successor resets the client. Then run `crd-net-tests "[quic-connection]"` on any lane
to see the same connection carried over seeded paths that lose, duplicate and reorder.
