# Network foundation: threat model, capabilities, channels and transport

<!-- doc-role: contract -->
> Design contract of [NET.1](../ROADMAP.md#slice-net.1); status lives only in the ROADMAP. Read
> [ADR-0035](../decisions/0035-networking-architecture.md) and the
> [networking research](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md) first.

Purpose: one secure, robust network foundation for every Cerid product class (games from duels to massively
multiplayer worlds, collaborative editors, DAW live sessions, Zoom-class calls, the crash pool, agent hubs). This
document is the NET.1 design: what is defended against whom, how authority is expressed, how channels are separated,
how packets are admitted, what browsers impose, which cryptography is allowed and how protocols and algorithms evolve.
The rows named in [the hand-off table](#hand-off) implement and prove each clause; nothing here is shipped capability.

## Review: the baseline and what the design reuses

Verified on 2026-10-31 against the tree: no `crd-net` module exists, and no source under `engine/`, `runtime/`,
`tools/` or `sandbox/` calls a socket, Winsock or a TLS library. The research's statement that networking is
entirely plan still holds. The foundation reuses these owned capabilities and duplicates none of them
([OWN.1](../ROADMAP.md#slice-own.1) records the owners):

| Need | Owner today | Use in the foundation |
|---|---|---|
| Checksums and non-cryptographic hashes | `crd-containers` checksum family (`crd/containers/checksum.hpp`: CRC-32, CRC-32C, XXH3-64, dispatched per CPU level) | Plaintext integrity hash at serialisation ([HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1)), lockstep and rollback state hashes, capture file checks. Never an authenticator. |
| Fault injection | `crd-core` fault seams (`crd/core/fault_seam.hpp`, DIAG.9b), compiled out of shipping | NET-SIM adds a socket seam on the same pattern; no second injector. |
| Deterministic time | `crd-time` `DeterministicClock` | Virtual clocks of NET-SIM and replay; the transport reads time only through an injected clock. |
| Redaction classes | `crd-core` `RedactionClass` | Keys, tokens and payloads are Restricted in every diagnostic record and capture. |
| Crash and failure bundles | `crd-perf` bundles ([DIAG.5d](../ROADMAP.md#slice-diag.5d)) | Network context of [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1) packs; no second bundle format. |
| Run records and replay | [DIAG.9a](../ROADMAP.md#slice-diag.9a), [4.2b](../ROADMAP.md#slice-4.2b) | One replay path for NET-SIM seeds, recorded sessions and rollback. |
| Seeded channel models | `hesap-comms` (AWGN, fading on a seeded Philox stream) | The pattern NET-DEBUG's loss models follow; network math lands in [HESAP-NET](../ROADMAP.md#slice-hesap-net). |
| File and async I/O | `crd-platform` | Crash pool and capture persistence; storage faults through the existing I/O seam. |

Two capabilities had no owner and this design assigns them (decisions [D1](#d1) and [D2](#d2)): `crd-net` owns
sockets, transport, sessions, channels, admission, conditioning, NAT traversal and capture; `crd-crypto` owns the
cryptographic provider interface, the key store, the CSPRNG and keyed pseudorandom functions. Both are OWN.1
registry entries. Since 2026-10-31 both modules exist (NET-STD's negotiation created `crd-net`, whose
`network.transport` signatures refuse a socket call or socket header in any other module, and its policy generations
created `crd-crypto`); since 2026-11-01 `crypto.providers` is landed with the [provider interface](#provider), whose
signatures refuse a cryptographic library header or randomness call in any other module. The review above records the
tree before both.

## Assets

| ID | Asset | Why it matters |
|---|---|---|
| A1 | Long-term identity keys (service, server, device, user) and policy-signing keys | Their theft lets an attacker impersonate a server or push a weaker policy. |
| A2 | Session secrets and tokens (traffic keys, resumption tickets, capability tokens, address-validation tokens) | Short-lived but enough to hijack or replay a session. |
| A3 | Authoritative simulation state (game worlds, digital twins) | Corruption breaks fairness or safety; plausible bad state is worse than a crash. |
| A4 | Project documents and history (collaboration) | Durable user work; loss or silent divergence is unacceptable. |
| A5 | Live media and music (voice, video, DAW audio) | Confidential content; latency is part of correctness. |
| A6 | Crash packs and telemetry | Personal data and process memory; regulated (COMPLY.2, CRA). |
| A7 | Agent authority (MCP tools, `ceridc` commands, fix-agent leases) | A remote command can edit projects, run code or actuate hardware. |
| A8 | Availability (CPU, memory, bandwidth, relay capacity, handshake capacity) | The cheapest thing to attack. |
| A9 | Metadata (who talks to whom, when, how much, from where) | Leaks behaviour even when content is encrypted. |
| A10 | Update and policy channel (signed fixes, crypto policy generations, rule generations) | A forged generation compromises every client at once. |

## Adversaries

| ID | Adversary | Capability assumed |
|---|---|---|
| V1 | Off-path spoofer | Sends packets with forged source addresses; sees no responses. |
| V2 | Passive on-path observer, including harvest-now-decrypt-later with a future quantum computer | Records all traffic for years. |
| V3 | Active on-path attacker or broken middlebox | Drops, delays, reorders, rewrites, injects; ossifies unknown fields. |
| V4 | Authenticated abusive client (cheater, griefer, scraper) | Holds valid credentials; sends well-formed but hostile traffic. |
| V5 | Compromised or faulty peer | Sends plausible but wrong state: a cheat, a bit flip before encryption, a bad NIC, a buggy build. |
| V6 | Honest-but-curious or malicious relay or server operator | Forwards traffic; must not read end-to-end encrypted groups. |
| V7 | Volumetric attacker (botnet, reflection) | Saturates links or handshake capacity; uses Cerid servers as reflectors if allowed. |
| V8 | Hostile content | Crash packs, logs, names and assets crafted to inject prompts or exploit parsers. |
| V9 | Insider or compromised service | Holds service credentials; moves laterally between services. |

## Trust boundaries

| ID | Boundary | Crossing it requires |
|---|---|---|
| B1 | Network to admission | Bounded parsing and admission ([admission pipeline](#admission)). |
| B2 | Admitted to authenticated session | A NET.2 handshake: mutual authentication for service links, server authentication plus a client credential otherwise. |
| B3 | Session principal to operation | A capability that covers the object, the operation and the budget ([capabilities](#capabilities)). |
| B4 | Endpoint to relay or forwarding server | End-to-end protection (MLS, SFrame) where the relay must not read. |
| B5 | Process to crash pool | Consent tier, redaction policy and pseudonymisation (COMPLY.2). |
| B6 | Remote agent to engine command surface | Capability tokens from the user; content and tool descriptions never grant authority. |
| B7 | Browser sandbox to Cerid service | Web-origin checks and token authentication; no client certificates. |

## Threat catalogue

Each threat names its mitigation and the row that implements and proves it. "Proves" means a test under
[NET-SIM](../ROADMAP.md#slice-net-sim) or [NET.4](../ROADMAP.md#slice-net.4) unless the row says otherwise.

| ID | Threat (adversary) | Mitigation | Row |
|---|---|---|---|
| T01 | Spoofed handshakes exhaust server state (V1, V7) | Stateless Retry under load, address-validation tokens, no per-source allocation before validation | [NET.2](../ROADMAP.md#slice-net.2), [NET.4](../ROADMAP.md#slice-net.4) |
| T02 | Reflection and amplification (V1, V7) | Three-times limit before address validation; version negotiation and resets only for datagrams large enough | NET.2, NET.4 |
| T03 | Post-quantum handshakes exceed the amplification limit (V1, V7) | Certificate compression (RFC 8879), raw public keys or pinned Cerid credentials for game and session servers (bound out of band, with the misbinding defence of the [TLS 1.3 profile](#tls-13-profile)), resumption, Retry; measured in round trips | NET.2 |
| T04 | Harvest now, decrypt later (V2) | Hybrid key exchange (X25519MLKEM768 by default) on every link; classical-only refused by a dated policy | NET.2 |
| T05 | Downgrade of version, extension or algorithm (V3) | Negotiated parameters bound to the handshake transcript; policy generations are monotonic ([agility](#agility)) | [NET-STD](../ROADMAP.md#slice-net-std), NET.2 |
| T06 | Ossification: middleboxes or peers break on unknown values (V3) | GREASE in every build, ignore-unknown semantics, QUIC bit greasing (RFC 9287) | NET-STD |
| T07 | Replay of captured packets or early data (V3) | AEAD packet-number windows; 0-RTT only for schema-declared idempotent operations with single-use tickets | NET.2, [NET.3](../ROADMAP.md#slice-net.3) |
| T08 | Session or token theft and reuse (V3, V9) | Short-lived capability tokens bound to the session's exporter secret; rotation and revocation generations | NET.2 |
| T09 | Parser exploits and decompression bombs (V3, V4, V8) | Generated bounds-checked decoders, size limits before allocation, ratio and output caps, continuous fuzzing | NET.3, NET.4 |
| T10 | Hash-flooding of connection and object tables (V1, V4) | Tables keyed by a secret keyed PRF from `crd-crypto`, never an unkeyed hash | NET.3, [4.2a](../ROADMAP.md#slice-4.2a) |
| T11 | Authenticated resource abuse (V4) | Per-principal quotas on streams, datagrams, bytes, reassembly and pending work; authorization before decode | NET.3, NET.4 |
| T12 | Plausible wrong state from a faulty or compromised peer (V5) | Server authority and validated transitions; plaintext integrity hash verified after decryption; lockstep and rollback state hashes; a named culprit and a typed error | NET.3, [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), [4.2c](../ROADMAP.md#slice-4.2c), [ANTICHEAT](../ROADMAP.md#slice-anticheat) |
| T13 | Relay used as an open proxy or reflector (V4, V7) | Authenticated allocation, permissions limited to the session's peers, per-allocation quotas and accounting | [NET-REACH](../ROADMAP.md#slice-net-reach) |
| T14 | Relay or forwarding server reads content (V6) | MLS groups for end-to-end sessions; SFrame media keys from the MLS exporter | NET.2, [NET-LIVE](../ROADMAP.md#slice-net-live) |
| T15 | Metadata and traffic analysis (V2, V6) | ECH on public endpoints; padding policies per channel class; connection IDs rotate on migration; pseudonymous identities in crash packs | NET.2, NET.3, [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1) |
| T16 | Path failure misread as attack or loss: LEO handovers, mobile handover, MTU blackholes (V3) | DPLPMTUD with a 1280-byte floor, migration, handover-aware congestion response | [NET-PATH](../ROADMAP.md#slice-net-path) |
| T17 | IPv6-only, NAT64 and CGNAT networks break reachability | Family-neutral addresses, Happy Eyeballs (RFC 8305), NAT64 prefix discovery (RFC 7050), no IPv4 literals in protocols | NET-REACH, [NET.5](../ROADMAP.md#slice-net.5) |
| T18 | Lateral movement between services (V9) | Mutual authentication on every service link; capabilities scoped per service; no shared secrets between tenants | NET.2, [NET-SERVER](../ROADMAP.md#slice-net-server) |
| T19 | Forged policy, rule or fix generations (V3, V9) | Generations signed by offline hardware-held keys; monotonic numbers; signed remote disable | NET-STD, NET.2, [SUPPLY.1](../ROADMAP.md#slice-supply.1) |
| T20 | Hostile crash data drives an agent (V8) | Packs are untrusted data; the fix agent consumes them under AGENT.3's defence | [CRASH-FIX.2](../ROADMAP.md#slice-crash-fix.2), [AGENT.3](../ROADMAP.md#slice-agent.3) |
| T21 | Remote agent exceeds its authority (V4, V8) | Capability tokens issued by the user; operations authorised per object and budget; content never widens them | NET.2, [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0) |
| T22 | Debug key logs or capture keys ship (V9) | RFC 9850 key logging and capture keys compiled out of shipping and refused at runtime when built in; a CI guard | NET.2, [NET-DEBUG](../ROADMAP.md#slice-net-debug) |
| T23 | An actively exploited vulnerability goes unreported | CRA Article 14 workflow (24-hour, 72-hour and 14-day reports) fed by the crash pool; provenance recorded for every networked build | [COMPLY.3](../ROADMAP.md#slice-comply.3), [SEC.1](../ROADMAP.md#slice-sec.1), CRASH-FIX.2 |
| T24 | Volumetric saturation beyond host capacity (V7) | Documented capacity and failure envelopes; upstream mitigation and relay fronting; never a "DDoS-proof" claim | NET.4, NET.5, NET-SERVER |

<a id="product-classes"></a>
## Threat model per product class

The catalogue applies everywhere; each class weights it differently and adds its own semantic duties.

| Class | Dominant threats | Class-specific design |
|---|---|---|
| Competitive duels and action games | T11, T12, T07, T24 | Server authority; inputs validated for range and rate; rollback and lockstep state hashes; lag compensation bounded by a declared rewind window (4.2c). |
| Large lobbies and massively multiplayer worlds | T01, T11, T12, T24 | Interest management limits what each client learns; authority leases and exactly-once handoff between shards; an overload mode that degrades fidelity before admission ([NET-SCALE](../ROADMAP.md#slice-net-scale)). |
| Collaborative editors | T08, T11, T21, T07 | Durable transactions with idempotency keys; per-object capabilities; offline edits re-authorised on reconnect; stale authorization rejected ([COLLAB](../ROADMAP.md#slice-collab)). |
| DAW live sessions and performances | T14, T15, T16 | Media on datagram channels with FEC; end-to-end keys where a relay is used; clock quality from [NET-TIME](../ROADMAP.md#slice-net-time). |
| Zoom-class calls and webinars | T14, T15, T13, T24 | MLS group keys and SFrame; selective forwarding servers see ciphertext only; relays authenticated (NET-LIVE, NET-REACH). |
| The crash pool | T09, T20, T23, T24 | A separate upload endpoint and principal; consent tier and redaction before upload; authorisation and size limits before decode (CRASH-FIX.2). |
| Agent hubs and remote tooling | T21, T18, T08 | Capabilities per tool and project; read, edit, install, publish and actuate separated; actions journaled ([CMD](../ROADMAP.md#slice-cmd)). |
| Digital twins and ROS 2 bridges | T12, T16 | Physical actuation is a separate capability; state from devices is validated as from a faulty peer ([4.2e](../ROADMAP.md#slice-4.2e)). |

<a id="capabilities"></a>
## Principals and capabilities

- **Principals** are users, devices, services, server instances and agents. Each has a long-term key in the
  `crd-crypto` key store (hardware-backed where available). Authentication (B2) establishes the principal;
  authorisation (B3) is a separate step on every operation.
- **A capability** is a signed, expiring token: principal, audience service, scope (project, world, object set or
  tool), permitted operations, budget (rate, bytes, objects), issue and expiry times, the policy generation it was
  issued under and its holder's key, each presentation bound to its session by the session's exporter, so a stolen
  token is useless without the key and a proof useless in another session ([S15](#capability-tokens) binds the holder
  rather than one session, so a token outlives a reconnection and can be delegated). Capabilities only attenuate: a
  holder can derive a narrower one, never a wider one.
- **No ambient authority.** A connection grants nothing by itself; a message is executed only when a capability
  covers its object, operation and budget. Content, tool descriptions, asset names and model output never grant or
  widen authority (system quality contract).
- **Revocation** is a signed revocation generation; servers check token generation against it at admission (B3) and
  at every renewal. Expiry is short (minutes for game and agent tokens, hours for collaboration), with renewal over
  the authenticated session.
- **Early data.** TLS 1.3 and QUIC 0-RTT carry only operations their schema declares idempotent; a non-idempotent
  game or project mutation never executes as replayable early data.

<a id="channels"></a>
## Sessions and channels

**Game and project traffic never share a session.** Live simulation and durable project collaboration have different
semantics (G12): each is its own session type with its own application protocol identifier (ALPN), its own exporter
labels for keys and token bindings, its own schemas and its own authorisation context. A shared command schema does
not make one update protocol correct for both; the shared parts are the transport, admission and cryptography.

| Session type | Purpose | Protocol family |
|---|---|---|
| Game | Authoritative live simulation | Game control and replication schemas |
| Project | Durable collaboration on documents and assets | Project transaction schemas |
| Media | Calls, webinars, live music | Media control plus SFrame-protected media |
| Crash upload | Consented pack upload | Upload schema; a separate endpoint and principal |
| Agent | Remote tools and commands | Command schemas over capabilities |
| Service | Server-to-server links (fleet, matchmaking, relays, pool) | Mutually authenticated service schemas |

Every channel declares, in its authored schema: reliability, ordering, priority class, maximum message size, rate and
byte budgets, replay semantics (idempotent or not) and padding policy. Channel classes:

| Class | Reliability and order | Typical use |
|---|---|---|
| Control | Reliable, ordered | Session control, capability renewal, version and rule generation handshakes |
| Input | Unreliable, redundant (last N inputs per datagram) | Player input for prediction and rollback |
| State | Unreliable, latest wins | Snapshots and deltas |
| Event | Reliable, unordered | Game events, chat |
| Transaction | Reliable, ordered, durable acknowledgement, idempotency key | Project mutations |
| Bulk | Reliable, flow-controlled, chunked and resumable | Asset and project synchronisation |
| Media | Unreliable datagrams with FEC | Voice, video, music |

<a id="transport"></a>
## Transport

- **Layering** follows the amended ADR-0035: Layer 1 transport (`crd-net`, 4.2a) is standalone, with no scene or
  job dependency; it runs its own I/O loop behind an executor seam and reads time only through an injected clock,
  so NET-SIM drives it with virtual sockets and the `DeterministicClock`.
- **Paths.** QUIC v1 and v2 with unreliable datagrams (RFC 9000, RFC 9369, RFC 9221) is primary. A TCP and TLS 1.3
  path serves networks that block UDP. Browsers use WebTransport with a WebSocket fallback ([below](#browsers)). A raw
  UDP path serves LAN and headless simulation ([S55](#raw-path)), but every path that can reach a non-loopback address
  is authenticated and encrypted with the same session keys; there is no plaintext mode in any build. NET-SIM's
  virtual sockets never leave the process.
- **One session model over every path.** Sessions, channels, capabilities and schemas are identical across paths;
  a session migrates between paths (QUIC migration, or a resumption on the fallback path) without the application
  seeing anything but a latency change.
- **Memory.** Connection state comes from fixed-size pools sized by configuration; steady-state packet handling makes
  no heap allocation. Reassembly and pending buffers are counted against the principal's quota.
- **Path behaviour** (PMTU, ECN, pacing, congestion control, offloads, migration policy) belongs to NET-PATH;
  reachability (ICE, STUN, TURN, MASQUE, Cerid relays) to NET-REACH.

<a id="sockets"></a>
### Sockets

**S33. Addresses and datagram sockets** (4.2a, threats T01, T02 and T10;
[endpoint.hpp](../../engine/foundation/net/include/crd/net/endpoint.hpp),
[udp.hpp](../../engine/foundation/net/include/crd/net/udp.hpp); 2026-11-08).

- **One representation per address.** An IPv4 address keeps four bytes and twelve zeros, a zone index only on
  IPv6, and a dual-stack socket reports an IPv4 peer as IPv4, never as `::ffff:a.b.c.d`; so S3's prefix buckets
  (`IpAddress::prefix`) and every table keyed by a peer see one key for one host. Text is read per RFC 4291 and
  written per RFC 5952 by crd-net itself, never by the host, so captures and NET-SIM print alike on every platform;
  IPv4 octets with leading zeros are refused (some readers take them as octal).
- **Stage S0 is a batch into the caller's storage.** `DatagramSocket::receive` fills caller-owned slots without
  waiting (Linux: one `recvmmsg` per 32 datagrams; Windows and macOS: one call each), marks a datagram longer than
  its slot as truncated, and allocates nothing; `send` stops at the first datagram that cannot leave and names why,
  with `Busy` for back-pressure (a full socket buffer or device queue). Datagrams above a family's UDP payload and
  crossings of families are refused before the host sees them.
- **A server is not stopped by one peer.** Windows sockets hold their port exclusively (`SO_EXCLUSIVEADDRUSE`)
  and ignore ICMP port and TTL reports (`SIO_UDP_CONNRESET`, `SIO_UDP_NETRESET`), and a stale ICMP report on any
  host is skipped instead of ending a receive; sockets are non-blocking and not inherited by child processes.
- **The seam.** The transport drives only `DatagramSocket`; NET-SIM's virtual sockets and NET-DEBUG's conditioner
  implement it, and `wait_readable` is the one blocking call. PMTU, ECN, pacing and offloads (GSO, GRO, USO, URO)
  are NET-PATH's, added behind the same interface.

<a id="ingress"></a>
### Ingress

**S34. The I/O loop and admission stages S1 to S3** (4.2a, threats T01, T02 and T10;
[quic_header.hpp](../../engine/foundation/net/include/crd/net/quic_header.hpp),
[connection_table.hpp](../../engine/foundation/net/include/crd/net/connection_table.hpp),
[ingress.hpp](../../engine/foundation/net/include/crd/net/ingress.hpp),
[io_loop.hpp](../../engine/foundation/net/include/crd/net/io_loop.hpp); 2026-11-08).

- **Two seams.** The loop reads time only through `TransportClock`: monotonic nanoseconds for timers and rates, Unix
  seconds for address validation tokens (`HostClock` maps crd-time's `TickClock` and `WallClock`). Its work is
  `poll()`, one bounded turn that never blocks: at most `batches_per_poll` receive batches, so timers run under a
  flood, then the queued answers and the handler's timers; it returns when it next needs to run. Whoever calls it is
  the executor: `run()` on a thread of its own, waiting in `wait_readable` until then, a frame, a job, or NET-SIM
  stepping virtual time. crd-net takes no job system: crd-jobs would tie Layer 1 to the engine's scheduler (ADR-0035).
- **S1** reads the first packet's form, version, connection IDs, an Initial's token and the packet's extent, nothing
  under protection. Version 1 and 2 IDs above 20 bytes, packets with fewer than 20 bytes from the packet number on
  (RFC 9001's sample) and Lengths past the datagram are dropped; other versions are read by RFC 8999's invariants (IDs
  to 255 bytes) so they can be answered. The QUIC bit is not checked here (RFC 9287). A client Initial under 1200
  bytes or with a Destination Connection ID under 8 bytes is dropped (RFC 9000 sections 14.1 and 7.2).
- **S2** is a table keyed by SipHash-2-4 (`PrfKey`), linear probing at load at most one half with backward-shift
  deletion, its slots allocated once. containers' `HashMap` does not fit: its hasher is stateless and public, it grows,
  and it stops the process when memory runs out, where this table must refuse when full and allocate nothing after
  construction. A coalesced datagram goes where its first packet goes.
- **S3** proves an address by a Retry token (an invalid one is answered with an INVALID_TOKEN close, S43) or by a
  single-use NEW_TOKEN token (an invalid one counts as none). Unproven handshakes pass a
  global and a per-prefix rate (/64 IPv6, /32 or a configured prefix IPv4) and otherwise get a Retry; proven ones pass
  a global rate and charge their prefix. Prefixes share a fixed array of `SharedRateGate`s chosen by a keyed hash, so
  no per-source state exists before validation, a collision only tightens, and a server's loops share one limit.
- **Answers.** Retry and Version Negotiation pass one global rate and answer only datagrams of at least 1200 bytes, so
  both stay below RFC 9000's three times (at most 553 and 129 bytes). Version Negotiation swaps the IDs, sets the
  fixed bit (RFC 7983 multiplexing) and lists one reserved version at a random position; Retry draws a fresh
  16-byte Source Connection ID (the server's default ID length, 8 to 20 configurable). An answer the host cannot take
  waits in a fixed ring; a full ring drops new answers, which the client's retransmission asks for again.

<a id="quic-frames"></a>
### QUIC frames and transport parameters

**S35. Frames and transport parameters** (4.2a, threats T01 and T02;
[quic_frame.hpp](../../engine/foundation/net/include/crd/net/quic_frame.hpp),
[quic_transport_parameters.hpp](../../engine/foundation/net/include/crd/net/quic_transport_parameters.hpp);
2026-11-08).

- **Frames** (RFC 9000 section 19, RFC 9221's DATAGRAM): one flat `QuicFrame` whose views point into the decrypted
  payload, read without allocation. Every section 19 bound is checked on read, ACK ranges included, so nothing read
  later can underflow; a type in a longer encoding than its shortest is PROTOCOL_VIOLATION (section 12.4), an
  unknown type FRAME_ENCODING_ERROR, and a refusal consumes nothing. A run of PADDING is one frame. Writers emit the
  shortest encodings into a span and refuse, writing nothing, what no reader would accept; ACK frames are written from
  descending ranges with their ECN counts. Placement (table 3, NEW_TOKEN and HANDSHAKE_DONE from servers only,
  DATAGRAM in 0-RTT and 1-RTT) is one function.
- <a id="quic-transport-parameters"></a>**Transport parameters** (section 18, RFC 9221, RFC 9287, RFC 9368): read for
  the side that sent them, defaults filled, unknown and reserved ones skipped; TRANSPORT_PARAMETER_ERROR for a
  malformed or repeated parameter, a value outside section 18.2's bounds, a server-only parameter from a client and an
  RFC 9368 parsing failure. Connection IDs are copied; a peer's version list is kept to 16 entries (no deployed set
  comes near it), so the reader allocates nothing. Writers emit only what differs from a default and always one
  reserved parameter (31 * N + 27) and one reserved version, from caller entropy, so peers that do not ignore unknowns
  fail in testing.
- **Checks after the handshake**: section 7.3's connection ID authentication (absence TRANSPORT_PARAMETER_ERROR,
  mismatch PROTOCOL_VIOLATION, a preferred address with zero-length IDs refused), and RFC 9368's compatible
  negotiation: the server's choice among the client's versions that the first flight converts to (versions 1 and 2
  convert both ways), the server's check of the client's chosen version, and the client's downgrade check after
  Version Negotiation, a version 1 server without the parameter read as listing version 1 alone (section 8).

Evidence: `test_quic_frame.cpp` and `test_quic_transport_parameters.cpp` (RFC 9001 appendix A's CRYPTO, ACK and
client parameters byte for byte, every kind round tripped, every truncation and bound, random ACK range sets, the
placement table restated, every refusal and check path), eleven source mutations each caught, and
`crd-fuzz-net-quic-codec` (rewrite and re-read oracles, ACK ranges, the reserved parameter).

<a id="quic-packets"></a>
### QUIC packets

**S36. Packets per encryption level** (4.2a, threats T06, T07 and T09;
[quic_packet.hpp](../../engine/foundation/net/include/crd/net/quic_packet.hpp); 2026-11-08).

- **One writer for every packet.** `QuicPacketWriter` lays the header into the room a datagram has left, the caller
  writes frames into `frames()`, and `seal()` pads with PADDING to the largest of the sample's floor (four bytes from
  the packet number on, RFC 9001 section 5.4.2), the caller's floor (`quic_datagram_floor`: 1200 bytes for a datagram
  carrying an Initial, RFC 9000 section 14.1) and NET.3's `padded_size()` under the channel's block (S30), then writes
  the Length in its shortest encoding (reserved at the room's size and moved down, so a small Handshake ACK costs one
  byte), seals and protects. The QUIC bit is cleared only when the caller says so, from entropy, toward a peer that
  sent grease_quic_bit (RFC 9287); the key phase bit comes from the protection state. Nothing is allocated.
- **Reading trusts nothing before the AEAD.** `QuicDatagramPackets` walks coalesced packets and stops at a packet
  whose Destination Connection ID differs from the first's (section 12.2). `open()` drops a cleared QUIC bit unless
  this end sent grease_quic_bit, removes header protection, decodes the packet number against its space, drops
  duplicates and stale packets (the space's 256-bit window) before the AEAD, and only after it opens checks the
  reserved bits (PROTOCOL_VIOLATION) and refuses an empty packet (section 12.4). A failed open leaves the last 16
  bytes as received, for the connection's stateless reset check; failed opens across all keys meet the AEAD's
  integrity limit or a lower one the transport sets (AEAD_LIMIT_REACHED). Frame placement is checked where frames are
  dispatched, by `quic_frame_allowed` (S35).
- **Keys are discarded in code** (RFC 9001 section 4.9): a client's Initial keys with its first Handshake packet, a
  server's with the first Handshake packet it opens, Handshake keys at `confirm()`, a client's 0-RTT keys when its
  1-RTT keys arrive; a server's 0-RTT keys and the previous 1-RTT receive key go when the connection's timers say
  (three PTOs). `install(handshake)` takes every key `QuicHandshake` has ready after each `provide()`; a Retry or a
  compatible version change re-derives the Initial keys.
- **Key phases** (section 6). Three receive keys rotate as previous, current and next, the next derived ahead so a
  key update costs no timing difference; a packet of the other phase with a number below the current key's smallest
  opens with the previous key, else with the next. An update is initiated only after confirmation, with a packet of
  the current phase acknowledged, the previous key gone and the peer's answer seen; `seal()` starts one by itself at
  three quarters of the key's limit or of the transport's own interval, and a key that reaches its limit unupdated
  refuses (AEAD_LIMIT_REACHED). A peer's update is followed by the send key; a second one before this end sent an
  acknowledgement in the new phase, newer keys on a lower packet number than older keys protected, and a current
  packet below an older key's largest are KEY_UPDATE_ERROR. An older key above the newer key's first packet is tried
  as the next phase's and never opens, so it is dropped rather than detected.

Evidence: `test_quic_packet.cpp` (RFC 9001 appendix A.2, A.3 and A.5 and RFC 9369's packets written byte for byte
from their frames and read back; every padding floor, block and Length size; every writer refusal; coalesced, foreign
and truncated datagrams; each discard; every trust check; key updates initiated, answered, reordered and refused on
each rule; an update interval reaching Exhausted; a real `QuicHandshake` carried in these packets to 1-RTT in both
versions, HANDSHAKE_DONE confirming it) and `crd-fuzz-net-quic-packet` (received datagrams and writer round trips).

<a id="quic-recovery"></a>
### QUIC recovery

**S37. Acknowledgements and loss recovery** (4.2a, threats T06 and T09;
[quic_recovery.hpp](../../engine/foundation/net/include/crd/net/quic_recovery.hpp); 2026-11-08).

- **Receiving** (RFC 9000 section 13.2). `QuicAckTracker` holds a space's received numbers as at most 32 merged
  ranges; when full, the smallest range goes, which is legal because the packet window (S36) refuses any number
  already opened and everything below its span. An ACK is owed at once for an ack-eliciting Initial or Handshake
  packet, a packet out of order or after a gap, a CE mark or the second ack-eliciting packet, otherwise within this
  side's max_ack_delay; non-ack-eliciting packets owe nothing. The ACK frame holds as many of the largest ranges as
  the room takes, the delay since the largest arrived (exponent 3 in Initial and Handshake, whose ACKs precede the
  parameters, this side's own after) and the ECN counts once a marked packet arrived. When a packet carrying an ACK is
  acknowledged, the ranges up to its largest are retired, the largest received number always kept (13.2.4).
- **Sending.** Each space's sent packets sit in number order in caller storage, which bounds what a connection
  holds in flight (`can_send`; a full space refuses, and data stops eight entries short so that ACK-only packets and
  probes can always free the peer's: `can_send_data`). Numbers rise and may leave gaps: a sender skips numbers from
  entropy, and an ACK naming any number it never sent, above the last or in a gap at or above the oldest entry, is
  `AckOfUnsent` (PROTOCOL_VIOLATION, sections 13.1 and 21.4) and changes nothing.
- **Recovery is RFC 9002 appendix A in code.** RTT samples only when the largest acknowledged is newly acknowledged
  and an ack-eliciting packet is among them; min_rtt ignores the delay, the delay is capped at max_ack_delay only
  after confirmation, ignored in Initial, and never takes a sample below min_rtt. Loss by three packets or 9/8 of
  max(smoothed, latest) RTT (at least 1 ms), the remainder setting the loss timer; the probe timeout is smoothed +
  max(4 rttvar, 1 ms), plus max_ack_delay in Application, doubled per expiry and reset by an ACK once the peer has
  validated the address; the Application space waits for confirmation, a client with nothing in flight before
  validation sends an anti-deadlock Initial (Handshake once it has those keys), a server blocked by the amplification
  limit arms nothing until a datagram arrives. Discarding Initial or Handshake keys removes the space's packets from
  flight as `on_discarded`, forgets its ranges and resets the backoff. Persistent congestion is a run of lost
  ack-eliciting packets sent after the first sample with no acknowledged packet between whose send times span more
  than three times (PTO + max_ack_delay).
- **Spurious losses raise the thresholds** (RFC 9002 section 6.1, by RFC 8985 section 6.2's mechanism; 2026-11-08).
  A lost packet stays in its slot for one probe period after its loss, or until a send needs the slot, so a late
  acknowledgement shows the loss was spurious: no event (its loss was reported), nothing back in flight, counted in
  the outcome for a controller's undo, and an RTT sample when it is the frame's largest (appendix A, which forgets
  lost packets, takes none; the send-to-acknowledgement time is still that packet's path). A send needing a slot
  drops held packets from the head only until one is free. The packet threshold rises to
  the reordering seen (the largest acknowledged before the frame, less the packet, plus one), capped at 32; a loss the
  time threshold declared widens the time threshold by a quarter of min_rtt once a round trip, the window capped at
  smoothed_rtt. Both return to RFC 9002's after 16 loss detections with no spurious loss. A peer timing its ACKs to
  fake reordering (T09) gains at most the caps: a loss then waits for 17/8 of an RTT or the probe timeout.
- **Congestion is NET-PATH's.** Controllers read the per-packet events (losses of a detection before the
  acknowledgements of the same frame) and the outcomes' totals; nothing here allocates or reads a clock.

Evidence: `test_quic_recovery.cpp` (25 cases: ranges merged, full and retired, every ACK trigger, frames read back
with trimmed ranges and ECN, the estimator worked by hand, each threshold, the backoff, confirmation, anti-deadlock,
amplification, discards, persistent congestion's three conditions, unsent acknowledgements, full storage and its
ring, spurious losses with their retention, steps, caps and decay), 43 source mutations each caught, and
`crd-fuzz-net-quic-recovery` (a model of every packet sent and received, spurious losses and the thresholds used).

<a id="quic-streams"></a>
### QUIC byte streams and flow control

**S38. Byte streams and flow control** (4.2a, threats T09 and T11;
[quic_stream.hpp](../../engine/foundation/net/include/crd/net/quic_stream.hpp); 2026-11-08). One owner for every
ordered byte stream QUIC carries: each stream's data and each encryption level's CRYPTO data (S11's `provide` takes
it in order; `take` consumes the handshake's copy, so the sent bytes stay here until acknowledged).

- **Receiving** (RFC 9000 sections 2.2, 4.5 and 7.5). `QuicReceiveBuffer` stores data by offset in a window of its
  capacity from the first unread byte, with one bit per byte for what arrived: memory is fixed and exact whatever the
  peer overlaps, leaves out or repeats, and a frame of an acknowledged packet is never dropped. Refused, changing
  nothing: a final size contradicted (a FIN at another size, data past it, a FIN below the data received;
  FINAL_SIZE_ERROR), data past the window (FLOW_CONTROL_ERROR, or CRYPTO_BUFFER_EXCEEDED for CRYPTO data, whose
  window is the handshake's `receive_limit`), and other bytes at an offset already held (PROTOCOL_VIOLATION, the
  "MAY" of section 2.2 taken). A RESET_STREAM's final size is a zero-length FIN at that offset.
- **Sending.** `QuicSendBuffer` keeps written data until acknowledged, with one bit per byte for acknowledged and for
  lost data. The next chunk is the lowest lost run, else data never sent below the flow-control limit, never across
  the storage's end; the FIN rides on the chunk ending at the final size or goes alone (it takes no credit). A loss
  never marks bytes another copy had acknowledged; the room frees as the acknowledged prefix grows.
- **Credit** (sections 4.1, 4.2 and 13.3). `QuicSendCredit` only rises and owes one DATA_BLOCKED or
  STREAM_DATA_BLOCKED per limit while data waits, again if that frame is lost. `QuicReceiveWindow` checks every end
  against the limit, owes an update once half the window was read, when the peer reports itself blocked (only if the
  limit can rise) or when the latest update was lost, and doubles the window up to its maximum when updates come
  within two smoothed RTTs (the auto-tuning of Chromium's QUIC flow controller), so the window follows the
  path's bandwidth-delay product without a configured guess; a stream's maximum is its buffer's capacity. An update
  owed only because the latest was lost goes again unchanged and tunes nothing (a loss says nothing of the reader).
- **Storage.** Capacities are multiples of 64 bytes; the bitmaps are 1/8 of the receive capacity and 1/4 of the send
  capacity, the caller's. Nothing allocates or reads a clock.

Evidence: `test_quic_stream.cpp` (13 cases: storage, order, gaps, duplicates, changed bytes, the window and its wrap,
the final size's three refusals, any order of 50 overlapping streams, room, flow-controlled fresh data, lost data
first and acknowledged copies kept, the FIN sent, lost, resent and acknowledged, both credits, and a 6000-byte stream
carried whole over 20 seeded paths that lose, reorder and duplicate, with auto-tuned updates), 24 source mutations each
caught, and `crd-fuzz-net-quic-stream` (per-byte models of both buffers and the window's bounds).

<a id="quic-connection"></a>
### The QUIC connection

**S39. The connection core** (4.2a, threats T01, T06, T07 and T09;
[quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp); 2026-11-08). One owner for a
connection's state: it carries S11's handshake in S36's packets, settles S37's events and S38's buffers, and is
sans-I/O (`receive`, `send` until 0, `next_timer_ns`, `on_timeout`), so NET-SIM and NET-DEBUG drive it unchanged.

- **Handshake.** Each level's CRYPTO data is reassembled by S38 and provided in order; every key the handshake has is
  installed after each `provide()`. The peer's transport parameters are read and checked the moment they arrive
  (section 7.3's connection IDs, RFC 9368's version_information both ways, after Version Negotiation too), and set
  recovery's max_ack_delay and exponent, the grease bit and the idle timeout. The server confirms on completion and
  sends HANDSHAKE_DONE until it is acknowledged; the client confirms on reading it. Initial keys and their space go
  with a client's first Handshake packet and a server's first opened one, Handshake keys at confirmation.
- **Sending.** One datagram coalesces Initial, Handshake and 1-RTT packets, each with the ACK its space owes or may
  carry. A datagram with a client's Initial, a server's ack-eliciting Initial or a PATH_RESPONSE reaches 1200 bytes,
  the padding in its last packet; a server discards a client Initial in a datagram under 1200 bytes. Before the
  client's address is proven (a token at accept, or its first Handshake packet) a server sends at most three times
  every byte it received, and with less than one useful packet of credit it tells recovery it is blocked, which
  disarms its timer until a datagram arrives. The QUIC bit is cleared per packet from entropy toward a peer that sent
  grease_quic_bit, and one 1-RTT packet number in 64 is skipped, so an ACK naming it closes with PROTOCOL_VIOLATION
  (section 21.4). The entropy is SipHash-2-4 over a counter under a key from the provider, or a configured seed for a
  deterministic run.
- **The frame ledger.** CRYPTO ranges, HANDSHAKE_DONE and RETIRE_CONNECTION_ID frames of each sent packet are records
  in a bounded ring that `QuicSentPacket::token` points into; recovery's events settle them (acknowledged, lost:
  resent, discarded: dropped). A full ring holds those frames back; ACKs and probes still go. A probe sends its
  space's unacknowledged CRYPTO data again, else a PING (RFC 9002 section 6.2.4), and the 16th ACK-only packet in a
  row carries a PING, because a peer need not acknowledge ACK-only packets and they would fill the space's storage
  (RFC 9000 section 13.2.4). An owed ACK counts toward `next_timer_ns()` only where `send()` can write it (keys held,
  space kept, storage free), so a caller's loop never spins on a timer nothing serves.
- **Frames.** Every type is dispatched. Stream and flow-control frames go to S40's stream set, whose frames follow the
  connection's own in 1-RTT packets as records in the same ledger; DATAGRAM frames go to S41's datagrams. The peer's
  connection IDs are held to this side's active_connection_id_limit with Retire Prior
  To honoured, the ID in use moved off a retired one, and retirements owed or in flight bounded by twice the limit
  (CONNECTION_ID_LIMIT_ERROR). A PATH_CHALLENGE is answered; a PATH_RESPONSE (none was sent) is PROTOCOL_VIOLATION, and
  RETIRE_CONNECTION_ID is S42's. A client keeps its server's latest NEW_TOKEN token.
- **Close.** An error closes with CONNECTION_CLOSE at the highest level and, before confirmation, the lower levels
  this side still holds keys for; CRYPTO_ERROR at the alert's level; an application close in Initial and Handshake
  packets becomes APPLICATION_ERROR without its reason. Closing answers datagrams after 1, 2, 4, ... of them for three
  PTOs; a peer's close drains for three PTOs; the idle timeout is the smaller non-zero max_idle_timeout, at least three
  PTOs, restarted by an opened packet and by the first ack-eliciting packet sent after one (section 10.1); `ping()` is
  the keep-alive. A client's Version Negotiation packet that lists its version is ignored, and one that does not ends
  the connection naming the version to retry in.
- **Memory.** Everything comes from the injected allocator at construction; receiving and sending allocate nothing.

Evidence: `test_quic_connection.cpp` (22 cases: both versions to confirmation, nothing allocated after construction;
40 seeded paths that lose 20%, duplicate 10% and reorder; a server held to three times what it received and silent
while blocked; lost first flights and a lost HANDSHAKE_DONE; idle timeout and keep-alives; key updates and the
previous key's discard; closing, draining and an early application close; an alert's CRYPTO_ERROR at its levels;
connection ID and downgrade checks; Version Negotiation; refusals; and, from a raw peer, every refused frame with its
error, CRYPTO past the window, frames out of place, connection IDs with retirement, PATH_CHALLENGE and acknowledgements
of skipped and unsent numbers, a small Initial discarded, ACK-only runs freed by a peer that acknowledges only
ack-eliciting packets, full storage without a due timer; the test network fails any timer that stays due), 27 source
mutations each caught, and `crd-fuzz-net-quic-connection` (network scripts and raw-peer frames in 1-RTT and Initial
packets, with the same stall oracle).

<a id="quic-stream-set"></a>
**S40. The connection's streams** (4.2a, threats T09 and T11;
[quic_stream_set.hpp](../../engine/foundation/net/include/crd/net/quic_stream_set.hpp); 2026-11-08). `QuicStreamSet`
is the one owner of a connection's streams (RFC 9000 sections 2 to 4, 13.3 and 19.4 to 19.14): their states, both
credits and their frames. It knows nothing of packets: the connection asks it for frames and settles their records.

- **Slots.** Each of the four stream types has a fixed pool of slots, each holding S38's buffers and credits, taken
  from the injected allocator once. Open streams of a type are kept in ID order (IDs open in rising order, so an
  append keeps it) and found by binary search; a free stack hands out slots. A peer's frame for a higher stream of
  its own opens every lower one (section 3.2); a frame for a stream already freed is ignored; a stream this side
  never opened is STREAM_STATE_ERROR, as is a frame for the part a unidirectional stream lacks, and one past this
  side's grant is STREAM_LIMIT_ERROR.
- **Limits** (section 4.6). The peer may hold as many streams of a type as this side has slots for it, so its
  implicit opens always find a slot: MAX_STREAMS grants the streams freed plus the slots, owed once half the slots
  were freed since the last grant or at once to a peer that sent STREAMS_BLOCKED. A slot is freed only when both parts
  are terminal and the application polled its events, so a peer can never outrun an application that does not read.
  This side opens within the peer's grant and owes one STREAMS_BLOCKED per limit it reached.
- **Credit** (sections 4.1, 4.2 and 4.5). Every stream's data passes the stream's credit and the connection's, both
  ways, through S38's `QuicSendCredit` and `QuicReceiveWindow`; the connection's sums are running totals (each
  stream's highest offset received, its bytes read, its fresh bytes sent), so freeing a stream changes neither window,
  and a RESET_STREAM's final size counts as received and read. Lost data resent takes no new credit.
- **States** (sections 3.1 to 3.5). A STOP_SENDING resets the send part with the peer's code; a reset stream's final
  size is its bytes sent and its lost data is never sent again; RESET_STREAM and STOP_SENDING go again when lost until
  acknowledged (STOP_SENDING only while the data is still wanted); a lost MAX_STREAM_DATA goes again unless the final
  size is known, a lost MAX_DATA or MAX_STREAMS while it is the latest, a lost blocked frame while that is the limit
  (section 13.3). After STOP_SENDING arriving data is dropped and its credit returned.
- **Reliable resets** (draft-ietf-quic-reliable-stream-reset-11, parameter 0x1d, frame 0x24, 2026-11-09). A reset may
  keep the stream's first bytes, toward a peer that advertised it. In the machinery it is a FIN at a smaller offset:
  the kept bytes are sent and resent as data before a FIN is, what lies above leaves the buffer and is never sent
  again, and the part ends when the frame with the current size and those bytes are acknowledged. The frame waits until
  the kept bytes have all gone once, so its final size lies within both credits with no separate check. A second call,
  or a STOP_SENDING, only lowers the size; the code and final size never change. The receiver offers bytes up to the
  smallest size named, never `fin`, and shows the reset when they are read; a reordered larger size is ignored. A size
  may stop inside a frame, so a framing layer told `reset_coming` gives the bytes up instead of waiting. It is in the
  early context, so a server cannot withdraw it under a ticket. QMux has no such frame and refuses the option.
- **Sending.** `next_frame()` hands the connection one frame that fits its room with the record that settles it:
  MAX_DATA, MAX_STREAMS, STREAMS_BLOCKED and DATA_BLOCKED first, then the listed streams by RFC 9218's urgencies
  (0 first, 3 by default) and round robin within one, each stream's control frames before its data. A stream is
  listed while it owes anything; `has_frame()` is exact (a credit-starved stream is listed but reports nothing), so a
  connection never spins on work it cannot write. Stream frames carry an explicit Length, so the packet's padding can
  follow them.
- **Application.** `open`, `write` (with the FIN), `reset`, `stop_sending`, `set_urgency`, and a zero-copy read:
  `peek()` gives the bytes that arrived without a gap as a view of the stream's buffer, `consume()` gives them up.
  Events are a per-slot mask (opened, readable, writable, reset, stop asked, all sent) in a queue of slots: at most one
  entry per slot, so it never fills and nothing is dropped.
- **The connection.** Its limits are `QuicConnectionConfig::streams`, from which it fills the transport parameters;
  the peer's arrive with the handshake. Its frame ledger now records the stream set's frames too, eight records per
  packet by default; a full ledger holds frames back and drains as packets settle.

Evidence: `test_quic_stream_set.cpp` (14 cases, every frame crossing as bytes: the limits and parameters; opening
within the grant with STREAMS_BLOCKED once per limit and again when lost; every stream frame error; implicit opening
through each of the five frame kinds; both credits with one blocked frame each per limit; lost credit frames by
section 13.3; MAX_STREAMS following the freed slots only after the events were polled; a reset mid-transfer with its
lost data not resent; STOP_SENDING answered with the peer's code and resent; the FIN alone, reads across the buffer's
end and consumption bounds; urgencies with round robin; each kind of stream on its own peer grant; writable and
readable events only when room or data is at the front; DATA_BLOCKED only when the connection holds data back; a
blocked peer's grant at once; 24 streams echoed both ways over 12 seeded exchanges that lose 15%, duplicate 8% and
reorder), `test_quic_connection.cpp` (streams echoed both ways over 16 seeded paths in both versions that lose 10%,
duplicate 5% and reorder, nothing allocated after construction; a 12-record ledger under the same load), 35 source
mutations of which 34 are caught (the survivor is the recovery of a sized frame that fails to fit, unreachable by
`next_frame`'s contract; two conditions the first pass showed redundant were removed),
`crd-fuzz-net-quic-stream-set` (two ends under any application calls, deliveries, losses and injected frames:
`has_frame()` exact, every byte read the byte written) and `crd-fuzz-net-quic-connection`'s scripts now ending with
a stream carried whole.

<a id="quic-datagrams"></a>
**S41. The connection's datagrams** (4.2a, threats T09 and T11;
[quic_datagram.hpp](../../engine/foundation/net/include/crd/net/quic_datagram.hpp); 2026-11-08). `QuicDatagrams` is
the one owner of a connection's unreliable datagrams (RFC 9221 sections 3 to 5): both rings, the negotiated sizes and
the outcomes. Like S40 it knows nothing of packets.

- **Negotiation** (section 3). `QuicDatagramLimits::max_frame_bytes` is the max_datagram_frame_size this side
  advertises, counting the type, the Length and the payload; the receive ring must hold the largest payload it admits
  (one byte less, in a frame without a Length). A DATAGRAM frame when none was advertised, or one larger, is
  PROTOCOL_VIOLATION. This side sends only to a peer that advertised a size.
- **The largest payload** is fixed when the peer's parameters arrive: the largest whose frame with its Length fits
  both the peer's size and one 1-RTT packet of the path (min of this side's max_datagram_bytes and the peer's
  max_udp_payload_size) less the short header's worst case (a 20-byte connection ID, a 4-byte packet number) and the
  tag. So a peer's longer connection ID later can never strand a queued datagram. `queue()` refuses a larger payload
  at once (TooLarge), as it refuses before the parameters (NotReady), toward a peer that takes none (Unsupported) and
  into a full ring (Full): frames are never split.
- **Rings.** Both are byte rings of payloads in arrival order, each payload contiguous: one that would pass the end
  starts the next lap, and an empty ring restarts at 0, so a payload of the ring's size always fits an empty ring.
  The application reads in place (`peek`, `pop`); a full receive ring drops and counts (section 5 lets a receiver
  drop). Both come from the injected allocator at `init()`.
- **Sending.** 1-RTT packets carry the connection's own frames and CRYPTO, then datagrams, then stream data: latency
  first. Each frame carries its Length, so a packet's padding can follow it; one that does not fit the room left
  waits for the next packet. An owed ACK goes first; an ACK only carried along follows the waiting datagrams, so a
  datagram of the largest size goes even while every packet carries an ACK. A DATAGRAM frame is ack-eliciting and
  congestion controlled, never resent and takes no flow-control credit. A datagram queued longer than `max_age_ns` is
  dropped unsent.
- **Outcomes** (section 5.2, optional per connection). Each sent datagram's id is a ledger record; acknowledgement
  reports Acked (the peer's transport has it), loss or discard Lost, expiry Expired, each once. A datagram counts
  against `send_count` until its report is polled, so the report ring never fills; a full ledger holds datagrams back.
- **0-RTT** ([S45](#quic-early)): a client sends datagrams in 0-RTT under the max_datagram_frame_size its ticket
  remembers and closes an accepted connection whose server advertises less (section 3); a server's early data
  context names its size.

Evidence: `test_quic_datagram.cpp` (7 cases: the payload limit against every frame size to 70,000 bytes; every limit
refused and init all or nothing; both parameters bounding queue and write, a room one byte short; a received frame at
and past the size in both encodings, a full ring dropping and freed, an empty payload; a wrap that skips the tail and
16 seeded runs against a model; expiry with and without outcomes; every outcome once with send_count held until
polled), `test_quic_connection.cpp` (7 cases: datagrams both ways beside 12 echoed streams over 12 seeded paths in
both versions that lose 10%, duplicate 5% and reorder, each id arriving at most once byte-exact, each reported once,
an acknowledged one arrived, nothing allocated after construction; the largest datagrams both ways beside streams;
the largest one in the first packet beside a carried ACK and after an owed one, with a 20-byte connection ID; the
smaller of both sides' UDP payload sizes, every datagram acknowledged on a clean path; a 12-record ledger; a peer
without datagrams; frames at and past 100 bytes from a raw peer, and none advertised), 30 source mutations of which
29 are caught and one does not compile (the first pass left five: three became tests, two were redundant code now
removed), `crd-fuzz-net-quic-datagram` (a sender and a receiver against a model of every id's fate) and
`crd-fuzz-net-quic-connection`'s scripts now ending with datagrams carried and reported.
An outcome reports loss detection's verdict: on a path that reorders past RFC 9002's thresholds a datagram reported
Lost may still arrive.

<a id="quic-connection-ids"></a>
**S42. This side's connection IDs and stateless reset** (4.2a, threats T01, T07 and T10;
[quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp),
[ingress.hpp](../../engine/foundation/net/include/crd/net/ingress.hpp), crd-crypto's `QuicResetKey`; 2026-11-08).
Sources: RFC 9000 sections 5.1.1, 5.1.2, 10.3 to 10.3.3, 19.15 and 19.16.

- **Tokens.** A token is the first 16 bytes of HMAC-SHA256(static key, connection ID) (section 10.3.2's example),
  in crd-crypto beside the address validation tokens. A server that lost a connection, or another server of its
  fleet sharing the key, derives it from a packet's Destination Connection ID alone. IDs are drawn from at least 128
  random bits (`QuicRoutedIds`, 64 with an 8-byte ID), so no ID repeats under one key.
- **Issuing.** The host (`QuicConnectionIds`) routes each ID and derives its token; `QuicRoutedIds` does so over the
  loop's `ConnectionIdTable`. After confirmation the peer holds its active_connection_id_limit of this side's IDs
  (at most 8, sequence 0 counted). NEW_CONNECTION_ID frames are ledger records resent until acknowledged, a rotated
  ID's included. A server sends sequence 0's token as stateless_reset_token; a client sends none. With zero-length
  IDs nothing is issued.
- **Retiring.** A RETIRE_CONNECTION_ID for a sequence never issued, for the ID its packet was sent to, or toward
  zero-length IDs is PROTOCOL_VIOLATION. Otherwise the ID goes back to the host and `send()` issues a replacement.
  `rotate_connection_ids()` issues a full new set with Retire Prior To at its first sequence. The earlier IDs stay
  routed until the peer retires them, and no second rotation starts before then (section 5.1.2's SHOULD NOT). A
  frame's Retire Prior To never passes its own sequence. Closing hands every ID back, sequence 0 included.
- **Detecting.** The trailing 16 bytes are copied before packets open in place. When the first packet does not
  open (section 10.3.1's MUST) and the datagram has at least 21 bytes, they are compared in constant time with the
  token of every peer ID this side has sent to and not retired; none is skipped once one matches. A match drains the
  connection, from Closing too (keeping the close it sent). The host calls `stateless_reset()` for a datagram from
  the peer's address that routes nowhere. Every datagram reaches the ID length plus 22 bytes, so a peer's reset of it
  can look like a packet (section 10.3).
- **Sending.** The ingress answers a short header for no connection with a reset when it holds a key and the answer
  rate allows: one byte shorter than a trigger of 43 bytes or fewer, else a random length from 43 to 128, always
  shorter than the trigger and at least 21 bytes, so a reset loop shrinks to nothing (section 10.3.3). Its fixed bits
  are 01 and the rest is random. 0-RTT and Handshake packets for no connection are never reset: the client has no
  token yet (0-RTT goes to the server's held ring, [S46](#quic-server)). The I/O loop sends resets through the same
  answer ring as Retry, after the handler checked the datagram as its own peer's reset.
- **Cost under abuse (T10).** A retirement names an ID the peer has learnt, so a peer can force at most one HMAC, one
  random draw and one table insert per ID per round trip, bounded by its limit (at most 8). No cap on the total IDs
  issued is needed. Tokens are kept per connection, not per remote address: one path exists until NET-PATH adds more.

Evidence: `test_quic_token.cpp` (independent HMAC vectors, a shared and a fresh key, every refusal zeroing the
output), `test_ingress.cpp` (sizes over nine trigger lengths 64 times each, the spread of lengths, the token and
fixed bits, keyless, routed and rate-limited cases), `test_io_loop.cpp` (resets sent by the loop), and
`test_quic_connection.cpp` (4 cases):
- both ends at the peer's limit over 12 seeded paths that lose 20% and duplicate 10%, with the host's tokens, the
  padding and every ID handed back on close;
- two rotations, with packets routed to the new IDs and the retired token ignored;
- replacement and both refusals from a raw peer (also after an earlier retirement in the same packet), and a host
  that routes nothing (the refusal toward zero-length IDs needs a raw server and is untested);
- resets ignored when too short, for an unused ID or with a forged token, a reset at 21 bytes, from Closing, and
  from a restarted server's ingress.

Source mutations, all caught, are counted in the session note. Fuzzing: `crd-fuzz-net-ingress` holds every reset to
the size and token rules. `crd-fuzz-net-quic-connection` holds each host's table to the IDs its connection holds
after every step, rotates on a script byte, and ends confirmed runs with a forged and a real reset.

<a id="quic-retry"></a>
**S43. Retry and address validation tokens on the connection** (4.2a, threats T01, T02 and T10;
[quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp),
[quic_recovery.hpp](../../engine/foundation/net/include/crd/net/quic_recovery.hpp),
[ingress.hpp](../../engine/foundation/net/include/crd/net/ingress.hpp); 2026-11-08). Sources: RFC 9000 sections 7.2,
7.3, 8.1.2, 8.1.3, 17.2.5.1 to 17.2.5.3 and 19.7; RFC 9001 sections 5.2 and 5.8; RFC 9002 sections 6.2.2 and 6.3.

- **A client's Retry.** One is taken per connection attempt, only before any packet of the server opened, only to
  the ID the client sent from, only with a token, only when its Source Connection ID differs from the ID the client
  sends to, and only when its integrity tag verifies against the first Destination Connection ID. Anything else is
  dropped and counted. Taking it re-derives the Initial keys from the new ID, sends every later Initial there with
  the token, and continues the packet numbers. Recovery starts over: packets in flight leave as discarded (no
  congestion signal), timers and the probe count reset, and the Initial's CRYPTO data goes again. NET-PATH's
  congestion controller must reset at the same call (RFC 9002 section 6.3). When no probe was
  sent, the time from the first flight to the Retry becomes the initial RTT estimate (not a sample). The server's
  next Initial still sets the ID the client sends to (section 7.2). Its retry_source_connection_id must name the
  Retry's ID, and a server that names none closes the client with PROTOCOL_VIOLATION (section 7.3).
- **Tokens a client carries.** Up to kQuicMaxCarriedTokenBytes (256): this build's tokens and longer ones from
  other servers. A longer Retry token is dropped, because the client could not send it. A server Initial carrying a
  token is dropped (section 17.2.2).
- **A server's NEW_TOKEN.** The host mints the token once the connection's version is final
  (`QuicConnection::give_new_token` after `peer_parameters_known()`, from `QuicTokenKeys::mint_new_token` for
  `version()` with the client's address and the fleet clock: a token belongs to the version its connection ran,
  [S44](#quic-version-switch)). The connection sends it once the handshake is confirmed and keeps it as a ledger
  record, resent on loss and on an Application probe until it is acknowledged. A client keeps the latest token for its next connection, where the ingress takes it once (its
  replay filter) as proof of the address.
- **INVALID_TOKEN.** A Retry token that does not validate (forged, expired, another key, port or ID) is answered,
  within the answer rate, with one server Initial: sealed under the keys of the Destination Connection ID the client
  chose, sent to its Source Connection ID, holding CONNECTION_CLOSE with INVALID_TOKEN. The client then fails within
  a round trip instead of a handshake timeout (section 8.1.2's SHOULD). The close is not ack-eliciting, so it is not
  padded, and it is smaller than the 1200-byte Initial that triggers it. The packet is not opened first: Initial keys
  are public, so opening would prove nothing about the sender, and the answer rate bounds the work (one key
  derivation and one seal). With the rate spent, the Initial is dropped.

Evidence: `test_quic_recovery.cpp` (the estimate, after a probe, before anything was sent), `test_ingress.cpp` (three
invalid tokens opened as the client would, the spent rate, an answer too small), `test_io_loop.cpp` (the loop sends
the close), and `test_quic_connection.cpp`:
- seven refused Retries;
- an accepted one in both versions with a 256-byte foreign token, through to confirmation;
- a server whose parameters name no Retry;
- a Retry after the server's first packet;
- a server Initial with a token;
- behind a Retry-everything ingress over clean and seeded paths that lose 15%, duplicate 5% and jitter, the
  NEW_TOKEN reaching the client and proving its next connection once, its replay sent to Retry;
- a rotated token key ending the client with INVALID_TOKEN within a round trip.

Fuzzing: `crd-fuzz-net-ingress` opens every INVALID_TOKEN close as the client would (a new seed carries a forged
Retry token). `crd-fuzz-net-quic-connection` has a script byte (0xFD) that answers the client's first flight with a
Retry, and holds a confirmed pair to agree on it.

<a id="quic-version-switch"></a>
**S44. Compatible version negotiation on the connection** (4.2a, NET-STD's `quic.version-negotiation`, threat T01;
[quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp),
[quic_packet.hpp](../../engine/foundation/net/include/crd/net/quic_packet.hpp),
[tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp); 2026-11-08). Sources: RFC 9368 sections 2 to 4
and RFC 9369 sections 4, 4.1 and 5, read from the RFC Editor's text.

- **Where the server chooses.** RFC 9369 section 4.1 forbids any CRYPTO frame before the client's parameters are
  read and requires every CRYPTO frame in the negotiated version, and the server's own parameters must name that
  version. The TLS provider reads the whole ClientHello and writes EncryptedExtensions in one `provide()`, so the
  choice is made inside it: `crypto::QuicHelloAnswer`, asked once from the provider's select-certificate callback,
  which runs on the first ClientHello (the inner one under ECH, before HelloRetryRequest), before the extensions are
  parsed and before any secret exists. The connection's answer reads the client's parameters, picks the first of
  its versions that the client lists and its first flight converts to (`quic_server_select_version`), switches its
  keys and returns its parameters re-encoded with that chosen version. Parameters that do not parse or name another
  chosen version change nothing: `read_peer_parameters()` closes with TRANSPORT_PARAMETER_ERROR or
  VERSION_NEGOTIATION_ERROR after `provide()` returns, in the original version (never the provider's
  handshake_failure). A key that cannot be derived fails the handshake.
- **Keys.** `QuicPacketProtection` keeps three Initial keys: the send key and the receive keys of the running version
  and of one other. `change_version()` re-derives the send key and the running receive key and keeps the old receive
  key, so that the client's Initial packets still in the original version open on the server until the first
  Handshake packet ends every Initial key (the "MUST NOT discard" of section 4.1). Handshake and 1-RTT keys are the
  negotiated version's; 0-RTT keys stay in the original version. Both changes are refused once a Handshake key
  exists. A Retry re-derives all of them for its ID; a Retry is always in the original version.
- **How a client learns.** A client whose versions list a compatible one derives that version's server Initial key at
  start (`expect_version()`). The first server Initial in it that opens switches the connection (section 4.1's
  mechanism: the first long header in another version). A server CRYPTO frame in the original version settles the
  original first, and a CRYPTO frame in the original version after a switch closes with PROTOCOL_VIOLATION. Afterwards
  a client still opens the server's original-version Initial packets (acknowledgements sent before the switch),
  and both sides drop Handshake and 1-RTT packets of any other version. An attacker on the path can forge the switching
  Initial (Initial keys are public), but the server's authenticated chosen version then differs from the one the
  client switched to and the client closes with VERSION_NEGOTIATION_ERROR: no worse than the forged Initial close
  that is possible today.
- **Downgrade checks.** The server checks that the client's chosen version is its first flight's. The client checks
  that the server chose the version it switched to and, after a Version Negotiation packet, that it would have
  attempted the same version from the server's list plus the negotiated one. The comparison is with the attempted
  version, not the negotiated one (figure 1 of RFC 9368: attempted after Version Negotiation, then switched).
  Before this slice the two were always equal.
- **Tickets and tokens** (section 5). `QuicHandshake::set_version()` puts the version into the QUIC binding, the
  session ID context of both ends. A client offers only tickets of the version it starts in. A server rebinds in its
  answer, before the offered ticket is checked, so it resumes only a ticket of the version it runs. A ticket issued
  after a switch belongs to the negotiated version, and the client rebinds when it learns. A client holding a
  version 2 ticket starts in version 2 (section 6), and 0-RTT therefore never meets a switch. A server's NEW_TOKEN
  token is minted after the version is final (`give_new_token()`). The ingress validates a token against the
  version of the Initial carrying it.

Evidence: `test_quic_handshake.cpp` (the answer sees the client's parameters exactly, its parameters reach the
client, it is asked once across a HelloRetryRequest, an oversized answer fails, tickets resume only in their
version in both directions of a switch), `test_quic_packet.cpp` (both versions both ways, the late Initial packets,
a version neither key opens, Handshake keys of the wrong version, the discard, 0-RTT in the original version, a
Retry's other key, and a real handshake's 1-RTT key held to the key log's secret keyed apart in each version),
`test_quic_transport_parameters.cpp` (figure 1's check), and `test_quic_connection.cpp`:
- a switch both ways on a clean path and twelve seeded paths that lose 15%, duplicate 5% and jitter, where no long
  header returns to the original version once the negotiated one appears;
- no switch with a client that offers one version, a server that prefers the original or runs only it;
- two servers answering one first flight: a client settled in version 1 drops the switching server's packets,
  and a switched client closes on the other's version 1 ServerHello;
- client parameters that do not parse or name version 2 in a version 1 Initial;
- behind a Retry-everything ingress, a switch after the Retry, whose NEW_TOKEN token proves a connection that
  starts in the negotiated version and not one that starts in the original version.

Fuzzing: `crd-fuzz-net-quic-connection`'s byte 0 bit 3 makes the client offer both versions and the server prefer
the other, and a confirmed pair must run it (seeds `script-switch` and `script-switch-retry`); `crd-fuzz-net-quic-codec`
checks the client's verdict against both attempted versions.

<a id="quic-early"></a>
**S45. 0-RTT and packets held for their keys** (4.2a, threats T07 and T10;
[quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp),
[quic_stream_set.hpp](../../engine/foundation/net/include/crd/net/quic_stream_set.hpp),
[quic_datagram.hpp](../../engine/foundation/net/include/crd/net/quic_datagram.hpp); 2026-11-08). Sources: RFC 9000
sections 5.2.2, 7.4.1, 13.2.6 and 17.2.3, RFC 9001 sections 4.1.4, 4.6, 4.9.3 and 5.7, RFC 9002 section 6.4 and
RFC 9221 section 3, read from the RFC Editor's text. The TLS side is S17's.

- **Client.** When the handshake offers early data, the connection decodes the ticket's remembered server
  parameters and grants its streams and datagrams from them alone (never ack_delay_exponent, max_ack_delay, the
  connection IDs or the reset token), installs the 0-RTT key and sends Application data in 0-RTT packets of the
  original version until it knows its server's parameters (section 7.4.1's "only remembered" rule) or has 1-RTT keys,
  whichever comes first. A 0-RTT packet carries no ACK, CRYPTO, HANDSHAKE_DONE, NEW_TOKEN or PATH_RESPONSE frame
  (table 3), and the client has nothing to retire before its server's IDs arrive. The stream set's `set_peer_parameters()` only raises, the open streams' credit included,
  so the server's parameters replace the remembered ones without a second state.
- **Decision.** Accepted: the server's parameters must lower none of the limits the 0-RTT kept to (the seven of
  section 7.4.1 and max_datagram_frame_size), else PROTOCOL_VIOLATION (RFC 9221 section 3's error). Rejected (known
  from the server's hello or EncryptedExtensions, before any 1-RTT packet is sent): the 0-RTT keys go, recovery drops
  every outstanding Application packet without a congestion signal (`QuicRecovery::reject_early`, RFC 9002 section
  6.4), the stream set returns to `init()`'s state and the queued datagrams are reported Lost (RFC 9001 section
  4.6.2); `handshake().early_data()` tells the application, which sends again under the server's parameters.
- **Retry.** A Retry discards every packet (S43); a discarded stream frame now goes again like a lost one, so the
  0-RTT data is resent to the Retry's ID with fresh packet numbers (sections 17.2.3 and 17.2.5.3).
- **Server.** A server whose context accepts early data names its early data context: `write_quic_early_parameters`
  (a format byte and eleven fixed u64 values: the remembered limits, max_idle_timeout, max_udp_payload_size and
  disable_active_migration, no grease) followed by the application's part, so the provider refuses a ticket's
  0-RTT once any of them changed and acceptance never meets a lowered limit. It opens 0-RTT packets only after it
  accepted; once the ClientHello is read without acceptance it discards the Early level (section 4.6.2's "MUST NOT
  process"). The 0-RTT keys go three PTOs after the first 1-RTT packet (section 4.9.3). No 1-RTT packet opens before
  the handshake completes (section 5.7: the provider installs the client's 1-RTT key with its Finished).
- **Replayable bytes are marked** (T07). Each stream keeps the highest end that came in a 0-RTT packet;
  `QuicStreamRead::early_bytes` counts the bytes of a read below it (conservative: a 1-RTT copy of an early byte
  stays marked), and `QuicDatagrams::oldest_early()` marks a 0-RTT payload. The application runs on them only what
  its schema declares idempotent and checks capability tokens with `verify_early()` against `handshake()`.
- **Held packets** (section 4.1.4). A packet whose keys may still come (`QuicPacketProtection::keys_pending`: a
  server's 0-RTT before its ClientHello is complete, Handshake and the first 1-RTT keys) is copied into a ring of
  `held_packets` slots of max_datagram_bytes (default 8, at most 64, allocated at construction). After each datagram
  the ring is read until no held packet opens; one whose keys went is dropped. A full ring drops the packet (the
  peer sends it again), so a peer can make a connection hold at most the ring (T10). 0-RTT before the Initial that
  creates the connection is held by the server behind the ingress ([S46](#quic-server)).
- **Padding.** A datagram that must reach 1200 bytes is padded by its last packet, or by an earlier one that leaves
  too little room for the next (a 0-RTT packet behind a full Initial), so the floor holds whatever fits.

Evidence: `test_quic_connection.cpp` (6 cases): 0-RTT accepted in both versions with the stream read and the
datagram held at the server before its handshake completes, every byte marked, no 1-RTT packet opened early, the
datagram acknowledged, later data unmarked and the keys gone three PTOs after; twelve seeded paths losing 10%,
duplicating 5% and jittering 20 ms with every byte delivered once and packets held; rejections by a server without
early data, another application context, a lowered stream window and a version switch, each resetting the client's
streams, reporting both datagrams Lost and leaving the server without any, then the data resent unmarked; 0-RTT
before a Retry resent after it over clean and lossy paths; held 0-RTT packets read after the Initials and the server's
reversed flight completing the client's handshake with no time passing, a ring of one dropping the rest; a rejecting
server dropping what it held; and the configuration refusals. `test_quic_stream_set.cpp`, `test_quic_datagram.cpp`,
`test_quic_recovery.cpp` and `test_quic_transport_parameters.cpp` hold the raising, the reset, the marks, the Lost
reports, the recovery drop and the context and check functions. `crd-fuzz-net-quic-connection` gains a 0-RTT bit
(byte 0, bit 4: a first connection leaves a ticket and the scripted one resumes with a stream and a datagram
written before its first flight) and three seeds; its oracles hold the decision alike on both ends, the early stream
whole and marked no further than it came, and a rejection's reset.

<a id="quic-server"></a>
**S46. The server behind the ingress** (4.2a, threats T01, T02 and T10;
[quic_server.hpp](../../engine/foundation/net/include/crd/net/quic_server.hpp),
[io_loop.hpp](../../engine/foundation/net/include/crd/net/io_loop.hpp),
[ingress.hpp](../../engine/foundation/net/include/crd/net/ingress.hpp); 2026-11-08). Sources: RFC 9000 sections
5.2.2, 7.2, 8.1.3, 9, 10.2.3 and 10.3, RFC 9001 section 4.1.4, read from the RFC Editor's text.

- **The loop's handler.** `QuicServer` is the `IngressHandler` S34 names. The handler gains `on_early` (the ingress's
  new `Early` action: 0-RTT for no connection under a Destination Connection ID of at least 8 bytes, unanswered and
  uncharged) and `on_unrouted` (a short header for no connection, offered before the ingress's own stateless reset).
  The loop's answer ring became `SendQueue`, which the server reuses for its connections' datagrams: written in place,
  held while the host is Busy, a hard refusal dropping that datagram alone.
- **Admission.** A fixed pool (`connections`, raw storage for the non-movable connection) with a free list. An admitted
  Initial takes a slot: a fresh sequence 0 ID routed through the slot's `QuicRoutedIds`, the client's Destination
  Connection ID routed to the slot for the connection's life (a late copy of the first Initial reaches it and is
  dropped there instead of opening a second connection), the connection built in the Initial's version, `accept()`
  with what the ingress proved and the datagram. A full pool or a connection that refuses to start is answered with
  CONNECTION_REFUSED in the INVALID_TOKEN close's form (`Ingress::refuse`, one writer for both, within the answer
  rate). Building the connection allocates its buffers from the injected allocator; nothing else does.
- **0-RTT ahead of its Initial** (section 5.2.2). A ring in arrival order of `early_datagrams` slots of `early_bytes`,
  at most `early_per_client` per Destination Connection ID and address, each kept `early_lifetime_ns` (100 ms), the
  oldest evicted when full; the admitted connection reads its own after its Initial. After a Retry the ID is the
  Retry's, so nothing is special-cased. A peer can make the server hold at most the ring (T10).
- **Serving.** Routed datagrams are read at once from the connection's own address only; another address is a
  migration or a NAT rebinding, which NET-PATH validates (section 9), so it is dropped and counted and every
  connection's parameters carry disable_active_migration until then. A connection that received, timed out or was
  woken is served once per turn from a binary min-heap of deadlines: timeout, the application's `on_service`, its
  NEW_TOKEN token once its version is final (minted for its address, section 8.1.3), then its datagrams until it has
  none or the queue is full and the host busy (served again within `send_retry_ns`). A Closed connection is reported,
  destroyed (its IDs leave the table) and its slot reused under a new generation, so stale handles find nothing.
- **A peer's stateless reset** (section 10.3.1). The server keeps a keyed index from a peer's address and port to the
  first of its connections; a short header for no connection from that address is checked against the IDs the
  connection used, and a match drains it unanswered.

Evidence: `test_quic_server.cpp` (7 cases): 16 clients in both versions over clean, lossy (4%, 3% duplicated, 8 ms
jitter) and busy-host paths, each echoed four streams and given a NEW_TOKEN token, no allocation while serving, and
every route gone once they close; a resumed client's 0-RTT arriving 20 ms before its Initial held and read before the
handshake completes in both versions, with and without a Retry-everything ingress, the NEW_TOKEN token proving the new
port's address; a full pool's CONNECTION_REFUSED read by the client, the slot reused and the old handle stale; another
address dropped, a late first Initial opening nothing, a client's stateless reset draining its connection unanswered
and the same from another address answered by the ingress; the ring's share, eviction, size limit and expiry; every
configuration refusal and allocation refusal; and a client served over the host's loopback. `test_io_loop.cpp` and
`test_ingress.cpp` hold the two hooks and the Early action; `crd-fuzz-net-ingress` holds Early and the refusal close.

<a id="quic-client"></a>
**S47. The client's host** (4.2a, threats T01, T02 and T10;
[quic_host.hpp](../../engine/foundation/net/include/crd/net/quic_host.hpp),
[quic_client.hpp](../../engine/foundation/net/include/crd/net/quic_client.hpp),
[ingress.hpp](../../engine/foundation/net/include/crd/net/ingress.hpp); 2026-11-08). Sources: RFC 9000 sections
5.2.1, 6.2, 7.2, 8.1.3 and 10.3, RFC 9368 sections 2.1 and 4, RFC 9369 section 5, read from the RFC Editor's text.

- **One host, two sides.** What S46's server did beyond admission is now `QuicHost`, which both hosts derive: the pool
  with its free list and generations (`QuicHandle`), routing from the peer's address only, the deadline min-heap, the
  `SendQueue` drain with the busy-host retry, the peer index for stateless resets and the close. The connections'
  storage holds one cell more than the pool, the spare a connection is built in before it goes live, so a successor
  can be built while its predecessor stands. `QuicServer` keeps admission, the held 0-RTT ring and NEW_TOKEN minting.
- **A client's ingress.** `Ingress`'s second constructor runs S1 and S2 alone over the loop's table: everything whose
  Destination Connection ID routes goes to its connection, Version Negotiation and Retry among them (they carry the ID
  the client chose), which judges them; 0-RTT and long headers of versions the client does not run are dropped; it
  admits and answers nothing (`answer()` and `refuse()` write nothing). So the client runs the same `IoLoop`, batches
  and executor seam as the server.
- **A short stateless reset.** A reset can be 21 bytes, shorter than any packet the receiver would open; the loop now
  offers a short header dropped as too short to protect to `on_unrouted` as well (both hosts), never answering it,
  and a datagram under 21 bytes is never a reset. The hosts find its connection by its token (S48).
- **Connecting.** `QuicClient::connect()` takes a slot, issues a sequence 0 ID of the ingress's length (8 to 20
  bytes, so many connections to one or many servers share one socket), draws a 16-byte first Destination Connection
  ID from the provider (section 7.2) and starts the connection, optionally under another TLS context (a server name
  and pins per connection); the first flight leaves in the next turn, after the application's first `on_service`, so
  a resumed connection's 0-RTT is written there.
- **Tokens.** A NEW_TOKEN token is remembered per server address, port and the version the connection ran (RFC 9369
  section 5) in a fixed table, the oldest replaced, and carried by the next connection there once (section 8.1.3: a
  client does not reuse a token); an application may pass a token of its own (an earlier process's).
- **Version Negotiation** (RFC 9368 section 4). A connection closed by a Version Negotiation that offers a version of
  the client's preference is replaced under the same handle by one in that version, with after_version_negotiation
  set so the downgrade check runs; `on_restart` tells the application its writes are gone. One that offers nothing the
  client runs, or a second one, ends the connection.

Evidence: `test_quic_client.cpp` (8 cases): twelve connections over one socket to two servers in both versions over
clean, lossy (4%, 3% duplicated, 8 ms jitter) and busy-host paths, echoed, given tokens, served with no allocation
and closed with nothing routed; Version Negotiation followed into version 2, a second one and one listing nothing the
client runs ending it; a Retry taken and its NEW_TOKEN token proving the next connection once in both versions;
0-RTT written in the first service and accepted; the server's stateless reset honoured only from its address; another
address dropped; the pool's bounds, stale handles, a TLS context per connection, every configuration and allocation
refusal; and a client host on the host's loopback. `test_quic_server.cpp` runs unchanged on `QuicHost`;
`test_io_loop.cpp` holds the short reset; `crd-fuzz-net-ingress` runs every input through a client's ingress too.

<a id="quic-reset-index"></a>
**S48. A reset found by its token** (4.2a, threat T10;
[quic_host.hpp](../../engine/foundation/net/include/crd/net/quic_host.hpp); 2026-11-09). Source: RFC 9000 sections
10.3.1, 10.3.2 and 21.11. An index of a peer address and its first connection left a client's second connection to
one server, or a server's second connection from one address, unable to be reset, and its 22-byte IPv6 key was over
`ConnectionIdTable`'s 20 and never inserted, so no IPv6 peer's reset was found (5ca261ef). The hosts now hold one table
from a live token to its slot, the connection's `watch_reset`/`unwatch_reset` feed it through `QuicConnectionIds`: a
token is live once it is known and its ID has been sent to (the server's first token arrives after the client's first
packet to that ID), and ends with the ID's retirement or the connection's close. Capacity is
`connections * kQuicMaxPeerConnectionIds`. The table's secret SipHash key compares bytes only after a 64-bit keyed hash
matched, so the walk says nothing of a token's value; the connection then compares in constant time and the host
requires the token's connection's peer address. A token repeated across connections stays its first holder's, which
only a peer repeating its own tokens can cause. One token under two IDs of a connection is PROTOCOL_VIOLATION (section
10.3.2's MAY), so a connection never holds the same live token twice.

Evidence: `test_quic_client.cpp` (three connections to one server, the reset ending the one it names),
`test_quic_server.cpp` (an IPv6 peer with a zone, a retired token and a token from another address ending nothing),
`test_quic_connection.cpp` (exactly when the host learns and forgets a token; one token under two IDs; the first
holder's table entry).

<a id="ticket-cache"></a>
**S49. The client's ticket cache** (4.2a, threats T07 and T15;
[ticket_cache.hpp](../../engine/foundation/net/include/crd/net/ticket_cache.hpp); 2026-11-09). Sources: RFC 9846
sections 4.6.1 and C.4, RFC 9369 section 5. `TicketCache` is the store a `QuicClient` uses when its configuration names
none: fixed entries of a fixed size taken at construction, at most `per_binding` under one binding (one server name,
pin set and QUIC version, so a server flooding NewSessionTicket replaces only its own), the oldest replaced when full;
`take()` hands the newest ticket of a binding out once; no ticket stays past `max_age_ns`, at most seven days, and the
provider checks a ticket's shorter lifetime when offering it. A dropped ticket is zeroed at once and bytes handed out
on the next call. One client is one correlation domain (T15): a client meant to be unlinkable keeps a store and a socket
of its own. `QuicClientConfig::tickets.entries = 0` keeps none, and a store in `connection.tickets` replaces it.

Evidence: `test_ticket_cache.cpp` (every refusal, order and single use, both bounds, the size at its edge, the age to
the nanosecond, and each way a ticket leaves memory read back from a retaining allocator), `test_quic_client.cpp` (a
resumed connection's 0-RTT from the client's own cache in both versions, a cache turned off, a caller's store, expiry
within a turn), and `crd-fuzz-net-ticket-cache` (keeps, takes and clock steps against a model of the contract).

<a id="quic-interop"></a>
**S50. Interoperability with a real QUIC stack** (4.2a; [quic_interop.py](../../tests/foundation/net/interop/quic_interop.py);
2026-11-09). `crd-quic-peer` (crd-net's server and client over real UDP on the loopback) meets `crd-ngtcp2-quic-peer`, a
program over the pinned ngtcp2 1.26.0 and OpenSSL 4.0.3 (`SSL_set_quic_tls_cbs`), each in its own process, both ways round,
with one application: a client opens bidirectional streams and writes a pattern and a FIN, a server writes back as many
bytes of another pattern as it read and then its FIN, every byte checked by both ends, and each end reports the version,
streams, bytes, close and what OpenSSL negotiated, which the driver holds against the other's. ngtcp2's own example
programs are not used: they speak HTTP/3 and hq-interop only through wolfSSL. Key updates are soft: `key_update_interval`
starts an update (at three quarters of it) as soon as RFC 9001 section 6 allows one, and a peer slow to acknowledge
the phase holds it back while the key seals on; only the AEAD's limit, or `limit_sealed_packets`, refuses. Before this
a 64-packet interval closed a connection with AEAD_LIMIT_REACHED as soon as more than a quarter of the interval sat
in flight, which only a real peer's flights showed.

Evidence: `crd-net-quic-interop-ngtcp2` (102 cases on `linux-gcc-public-checks`, about 105 s): each way, QUIC v1 and v2 with
either end closing and the code the other end read; the three hybrid groups and X25519; the three AEADs; 8 MiB on one
stream past every window; 48 streams through a limit of 8 at once; compatible version negotiation to v2 (RFC 9368);
a Retry from each server with the Retry packets counted off the wire; key updates each end starts, counted by ngtcp2;
and three seeded relays (5% loss, 3% duplication, delay and jitter that reorders). An ngtcp2 server drops a first flight
it receives out of order (`NGTCP2_ERR_RETRY`), and the peer takes the client's next Initial. Datagrams (RFC 9221): 256
of 1000 bytes alone and beside streams, frames filling a 600-byte limit exactly and one byte more refused by each
sender, over two seeded relays. Resumption, 0-RTT accepted and 0-RTT refused by the server on a second connection in the
same process. Version Negotiation to the one version a server runs, counted off the wire. A stateless reset from a
server killed in mid-transfer and restarted on its port with the same key: both programs derive their tokens from a
fixed key, so the second process answers for the first one's connections. NET.4 adds the interop runner and the other
peers.

<a id="admission"></a>
## Bounded admission

"No processing before authentication" is not achievable: initial transport messages must be parsed before identity
exists (G11). The contract is that every stage's cost is bounded and that expensive work waits for the stage that
justifies it. Stages run in order; a datagram that fails a stage is dropped and counted, with no response unless the
stage names one.

| Stage | Check | Bound |
|---|---|---|
| S0 Receive | Batched receive into preallocated buffers | Fixed batch size per loop iteration; no allocation |
| S1 Sanity | Length within [minimum, path MTU], header form, version field | Constant time; an unsupported version gets a version negotiation answer only to a datagram of at least 1200 bytes |
| S2 Route | Connection ID lookup in a fixed table keyed by a secret PRF | Constant expected time under adversarial IDs (T10) |
| S3 Admit | New connections: per-prefix and global handshake buckets (/64 for IPv6, /32 or a configured prefix for IPv4); Retry with an address-validation token when over threshold | No per-source state before validation; responses at most three times the bytes received |
| S4 Protect | Header protection, AEAD open, packet-number replay window | At most one AEAD open per datagram; no signature verification for an unvalidated address under load |
| S5 Authenticate | Handshake completion binds the principal; capability tokens verified (cached by token hash), expiry and revocation generation checked | Pending handshake state capped per server and per prefix |
| S6 Quota | Per-principal streams, datagrams, bytes, reassembly, decompression ratio and output size, pending messages | A message's declared length is checked against its schema maximum and the quota before any buffer is reserved |
| S7 Decode and authorise | Generated zero-copy decoder with depth and count limits; the capability must cover object, operation and budget before execution | Decode cost linear in the bytes admitted; nothing executes on an unauthorised message |

Overload sheds in a fixed order: new handshakes (Retry, then refusal) before established sessions, and lower channel
priority classes before higher ones; legitimate-client latency under attack is a published NET.4 measurement. The
numbers behind each bound (handshake state size, bucket rates, pool sizes) are configuration with measured defaults
set and published by NET.4; this design fixes the stages, their order and what each may spend.

<a id="stream-sockets"></a>
**S51. Byte-stream sockets** (4.2a, threats T01 and T10; [stream_socket.hpp](../../engine/foundation/net/include/crd/net/stream_socket.hpp);
2026-11-09). `StreamSocket` is the seam for byte streams, as `DatagramSocket` is for datagrams: `read`, `write`, `shutdown_send` and
one blocking `wait`, into caller-owned storage, with Busy as back-pressure and a read's `ended` as the peer's clean end. `TcpSocket`
and `TcpListener` implement it on the host: non-blocking and not inherited, `TCP_NODELAY` (the transport assembles its own
records), no `SIGPIPE`, a non-blocking `connect` finished by `connected()`, and `abort()` (linger zero) for peers the transport
gives up on. A listener holds its port exclusively on Windows and takes `SO_REUSEADDR` elsewhere, so a restarted server gets its
port back from `TIME_WAIT` while a live listener still refuses a second. The host calls live once, in
[native_socket.hpp](../../engine/foundation/net/src/native_socket.hpp), which UDP and TCP share.

<a id="qmux"></a>
**S52. QMux: the TCP and TLS path** (4.2a, threats T01, T02, T06 and T10; [qmux.hpp](../../engine/foundation/net/include/crd/net/qmux.hpp);
`quic.qmux`, draft-ietf-quic-qmux-02; 2026-11-09). "One session model over every path" made literal: the same `QuicStreamSet` and
`QuicDatagrams` as on QUIC, carried as QUIC version 1 frames in QMux records inside TLS 1.3. No second stream or flow-control
implementation exists. `QmuxConnection` is sans-I/O (`receive`, `take`), so NET-SIM's virtual streams and a test's pipe drive it alike.

- **Records.** A Size and whole frames, at most 16382 bytes either way. This side never advertises more: a larger record buys nothing
  over 16 KiB TLS records, and a fixed buffer per direction bounds the connection. A Size arriving in pieces waits; over 16382 is
  FRAME_ENCODING_ERROR; **zero is PROTOCOL_VIOLATION** (the draft is silent: section 3.2's Frames holds frames, and RFC 9000 section
  12.4 refuses a packet of none). A frame cut by its record is FRAME_ENCODING_ERROR.
- **Parameters.** `QX_TRANSPORT_PARAMETERS` goes first the moment TLS can carry it, and a peer's must be the first frame it sends and
  only once (else PROTOCOL_VIOLATION). Only section 5's set is written, so no greasing here; a QUIC parameter about IDs, paths or
  ACKs is TRANSPORT_PARAMETER_ERROR, one sent twice too, one from another document is ignored.
- **Frames.** Section 4.2's list, and DATAGRAM when negotiated; anything else is FRAME_ENCODING_ERROR. Streams set `in_order`: a STREAM
  frame must continue its stream exactly, so repeats (which the draft does not name) are refused too. Frames are acknowledged as TLS
  takes them (section 4.1): `kQuicStreamFinished` and `QuicDatagramOutcome::Acked` mean handed to the transport. Datagrams are bounded
  by the record as well as the peer's limit.
- **Reads never wait for the application** (section 6): every byte is read and parsed; streams are bounded by flow control, late datagrams
  drop. Sending stops at one record's room in TLS's bound, so a peer that stops reading holds at most that much here.
- **Time.** The idle timeout is the smaller offered. It restarts on a whole record received and when **the last byte is handed to the
  transport**, so a peer that stops reading times out. It ends the connection with close_notify, no frame (section 7.1). `QX_PING` keeps
  intermediaries awake; one goes at the start and every `keepalive_ns`; requests increase strictly and the latest answer gives
  the RTT that tunes windows. No idle timer needs a PTO: TCP retransmits.
- **Close.** CONNECTION_CLOSE, close_notify, then the transport's `shutdown_send`; then the peer's end or `close_wait_ns`. TLS is shut
  by then, so bytes still arriving are discarded unread and only the transport's end is awaited. A transport end without close_notify
  is Truncated, a reset Reset, a fatal alert TlsFailed: none is a clean close.
- **Padding** to `padding_block` is PADDING frames in the record (NET.3's `padded_size()`).
- **Resumption.** TLS tickets through `TlsTicketStore`, under the stream's binding, never a QUIC version's: a QUIC ticket carries QUIC's
  remembered parameters and resumes only QUIC, so the first fallback connection is a full handshake and later ones resume.
- **ALPN** is required (section 3.1); each application protocol's QMux identifier differs from its QUIC one (its register entry names it).

Evidence: `test_qmux.cpp` (every refusal above through a real TLS peer, flow control both ways past every window, byte-at-a-time and
seeded-stall paths, timers to the nanosecond, the close paths, a run over real TCP), `test_stream_socket.cpp`, `crd-fuzz-net-qmux`
(records, cuts, time and closes against a real TLS peer), held to source mutations ([session](../sessions/2026-11-09-4-2a-qmux.md)).
0-RTT is [S54](#qmux-early). **Interop** (2026-11-09): `qmux_interop.py` runs `crd-qmux-peer` (QmuxHost over real TCP) against
`crd-qmux-crate-peer`, the pinned `qmux` crate 0.6.6 over rustls and aws-lc-rs, both ways round: closes with code and reason, the groups and AEADs,
transfers past every window, MAX_STREAMS turning over, unidirectional streams, RESET_STREAM and STOP_SENDING, datagrams at and over the limit, resumption, QX_PING and the RTT,
a relay cutting the bytes into pieces of 1 to 7, a silent path and a refused ALPN. The crate reads QMux only after the handshake, so 0-RTT is proven against crd-net's own peers
and fuzzing. Application protocols here take the identifier `qmux-02.<name>`, the crate's convention, which section 8.1 allows and does not define.

<a id="stream-host"></a>
**S53. The stream path's host** (4.2a, threats T01 and T11; [stream_poller.hpp](../../engine/foundation/net/include/crd/net/stream_poller.hpp),
[qmux_host.hpp](../../engine/foundation/net/include/crd/net/qmux_host.hpp); 2026-11-09). QMux connections behind one thread, the counterpart of
`QuicHost`: `QuicHandle` generations, one `DeadlineHeap` shared with `QuicHost`, and the same executor split (`poll()` is one bounded turn, `run()`
waits).

- **Seam.** `StreamSocket` is one connection; `StreamPoller` is the set: accept and connect into numbered slots, interest, close, and a `wait` of
  level-triggered reports. `TcpPoller` is the host's TCP (epoll on Linux, a dense `WSAPoll` or `poll` set elsewhere: O(live connections) per wait,
  acceptable for the fallback path and measured under NET.4). NET-SIM's virtual streams and a test's pipes stand in its place; the host's header
  includes no OS header and reads time only through `TransportClock`.
- **Level, not edge.** A turn takes one read of at most 64 KiB and `write_bytes_per_turn` per connection and `accepts_per_turn` connections; what is
  left is simply reported again. `kStreamWritable` is asked for only while a write is blocked, so ordinary turns make no `epoll_ctl`. A full report
  list starves nobody (the kernel's ready list rotates, a poll set rotates its start).
- **Writes.** `QmuxConnection::outgoing` shows TLS's output in place; `take` removes what the socket accepted. A partial write needs no staging
  buffer per connection, so a connection's memory stays what S52 states.
- **Admission.** An accepted connection is charged to the `HandshakeBuckets` QUIC shares as a *validated* address (TCP's handshake proved it). Over the
  rate, or with every slot in use, it is reset at once, from one spare slot of the poller. A shortage of descriptors pauses the listener for
  `accept_retry_ns`, not spinning on a listener that stays readable.
- **Connect.** The TLS session starts with `connect()`, so `handshake_timeout_ns` bounds the TCP connection too; nothing is written before it stands.
  A failed connect is reported through `on_closed` with the `SocketError` (Refused, TimedOut).
- **Close.** A write that finds the peer gone first reads what the socket still holds (at most kReadsAfterFailure reads, bounded by the host's receive buffer), so a peer that reset behind its last bytes and its close is reported by them. A Closed connection's bytes are tried once. It then ends with a FIN if everything went out (close_notify must reach the peer, also after
  an idle timeout or a TLS failure), and a reset if bytes could not be sent, the handshake timed out (a silent peer earns no TIME_WAIT) or the
  transport had already failed.

A connection that ended on what it read or its timer is served once more, Closed, so the application reads what arrived before the end; the first interop run showed it was
lost (the peer's last stream bytes and its CONNECTION_CLOSE in one read). QUIC has no such gap: a peer's close leaves a connection Draining, which is still served.

Evidence: `test_qmux_host.cpp` (over pipes under a test clock: both directions past every window, the pool and rate refusals, every close path,
back-pressure at 512-byte sockets with no busy turn, accept pauses, each share of a turn, deadlines bounding the wait, allocation refusal at every step,
no allocation in an established turn; over real TCP: 8 clients with 16 KiB socket buffers, a refused port), `test_stream_poller.cpp` (epoll and `WSAPoll`
on the loopback), `crd-fuzz-net-qmux-host` (any order of connects, resets, cuts, stalls, raw bytes, time, closes, stale handles and accept failures: every
connection closed once, counts consistent, all memory back), held to source mutations ([session](../sessions/2026-11-09-4-2a-stream-host.md)).

<a id="qmux-early"></a>
**S54. QMux 0-RTT** (4.2a, threats T07 and T10; [qmux.hpp](../../engine/foundation/net/include/crd/net/qmux.hpp); draft-ietf-quic-qmux-02 sections 4.2,
5.1 and 8.2, RFC 9000 section 7.4.1, RFC 9846 section 2.3; 2026-11-09). The draft defers to QUIC for what 0-RTT keeps to and says nothing of a refusal, so S45's rules
carry over, on TLS early data in place of 0-RTT packets.

- **What a ticket remembers.** QUIC reads its server's parameters from the handshake; here they are a frame, which may arrive before or after a NewSessionTicket. So
  the ticket record's `parameters` field (S9) also serves a records session: after `hold_tickets()` the newest ticket waits, by the provider's own reference, until
  `remember_parameters()` hands over the frame's body. A ticket whose parameters never came goes with the session; no copy of it exists meanwhile.
- **Client.** A ticket that allows early data and whose parameters parse grants streams and datagrams from them alone (`set_peer_parameters` only raises, as in S45). Its
  own QX_TRANSPORT_PARAMETERS goes first, as early data (section 5's rule stands in 0-RTT). A ticket allowance too small for that frame sends nothing early; a frame the allowance
  cannot hold waits for the handshake, and no further record is built early, so a client never spins on a full allowance. `early_data_room()` bounds every record.
- **Delivered means accepted.** S52 acknowledges a frame as TLS takes it, which early data must not: the bytes are gone if the server refuses them. Frames sent early are
  left unacknowledged and `QuicStreamSet::on_all_acked()` runs when the server accepts, so no per-frame record is kept (a tiny-frame flight would need thousands); datagram
  outcomes are held in their ring (`hold_outcomes`) and released, or turned to Lost.
- **Refused.** As S45: streams back to `init()`'s state, queued datagrams Lost, the transport parameters owed again as the first frame, and `handshake().early_data()` tells the
  application to send again. Accepted but lowered limits are PROTOCOL_VIOLATION.
- **Server.** The early data context is `write_quic_early_parameters` (one encoding with S45) and the application's part, so a limit that changed refuses a ticket's 0-RTT.
  It goes Established at the client's first flight and reads early bytes through `read_early()`; a record with any early byte in it is marked whole (a client may cut one at
  the end of its 0-RTT). `handshake_timeout_ns` still bounds the client's Finished.
- **0.5-RTT.** A server may answer before the client's Finished (`TlsSession::send_half_rtt`, RFC 9846 section 2.3). Without it an early request would be answered a full round
  trip later than QUIC answers it (T07 is unchanged: the marks say which requests are replayable). A close at 0.5-RTT is a CONNECTION_CLOSE and a FIN: TLS has no close_notify
  before its handshake ends.

Evidence: `test_qmux.cpp` `[qmux-early]` (accepted with every cut of the flights, refused three ways and all their combinations, a replayed flight, lowered and raised limits through a real TLS peer, a too-small allowance,
parameters that no longer parse, a ticket with none, a record cut by the end of early data, the first-frame rule, the handshake timer, a close at 0.5-RTT, no allocation),
`test_tls_early.cpp` (the ticket either side of the parameters, `send_half_rtt` in every state), `test_qmux_host.cpp` (over a host), `crd-fuzz-net-qmux-early` (either side
under test against a real TLS peer: hostile early data, hostile answers at 0.5-RTT, refusals), held to source mutations ([session](../sessions/2026-11-09-4-2a-qmux-early.md)).

<a id="raw-path"></a>
**S55. The raw UDP path** (4.2a, threats T01, T02, T06, T07, T10 and T15; [quic_connection.hpp](../../engine/foundation/net/include/crd/net/quic_connection.hpp),
[quic.hpp](../../engine/foundation/crypto/include/crd/crypto/quic.hpp); ADR-0035, decision D4; 2026-11-09). The path for LAN peers and headless processes. It carries what QUIC carries without
what a stranger's connection needs: no certificate, no asymmetric operation per connection and no Initial or Handshake packet, because the two ends already share a session.

- **What it is not.** A second transport. Its packets are QUIC 1-RTT short headers, its frames, stream sets, datagrams, acknowledgements, recovery, key updates, padding, IDs and close are the
  connection's own, so `QuicConnection` starts keyed (`open_keyed`) and has one code path after that. Retransmission has one owner, and NET-PATH's congestion control and pacing, NET-SIM's clock and NET-DEBUG's conditioning reach it unchanged.
  ADR-0035's "keeps its own" reliability means this path leans on nothing beneath it, as QUIC does; it never meant a second loss detector.
- **The same session keys** (D4). A 32-byte session secret, for this connection alone: the session's TLS exporter under a label and context of its session type (D3), so hybrid key exchange and the handshake's
  authentication carry over, or a launcher's fresh random bytes where no session exists yet (a cluster of bots). HKDF-Extract under the path's own salt, then HKDF-Expand-Label for each side's traffic
  secret, in the session AEAD's hash; RFC 9001's schedule runs from there in the connection's QUIC version. Holding the secret is the authentication; there is no plaintext mode in any build. A second connection on one secret
  would repeat nonces, so `open_keyed` zeroes its caller's copy whatever it answers; the policy generation refuses an AEAD here as it does in a handshake (S20).
- **Routing.** Each end's sequence 0 ID comes from the secret (8 to 20 bytes, each end its ingress's length), and is routed by the `ConnectionIdTable` like any other, so the path shares its socket, loop and host with QUIC and needs no demultiplexing of its own.
  Fresh IDs, Retire Prior To and stateless resets are the ordinary ones.
- **The hello** (NET-STD). A two-byte length and the QUIC transport parameters, in CRYPTO frames of 1-RTT packets, resent by the same recovery: the extension negotiation with its ignore-unknown rule and grease, and `version_information`.
  A client's goes in its first packet, and streams and datagrams follow one round trip later, once the peer's limits are known. Reading stops at the hello: more CRYPTO data, or a length past 4096, closes the connection.
  Both ends are configured for one QUIC version; a mismatch opens nothing, since the versions' key labels differ.
- **The address.** A client knows its server. A server opens with its client's address or with none, and then takes the address of the first packet that opens: nothing else can bind it, since a stranger cannot seal one. It sends nothing before
  that, and at most three times what it received until the client acknowledges one of its packets; an on-path attacker replaying the first packet from a spoofed source gets that 3x and a path that goes nowhere, no more than it could cause on QUIC. Replays die in the packet number window (T07).
- **Limits.** Key usage limits and updates (T06), `padded_size()` padding (T15), the idle timeout and the fixed allocation are QuicConnection's, unchanged.
- **Not here.** The `crd-net-raw` session adapter of ADR-0035's Layer 4 belongs to 4.2d, and sits on this.

Evidence: `test_quic_keyed.cpp` `[raw-path]` (three AEADs, both versions, 8 and 20-byte IDs, streams past every window and datagrams both ways, six hostile seeds, a real TLS session's exporter, every refusal, another secret, each byte flipped, a replay, an Initial,
the 3x limit, the hello split, reordered, resent, too long, followed by more, unreadable and naming the wrong ID, key updates, padding, close and idle), `test_quic_client.cpp` (hosts sharing a socket with TLS connections over hostile paths and over real UDP, a binding only an
opened packet makes, refusals that leave the route and slot free), `test_quic.cpp` (fixed answers for the derivation), `crd-fuzz-net-quic-connection` modes for the path, held to source mutations ([session](../sessions/2026-11-09-4-2a-raw-path.md)).

<a id="qpack"></a>
**S56. QPACK and the field coding** (4.2a, threats T09 and T11; [qpack.hpp](../../engine/foundation/net/include/crd/net/qpack.hpp), [http_fields.hpp](../../engine/foundation/net/include/crd/net/http_fields.hpp)). RFC 9204's field compression for HTTP/3, on the integers and strings RFC 7541 defines and that [HPACK](#hpack) uses: one owner of Huffman coding and prefix integers.

- **Coding.** A prefix integer is read to 2^62 - 1 in ten continuation bytes at most. A string takes its Huffman form only when strictly shorter. The canonical code is built at compile time from the 257 lengths, a 9-bit table covering the common symbols and the first-code comparison the rest. RFC 7541 section 5.2's refusals hold: padding longer than seven bits, not all ones, or an EOS.
- **Table.** One ring of bytes and one of entries, of the size SETTINGS allowed, taken at `init()`. Evicting moves only the head, so an insert or Duplicate that names the entry it evicts copies from bytes still there; the ascending copy can only approach unread bytes from behind them and never overruns them. Each side's table has the same shape (`HttpFieldTable`, shared with HPACK).
- **Decoder.** Instructions arrive split anywhere; an incomplete one waits in a buffer of four times the capacity, the most a conformant instruction can code. A name reference that cannot resolve is refused when read, not after its value arrives. A section ahead of the table blocks, up to the number promised (one more is section 2.2.1's connection error); the caller keeps the bytes. Every reference is checked against the Required Insert Count and liveness. A section larger than the caller's storage is still read to its end and acknowledged, so the dynamic table stays in step, and returns `TooLarge`. The decoder stream's queue is bounded: a peer withholding its credit ends in `H3_EXCESSIVE_LOAD`.
- **Encoder.** A section may reference an entry the peer acknowledged, or an unacknowledged one only within the peer's blocked streams. A field is inserted the second time it is seen (64 hashes), so one-off values cost the stream nothing; an entry goes only once its insertion is acknowledged and no outstanding section references it (section 2.1.1). A field still in use that is in the oldest quarter of the table is duplicated, which costs a byte or two where falling out would cost the literal again.
- **Sensitive fields.** `authorization`, `proxy-authorization`, `cookie`, `set-cookie` and anything marked `never_index` are never indexed, however they were offered, and carry the N bit: no attacker-driven request can measure them through the shared table (RFC 9204 section 7.1).
- **Not here.** HTTP semantics (pseudo-fields, name validity, field-section size SETTINGS, 0-RTT's remembered settings) are the HTTP/3 layer's.

Evidence: `test_http_fields.cpp` and `test_qpack.cpp` `[qpack]` (RFC 7541's integers and strings and RFC 9204 appendix B byte for byte with its table sizes, every form and instruction with its refusals, the wrap, blocked streams, eviction from under an insert and a duplicate, a section past its storage, every encoder path, random traffic through a real decoder); `crd-fuzz-net-qpack` (a table written apart predicting every instruction and field); `crd-net-qpack-interop-qifs` (the qifs corpus: every section from every encoder decoded, and the sizes); the [board](../bench/2026-11-09-4-2a-qpack-compression.md); held to source mutations ([session](../sessions/2026-11-09-4-2a-qpack.md)).

<a id="http3"></a>
**S57. HTTP/3** (4.2a, threats T09, T10 and T11; [http3.hpp](../../engine/foundation/net/include/crd/net/http3.hpp), [http_message.hpp](../../engine/foundation/net/include/crd/net/http_message.hpp); RFC 9114, RFC 9220, RFC 9297 section 2). An HTTP/3 connection over a QUIC connection's stream set and datagrams: sans-I/O, one thread, everything from the injected allocator at `init()`.

- **One port, two applications.** `QuicConnection::handshake().application_protocol()` names what the connection is: `h3` goes to an `Http3Connection`, Cerid's own protocols to the session layer. Browsers and native clients share the socket, ingress and hosts.
- **Streams.** Each side opens its control and two QPACK streams at `start()` and sends SETTINGS without waiting (section 7.2.4.2). The peer's are found by type; a second of a type is H3_STREAM_CREATION_ERROR, one closing or asked to stop is H3_CLOSED_CRITICAL_STREAM, and an unknown type is stopped and read no further. Per-stream state sits beside the QUIC slot of the same index (`QuicStreamSet::slot_of`), so there is one lookup and no second table.
- **No frame is held twice.** Frames are parsed where they lie in the QUIC receive buffer. DATA reaches the application through `peek_body()` without a copy, at most one frame at a time, and a frame length up to 2^62 costs a counter. A HEADERS frame stays unconsumed until QPACK decodes it (`QuicStreamSet::copy_readable` gathers the bytes across the ring's wrap), which is also where a blocked section waits, so blocking costs no storage beyond the buffer the stream already has. `init()` refuses a stream window that cannot hold a section with its frame header. A control frame is held the same way and read once whole.
- **Hostile lengths.** A HEADERS frame over the section limit is refused on its length before any of it is read: `QpackDecoder::cancel` tells the peer, and a request the application never saw is answered 431 from here. A section that decodes too large is acknowledged, so the table stays in step, and answered the same. The limit itself is sent as SETTINGS_MAX_FIELD_SECTION_SIZE, the field array holds the most fields that limit admits, and one decoded section per connection lives in a scratch that the next `poll()` reuses.
- **Messages.** [http_message.hpp](../../engine/foundation/net/include/crd/net/http_message.hpp) holds a field section to RFC 9110, 9113 and 9114 on both directions, the HTTP/2 path's single owner of those rules: lower-case token names, no CR, LF or NUL in a value and no edge spaces, pseudo-fields first and known, connection-specific fields refused, one content-length. A malformed message is a stream error, H3_MESSAGE_ERROR; a request reaches the application only when it passes. A body is held to content-length at each frame's length, so an overrun is refused before its bytes arrive. HEAD, 204 and 304 expect no content whatever content-length says.
- **Extended CONNECT.** `:protocol` needs SETTINGS_ENABLE_CONNECT_PROTOCOL from the server: refused by a server that did not send it, not sent by a client until the server's SETTINGS allow it. A tunnel (CONNECT and a 2xx) carries only DATA from there on, in both directions.
- **HTTP datagrams.** A datagram carries a Quarter Stream ID (RFC 9297 section 2). A prefix too short or above 2^60 - 1 is H3_DATAGRAM_ERROR for the connection, and one on a request with no datagram semantics aborts that stream with it. One for a stream closed or not yet open is dropped, and one ahead of its own request's HEADERS waits for no refusal. Only an extended CONNECT carries them, sent only once SETTINGS_H3_DATAGRAM passed both ways.
- **GOAWAY and push.** A gentle GOAWAY (2^62 - 4) then the exact one; values never rise. A client cancels what lies at or above a server's GOAWAY, reports each as `H3_REQUEST_REJECTED` and opens no more; a server resets what arrives at or above its own. Push is never offered: no MAX_PUSH_ID is sent, so a promise or push stream is H3_ID_ERROR.
- **0-RTT.** A client resuming passes the settings it stored and uses them until the server's arrive, whose values may only be equal or better (section 7.2.4.2); after a rejection the owner builds a new connection. A server binds its settings into its tickets (`early_context`), so a changed server does not take 0-RTT from an old one. A section or datagram from 0-RTT is marked replayable.
- **Grease.** Every SETTINGS has a reserved identifier, the control stream a reserved frame, and a reserved stream type goes once when the peer's limit leaves room.
- **Capsules.** [http_capsule.hpp](../../engine/foundation/net/include/crd/net/http_capsule.hpp) reads RFC 9297 section 3's capsules from any data stream split anywhere, handing each value on as it arrives and never gathering it (section 3.2: that can deadlock on flow control). A stream may end only between capsules; a Capsule-Protocol field counts only as one Boolean true.
- **WebTransport** is [S59](#webtransport)'s. Only `:protocol` matters to this layer, so its two spellings are that section's concern.

Evidence: `test_http3.cpp` and `test_http_message.cpp` `[http3]` (84 cases: every frame on every stream with the error RFC 9114 names, each SETTINGS refusal, the critical streams, messages, tunnels, datagrams, GOAWAY both ways, QPACK blocking and cancelling, 431, 0-RTT's rules), `crd-fuzz-net-http3` (a hostile peer at a server and at a client, and both sides honest over any frame size with a held encoder stream).

<a id="http3-interop"></a>
**S58. Interoperability with a real HTTP/3 stack** (4.2a; [http3_interop.py](../../tests/foundation/net/interop/http3_interop.py);
2026-11-09). `crd-quic-peer --http3 1` (`Http3Connection` over crd-net's QUIC) meets `crd-ngtcp2-quic-peer --http3 1`
(nghttp3 1.19.0 over ngtcp2 1.26.0), each in its own process over real UDP on the loopback, both ways round. Both programs
run one exchange, defined once in [http3_exchange.h](../../tests/foundation/net/interop/http3_exchange.h); every field and
body byte is checked by both ends. nghttp3 was not usable as it stands in one respect: it deletes a closed stream with
whatever a QPACK-blocked section still holds, and ngtcp2 closes a stream once it has delivered the last byte, so a
response arriving ahead of its table insert vanished; the C peer holds the close until the message ends.

The second implementation is `crd-quiche-peer` (quiche 0.30.0 over BoringSSL, a sans-I/O Rust program running the same
exchange), both ways round. quiche speaks QUIC version 1 alone and its QPACK has no dynamic table, so what it is not asked
is exactly that: Cerid advertises a table and quiche's zero holds it to inserting nothing. It sends GREASE frames and
settings and writes a server's GOAWAY and refusals through the application's own calls, so those paths meet a different
reading of RFC 9114 than nghttp3's.

<a id="webtransport"></a>
**S59. WebTransport over HTTP/3** (4.2a, threats T09, T10 and T11; draft-ietf-webtrans-http3-17, 2026-11-02; contract,
written before the code). The stream format is one wire for every revision browsers speak, so one implementation serves
them: what differs is negotiation and whether flow control exists.

- **Revisions.** A server offers both and each CONNECT says which it is. `webtransport-h3` is -17: SETTINGS_WT_ENABLED
  0x20b8933b = 1, initial limits 0x2b64 (uni), 0x2b65 (bidi), 0x2b61 (data), a value above 1 from a server is
  H3_SETTINGS_ERROR, and RESET_STREAM_AT and QUIC datagrams are required of both.
  Section 7.1 has an end that supports several revisions send one SETTINGS_WT_ENABLED per codepoint: Cerid also sends
  and reads -16's 0x2c7cf000, since webtransport-go 0.13 speaks only that one. A server reads the codepoints a client
  sends to select a revision, and here the selection changes nothing: between -16 and -17 the wire is the same, and the
  differences are a misplaced WT_STREAM's error (H3_FRAME_ERROR before, H3_FRAME_UNEXPECTED now; Cerid always sends
  -17's), retained SETTINGS (Cerid retains none) and limits a -17 reader holds to the initial values too (Cerid always
  does). `webtransport` is the legacy spelling, which
  [N02](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md#n02)'s survey has Chrome and Safari still
  sending and `web-transport-proto` 0.6.4 does send: SETTINGS 0xc671706a carries the session count, no flow control, a
  plain RESET_STREAM is all it can rely on, and its datagram setting may be the older 0xFFD277, read as 0x33 is (each
  browser's exact settings are confirmed against a real one under WEB.5). A server must
  not act on a WebTransport request before the client's SETTINGS, so it leaves the section unread in the QUIC buffer, as
  a QPACK-blocked one is (section 7.1). The WT settings are outside 0-RTT: neither remembered nor in `early_context`
  (section 3.2). A session never opens in 0-RTT.
- **Layers.** `Http3Connection` classifies; a `WebTransport` layer above it owns sessions. The classifier takes uni type
  0x54, and 0x41 as the very first varint of a bidirectional stream (from the client, or any server-initiated one once
  offered; elsewhere H3_FRAME_UNEXPECTED, and 0x41 has no length, so it is read before the frame header). A session ID
  that is not a client bidirectional stream ID is H3_ID_ERROR for the connection, and a reliable size that cuts the
  header is the stream's reset. It then reports `WtStream`,
  `WtReadable`, `WtAborted` and `WtStopped` and leaves the bytes to the owner through QuicStreamSet's zero-copy reads.
- **Sessions.** A fixed pool searched by CONNECT stream ID. Capsules on the CONNECT stream through `HttpCapsuleReader`,
  none processed before the 2xx. A rejected Origin answers 403, an unsupported resource 405, too many sessions
  H3_REQUEST_REJECTED or 429; a 3xx is never followed. WT-Available-Protocols and WT-Protocol are read as Structured
  Fields of Strings only, parameters ignored; a missing or unlisted answer to a client that requires one closes with
  WT_ALPN_ERROR.
- **Before a session is known.** A stream waits in QUIC's own buffers, counted against a bound; past it, stream reset
  and stopped with WT_BUFFERED_STREAM_REJECTED. Datagrams wait in a small fixed ring and past it are dropped.
- **Flow control** (on when both ends sent a non-zero initial limit; otherwise one session, excess CONNECTs
  H3_REQUEST_REJECTED, flow control capsules ignored). Limits count stream bodies, never the header. WT_MAX_STREAMS and
  WT_MAX_DATA must rise or the session closes with WT_FLOW_CONTROL_ERROR, as does a value above 2^60, a stream over the
  limit, or body bytes over it. WT_MAX_STREAM_DATA or WT_STREAM_DATA_BLOCKED is a session error. Credits are sent as the
  application consumes, never held for a BLOCKED capsule. **A sender's count must follow the final size**: bytes written
  to QUIC and never sent by a reset stream are given back, or repeated resets leak the window and the session stalls.
  The receiver counts a reset stream at its final size (`QuicStreamEvent` gains it).
- **Resets.** Application codes are 32 bits, mapped by the draft's figure 4 into 0x52e4a40fa8db..0x52e5ac983162, skipping
  every 0x1f * N + 0x21; a code outside that range is a reset with no application code. A -17 stream resets with
  RESET_STREAM_AT at header + the offset the application names, so the session ID always arrives.
- **Closing.** WT_CLOSE_SESSION (32-bit code, message at most 1024 bytes, cut on a UTF-8 boundary, a longer or invalid one
  H3_MESSAGE_ERROR), then FIN at once; a clean FIN means code 0; data after the capsule is H3_MESSAGE_ERROR. Every stream of
  the session then resets and stops with WT_SESSION_GONE, no new stream or datagram. WT_DRAIN_SESSION and GOAWAY reach the
  application and permit further use.
- **Exporter** (section 4.8). TLS exporter with label `EXPORTER-WebTransport` and the context struct: session ID (64),
  label length (8), label, context length (8), context; through `export_keying_material`.
- **Channels.** A session presents the surface the channel layer runs on over a `QuicStreamSet`: open, write, peek,
  consume, reset, stop, datagram, filtered by session with flow control layered in, so there is no second channel
  implementation. [S60](#channel-link) is that layer.
- **Peers.** `web-transport-quinn` 0.13.6 for the legacy spelling (`crd-net-webtransport-interop-quinn`; what it is not
  asked and why: `webtransport_interop.py`). The `webtransport-h3` peer is webtransport-go v0.13.0 (quic-go 0.62, draft
  -16, with RESET_STREAM_AT and flow control; `crd-net-webtransport-interop-go`, Go pinned as `tools/go-linux-x64`).

Evidence (the classifier, 2026-11-09): `test_http3_webtransport.cpp` `[webtransport]` (figure 4 around the reserved points,
the offer, what each side requires, the wait for a client's SETTINGS, headers split anywhere, reliable and plain resets
both ways, 0-RTT, GOAWAY, streams given up before naming a session), and `crd-fuzz-net-http3` modes 4 to 6. A server holds
every bidirectional stream before the client's SETTINGS, ordinary requests included: QPACK decodes a section once and the
token is inside it. A reset keeps a header only in the direction the stream's opener sent one.

Evidence (the sessions, 2026-11-09): `WebTransport` in
[webtransport.hpp](../../engine/foundation/net/include/crd/net/webtransport.hpp) takes over `Http3Connection::poll()`
and passes on what is not WebTransport. `test_webtransport.cpp` `[webtransport]` (22 cases), `crd-fuzz-net-webtransport`
and the source mutations in the session note prove it. What the contract did not say, decided here:

- A STOP_SENDING can overtake what it travels with. A client still waiting for a response ignores one, since a complete
  response may follow it (RFC 9114 section 4.1), and an open session ignores one with WT_SESSION_GONE, since section 6
  sends it beside the capsule and FIN. Any other ends the session.
- A session error is a reset of the CONNECT stream with the code the draft names; WT_MAX_STREAM_DATA and
  WT_STREAM_DATA_BLOCKED, which name none, use WT_FLOW_CONTROL_ERROR, flow control or not (they are outside section 5.6's
  set). A malformed capsule, or a value too long for its type, is H3_MESSAGE_ERROR. Once this side's FIN has gone, data
  after a WT_CLOSE_SESSION can only be answered with STOP_SENDING.
- Credit is counted at the highest offset a stream reached, when bytes arrive contiguously, and at the final size for a
  reset, so what a reset drops unread comes back to the sender at once. A stream counts as closed when its reading part
  ended and its writing part was finished or reset, not when the FIN is acknowledged.
- A stream refused with WT_BUFFERED_STREAM_REJECTED counts nowhere at the receiver, as no session is known to charge,
  while its sender has already paid: an overflowing sender loses that window, which only an abnormal peer pays for.
- An offer with a limit of 0 and flow control on means that kind is never granted. The offer's values are also the
  windows granted again.
- A DATAGRAM capsule on a CONNECT stream arrives like any datagram, up to `datagram_bytes`; a larger one is skipped.
- Sessions, once their streams go, keep nothing that could reach a later session in the same pool slot.

<a id="channel-link"></a>
**S60. Channels over every path** (4.2a, threats T01, T09, T10, T11 and T15; [channel_link.hpp](../../engine/foundation/net/include/crd/net/channel_link.hpp); 2026-11-09). NET.3 wrote the frames and their
admission and left the carrier open. `ChannelLink` is the one reader and writer of them, one per session, and is why "one session model over every path" is a statement about code.

- **Carrier.** A path offers either a connection's `QuicStreamSet` and `QuicDatagrams` (QUIC, QMux, the raw UDP path) or one `WebTransport` session. Two shapes, so `ChannelCarrier` is a class with one branch, not an interface: a read is zero-copy and
  costs no virtual call. The calls it needed that WebTransport lacked are `set_urgency`, `send_room`, `max_datagram` and `can_open`; every stream operation stays the session's own, with its flow control.
- **A channel maps once.** A reliable ordered channel is one unidirectional stream, opened by its first write, whose route goes once. A reliable unordered channel is one stream per write, ended, so no message waits behind another's loss.
  An unreliable channel is datagrams the caller assembled (Whole, Listed or Fragment), whose route is its own. A channel runs one way: each side opens its own, and a peer's bidirectional stream is refused.
- **Priority.** `ChannelDesc::priority` is the stream's RFC 9218 urgency, the lowest priorities sharing urgency 7, so the path's scheduler serves control first on every path.
- **Padding.** A path that controls its packets pads them (S30). WebTransport cannot, since the browser's end is not ours, so there a channel with a block pads in band: `pad_datagram()` to at most the path's room, `write_stream_padding()` after each complete write. Padding that
  does not fit whole waits for the next write, because a cut padding frame would misread everything after it (a stream's room is `send_room()`, the smaller of the stream's buffer and the session's credit).
- **Reading.** `StreamAssembler` per peer stream, from a fixed pool; `DatagramAssembler` per unreliable channel. `next()` gives one frame from each ready stream in turn and every datagram first. A frame all in hand is a view of the stream's buffer and is consumed only when the next call gives it up, so nothing is copied
  and credit returns only for what the application has finished with. Bytes that may be replayed are fed apart, and a frame whose head began among them stays `Early`.
- **What ends what.** `Wait` leaves the bytes unread, so flow control holds the sender, until the rate gate's time or the moment another frame frees room; a limit shared with the principal's other sessions is retried after 10 ms, since nothing here would announce it. A refused route or a full pool is `STOP_SENDING` for that stream alone.
  Broken framing, and a stream that ends inside a frame, fail the link: `Failed` once, then nothing, for the owner to send GoAway ProtocolError. A peer's reset gives back the partial frame and the quota at once.
- **The sender's limits.** A partial write says `taken` and the channel reports `Writable` when more can go, from the stream, the session's credit, or a stream limit lifting (`can_open()`, polled while one is owed, since QUIC reports no event for a higher MAX_STREAMS). The peer's STOP_SENDING reports `Stopped` and the next write opens a fresh stream with its route.
- **Who polls.** A QUIC or QMux link polls its stream set and datagrams inside `next()`. WebTransport's events are the connection's, so the owner hands over each one naming the session and drains `next()` before the next poll.
- **What is not here.** `SendBudget` belongs above, with the session that decides what to shed. Sessions, capabilities and revision announcements are [4.2d](../ROADMAP.md#slice-4.2d)'s, on this.

Evidence: `test_channel_link.cpp` `[channels]` (19 cases, each on QUIC-shaped, QMux-shaped and WebTransport carriers where the behaviour is shared), `crd-fuzz-net-channel-link`, and 24 source mutations in the [session note](../sessions/2026-11-09-4-2a-channels.md).

<a id="websocket"></a>
**S61. WebSocket over HTTP/1.1** (4.2a, threats T01, T02, T06 and T10; [websocket.hpp](../../engine/foundation/net/include/crd/net/websocket.hpp), `websocket`, RFC 6455; 2026-11-09). The browser fallback where UDP is blocked. A WebSocket is a QMux path, not a transport of its own: the same `QmuxConnection`, `QuicStreamSet` and `QuicDatagrams`, so `ChannelCarrier` gains no third shape and `QmuxHost` gains no line.

- **One record per Binary message, no Size.** draft-ietf-quic-qmux-02 does not mention WebSocket; the message boundary is the record's, as the `qmux` crate's WebSocket transport writes it. The limit stays 16382 bytes. A Text message is 1003, an empty one is what a Size of zero is (PROTOCOL_VIOLATION).
- **Where it sits.** Between TLS and the record parser: `Framing::Records` or, once TLS agrees on `http/1.1` and a `WebSocketConfig` is set, `Upgrading` then `Messages`. One port serves both by ALPN; a server's context lists both. Neither fork nor second buffer: the opening handshake and fragments use `m_in`, the frame header the room before the record in `m_out`.
- **Opening handshake** (section 4, RFC 9112). Fixed limits (8 KiB, 64 fields), CRLF only, no folded lines or space before a colon, `GET` and HTTP/1.1 only, no body (Content-Length other than 0 and Transfer-Encoding are refused, so nothing is smuggled behind it). 426 with `Sec-WebSocket-Version: 13`, 403 for an Origin not listed ([same policy as WebTransport](#webtransport): `http_origin_allowed`), 404 for another path. The subprotocol (the server's order) is required, since nothing else names what the messages carry. Extensions are never taken, an offer ignored. It runs under `handshake_timeout_ns`.
- **Replay.** A client configuration asking for early data is refused, and a server that TLS accepted 0-RTT from on `http/1.1` ends the connection (no conforming client sends it): an opening handshake that could be replayed is none.
- **Frames** (section 5). A header is read whole and refused there: reserved bits, unknown opcodes, a mask on the wrong side, a non-minimal length, a control frame fragmented or over 125, a message over the record limit (1009, as soon as announced, also across fragments). Fragments are compacted in `m_in` in one pass and a control frame between them is served in place, so hostile fragmentation costs linear time. An unfragmented record is read where it arrived. Masks are fresh from the CSPRNG on every client frame (section 10.3); a failure to get one sends nothing.
- **Closing.** CONNECTION_CLOSE, Close, close_notify, FIN. A QMux error is Close 1002, the idle timeout 1001 with no frame before it (S52). A peer's Close is answered with its status and is `PeerShutdown` with that status and reason; a browser's `close()` lands here. A status never sent (1005, 1006, 1015) or a reason not UTF-8 fails the connection.
- **Ping.** Only the latest is answered (section 5.5.3), with its payload. Liveness stays QX_PING; a Pong handed on restarts the idle timer as every byte handed on does.
- **SHA-1** is `crypto::legacy_sha1`, its one caller `Sec-WebSocket-Accept`, which RFC 6455's wire format dictates and which protects nothing (TLS beneath it does). The register keeps `cryptography: []`.
- **What is not here.** HTTP/2 and HTTP/3 tunnels (RFC 8441, RFC 9220) need the same message stage without this connection's TLS.

Evidence: `test_websocket.cpp` `[websocket]` (40 cases: RFC 6455's examples, every refusal by its status, every byte cut, both sides through real TLS, ALPN dispatch, the host over pipes), `crd-fuzz-net-websocket`, the WebSocket cases of `crd-net-qmux-interop-crate` both ways round (tokio-tungstenite 0.30 inside the `qmux` crate, over rustls), and the mutations in the [session note](../sessions/2026-11-09-4-2a-websocket.md).

<a id="hpack"></a>
**S62. HPACK** (4.2a, threats T09 and T11; [hpack.hpp](../../engine/foundation/net/include/crd/net/hpack.hpp), `http.hpack`, RFC 7541; 2026-11-09). HTTP/2's field compression, a pure codec with no connection coupling, on [S56](#qpack)'s integers, strings and `HttpFieldTable`; the static table is its own, appendix A's 61 entries. There are no acknowledgements and nothing blocks: a block is decoded whole, in connection order, and every line changes the table.

- **Decoder.** Index 0 and an index past the table are refused. A dynamic index is checked against the live entries at that moment, so it shifts inside a block as insertions shift it. A block too large for the caller's storage is still read to its end and applied, and comes back `TooLarge`: a line past the storage continues in a scratch of the table's size, and one too big even for that was too big for the table, which empties (section 4.4, no error). Any refusal stays: RFC 9113 section 4.3 makes it COMPRESSION_ERROR for the connection. A block of references expands by copying nothing past the storage, so the cost is the block's length.
- **Table size.** A limit binds only when the peer has acknowledged the SETTINGS carrying it, and the table holds the default 4096 until then even where this side asks for less. A limit below the table's size obliges the next block to open with an update at or under it, an empty block included (section 4.2); lowered then raised, the smallest binds the first update and the limit the second. Updates come only before the first field, at most two.
- **Encoder.** A block is refused before anything changes when `out` is under `max_bytes()`, since a partial block would leave the tables apart. A field goes in the table the first time it is seen unless it would take more than three quarters of it or is of a kind that rarely repeats (`:path`, `age`, `content-length`, `etag`, `if-modified-since`, `if-none-match`, `location`), which would only push useful entries out. `authorization`, `proxy-authorization`, `cookie`, `set-cookie` and anything marked `never_index` are never indexed and carry the never-indexed form (section 7.1). Size changes go at the start of the next block: the smallest since the last block if lower than what was last signalled, then the final one, capped at what this side allows.
- **Not here.** Pseudo-fields, name validity and field-section limits are [http_message.hpp](../../engine/foundation/net/include/crd/net/http_message.hpp)'s and HTTP/2's own.

Evidence: `test_hpack.cpp` `[hpack]` (RFC 7541 appendix C byte for byte, the 61 static rows, every refusal, size updates and what a limit obliges, entries too large or naming their own eviction, blocks past storage, the encoder's choices and updates, random blocks across SETTINGS changes), `crd-fuzz-net-hpack` (a table written apart predicting every field, refusal and capacity), `crd-net-hpack-interop-corpus` (hpack-test-case: every story of fourteen encoder runs decoded and compared), the [board](../bench/2026-11-09-4-2a-hpack-compression.md) and source mutations ([session](../sessions/2026-11-09-4-2a-hpack.md)).

<a id="browsers"></a>
## Browser constraints

Designed here, qualified under [WEB.5](../ROADMAP.md#slice-web.5):

- No raw UDP or TCP: browsers reach Cerid through WebTransport over HTTP/3, WebSocket over HTTP/1.1, HTTP/2
  (RFC 8441) and HTTP/3 (RFC 9220), and WebTransport over HTTP/2 once its draft settles. Direct Sockets (Isolated Web
  Apps on ChromeOS only) is never required.
- The server side is part of `crd-net` (4.2a): it maps every browser path onto the same sessions, channels and
  capabilities as native clients, accepts both `:protocol` spellings and negotiates the WebTransport draft revision
  through the NET-STD register.
- No ECN, DSCP, socket options or path control: browser sessions take the congestion control the browser runs, and
  the server's channel budgets and priorities still apply.
- No client certificates and no access to the key store: browser principals authenticate with capability tokens
  from a web sign-in, bound to the session; servers check the Origin of every session request.
- Certificates: public endpoints use Web PKI certificates (with ECH); `serverCertificateHashes` (short-lived
  certificates of at most 14 days) is allowed for development and for game servers that rotate them.
- Per-origin connection limits and the browser's datagram size bound the channel layout: a browser session
  multiplexes all its channels over one WebTransport session.
- Browser peer-to-peer runs over WebRTC data channels through NET-REACH; the threading model of a WebAssembly build
  (cross-origin isolation) is WEB.5's.

<a id="crypto"></a>
## Cryptographic provider policy

- **Owned interface, vetted providers.** `crd-crypto` defines the interface (AEAD, KEM, signatures, KDF, keyed PRF,
  CSPRNG, key store, TLS and QUIC handshake integration); providers implement it. Allowed providers are maintained,
  reviewed implementations: AWS-LC (with mlkem-native), OpenSSL 3.5 LTS or 4.x, BoringSSL, and verified primitives
  (libcrux, HACL*) where a provider lacks one. Cerid writes no cryptographic primitive. Provider downloads need the
  user's approval (NET.2).
- **Identified algorithms.** Every algorithm is named by its NET-STD register identifier; code never hard-codes an
  algorithm choice outside the policy ([agility](#agility)). Swapping a provider never changes the wire format.
- **Defaults** (pinned by NET.2): X25519MLKEM768 key exchange, SecP384r1MLKEM1024 for high assurance, ML-DSA
  (RFC 9881) with a classical companion for Cerid-issued service and code-signing certificates, AES-GCM or
  ChaCha20-Poly1305 chosen by CPU capability, HKDF.
- **Randomness** comes only through `crd-crypto`: the provider's DRBG seeded from the operating system
  (BCryptGenRandom, getrandom) with CPU jitter as a second source ([P2](#provider)).
- **Keys.** Long-term keys live in hardware where available (TPM 2.0, Secure Enclave, Android Keystore, YubiKey-class
  keys), with a software fallback protected by the platform (DPAPI, the Keychain, the Secret Service): the
  [key store](#key-store). Keys and secrets are Restricted in every diagnostic record, zeroed on release and never
  logged.
- **Debug keys never ship.** RFC 9850 key logging and NET-DEBUG capture keys exist only in non-shipping builds and
  are refused at runtime when built in; a CI guard proves it (T22).
- **Export control** of everything above is [COMPLY.1](../ROADMAP.md#slice-comply.1)'s: its inventory is generated
  from the provider registry and the NET-STD algorithm entries.

<a id="provider"></a>
### The provider interface

`crd-crypto` implements the interface in [provider.hpp](../../engine/foundation/crypto/include/crd/crypto/provider.hpp)
over the pinned AWS-LC (NET.2, 2026-11-01): randomness, constant-time equality and zeroing, key identifiers,
AES-128-GCM, AES-256-GCM and ChaCha20-Poly1305, HKDF with SHA-256 and SHA-384 and HMAC over their hashes (`kdf_mac`,
MLS's MAC), ML-DSA-44/65/87, Ed25519 and ECDSA
signatures (`ProviderVerifier` is the production `SignatureVerifier` of the policy generations), X25519, P-256 and
P-384 key agreement, ML-KEM-768 and ML-KEM-1024 key encapsulation and the SipHash-2-4 table PRF. Every operation
names its algorithm by register identifier; the length tables (`aead_key_bytes`, `hkdf_hash_bytes`,
`signature_sizes`, `agreement_sizes`, `kem_sizes`) answer only for their own class; every
buffer has its exact length; failures zero their secret outputs; key objects zero their material when cleared or
destroyed and are read-only, so shared across threads, after `init`. `provider_algorithms()` is a policy's ceiling
(`PolicyConfig::supported`). Decisions (numbered for the tests):

- **P1. AWS-LC is the provider.** It is maintained, FIPS 140-3 validated in its FIPS branch, carries formally verified
  pieces (mlkem-native and mldsa-native, s2n-bignum's proven assembly, fiat-crypto's field arithmetic), implements
  every algorithm of the register's identifiers and holds the TLS 1.3 stack with QUIC integration, ECH and certificate
  compression that the handshake slice needs, under Apache-2.0 and ISC terms. It is pinned as a package (5.12.0,
  `cmake/pins.json`), built from its pregenerated sources without Perl or Go, linked privately into `crd-crypto` only,
  and exported link-only beside `Cerid::crypto`. BoringSSL, OpenSSL (a Perl-driven build) and the verified libraries
  (HACL*, libcrux: no TLS stack) are the alternatives the provider interface keeps open.
- **P2. Randomness** is the provider's NIST SP 800-90A CTR_DRBG, seeded from the operating system's source with CPU
  jitter entropy as a second independent source and reseeded on fork; no other generator exists in the engine.
- **P3. Assembly on every lane.** AWS-LC's own assembly (AES-NI, VAES, AVX2 and AVX-512 paths, ARMv8 crypto) runs on
  Linux, Android and Windows; Windows x64 assembles with the pinned NASM, acquired by the configure like every
  package, and Windows ARM64 with clang's integrated assembler: under `cl.exe` the configure names clang's GNU driver
  (`--target=aarch64-pc-windows-msvc`; Visual Studio's LLVM, else an LLVM installation, else PATH) as its one ASM
  compiler before any package enables the language, and fails without one. Its above-floor code runs only behind its
  own CPU capability checks, which the ISA isolation guard reviews
  ([ADR-0134](../decisions/0134-runtime-cpu-dispatch.md)).
- **P4. Encodings.** ML-DSA keys and signatures as FIPS 204 encodes them, signed purely with an empty context and
  hedged; Ed25519 as RFC 8032; ECDSA public keys as uncompressed SEC 1 points and signatures as strict DER
  Ecdsa-Sig-Value (the TLS 1.3 and X.509 forms; BER and malleated forms are refused); X25519 as RFC 7748 with an
  all-zero secret refused (`WeakSecret`); EC agreement peers and ECDSA keys as uncompressed points on the curve (the
  invalid-curve defence; the form byte is checked by Cerid, because AWS-LC also decodes the hybrid form 0x06/0x07 at
  that length). On a forged opening the provider zeroes the output (`EVP_AEAD_CTX_open` clears it on every failure,
  and the tests hold any provider to it). Seeds: ML-DSA's 32-byte xi, Ed25519's private key, the ECDSA scalar in
  [1, n - 1].
- **P5. Symbols.** Every symbol AWS-LC's libcrypto and libssl define for the linker carries the prefix `crd_awslc_`,
  and their C++ lies in `bssl::crd_awslc` (AWS-LC's `BORINGSSL_PREFIX`, since 2026-11-03), so a program that links the
  package beside OpenSSL, BoringSSL or another AWS-LC binds each call to its own library. The configure writes AWS-LC's
  three renaming headers (C, assembly, NASM) from the committed `cmake/aws-lc-symbols.txt` with
  `scripts/provider-prefix.py`, which reads ar archives of ELF, COFF and Mach-O objects itself (no Go, no first
  unprefixed build), and names the prefix on every AWS-LC command and every unit that includes its headers, so an
  existing tree rebuilds. The list is the union of every lane's definitions read from built libraries (Windows x64,
  Linux GCC and Clang with ASan and TSan, Android x86_64 and arm64); a global struct whose C++ members AWS-LC defines
  (`ssl_st`) is renamed by its tag, and `X25519`, which the engine's `AlgorithmId::X25519` shares, by a function-like
  macro, so only its calls are renamed. A definition the linker merges (a COFF COMDAT of selection any, a weak C++
  inline or template) never collides and is left alone; AWS-LC's weak `sdallocx` merges with jemalloc's on purpose.
  The `crd-check-provider-symbols` CTest judges the built libraries on every lane: a definition without the prefix
  fails it, naming the `collect` command that adds it. `crd-crypto-coexist-tests` links crd-crypto and the pinned
  OpenSSL 4.0 into one program (with `CRD_BUILD_TLS_VS_OPENSSL`): both libraries' SHA-256 and AES-256-GCM agree,
  OpenSSL completes its own TLS 1.3 connection, and crd-crypto's TLS and QUIC handshake tests run beside it.
- **P6. The hybrid groups** (X25519MLKEM768, SecP256r1MLKEM768, SecP384r1MLKEM1024) run inside the provider's TLS
  stack ([TLS sessions](#tls-sessions)); `provider_algorithms()` lists them since 2026-11-01, when each key share was
  read off the wire against the groups' layout with the provider interface's own primitives and every group
  interoperated with OpenSSL both ways (3.5.9, and since 2026-11-02 4.0.3), so no generation could enable them
  earlier.
- **P7. ML-KEM** (FIPS 203; `crypto.ml-kem`, class `0x07`). Encapsulation keys and ciphertexts as FIPS 203 encodes
  them and 32-byte secrets; the private input is the 64-byte seed d || z (`KemKey::from_seed`, through the provider's
  deterministic key generation, its only route from a seed), never the expanded decapsulation key, so a malformed
  private key cannot exist and Wycheproof's expanded-key vectors are outside the profile. `kem_encapsulate` checks the
  encapsulation key as section 7.2 requires (every coefficient below q; AWS-LC's import only copies the bytes, so the
  provider's own key check runs before encapsulating and a failing key is `BadKey`, told apart from a provider
  failure). Decapsulation of a
  ciphertext of the right length never fails: a ciphertext not made for the key yields the implicit-rejection secret
  in the same time (section 6.3), and the protocol notices only when keys derived from it fail to authenticate.
  ML-KEM.Encaps_internal, which takes the randomness m, stays out of the interface (section 6.2); the encapsulation
  vectors' known answers are checked through the provider's own deterministic encapsulation, the encapsulation
  `kem_encapsulate` runs with a fresh m.
- **P8. The table PRF** is SipHash-2-4 (`crypto.siphash`, class `0x08`, the provider's `SIPHASH_24`): every table an
  adversary can fill (connection identifiers, address buckets, object tables) is indexed by `PrfKey::hash` under a
  128-bit secret key (T10). A table generates its key (`PrfKey::generate`); processes that must agree, such as the
  servers behind one load balancer, share one (`PrfKey::init`, k0 then k1 little-endian, the paper's byte order). A key
  that holds none answers `NotReady`, never a hash under a public key. The key and the hash allocate nothing.
- **P9. Boards.** `crd-crypto-board` (`CRD_BUILD_CRYPTO_VS_PEERS`, Linux) times crd-crypto, AWS-LC called directly,
  BoringSSL (a dated release, prefixed `crd_bssl`) and OpenSSL 4.0 in one process on the same work: AEAD sealing and
  opening, an ephemeral X25519 agreement, ML-KEM-768, Ed25519, ECDSA P-256 and ML-DSA-65, and TLS 1.3 full handshakes,
  resumptions and records with one profile on every arm, the records moved through the caller's buffer as a transport
  would. Every arm's results are checked against every other's before anything is timed. The direct AWS-LC arm
  measures crd-crypto's own cost: a gap to it is crd-crypto's to close; a gap to BoringSSL or OpenSSL with AWS-LC level
  is the provider's, named on the board ([2026-11-03](../bench/2026-11-03-net-2-crypto-boards.md)).

The oracle is Project Wycheproof (C2SP/wycheproof, pinned as a test package and read at run time): every AES-GCM,
ChaCha20-Poly1305, HKDF, X25519, ECDH P-256 and P-384, Ed25519, ECDSA and ML-DSA vector (keygen from seed, verify,
sign-then-verify), every ML-KEM-768 and ML-KEM-1024 vector (keys from seeds, decapsulation with implicit rejection,
encapsulation with the modulus check) and every SipHash-2-4 vector, with RFC 8032's deterministic signatures, FIPS
180-4's SHA-256 example and the refusal of every vector outside Cerid's profile (other nonce, tag and key lengths,
compressed points, context strings, expanded ML-KEM decapsulation keys).

<a id="key-store"></a>
### The key store

[key_store.hpp](../../engine/foundation/crypto/include/crd/crypto/key_store.hpp) keeps a principal's long-term keys
and the secrets a fleet shares (NET.2, 2026-11-02). Each platform's interface was read first: DPAPI
(`CryptProtectData` and `CryptUnprotectData`, user scope, optional entropy, no prompt), the freedesktop Secret Service
(items in a collection, session algorithms `plain` and `dh-ietf1024-sha256-aes128-cbc-pkcs7`, unlock prompts), the
Keychain and Secure Enclave (P-256 only), TPM 2.0 (ECDSA P-256/P-384, sealing, NV counters), the Android Keystore and
PIV tokens of the YubiKey class (ECDSA P-256/P-384, Ed25519 on recent firmware). No device holds ML-DSA, every one
holds ECDSA. The decisions (numbered for the tests):

- **K1. Private material stays inside.** Signature, agreement and KEM private inputs are generated in the store, or
  imported once (a key made offline), and are used only through `StoredSigner` and the provider key objects the store
  loads (`agreement_key`, `kem_key`); no call reads them out. Secrets (AEAD keys, HKDF secrets such as a fleet's ticket
  and token sealing keys, SipHash keys) are read out (`secret`), since every server of the fleet holds them. Each key
  is named by its NET-STD algorithm identifier; its identifier is `key_id_of` its public key, a secret's HKDF-SHA256 of
  it under "cerid key id", so neither reveals it.
- **K2. One master key, one protector.** A store's 32-byte master key comes from the provider's randomness and is
  guarded by a `KeyProtector` (protect, recover, forget), which the store names in its file and opens only with:
  `Application` protection (the application's own key, a cloud key service, a deployment secret), `Platform` (the
  operating system's per-user vault) or `Hardware` (sealed to a device). `PlatformKeyProtector` is DPAPI on Windows:
  user scope, `CRYPTPROTECT_UI_FORBIDDEN`, its entropy the label "cerid key store master 1" and the store's
  identifier, so a reference recovers only in its own store and only for its user; DPAPI cannot forget. On Linux it is
  the Secret Service (K10), which can, and on Android the Android Keystore (K15), which can too. `TpmKeyProtector` is
  `Hardware` protection (K11 on Windows, K13 on Linux); on macOS the Keychain is the platform protector (K16). On a
  platform without one a protector answers `Unavailable`, and the store is never written unprotected.
- **K3. One sealed file.** A store is a directory created owner-only (`create_private_directory`: mode 0700, or a
  protected DACL with one inheritable entry for the user) holding the file `keys`, a durable save of crd-platform
  (atomic replace, envelope checksums, no older generation kept so an erased key is not kept either), and the lock
  file. The payload is a header (magic, format, store identifier, sequence, protector name, the master key's
  reference, a pending reference, a fresh 32-byte salt) and the body, every generation ordered by name and generation,
  sealed with AES-256-GCM under the key and nonce HKDF-SHA256 derives from the master key and the salt (the
  construction of the session tickets and address tokens), the header its associated data. A file that fails the
  save's checks or the framing is `Damaged`, one that does not authenticate `Forged`, an authentic one that breaks the
  store's rules (order, generations, lengths, identifiers) `Malformed`.
- **K4. Names and generations.** A name is 1 to 64 bytes of `[a-z0-9._-]`, starting with a letter or digit, and
  holds generations 1, 2, ... of one algorithm: only the last may be `Current` (it signs, and a fleet seals with it),
  every earlier one is `Retired` (it opens, decapsulates and verifies, never signs) or `Revoked`. Times are the
  caller's (`now`).
- **K5. Rotation, revocation and destruction.** `rotate` adds the next generation as Current and retires the previous
  one (`rotate_import` takes a fleet's next secret). `revoke` erases a generation's private material and keeps its
  public key and identifier, so a revocation can name it; revoking the Current generation leaves the name without one
  until it rotates. `destroy` removes a name. Both replace the master key when the protector can forget
  (`forgets()`): the store is saved under the new key with the old reference pending, the old key forgotten, then
  the pending reference cleared, and an interrupted replacement is completed by the next change before anything else.
  An earlier copy of the store file then no longer opens. A protector that cannot forget (DPAPI) cannot give that
  guarantee and says so. `erase` ends a whole store: under the lock its newest save's master keys (a pending one too)
  are forgotten first, so a file that cannot be removed no longer opens either, then the file is removed and the store
  closed; the directory and its lock file stay for `create`.
- **K6. Writers.** Each change takes the store's lock (`FileLock`: LockFileEx, flock), reads the newest save, applies
  itself to it and replaces the file, so writers in several processes never lose one another's changes. An open store
  refuses a save older than the one it holds (`Replayed`) or another store put in its place (`Forged`). Lookups and
  loads are read-only and run on several threads at once.
- **K7. Secrets in memory.** The master key and every private input are zeroed when an entry, a read snapshot or the
  store is released; the plaintext body is laid out in place in the save's buffer, sealed there and the buffer zeroed.
- **K8. Placement.** A key is kept in the sealed file (`Software`) or in a device that signs without releasing it
  (`Hardware`, ECDSA signature keys only: no device holds ML-DSA, agreement or KEM keys for a store). Without a device
  that holds the curve `HardwarePreferred` falls back to the file and `HardwareRequired` is `Unsupported` (K12). A
  principal's hybrid identity is its ML-DSA key in the file and its ECDSA key in a device. `StoredSigner` hides the
  placement from every signer of a Cerid artefact; `TlsCredential::init(StoredSigner)` binds a software key to the TLS
  stack, and a device key through the provider's private-key method (`SSL_PRIVATE_KEY_METHOD`, K12).
- **K9. Errors.** `NotFound`, `Exists`, `Revoked`, `Unavailable` (no protector service), `Denied` (the protector
  will not recover: another user's or store's reference, a locked vault, a refused prompt), `Damaged` and
  `StorageFailure` (a file that cannot be written, locked or created) joined `CryptoError`.
- **K10. The Secret Service (Linux).** `PlatformKeyProtector` is "secret-service" there, read from the Secret Service
  API 0.2 (`freedesktop.secret-service` in the standards register). libdbus-1 is loaded at run time (`libdbus-1.so.3`
  through crd-platform's `DynamicLibrary`), so nothing links it and a system without it, a session bus or a service
  answers `Unavailable`. Each call opens a private connection (never the process's shared one) and, where a secret
  crosses it, its own `dh-ietf1024-sha256-aes128-cbc-pkcs7` session: provider.hpp's `SecretTransportKey`, which no
  policy can name (it has no algorithm identifier); the service's value must lie in [2, p - 2]. The master key is an
  item of the default collection (made if there is none) with the attributes `xdg:schema`
  "org.cerid.KeyStore.Master", the store's identifier and a random 16-byte handle; the reference is a format byte and
  the handle, so a reference copied into another store, or forgotten, finds nothing (`Denied`), as does a second item
  with its attributes. `forget` deletes the item: this protector forgets (K5's guarantee holds on Linux). A prompt
  belongs to the connection that asked for it, so one call keeps one connection throughout; by default no prompt is
  shown and a locked collection or item is `Denied`. `PlatformKeyProtector(prompt_milliseconds)` lets the service show
  its unlock or confirmation prompt and waits that long for its `Completed` signal; dismissed or unanswered is `Denied`
  (on Windows it only lifts `CRYPTPROTECT_UI_FORBIDDEN`). Every reply's signature is checked before it is read.
  Proven against the pinned gnome-keyring 46.1 (`tools/gnome-keyring-linux-x64`) in a session bus and scratch home of
  their own, unlocked and locked (`secret_service_session.py`, the `crd-crypto-secret-service` CTests).

- **K11. The TPM 2.0 protector (Windows).** `TpmKeyProtector` is "tpm-pcp": the Microsoft Platform Crypto Provider
  (NCrypt), which offers the TPM's asymmetric keys only (no sealing, AES or HMAC key reaches a user through it, probed
  2026-11-03 on this PC's Intel PTT TPM). Each master key gets its own persisted, decrypt-only RSA-2048 key named for
  the store and a random 16-byte handle; RSA-OAEP with SHA-256 wraps the master key under the label "cerid master 1",
  the store's 16-byte identifier and a closing zero byte (a TPM closes a label with a zero byte itself, so the label
  carries one and wrapping and unwrapping agree wherever they run; a TPM takes labels only up to its largest digest, 32
  bytes with SHA-256 alone, and answered `TPM_RC_SIZE` to a 58-byte one). The reference is a format byte, the handle and
  the 256-byte wrapped key; it holds no public key, so the store file alone carries no modulus, but a copy of the user's
  profile does (the provider's key blob, its public half in the clear): against a quantum adversary this protector rests
  on RSA-2048, as DPAPI's blobs and the Secret Service's keyring files rest on their own protection. A changed handle or
  another store's identifier names no key, and a changed wrapped key fails OAEP inside the TPM (the label binds the
  store too): both `Denied`. `forget` deletes the key (`forgets()` is true, so K5 holds on Windows too; the provider
  keeps a key as a blob its TPM wraps, so a profile backup taken before the deletion still holds it). A store under it
  opens only on the machine whose TPM holds the key. `available()` opens the provider and reads its platform type.
  Linux's is K13. Evidence (2026-11-03, [session](../sessions/2026-11-03-net-2-tpm-protector.md)): the TPM cases of
  `test_key_store.cpp` on this PC's Intel PTT TPM (a runner without one skips the hardware case), no TPM key left after
  the run, and a forget that deletes nothing caught.

- **K12. Device keys; the TPM's on Windows.** A `KeyDevice` holds ECDSA P-256 and P-384 keys and signs digests with
  them: `create` (its reference, a handle of at most 64 bytes and never key material, and its public key),
  `sign_digest` (r || s) and `forget`. A store takes one device at `create` or `open`; a `Hardware` entry keeps the
  device's reference in place of a private input, sealed with the rest, and the public key the identifier binds. A
  device that lacks the curve (`Unsupported`) sends a `HardwarePreferred` key to the file and refuses a
  `HardwareRequired` one; any other failure fails the change, so an absent or failing device is never hidden by a
  fallback. A rotation stays in the device; an import never enters it. Revocation, destruction and `erase` delete a
  device key before the save that drops it, so an interruption leaves an entry whose key no longer signs, which
  repeating the change completes, never a revoked key that still signs; a key made for a change whose save fails is
  deleted again. A store opened without its device lists its `Hardware` keys, signs with none (`Unavailable`) and
  refuses every change to them. `DeviceSigner` hashes the message (SHA-256 for P-256, SHA-384 for P-384), has the
  device sign the digest, encodes r || s as P4's DER Ecdsa-Sig-Value and verifies it under the stored public key before
  giving it out, so a device that signs with another key or faults is caught. `TlsCredential::init(StoredSigner)` has
  the device sign the self-signed certificate (the signature algorithm set, the TBSCertificate re-encoded and signed,
  the finished certificate verified under its key) and gives each context its own copy of the signer behind the
  provider's private-key method, so every CertificateVerify is signed in the device. `TpmKeyDevice` is "tpm-pcp": one
  persisted sign-only key per `Hardware` generation through the Platform Crypto Provider, named "Cerid signer" with the
  store's identifier and a random 16-byte handle (the reference: a format byte and the handle), its public key read
  once at creation; each signature opens its own key handle (the provider's handles are not documented as thread safe,
  and the TPM serialises its commands). The provider's user keys answer to any process of the user, as DPAPI does; the
  TPM keeps them from leaving the machine. Linux's is K13; elsewhere it answers `Unavailable`. Evidence
  (2026-11-03, [session](../sessions/2026-11-03-net-2-tpm-signers.md)): the device cases of `test_key_store.cpp` (a
  simulated device on every lane; this PC's Intel PTT TPM holding P-256 and P-384 keys and signing a TLS handshake),
  five source mutations caught, and `crd-fuzz-crypto-key-store`'s restatement and model extended to device keys.

- **K13. The TPM 2.0 on Linux.** `TpmKeyProtector` and `TpmKeyDevice` are "tpm-tss2" there, through the TCG's Enhanced
  System API: libtss2-esys.so.0, libtss2-tctildr.so.0 and libtss2-mu.so.0 are loaded at run time and kept loaded,
  compiled against the headers of the pinned tpm2-tss 4.0.1 (the ESYS 3.0.2 ABI of Ubuntu 24.04), so a system without
  them or without a TPM answers `Unavailable`. `TpmConnection` names the TPM: a TCTI configuration of at most 255 bytes,
  empty for tss2's default order (the kernel's resource manager first); Windows ignores it. Each call opens its own
  TCTI and ESYS context under one process-wide lock, reads `TPM_PT_PERMANENT` and answers `Unavailable` when the owner
  hierarchy has an authorization (so no failed owner authorization ever reaches the dictionary-attack logic: Linux
  leaves it empty, an administrator who sets one opts out), creates the TCG ECC P-256 storage root template
  transiently (one owner seed, one key) and starts an HMAC session salted to it with AES-128-CFB parameter encryption:
  a master key written or read, an index's authorization and a key's authorization cross the bus encrypted (salting to
  a storage root whose certificate is not checked defeats a passive bus observer, not an active interposer). What a
  store keeps lives in an ordinary NV index at a random handle of the owner's range of the TCG handle registry
  (0x01000000 to 0x013FFFFF; another handle on `TPM_RC_NV_DEFINED`, eight attempts; `TPM_RC_NV_SPACE` is
  `StorageFailure`) with `AUTHREAD`, `AUTHWRITE`, `NO_DA` and `WRITEDEFINE`, written once in chunks of
  `TPM_PT_NV_BUFFER_MAX` and write-locked; an index is used only when it is exactly that (SHA-256, no policy, written,
  locked), so another owner's index at a reused handle is `Denied`. The protector's index holds the master key itself:
  `forget` deletes it, and nothing outlives it (unlike K11's profile blob). The device's holds the TPM-wrapped private
  and public areas (tss2's marshalling) of a sign-only ECDSA P-256 or P-384 key it created under the storage root
  (`FIXEDTPM`, `FIXEDPARENT`, `SENSITIVEDATAORIGIN`, `USERWITHAUTH`, `NODA`, `SIGN_ENCRYPT`, ECDSA with the curve's
  hash; a curve absent from `TPM_CAP_ECC_CURVES` is `Unsupported`); a signature reads the blob, checks its public area
  against the template, loads it, signs the digest and flushes it. Every authorization is HMAC-SHA256 under the
  reference's 32 random bytes of a label ("cerid tpm master 1", "cerid tpm signer index 1", "cerid tpm signer key 1")
  and the store's identifier, so another store's identifier derives another value: `TPM_RC_BAD_AUTH`, `Denied`, and
  (`NO_DA`, `NODA`) no dictionary-attack count. A reference is a format byte, the index's handle (in the owner's
  range, or `Denied` unasked) and the secret: 37 bytes. `forget` reads one byte with the derived authorization before
  `TPM2_NV_UndefineSpace` under the owner, so an index that is not this reference's is never deleted (`None`). Each
  call creates the storage root: an ECC key generation in the TPM, the price of keeping no persistent handle (a TPM
  holds few persistent objects, shared by the whole machine). tss2 writes its own warnings to stderr for refusals the
  code expects (a forgotten index, another store's authorization); `TSS2_LOG` silences them, and the protector never
  sets a library's environment. Evidence (2026-11-03, [session](../sessions/2026-11-03-net-2-tpm-linux.md)):
  `crd-crypto-tpm-swtpm` (tests/foundation/crypto/tpm_simulator_session.py) runs every key store case against the
  pinned swtpm 0.7.3 and tss2 libraries (tools/swtpm-linux-x64, scripts/install-swtpm.py), the TPM's protector and
  device cases among them (a TLS handshake and three concurrent signers), then gives the owner hierarchy an
  authorization and finds both `Unavailable`.

- **K14. PKCS #11 tokens.** `Pkcs11KeyDevice` ("pkcs11") holds a store's ECDSA keys in any PKCS #11 token: an HSM, a
  smart card or USB token through its vendor's module, SoftHSM. `Pkcs11Token` names the module (a library path, loaded
  at run time through crd-platform's `DynamicLibrary`), the token's label (exactly one present token must carry it) and
  the user PIN (fixed storage, zeroed with the object; empty only for a token with a protected authentication path).
  crd-crypto compiles against the OASIS Standard's 3.2 headers (`packages/pkcs11`) and calls only the 2.40 function
  list, so modules from 2.20 on answer. A module is initialised once per process (`CKF_OS_LOCKING_OK`; a module already
  initialised by another user in the process is accepted) and never finalised or unloaded. One process-wide lock orders
  every call and each call opens its own session. Login state belongs to the token and the whole application, and
  `C_Logout` in any session logs all of them out, so the device logs the user in only when no session of the process
  has, never logs out, and lets the token close the login with its last session. A PIN is tried once per device: a
  refused one (`CKR_PIN_INCORRECT`, `_INVALID`, `_LEN_RANGE`, `_LOCKED`, `_EXPIRED`) makes every later call through
  that device, and its copies, `Denied` without a login. A token that reports its PIN locked, on its final try or due
  for a change is `Denied` before any login, so the device never spends a token's last attempt. A key is a
  `CKM_EC_KEY_PAIR_GEN` pair: public `CKA_TOKEN`, verify only; private `CKA_TOKEN`, `CKA_PRIVATE`, `CKA_SENSITIVE`, not
  `CKA_EXTRACTABLE`, sign only (no decrypt, unwrap or derive). Its `CKA_ID` is a random 16-byte handle and its
  `CKA_LABEL` is "cerid key store " and the store's identifier in hexadecimal. After generation the private half must
  read back sensitive, never extractable, always sensitive, never extractable since generation and signing; otherwise
  both halves are destroyed and the change is `Denied`. The public key is the DER OCTET STRING of `CKA_EC_POINT` around
  the curve's uncompressed point, every length checked (a token that answers otherwise: `ProviderFailure`, both halves
  destroyed). The reference is a format byte and the handle: 17 bytes. A signature finds the private key by class,
  type, handle, label and curve (`CKA_EC_PARAMS`), so another store's reference or a key of another curve is `Denied`.
  It signs the digest with `CKM_ECDSA` (r || s, its length checked); a key the token makes reauthenticate
  (`CKA_ALWAYS_AUTHENTICATE`) takes the PIN once more as `CKU_CONTEXT_SPECIFIC`. `forget` destroys every object whose
  handle and label both match, so another store's reference deletes nothing. Return values map to `Unsupported` (no
  EC generation or ECDSA, the curve, a template the token refuses), `Denied` (login, a key handle or a function the
  token does not permit), `StorageFailure` (the token is full) and `Unavailable` (no module, no single token of the
  label, a removed or failing device). **The PIV profile** (`Pkcs11Token::piv`, device "pkcs11-piv"): PIV cards
  through Yubico's ykcs11 (read 2026-11-03 and 2026-11-04 in yubico-piv-tool 2.7.3's ykcs11.c, objects.c and
  token.c: `C_GenerateKeyPair` needs the security officer's login, the management key in hexadecimal or the PIN of a
  PIN-protected one, and takes the slot from the first byte of `CKA_ID`, 1 to 25; it ignores labels and usages and
  writes the slot's certificate with the key; `C_DestroyObject` deletes a slot's certificate, never its key; the
  last session closes the login). The token carries the security officer's secret beside the PIN, held and zeroed
  the same way and tried once per device by the same rules (`CKF_SO_PIN_LOCKED`, `_FINAL_TRY`, `_TO_BE_CHANGED`
  first). A key takes the first retired key-management slot (`CKA_ID` 5 to 24, PIV slots 82 to 95) that holds no
  certificate (a token `CKO_CERTIFICATE`; ykcs11's attestation certificates are session objects), since PIV's data
  model pairs every key in use with its certificate; none free is `StorageFailure`, and slots 1 to 4 and 25 are never
  touched. After generation the key must read back made on the token (`CKA_LOCAL`, which this profile and the
  Objects one both require: ykcs11 reports even an imported key sensitive and never extractable) and its point
  well-formed, else the slot is overwritten at once. The reference is a format byte (2), the slot and a 16-byte tag:
  SHA-256 over "cerid key store piv slot", the store's identifier, the slot and the slot's public key. Signing and
  forgetting read the slot's public key and compare the tag in constant time before any login, so another store's
  reference, or a slot another key has since taken, is `Denied` and spends nothing. Forgetting overwrites the slot
  with a P-256 key nobody holds and deletes its certificate, which frees the slot. The card's key can also agree
  keys (a PIV slot's capability, not a template's); the device never asks it to. Evidence (2026-11-04,
  [session](../sessions/2026-11-04-net-2-piv-profile.md)): three cases of `test_key_store.cpp` on a PIV test card
  (`tests/foundation/crypto/piv_module.cpp`, a PKCS #11 module that presents one card as ykcs11 does, its
  cryptography the test's AWS-LC keys) on win-debug, linux-gcc-debug and linux-gcc-asan: the profile's bounds and
  references, slots taken and freed around another application's, a TLS handshake, the store and slot bindings,
  forgetting and reuse, erasure, a full card, and eleven refusals (the security officer's secret refused once and
  never on its final try, a read-only session, no EC generation, a failing or removed card, an imported key, a
  malformed point, a refused PIN, a short signature, a key that asks for the PIN each time); four source mutations
  each caught. The SoftHSM run passes with `CKA_LOCAL` required. A YubiKey 5 against the real ykcs11 is the one
  physical item. Evidence (2026-11-03,
  [session](../sessions/2026-11-03-net-2-pkcs11-device.md)): the PKCS #11 cases of `test_key_store.cpp`. The contract
  case runs on every lane: fields out of range, an absent module, a library that is no PKCS #11 module, every refusal
  before the token. `crd-crypto-pkcs11-softhsm` (tests/foundation/crypto/pkcs11_token_session.py) runs every key store
  case against a fresh token of the pinned SoftHSM 2.6.1 (tools/softhsm-linux-x64, scripts/install-softhsm.py): P-256
  and P-384 keys, a TLS handshake, three concurrent signers, the store and curve bindings, revocation and erasure. The
  token must hold no object afterwards. A wrong PIN is then recorded by OpenSC's pkcs11-spy as exactly one refused
  `C_Login`.
- **K15. The Android Keystore.** `PlatformKeyProtector` and `PlatformKeyDevice` are "android-keystore" on Android
  (2026-11-03, the user's order that NET.2 proves them in the emulator). The Keystore is a Java API, so crd-crypto calls
  it through the application's own Java VM: `JNI_GetCreatedJavaVMs` from libnativehelper (public NDK from API 31,
  loaded at run time), the calling thread attached for the call when it is not and detached after, every local
  reference in the call's frame, every Java exception cleared where it is raised (an `AEADBadTagException`,
  `UnrecoverableKeyException` or `InvalidKeyException` is `Denied`, any other `Unavailable`). A process without a VM (a
  plain executable) answers `Unavailable`. Each master key gets its own non-exportable AES-256-GCM key (encrypt and
  decrypt only, the Keystore choosing every nonce) under the alias "cerid.master." and the store's identifier and a
  random handle in hexadecimal; the associated data is "cerid key store master 1" and the store's identifier. The
  reference is a format byte, the handle, the 12-byte nonce and the 48-byte wrapped key (77 bytes); forgetting deletes
  the alias. A device key is a sign-only ECDSA pair (`DIGEST_NONE`: the store hashes) under "cerid.p256." or
  "cerid.p384.", the store's identifier and a handle, so a key of the other curve or store is never found; its public
  key is read from its X.509 SubjectPublicKeyInfo and its DER signatures become r || s (`ecdsa_public_of_spki`,
  `ecdsa_raw_of`). Keys are made in StrongBox where the device has one (AES and P-256; StrongBox holds no P-384 key)
  and in the TEE when it answers `StrongBoxUnavailableException`. The master key crosses into a Java byte array for one
  call and is overwritten there before it returns. The proof runs where products run: an instrumentation APK (no
  Gradle: the pinned JDK, build-tools and API 31 platform, scripts/android-apk.py) loads the key store case as a JNI
  library in the app's own VM, uid and `untrusted_app` SELinux domain; `app_process` from a shell would run as shell
  or root, which proves keystore2's permissions for the wrong caller (the crd-crypto-keystore-app CTest).
- **K16. The Keychain and the Secure Enclave (macOS).** `PlatformKeyProtector` is "keychain" on macOS (2026-11-03, the
  user's order that NET.2 proves it on GitHub's macOS runners): Security.framework's item interface (C over
  CoreFoundation, no Objective-C). Each master key is a generic password item of the service "org.cerid.KeyStore.Master"
  whose account is "cerid.master.", the store's identifier and a random 16-byte handle in hexadecimal, accessible only
  while the Mac is unlocked and never in a backup or on another device (`WhenUnlockedThisDeviceOnly`). It goes into the
  data-protection keychain when the process holds that keychain's entitlement (a signed application) and into the user's
  default file keychain otherwise (an unsigned tool or test, told apart by `errSecMissingEntitlement`); the reference is
  the format byte that names which (1 the file keychain, 2 the data-protection one) and the handle, so `recover` and
  `forget` ask the keychain that holds it, and a reference of another store or handle finds nothing (`Denied`). `forget`
  deletes the item (`forgets()` is true). By default no prompt is shown: a locked file keychain asks for its password
  whatever `kSecUseAuthenticationUI` says (the macOS lane hung on it, 2026-11-04), so a file-keychain lookup searches
  only the unlocked keychains of the user domain's search list and an item is added only while the default keychain is
  unlocked (a locked one is `Denied`; `forget` that finds nothing while a locked keychain was left out is `Denied` too,
  never None, or K5 would drop a reference whose key survives), and queries carry `kSecUseAuthenticationUIFail` (another
  application's item is `Denied`). `PlatformKeyProtector(prompt_milliseconds)` lets the Keychain show its unlock or
  access prompt; the Keychain offers no bound, so the call waits for the user's answer. No keychain (a daemon without a
  user session, a default keychain whose file is gone) is `Unavailable`. `PlatformKeyDevice` is "secure-enclave": a
  permanent P-256 key of the data-protection keychain made in the enclave (`kSecAttrTokenIDSecureEnclave`, usable for
  signing only while the Mac is unlocked) under the application tag "cerid.p256.", the store's identifier and a handle
  (the reference: format byte 2 and the handle); it signs digests (`ECDSASignatureDigestX962SHA256`, DER to r || s) and
  forgetting deletes the key. The enclave holds P-256 alone, so P-384 is `Unsupported` (K12: a `HardwarePreferred` P-384
  key goes to the file). `available()` makes an ephemeral enclave key. Proof: the macOS runner lane (`macos-x64-debug`)
  runs the vault cases in a scratch keychain that is the user's default and only one for the run, unlocked, seeded then
  locked, and with its file gone (keychain_session.py, the crd-crypto-keychain CTests). A virtual machine has no enclave
  and an unsigned test keeps no enclave key, so the enclave's own keys need a real Mac with Apple silicon or a T2 chip
  and a test signed with a keychain entitlement: the one physical item, named once.

Evidence (2026-11-02, [session](../sessions/2026-11-02-net-2-key-store.md)): `test_key_store.cpp` (12 cases: every kind
of key through its whole life, every refusal, every changed byte of a store, an older and a substituted store, writers
in four threads, DPAPI on Windows, a TLS credential from a stored signer), `test_private_directory.cpp` and
`crd-fuzz-crypto-key-store` (a sealed body and a raw payload against restatements of K3, changes against a model of
K4 and K5), on win-debug, linux-gcc-debug and linux-gcc-asan, with seven source mutations each caught.

<a id="tls-13-profile"></a>
### The TLS 1.3 profile

TLS 1.3 is RFC 9846, which obsoletes RFC 8446 and keeps its version and wire format (`tls.v13`; the
[full re-research](../research/2026-11-01-tls-13-rfc-9846-re-research.md#delta) of 2026-11-01 maps each change). The
provider implements it; NET.2's conformance tests prove each point below of every allowed provider and of Cerid's own
record layers, never by trust:

- **TLS 1.3 only.** No Cerid endpoint negotiates an earlier version; a ClientHello without TLS 1.3 is refused with
  `protocol_version`.
- **Fresh key shares.** Every connection generates its own share, the ML-KEM half of a hybrid share included; a
  peer that reuses one is tolerated (RFC 8446 allowed it), never relied on.
- **Key usage limits.** Every record layer Cerid runs (TLS records, QUIC packets under RFC 9001's confidentiality and
  integrity limits, and the authenticated raw UDP path) counts the records protected under each key and updates the
  key before the limit (for AES-GCM, 2^24.5 full-size records); 0-RTT data, which cannot be rekeyed, stays below it.
  A receiver never enforces its peer's limit.
- **Key updates.** The epoch never exceeds 2^48-1; a sender has at most one `update_requested` outstanding; a
  KeyUpdate is protected with the old keys, and records under the new keys are accepted only after it. A key update
  is also where a session adopts a new policy generation ([agility](#agility)), so swaps count in the same epoch.
- **Tickets.** A client without a ticket store ignores NewSessionTicket; a server issues a new ticket with every
  connection; a client never reuses a ticket and never offers one across connections meant to be uncorrelated (T07,
  T15). External PSK identity labels are random; a PSK is shared by one client and one server only, or through an
  importer (RFC 9258).
- **Alerts.** `user_canceled` is followed by `close_notify`. Toward a peer not yet authenticated, a server answers
  with `general_error` where a specific alert would reveal whether an identity or capability exists; a PSK-only
  server treats an unknown identity and a wrong binder alike. Authenticated peers receive the specific alert.
- **Raw public keys and pinned credentials** are bound out of band; the server checks that `server_name` names its
  identity, or both sides use `external_id_hash` (RFC 8844), against misbinding.
- **GREASE.** Every build's ClientHello carries RFC 8701's reserved values (`ietf.grease`), chosen per connection, in
  its cipher suites, supported groups, supported versions and key shares and as two extensions; a ClientHello sent
  again after a HelloRetryRequest keeps them, and a server ignores them and never negotiates one.
- **Registered values.** Groups, signature schemes and extensions are the register's codepoints, with IANA's
  "Recommended" value recorded ([registers](registers.md#recommended-column)); a discouraged ("D") codepoint is never
  accepted. ML-DSA certificates sign with the `mldsa44`/`mldsa65`/`mldsa87` schemes (`tls.ml-dsa`); NET.2 decides
  the classical companion (a dual certificate, or a composite scheme once one is registered).

<a id="protocol-model"></a>
### The protocol model

NET.2's model clause: Tamarin models of Cerid's sessions in
[tests/foundation/crypto/model](../../tests/foundation/crypto/model), proven in CI by
[check-protocol-model.py](../../scripts/check-protocol-model.py) with the pinned Tamarin 1.12.0 and Maude 3.5.1
(`tools/tamarin-linux-x64`, scripts/install-tamarin.py; the `crd-crypto-protocol-model` CTest on the
linux-gcc-public-checks lane). Every lemma a model holds is listed in the checker and must verify; a wellformedness
warning, an incomplete or falsified proof, a lemma added or removed unlisted, or a design anchor the model cites that
no longer exists fails the check.

- **The full handshake** (`tls_handshake.spthy`): the hybrid group with its classical companion (S3), keys pinned by
  name and mutual authentication (S4, S5), ephemeral compromise split into a classical (X25519) and a post-quantum
  (ML-KEM) break. Proven: the handshake completes; stripping the hybrid share cannot downgrade a client to the
  classical group with an honest server; injective agreement on the handshake secret and group both ways; the
  handshake secret and both traffic secrets stay secret unless the peer's key was revealed before the handshake ended
  (forward secrecy) or both kinds of key exchange are broken (the hybrid guarantee).
- **The traffic key epochs** (`tls_key_update.spthy`, S7): one direction's chain from a traffic secret the handshake
  model proves secret, records under the current key at their sequence number, KeyUpdates under the old key, and
  either end's current key revealed at any time. The chain is unbounded: epoch e's key is a private `chain(root, e)`
  and the adversary's one computation over it is HKDF-Expand-Label's step from a key it holds to the next, so the
  induction runs over epochs (natural numbers). Proven for chains of any length: forward security of the updates (a
  key leaks only by its own or an earlier key's reveal), records and KeyUpdates accepted only as sent at their epoch
  and sequence number unless their key or an earlier one was revealed, and, as the guarantee's bound, no healing
  after a compromise. That a record is accepted once is the adversarial-bytes test's (test_tls.cpp: a replayed,
  reordered or earlier-key record fails).
- **Resumption and early data** (`tls_resumption.spthy`, S9, S16, S17): tickets sealed under a ticket key with a
  fresh salt and sent under the session's traffic key, a client store that hands each ticket out once, psk_dhe_ke
  resumption with the binder and early data, the server's single-use ticket filter, and reveals of the ticket key,
  either end's session state and either key share at any time. Proven: a PSK becomes known only after its ticket key
  or issuing session was revealed; a server accepts the same early data at most once unless its ticket key was
  revealed (a session reveal allows no replay); early data is accepted only as its client sent it and stays secret
  unless its PSK leaked; injective agreement on the resumed handshake secret both ways (the server's side also
  allows a broken key exchange: Tamarin did not terminate without that disjunct, which an attack needs together with
  the PSK); both resumed traffic secrets are forward secret; and, as the bound, a ticket key revealed after the early
  data was sent exposes it. One link of the chain is modelled: a resumed session issues tickets by the same rule, and
  its traffic secret lemmas give its master secret the guarantee the link assumes of a full session's, so the chain
  composes (Tamarin's unrolled chain did not terminate).
- **Address validation** (`quic_address_validation.spthy`, S13, S16): addresses an adversary can send to from any
  claimed source but read only where it owns the path, a fleet's token keys with rotation (the newest seals, the one
  before it opens) and reveals, Retry and NEW_TOKEN tokens with their associated data as `token_associated` builds
  it, validation by a token or by the handshake, NEW_TOKEN tokens published, and the single-use filter. Proven: an
  address is validated only after a datagram the server sent there reached the client there, unless the adversary
  owns that path or a token key leaked (no amplification toward a spoofed address); each kind is accepted only with
  the kind, version, address and, for Retry, port and connection IDs it was minted for; a NEW_TOKEN token is accepted
  once; a token from before the last rotation opens and one from before the last two never does; and, as the bound,
  a replayed Retry token is accepted again within its binding. Time (lifetimes, skew) and unlinkability are
  test_quic_token.cpp's.
- **The binding to the code** is [state learning](#state-learning): the implementation's machine, learned, holds the
  lemmas' trace properties.

<a id="state-learning"></a>
### State learning

The models prove the protocol; state learning proves that the code runs the protocol the models describe (protocol
state fuzzing: de Ruiter and Poll, USENIX Security 2015; Fiterau-Brostean et al. for SSH and DTLS).
[check-state-machine.py](../../scripts/check-state-machine.py) learns each end's `TlsSession` as a Mealy machine
through `crd-crypto-tls-sul` (tests/foundation/crypto/state), a system under learning that maps abstract inputs onto
a real session (the subject) and an honest peer of crd-crypto's, and their outcomes onto abstract outputs.

- **The alphabet.** What the subject sends reaches the peer; what the peer sends waits in a queue the learner drives:
  `deliver` it, `drop` it, `replay` the last delivery. The other inputs are each end's calls: data, a key update with
  and without a request, close. The subject's update bound is 3, so the bound is part of the machine. An output is the
  call's result, both ends' states (a failed end with the alert it sent) and what the step caused: data read, a
  KeyUpdate received, an update made by the session itself. Forged handshake messages are outside this alphabet; the
  fuzz harness and the adversarial-bytes tests carry them.
- **Learning.** L* for Mealy machines with Rivest and Schapire's counterexample analysis, the W-method with one extra
  state as the equivalence oracle (exact against every machine of at most one more state) and 2000 seeded random words
  beside it; every answer is cached in a trie, a sample is asked again outside the cache, and a different answer is a
  failure (a nondeterministic system is never learned). Each query starts fresh sessions, so copies of the SUL
  (`--jobs`, four in the CTest) share every batch, and the queries go in from a writer thread while the answers are
  read, whatever the pipes' buffers.
- **The reviewed machine.** Each end's learned machine is committed (`tls_server.mealy`, `tls_client.mealy`) after
  review; a learned machine that differs fails with the shortest input word that tells the two apart, so every change
  of behaviour is reviewed before `--write` accepts it.
- **The properties**, run as monitors over the product of the learned machine and the monitor, each violation with its
  shortest witness: `records_authentic` (record_authentic, update_authentic: data and KeyUpdates are accepted only
  from a delivery in order, never from a replay or after a drop, and no data before the handshake ends),
  `handshake_in_order` (handshake_completes and agreement: established only by the peer's flights in order),
  `terminal` (a failed session keeps its alert; a closed one never reopens or reads), `update_bound` (fewer own updates
  than the bound) and `requests_answered` (updates_complete: a requested update is answered, nothing else updates by
  itself, and an end asks again only after its request was answered, RFC 9846 section 4.7.3).

Evidence: the `crd-crypto-state-learning` CTest (every desktop lane); the learner's own tests in
test-repository-tools.py (random machines learned exactly, a combination lock found only within the W-method's bound,
nondeterminism and malformed files refused, each property's violation found with its shortest witness).

<a id="tls-sessions"></a>
### TLS sessions

[tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp) runs TLS 1.3 over the provider's TLS stack
(AWS-LC's libssl, linked privately into `crd-crypto` beside libcrypto; NET.2, 2026-11-01). Decisions (numbered for the
tests):

- **S1. Sans-I/O.** A `TlsSession` is fed the bytes its transport received (`receive`) and hands back the bytes to send
  (`pending`, `take`); it never sees a socket, so TCP, QUIC's crypto stream, NET-SIM's virtual sockets and a session in
  the same process drive it alike. At most `kTlsMaxBufferedBytes` (64 KiB) of received and of outgoing bytes wait in a
  session; more is `Full` with nothing taken (bounded admission's per-session share).
- **S2. Contexts and sessions.** A `TlsContext` is built once from a `TlsConfig` and is read-only, serving sessions on
  any thread; each session copies the settings its callbacks read (pins, accepted signature algorithms, the server
  name), so clearing a context never strands a live session.
- **S3. Algorithms by identifier.** Groups, the peer's signature algorithms and AEADs are `AlgorithmId`s the provider
  implements, checked by class before the provider sees them and mapped to the register's TLS codepoints. The
  server's group order decides; a client shares its first group and its first group of the other kind (post-quantum
  or classical), and a server whose preferred group has no share sends a HelloRetryRequest, so a hybrid group both
  sides hold is never traded for a classical one. The AEAD is the client's first one the server accepts, or
  ChaCha20-Poly1305 for a server without AES instructions (the provider's rule); `tls_aead_preference()` orders a
  client's AEADs by its CPU.
- **S4. Pinned keys, not authorities.** A peer is accepted when the SHA-256 of its certificate's public key in the
  provider interface's encoding (`key_id_of`) is pinned and its algorithm accepted; the certificate is the key's
  carrier, and the handshake's CertificateVerify proves possession. A `TlsCredential` issues the self-signed carrier
  for a `SigningKey` (X.509 v3, the serial from the key identifier, the name as DNS subjectAltName and, when it fits
  RFC 5280's 64-character ub-common-name, as common name too, else with an empty subject and a critical subjectAltName;
  valid from the epoch with no expiry: trust and rotation come from the pins). A client always authenticates its
  server; a server with pins asks every client for a credential (mutual authentication). AWS-LC implements no RFC
  7250 raw public keys, so the carrier certificate stands in for them on the wire.
- **S5. Names and alerts.** A client names the server it expects (`server_name`); a server with a name refuses any
  other with `general_error`, and refuses an unpinned client key with `general_error` too, so an unauthenticated
  client learns neither which names nor which keys exist. Every alert sent and received is reported, as is a
  HelloRetryRequest (read from the ServerHello's random).
- **S6. Limits.** 0-RTT is off unless both ends configure it ([S17](#early-data)): early data carries only
  operations a schema declares idempotent (the TLS 1.3 profile, T07), which the server-authorized message schemas
  bind ([S18](#message-schemas)). `close()` is for an
  established session (a handshake is abandoned with `clear()`). The QUIC integration is [S11](#quic-handshake)
  and its address-validation tokens [S13](#address-validation); Encrypted Client Hello is [S14](#ech); the exporter
  that binds capabilities to sessions is [S15](#capability-tokens).
- **S7. Key usage limits and key updates.** A session counts the records protected under its sending key (the
  provider's write sequence, which a new key resets) and updates the key before `key_update_records()` records: the
  negotiated AEAD's limit (`tls_key_usage_limit`: floor(2^24.5) for AES-GCM, every 64-bit sequence number for
  ChaCha20-Poly1305), or a lower `TlsConfig::key_update_records` of at least 3. Each key keeps room for its own
  KeyUpdate and one alert, so data takes at most `key_update_records() - 2` records; `send()` updates the key between
  records and counts the updates' bytes against the outgoing bound, and a message is sent whole or not at all.
  `update_keys(request_peer)` updates on demand; while a request is outstanding (sent, not yet answered by a peer
  KeyUpdate) a second one updates only the session's own key (RFC 9846 section 4.7.3). The provider answers a peer's
  request by itself; `read()` moves the answer into the outgoing bytes at once when they have room, else ahead of the
  next record or update, and every bound counts it. KeyUpdates are counted each way (`key_updates_sent`,
  `key_updates_received`); a receiver never enforces its peer's limits. The session's own count never passes
  `max_key_updates` (2^48-1, or lower by configuration): an update of its own that would reach it, by `send()` or on
  demand, sends `close_notify` instead and returns `Closed` (the last update is kept for a peer's request, which the
  provider would answer unasked), and an answer that reaches it closes the session after it; the session resumes on a
  new connection.
- **S8. Closure.** The provider can send `close_notify` only after the handshake and no warning alert at all, so a
  Cerid session never sends `user_canceled`: a handshake is abandoned with `clear()`, which RFC 9846 section 6.1
  allows (the alert's `close_notify` rule binds only a sender). A peer's `user_canceled` is ignored (reported in
  `alert_received()`), and its `close_notify`, during the handshake too, makes the session `Closed`, never `Failed`;
  `close()` of a session its peer closed during the handshake answers `Closed` and sends nothing. More than four
  warning alerts in a row fail the session with `unexpected_message` (the provider's bound).
- **S9. Tickets and resumption.** A server issues one ticket with every connection, a resumed one included, and sends it
  with the handshake's last flight (the provider holds a ticket until its server writes, so the session releases it with
  a zero-length write). The context's ticket method seals it, not the provider's built-in keys (which rotate inside one
  process and cannot be shared): the ticket key's 16-byte name (HKDF-SHA256 of the key with an empty salt, expanded
  under "cerid tls 1.3 ticket name"), a random 32-byte salt, then the provider's serialized session sealed with
  AES-256-GCM under the key and nonce HKDF-SHA256 derives from the ticket key and that salt ("cerid tls 1.3 ticket
  seal"). Every derived key seals one ticket, so no count of tickets has to be shared across the servers of a fleet; the
  name only selects the key. Servers given the same `ticket_key` resume each other's tickets; a rotation keeps the
  former key as `previous_ticket_key`, which opens and never seals; a server without a key draws one at `init()` that
  only its context opens. A ticket under an unknown name, too short or forged is ignored and the handshake runs in full;
  only a provider failure fails it. A server keeps no session cache, so no unauthenticated peer fills its memory.
  Tickets live `ticket_lifetime` (two days unless configured, at most seven: RFC 9846 section 4.6.1); a resumption's
  ticket keeps the longer of its remaining lifetime and the configured one, and never outlives the seven days after the
  full handshake that authenticated the peer (the provider's non-renewable authentication timeout). A client keeps each
  ticket for the lifetime its server states.

  A client session started with a `TlsTicketStore` hands it every ticket it reads, serialized, with its context's
  `TlsBinding`, and takes one when it starts: the store hands a ticket out once (single use, T07), and the session
  offers it only when it parses as a TLS 1.3 session of this binding that has not expired; anything else is dropped
  and the handshake runs in full. Without a store a client offers nothing and ignores NewSessionTicket. The caller keeps
  one store per correlation domain, so connections meant to be uncorrelated share no ticket (T15). The binding is the
  SHA-256 of the role, the server name in lower case, the own key, the pins and the accepted signature algorithms
  (sets), and it is the provider's session ID context, which both ends check: a server whose pins, name, credential or
  algorithms changed resumes no ticket it issued before, so a client key no longer pinned authenticates in full and is
  refused (`general_error`), and another credential never answers for the key a ticket proved. Resumption is
  psk_dhe_ke only: a fresh key share and the same group negotiation (a HelloRetryRequest included, the ticket offered
  again with a new binder), so a hybrid group both sides hold is kept. A resumed session reports `resumed()`, its group
  and AEAD and the peer key the ticket's handshake proved; it takes the round trips of a full handshake, and its
  server's flight carries no Certificate or CertificateVerify (over 8,000 bytes fewer with an ML-DSA-65 key).
- <a id="certificate-compression"></a>**S10. Certificate compression** (RFC 8879, `tls.certificate-compression`). A
  client decodes its server's Certificate compressed with zstd or zlib (`TlsConfig::certificate_compression`, offered
  in that order by default); a server may compress its own with them. The codecs are the engine's (crd-compression:
  the pinned zstd, the engine's own zlib); brotli is not offered, since the engine has no brotli codec. A server makes
  its Certificate message once at `init()` (an empty request context and one entry without extensions, the bytes the
  provider writes for the credential), compresses it with each algorithm it may use (zstd at its highest level) and
  keeps a form only when its CompressedCertificate (the form and 8 bytes of header) is smaller than the plain message;
  the kept forms, smallest first, live in the provider context and are copied into each handshake, so no handshake
  compresses and a client never receives a form larger than the plain message. A client decodes into a buffer of
  exactly the claimed length: a claim above `kTlsMaxCertificateBytes` (64 KiB, the limit a plain Certificate has too,
  RFC 8879 section 5) is refused by the provider, a zlib claim DEFLATE's 1032:1 cannot reach before anything is
  reserved (one 3-byte zstd block header already reaches the bound), and a decoding of any other length, a corrupt frame
  and a wrong Adler-32 fail the handshake; any valid form is accepted, even one larger than the plain message. The
  provider answers a refused form with `decode_error` where RFC 8879 section 4 names `bad_certificate` (its alert, not
  Cerid's choice; the handshake fails either way). The provider's server never asks for a client's Certificate
  compressed, so clients compress nothing. `certificate_compression()` reports the algorithm a session sent or
  received (0 for a plain Certificate and a resumption). An empty list turns compression off, for a context whose
  record padding must not depend on the credential (RFC 8879 section 5). Measured (the server's first flight,
  ServerHello to Finished, the client offering both against offering none): Ed25519 1,651 to 1,612 bytes (zlib),
  ML-DSA-44 7,668 to 7,615, ML-DSA-65 10,086 to 10,044 and ML-DSA-87 13,362 to 13,297 (zstd), ECDSA P-256 and P-384 by
  zlib. A pinned
  carrier saves tens of bytes: its key and signature are random and only the names and the DER structure compress. So
  certificate compression does not bring a post-quantum flight under QUIC's amplification limit (three times the
  client's bytes); address-validation tokens and resumption do, and are measured with the QUIC integration.

Evidence: `test_tls.cpp` (every group and signature algorithm, each key share read off the wire and checked against
its group's layout with the provider's own X25519, ECDH and ML-KEM modulus check, the hellos' GREASE values read off
the wire, HelloRetryRequest, pins both ways, names, alerts, bounds, close_notify, key updates under record budgets, on
demand, with one request outstanding and at the update bound, a peer's `user_canceled` and `close_notify` during the
handshake, tickets and resumption (one ticket per connection, single use, the wire's pre_shared_key and
psk_key_exchange_modes and the ticket format, bindings, a careless store, a fleet's shared key and its rotation, a
drawn key, another credential, a client no longer pinned, tickets changed at rest in each field and past their
lifetime, the lifetime a client keeps, a HelloRetryRequest), certificate compression (the offer read off the
ClientHello, the forms and flight sizes per signature algorithm, the algorithm both ends allow, none, a resumption,
the bounded decoder, and a server of the provider's own stack sending a valid form larger than the message, a form of
another length, one that is no zstd frame and one with a wrong Adler-32), credentials named up to 253 bytes,
adversarial records, every configuration refusal, one context on four threads), the `crd-fuzz-crypto-tls` target (a
fixed ticket key, a forged ticket in a ClientHello, tickets a client's store hands out, compressed Certificates against
restated claim bounds and Adler-32), and `tls_interop.py` against the pinned OpenSSL (`CRD_BUILD_TLS_VS_OPENSSL`,
Linux; built with the pinned zstd and the system's zlib; since 2026-11-02 the `crd-crypto-tls-interop-openssl` CTest of
the complete-tier `linux-gcc-public-checks` lane, beside the package consumer test that builds an application against
`Cerid::crypto`): every hybrid group both ways, a HelloRetryRequest both ways,
hybrid kept beside a classical share both ways, mutual authentication, an unpinned key refused, a TLS 1.2 client
refused with `protocol_version`, key updates both ways (a Cerid client updating before every record under a
three-record budget, asking for an update and not asking, and an OpenSSL client asking a Cerid server), resumption both
ways across processes, and certificate compression both ways with zstd and with zlib, an end that decodes none, and
OpenSSL servers installing hostile forms (a claim one byte off either way, claims over the bound, an unreachable zlib
claim) and seeded corruptions of real forms; the driver checks the key shares, Cerid's GREASE values and each
compress_certificate offer on the wire and each end's KeyUpdate counts and compressed Certificates against the other's.

<a id="quic-protection"></a>
### QUIC packet protection

[quic.hpp](../../engine/foundation/crypto/include/crd/crypto/quic.hpp) is RFC 9001 section 5 for QUIC versions 1
and 2 (RFC 9369) over the provider interface: HKDF-Expand-Label and the AEAD keys of `provider.hpp`, and for header
protection only the provider's AES block and ChaCha20 stream functions. The handshake decides which secrets exist
([the QUIC handshake](#quic-handshake) of `tls.hpp`); framing, connection IDs and recovery are `crd-net`'s (4.2a).

- **Initial secrets** for the client-chosen Destination Connection ID (0 to 20 bytes) under each version's salt.
- **Packet keys** (`QuicPacketKey`): the AEAD key and IV from a traffic secret with "quic key" and "quic iv"
  ("quicv2 ..." for version 2), the suite named by its AEAD (AES-128-GCM and ChaCha20-Poly1305 with SHA-256,
  AES-256-GCM with SHA-384). A packet is one buffer, the header (associated data, its packet number encoded) then the
  payload and room for the tag, sealed and opened in place under the IV exclusive-or the packet number; a failed
  open zeroes the payload. `update()` derives the next key phase with "quic ku" and leaves the current key intact for
  reordered packets.
- **Usage limits** (RFC 9001 section 6.6) are enforced in code: a key counts what it seals and refuses (`Exhausted`)
  at 2^23 packets for AES-GCM (ChaCha20-Poly1305's limit lies beyond every packet number), or at a lower limit the
  transport sets; `update_due()` turns true at three quarters so a key update completes first.
  `quic_integrity_limit` (2^52 AES-GCM, 2^36 ChaCha20-Poly1305) is counted by the connection across all its keys,
  so a key counts nothing on open and a receiver never enforces its peer's limit.
- **Header protection** (`QuicHeaderKey`, "quic hp", kept across key updates): the mask from AES-ECB or ChaCha20 over
  the 16-byte sample 4 bytes past the packet number's start; a packet too short to sample is `BadLength`, never
  partly protected. Removal reports the packet number's length and truncated value; nothing it exposes (key phase,
  reserved bits) may change connection state before the AEAD opens.
- **Packet numbers**: the encoder's length covers twice the unacknowledged range (RFC 9000 appendix A.2, 0 when no
  4-byte encoding reaches it) and the decoder takes the candidate closest to the expected number within [0, 2^62)
  (appendix A.3).
- **Retry integrity tags**: AES-128-GCM under each version's fixed key and nonce over the pseudo-packet, built on the
  stack; a Retry is at most 1,472 bytes (the UDP payload of a 1,500-byte IPv4 path; a larger one is refused rather than
  given a heap buffer) and its tag is compared in constant time.

Nothing allocates; open, header protection and the Retry functions are read-only and run on several threads, while
init, update and seal take the sender's exclusive access. Evidence: `test_quic.cpp` (RFC 9001 and RFC 9369 appendix A
byte for byte: both Initial secrets, the client's 1,200-byte and the server's Initial packets sealed, protected and read
back, both Retry tags with every bit of the packet and the original connection ID authenticated, the ChaCha20-Poly1305
short header packet and its key update against the "ku" secret; RFC 9000's packet number examples and every window
edge; refusals, limits, a changed byte anywhere in a packet, random round trips in every suite and version, and four
threads on one key) and the `crd-fuzz-crypto-quic` target (header protection refused exactly when unsampleable and
restored by a round trip, an opened packet re-sealing to its input, Retry verdicts against the computed tag, packet
numbers against a restatement of appendix A.3, and seeds whose verdicts the harness checks).

<a id="quic-handshake"></a>
### The TLS handshake over QUIC

`QuicHandshake` ([tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp)) runs the TLS 1.3 handshake of
the same contexts over QUIC (RFC 9001 sections 4 and 8, `quic.tls`) through the provider's QUIC method; a stream
session and a QUIC handshake derive from one handshake state, so pins, names, groups and HelloRetryRequest, tickets,
certificate compression and every provider callback are shared code. Decisions:

- **S11. The QUIC handshake.** Sans-I/O as S1: `provide(level, bytes)` takes the CRYPTO data the transport received
  at an encryption level, in order (the transport reassembles CRYPTO frames by offset), and `pending(level)` and
  `take(level, ...)` hand back the CRYPTO data to send per level, at most `kTlsMaxBufferedBytes` across the levels (a
  flight beyond it fails the handshake). The levels carrying CRYPTO data are Initial, Handshake and Application; the
  Early level carries none and has the client's 0-RTT keys only ([S17](#early-data)). Each level's secret of each side (`has_keys`) becomes the `QuicPacketKey`
  and `QuicHeaderKey` of the connection's version through `derive_keys`, once: the secret is zeroed as it is handed
  out, so no raw secret leaves `crd-crypto`, and `QuicPacketKey::update` derives every later key phase; Initial keys
  are `quic_initial_secret`'s. A level's write secret comes before the peer's read secret, so a packet can always be
  acknowledged at its own level, and a server's Application write secret comes before its client's Finished
  (half-RTT; the client is not yet authenticated). The transport parameters (1 to `kQuicMaxTransportParameterBytes`,
  the connection's own) are set at `init()` and travel in `quic_transport_parameters` (57, never the drafts' 0xffa5);
  `peer_transport_parameters()` gives the peer's as received, and their encoding and checks are the transport's
  ([4.2a](../ROADMAP.md#slice-4.2a)); a hello without them fails with `missing_extension`. Data at a level the
  handshake does not read now fails it without an alert (the transport's PROTOCOL_VIOLATION); data past the level's
  `receive_limit()` (at least 4096 bytes, RFC 9000 section 7.5) is `Full` with nothing taken (CRYPTO_BUFFER_EXCEEDED).
  A sent alert is reported with the level it was written at (`alert_level`); the transport closes with CRYPTO_ERROR,
  `quic_crypto_error(alert)` = 0x0100 plus the alert, at that level and, before the handshake is confirmed, at the
  lower levels its peer may still read only (RFC 9000 section 10.2.3). There is no TLS KeyUpdate in QUIC (RFC 9001
  section 6): a peer's fails the handshake with `unexpected_message`, and stream-only calls (`send`, `read`, `close`,
  `update_keys`) do not exist on a QUIC handshake. A server's NewSessionTicket goes out at the Application level with
  its last flight (the provider flushes it), and a client provides it there after the handshake. Tickets are kept and
  offered under a QUIC binding (SHA-256 of "cerid quic binding" and the context's binding), which is also the session
  ID context, so tickets of a context's stream sessions and QUIC handshakes never meet (the provider would not resume
  one on the other transport).
- **S12. Application protocols** (RFC 7301, `tls.alpn`). `TlsConfig::application_protocols` (at most
  `kTlsMaxProtocols`, each 1 to 255 bytes and listed once, at most `kTlsMaxProtocolListBytes` on the wire) is a
  client's offer in its order and a server's list in its preference; the server selects its first protocol that the
  client offers (section 3.2). With protocols on either end the two agree or the handshake fails with
  `no_application_protocol` (120) and `PeerRefused`: a server refuses a client that offers none or none of its own, and
  a client refuses a server that selects none (over a stream, after its Finished, so the server reads the alert). A
  QUIC handshake needs at least one (RFC 9001 section 8.1). `application_protocol()` reports the agreement. Like the
  extension itself, a ticket does not bind the protocol: every handshake negotiates it afresh. The game and project
  channels of D3 are separate protocol identifiers.

Measured (CRYPTO bytes, X25519MLKEM768, the server's Initial and Handshake data): the ClientHello is 1,450 bytes, so
with its packet header and tag it exceeds the 1,200-byte minimum Initial datagram and needs two Initial packets, the
transport's to send (4.2a); a full server flight is 1,580 bytes with an Ed25519 key, 1,648 and 1,746 with ECDSA
P-256 and P-384, and 7,580, 10,010 and 13,267 with ML-DSA-44, -65 and -87 (compressed certificates); a resumed one is
1,259 bytes after a 1,704-byte ClientHello. A server sends at most three times what its client sent before it
validates the client's address: with the two Initial datagrams a hybrid ClientHello takes, at least 7,200 bytes, or
8,832 when both fill a 1,500-byte path. Every classical flight and every resumption fits in one round trip; an ML-DSA
flight does not fit two minimum datagrams' limit, and ML-DSA-65 and -87 not even two full ones', so a post-quantum
server validates its client's address first ([address-validation tokens](#address-validation), measured there in
round trips) or resumes.

Evidence: `test_quic_handshake.cpp` (every group and AEAD, every signature algorithm with mutual authentication, each
level's keys opening the other end's packets in both versions; the levels each message and secret arrives at; a
HelloRetryRequest at the Initial level; unpinned keys refused with their alerts at their levels; application protocols
over QUIC and over a stream, agreed, refused both ways and required; a ClientHello without transport parameters; the
flights against the amplification limit; tickets under the QUIC binding that never cross transports; a TLS
KeyUpdate, data at levels not read and garbage; the receive limit; refusals; four threads), the `crd-fuzz-crypto-tls`
target's QUIC mode (CRYPTO data per level, never established, no client Application secret, every held secret
derived once), and `tls_interop.py` against OpenSSL's QUIC TLS interface (`SSL_set_quic_tls_cbs`): the hybrid
groups both ways in versions 1 and 2, each of the three cipher suites, a HelloRetryRequest and mutual authentication
both ways, protocol selection and refusal both ways, an unpinned key, resumption and certificate compression both ways,
with the hellos read off the wire and, for each of the four secrets, a packet sealed and a mask made under the keys
each end derives (RFC 9001 section 5 through crd-crypto on one end and OpenSSL's HKDF and ciphers on the other)
compared byte for byte.

<a id="address-validation"></a>
### Address validation tokens

**S13. Address validation tokens** ([quic.hpp](../../engine/foundation/crypto/include/crd/crypto/quic.hpp), RFC
9000 section 8.1). A QUIC server validates a client's address statelessly, before it sends more than three times what
it received (threats T01 to T03): the tokens of a Retry packet and of NEW_TOKEN frames hold everything the server
needs back, sealed under a key the servers of a fleet share. `crd-crypto` mints and checks the tokens
(`QuicTokenKeys`, `ReplayFilter`, `quic_token_kind`); the transport (4.2a) builds the Retry
packet around a token with `quic_retry_tag`, carries tokens in Initial packets and NEW_TOKEN frames, and puts the
claims' original connection ID into `original_destination_connection_id` and the Retry's into
`retry_source_connection_id` (RFC 9000 section 7.3).

- **Format.** One clear byte names the format and the kind (0x11 Retry, 0x12 NEW_TOKEN; section 8.1.1 requires the two
  to be told apart, and the clear kind decides, before anything is authenticated, between closing with INVALID_TOKEN
  for a Retry token that does not validate, section 8.1.2, and proceeding as without a token for any other, section
  8.1.3), then the sealing key's 4-byte public name, a 16-byte random salt (128 bits of entropy, section 8.1.4, and
  what makes every token unique, section 8.1.3), then the sealed claims (the issue time, 8 bytes, and a Retry token's
  original Destination Connection ID with its length) and the 16-byte AEAD tag: 45 bytes for a NEW_TOKEN token and 46
  plus the original connection ID for a Retry token (54 for an 8-byte ID, at most 66).
- **Sealing.** Each token is sealed with AES-256-GCM under its own key and nonce, HKDF-SHA256 of the fleet key
  salted with the token's salt (the construction of TLS tickets, decision S9, shared with them through
  `provider_internal`'s sealing keys). A random nonce under one long-lived key would bound a fleet to 2^32 tokens a
  key it cannot count across servers; a derived key seals one token, so the servers share no count and no nonce ever
  repeats. The cost is three HMAC-SHA256 computations and one AES key schedule per token, the same as a ticket's.
- **Binding.** The associated data is the clear bytes, the QUIC version (a client uses a token with the version it
  came from, section 8.1.3) and the client's IP address; a Retry token also binds the UDP port and both connection IDs
  of the client's next Initial: the Retry's Source Connection ID, now its Destination, and the client's own Source,
  which it may not change (section 17.2.5.2). A NEW_TOKEN token binds no port (section 8.1.3: it changes between
  connections) and no connection ID. Nothing linkable is readable: the address and the original connection ID are
  sealed or only authenticated (section 8.1.3).
- **Time.** Tokens carry the fleet's clock (Unix seconds, passed in; no clock is read here). A Retry token is accepted
  for 30 seconds (a client repeats it in every Initial, and four retransmissions from a one-second probe timeout fit),
  a NEW_TOKEN token for a configured lifetime (one day unless set, at most seven), each from 10 seconds before it was
  issued (servers' clocks differ); outside that it is `Expired`.
- **Rotation.** The keys hold a sealing key and the one before the last rotation, which only opens: a fleet rotates
  every server to the same next key, and tokens issued under the previous one stay valid until they expire. A token
  under any other key, like any change to a token or anything it is bound to, is `Forged`: a server never tells a
  stale key from a forgery.
- **Replay.** A NEW_TOKEN token is accepted once (section 8.1.4) when the server checks it against a `ReplayFilter`
  ([S16](#anti-replay)): a split-block Bloom filter (one cache-line block a token, one bit in each of its eight 64-bit
  words, 16 bits a token) indexed by the token's authenticated random salt, so no keyed hash is needed and only valid
  tokens enter it. Four slots by issue period: the last, this and the next period (a token may come from the future by
  the clock skew) and the one after, which is cleared before it begins. A false positive (about 0.1% at capacity) only
  costs its client the validation; one token checked on two threads at the same instant may be accepted by both, and a
  check after either refuses it. A Retry token is limited by its 30-second lifetime and its port and connection ID
  binding.

Measured with real handshakes (`test_quic_handshake.cpp`, X25519MLKEM768, the flights packed into the Initial and
Handshake packets and 1,200-byte datagrams a transport sends, 8-byte connection IDs, RFC 9002's 12,000-byte initial
window): the client's first flight is two datagrams (2,400 bytes, so 7,200 bytes may come back unvalidated), with a
Retry token or a NEW_TOKEN token as well. A Retry packet is 93 bytes (its 54-byte token), one datagram well inside the
3,600 bytes the triggering datagram allows. The server's flight is 2,400 bytes with a classical key and 7,963, 10,484
and 13,910 bytes with ML-DSA-44, -65 and -87. Round trips to the server's last handshake byte:

| Server key | Unvalidated | After a Retry | With a NEW_TOKEN token |
| --- | --- | --- | --- |
| Ed25519, ECDSA P-256 and P-384 | 1 | 2 | 1 |
| ML-DSA-44 and ML-DSA-65 | 2 (the amplification limit) | 2 | 1 |
| ML-DSA-87 | 2 (the amplification limit) | 3 (the initial window) | 2 (the initial window) |

A Retry buys a server statelessness under load for one round trip; it never saves one. A NEW_TOKEN token saves the
post-quantum handshakes the round trip the amplification limit costs them, up to ML-DSA-65; ML-DSA-87's flight passes
the initial congestion window itself, which is the transport's to size (4.2a).

Evidence: `test_quic_token.cpp` (both kinds bound to exactly their address, port, connection IDs, version and kind;
every changed bit and every other length; the lifetimes and the skew at their edges and the clock's extremes; a
fleet's shared key and two rotations; every refusal; 512 unique tokens with nothing readable; single use, periods and
slot reuse, a 2^18-token filter's false positives, 55 and 44 in 52,428 probes on two runs; four threads), the round
trips above, the `crd-fuzz-crypto-quic` token mode (the verdict of every input against the format restated over the
provider's HKDF and AEAD; seeds sealed by the restatement, each refused one traced to its reason), and nine source
mutations, each caught. A token is the server's own: no peer reads one, so interoperability is the Retry packet's
(the transport's, 4.2a, against the peers NET.5 names).

<a id="ech"></a>
### Encrypted Client Hello

**S14. Encrypted Client Hello** ([tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp), RFC 9849,
`tls.ech`; threat T15). A client encrypts its true ClientHello to its server's ECH key, so the server name, the
tickets it offers and its credential cross the wire only inside it; the outer ClientHello names the configuration's
public name. The provider runs the protocol (AWS-LC's ECH, shared mode: one server decrypts and serves); Cerid
decides who holds which key, what a client may send, and what a rejection lets it do.

- **Keys.** A `TlsEchKey` is a DHKEM(X25519, HKDF-SHA256) private key (RFC 9180) and the ECHConfig that publishes it:
  version 0xfe0d, a config_id, HKDF-SHA256 with AES-128-GCM or ChaCha20-Poly1305 (the client picks by its CPU), a
  maximum_name_length and the public name, a DNS name that no IPv4 parser reads as an address (section 6.1.7).
  `tls_ech_config_list` makes the ECHConfigList a service publishes (its DNS HTTPS record, RFC 9848, or Cerid's
  service discovery). Servers of a fleet share the private key, as they share a ticket key.
- **Rotation.** A server context holds its current keys, which it sends as retry configurations, and the keys before
  its last rotation, which it still decrypts with and no longer sends (RFC 9849 section 4.1); at most four, with
  distinct config_ids, so the provider tries one key per offer.
- **A client never leaks the name it was told to hide.** A client given a list offers ECH with its first usable
  configuration. The provider proceeds without ECH when none is usable (another KEM, a mandatory extension, an
  IPv4-like public name); Cerid's context refuses a list without a configuration of version 0xfe0d, and its session
  refuses to start (BadConfig, nothing sent) unless the provider reported encrypting an inner ClientHello, which it
  does only when the outer one offers ECH.
- **Rejection.** A server that cannot decrypt an offer (a stale configuration) completes the handshake under the
  public name and sends its retry configurations; the client authenticates that handshake by its pins (Cerid trusts
  pinned keys, not certificate names: the pins are its reference identity for the public name too), sends no
  credential, aborts with `ech_required` and reports `EchRejected` with `ech()`: `Retry` (retry once, with
  `ech_retry_configs()` passed to the next `TlsSession::init` or `QuicHandshake::init`, never stored: section 6.1.6),
  `Disabled` (the server sent no configuration of this version: whether to reconnect without ECH, revealing the name,
  is only the caller's decision) or `Refused` (a retried connection rejected again: the server's configurations
  disagree). A rejection a pinned key does not authenticate is `PeerRefused` and its configurations are never used.
- **The public name is not a destination.** A server with a name answers its keys' public names only for that
  recovery: a handshake that completes under a public name alone (a client without ECH naming it) is refused with
  `general_error`. A rejected client's own Finished ends that way too, behind which its `ech_required` arrives.
- **GREASE.** A client without a list sends a GREASE ECH extension (`ech_grease`, on by default; section 6.2), so its
  hellos look like ECH offers; a server ignores it and serves its own name.
- **Padding.** The provider pads the inner ClientHello's server name to the configuration's maximum_name_length and
  the whole to 32 bytes (section 6.1.3): two names 48 bytes apart give equal hellos under a maximum that covers both.
- **What the outer hello still shows.** The public name, the key shares (compressed into the inner hello by
  ech_outer_extensions) and, from AWS-LC, the application protocols (ALPN), which it treats as public; Cerid's
  protocol identifiers name a protocol family, never a service. AWS-LC adds no GREASE `pre_shared_key` beside a
  resuming inner hello (section 6.1.2's SHOULD): every outer hello lacks one, so resumption stays hidden among ECH
  offers but not among hellos without ECH. The ECH key exchange is classical X25519 (the only KEM
  RFC 9849 and both providers define for it): a harvest-now-decrypt-later adversary could later read an inner
  ClientHello's metadata, never the session, whose keys come from the hybrid groups. The register tracks post-quantum
  HPKE for ECH (`tls.ech`).

Measured (`test_tls_ech.cpp`, X25519MLKEM768, the ClientHello's CRYPTO bytes over QUIC): 1,444 without ECH, 1,666 with
GREASE and 1,635 with ECH under a 32-byte maximum_name_length; a 12-byte and a 58-byte name give 1,606 and 1,670
bytes without a maximum and 1,670 each under 64. Every form stays within the two 1,200-byte datagrams a hybrid
ClientHello already takes, so ECH costs no round trip; a rejection costs one new connection.

Evidence: `test_tls_ech.cpp` (the ECHConfig layout and every refused key, name and list; every group; a
HelloRetryRequest that keeps encrypting; resumption inside the inner hello with no PSK outside; a stale configuration
rejected and retried with and without mutual authentication; a second rejection refused; the previous key; an
unpinned rejection; a server without ECH; the public name refused without ECH; GREASE; the padding; lists the client
skips or cannot use; QUIC accepted, rejected and retried; the server name absent from every byte a client sends), the
`crd-fuzz-crypto-tls` ECH mode (any list: the context's verdict against the restated list shape, and a session that
either refuses to start or sends an outer ECH offer without the server name, over records and QUIC; hostile outer
hellos to a server holding two keys), five source mutations (four caught; the fifth, GREASE left on beside a list, is
equivalent: the inner-hello report, not the wire, decides), and `tls_interop.py` against OpenSSL 4.0's ECH: offers
both ways over records and from a Cerid client over QUIC versions 1 and 2, a HelloRetryRequest both ways, stale
configurations rejected and retried with the configurations each library sent, both servers' previous keys, a server
without ECH, and GREASE both ways, with the outer hello read off the wire and the server name absent from every
visible byte. OpenSSL 4.0.3's client cannot offer ECH over its QUIC TLS interface (its transport parameters are a
custom extension its ECH cannot copy, internal_error before it sends anything), so a Cerid server's QUIC ECH is proven
against Cerid clients.

<a id="capability-tokens"></a>
### Capabilities

**S15. Capabilities** ([capability.hpp](../../engine/foundation/crypto/include/crd/crypto/capability.hpp), D5). The
authority a principal holds is a token `crd-crypto` issues, delegates, presents and verifies; NET.3 maps each message
of a schema to an operation bit and charges its budget at admission (S6, S7), and the session layer (4.2a) presents
the token over its session's control channel and renews it there.

- **Identities.** A principal (user, device, service, server, agent) is named by its long-term key: the SHA-256 of
  the public key in the provider interface's encoding (`KeyId`), the identifier TLS pins. A token names its subject
  (the principal the grant is about) and its holder (the key that presents it), which may differ: a user's grant held
  by one of the user's devices.
- **Token.** Magic `CRDCAPTK`, a format byte with an N-1 reader, then the root: a random 16-byte token identifier,
  issue and expiry times (at most one day apart: minutes for game and agent tokens, hours for collaboration), the
  policy generation the issuer held, the 16-byte audience (the service that admits it), the subject, the holder
  (algorithm, key, binding) and the grant. A grant is a scope (its kind, the session type, and a 16-byte identifier),
  at most 16 object ranges in canonical form (ascending, a gap of at least one object between two, none meaning every
  object of the scope), a 64-bit operation set and a budget (operations and bytes per second, objects held). Every
  integer is big-endian; decoding is exact, bounded (48 KiB a token) and allocation-free, every malformed field
  refused with its own error.
- **Hybrid issuers and rotation.** The issuing service signs the root with every key of its issuer set whose issuing
  window holds the issue time, the signatures in the signature list policy generations use (one shared codec,
  `signed_artifact`). At least one post-quantum and one traditional key must cover the issue time (D7's hybrid
  rule), or the token is refused (`UntrustedIssuer`). Rotation is a new pair whose window starts where the old one
  ends: a token keeps verifying under the keys that covered its issue time until it expires, and a key whose window
  ended more than a day ago can only sign tokens that have expired.
- **Attenuation.** A holder delegates offline by appending a block (an expiry, the next holder, a grant) signed with
  its own key over every byte of the token before the block's end, with its public key beside the signature, whose
  hash must be the holder named before (`WrongHolder`). A block may only narrow: the same scope, each range inside one
  of the parent's ranges, a subset of the operations, every budget and the expiry at or below the parent's, and a
  Peer binding kept (`Widened`). At most four blocks. The cost per block with an Ed25519 holder is 185 bytes plus
  16 a range, with ML-DSA-44 3,821 plus 16 a range.
- **Session binding.** A token is bound to its holder's key, and each presentation to its session: with a Proof
  binding the holder signs `CRDCAPPR`, the session's binding and the SHA-256 of the token; with a Peer binding the
  session's mutual authentication is the proof (the TLS peer key and algorithm must be the holder's). The binding is
  32 bytes of the session's exporter (RFC 9846 section 7.5; RFC 9001 section 4 over QUIC) under the session type's
  own label, `EXPERIMENTAL Cerid capability world` and its project, room, upload, tool and service siblings (RFC 5705
  section 4 leaves labels beginning with EXPERIMENTAL to private use without registration). D3's separate key labels
  per session type follow: a proof made in a project session never binds a game session. The design's earlier
  wording bound the token itself to one session; binding the holder's key instead keeps a token valid across the
  reconnections of its lifetime and makes delegation possible, while a stolen token without its key, or a proof
  replayed in any other session, is still useless (`Unbound`).
- **Service links.** An audience configured `peer_only` admits Peer tokens only (`PeerRequired`), so every service
  link is mutually authenticated by the session and bound to the authenticated key.
- **Revocation generations.** Magic `CRDREVOK`: a monotonic generation number, issue and expiry times (at most seven
  days), a cut-off (every token issued before it is revoked: a mass revocation after a compromise), the oldest policy
  generation still honoured (tokens issued under an older one are revoked: the follow-up to a remote disable), and
  ascending lists of revoked token identifiers (at most 4,096) and keys (at most 512, matching the subject and every
  holder of a chain), signed like a token by the issuer keys covering its issue time. `CapabilityAuthority` adopts
  them as `PolicyStore` adopts policy generations (a floor the caller persists, rollback refused before any signature
  is checked, conflict, replay, issue skew, expiry, issue order) and fails closed: without a current generation, or
  past its expiry, nothing is admitted (`RevocationStale`). Every admission and every renewal verifies again.
- **Policy.** Every signature algorithm on a token (the issuers' and every holder's) must be accepted by the session's
  policy snapshot (`AlgorithmRefused`), so a remote disable reaches tokens at once.
- **Order.** `verify` checks the configuration, the decoding, the audience and session type, the audience's lifetime
  bound, each block within its parent, the issue time (300 seconds of skew) and the chain's expiry, peer-only, the
  revocation generation, the issuer set, the policy, the issuers' signatures, each block's signer and signature, and
  the presentation. Everything that needs no signature comes first, so a forged token costs a server no verification.

The bytes a token costs, measured (`test_capability.cpp`): a root with two ranges is 207 bytes; ML-DSA-65 and Ed25519
issuer signatures add 3,446 (3,653 in all); an Ed25519 holder's proof is 100 bytes. A token is sent once per session
and per renewal over the session's reliable control channel, never in a datagram.

Evidence: `test_capability.cpp` (every hybrid issuer pair of ML-DSA-44, -65 and -87 with Ed25519, ECDSA P-256 and
P-384, every holder algorithm; every malformed field, bound and prefix; the magics of tokens, revocation generations
and policy generations refused by each other's decoders; every way of widening; four blocks and a fifth refused; a
forged, a re-signed and a widened block; every verification refusal at its boundary in the contract's order; rotation;
every revocation listing and the adoption rules; a proof bound to a real TLS session and refused in another connection,
another session type's label and an unfinished session; a service link over mutual TLS refusing an anonymous session,
another key and a Proof token; four verifying threads beside a revoking one), the exporter tests in `test_tls.cpp` and
`test_quic_handshake.cpp` (equal ends, the bounds, unique per connection and per resumption, unchanged by key
updates, the six labels distinct), the OpenSSL interop (each library's exporter and world binding equal on every
established connection, over records and QUIC), and `crd-fuzz-crypto-capability` (both decoders against restated
layouts; generated lives against a restated containment rule and a model of the verification order; any flipped bit of
a verified token refused).

<a id="anti-replay"></a>
### Anti-replay

**S16. Anti-replay** ([replay.hpp](../../engine/foundation/crypto/include/crd/crypto/replay.hpp), threat T07). A
receiver keeps two kinds of record so that an authenticated message an attacker captured and sends again is refused;
both are checked before anything is decrypted and written only after the message authenticated, so a forgery moves
nothing. One owner holds both, for every channel that needs them (QUIC packets and 0-RTT tickets now, the media and
datagram counters after).

- **Sequence numbers: `ReplayWindow<Bits>`.** QUIC packet numbers (RFC 9000 section 12.3: a receiver discards a
  packet unless certain it has not processed its number in that space) and any later counter-numbered channel. The
  largest number accepted and a ring of `Bits / 64` words (RFC 6479): raising the largest clears only the words it
  passes (at most the whole ring), never shifts, so `accept` is O(1) and touches one cache line in the common case.
  The `kSpan = Bits - 64` numbers that end at the largest are remembered exactly; an older one is `Stale` (it cannot
  be told from a replay), a remembered one `Duplicate`, anything else `Fresh`. Held inline, no allocation, one
  receiver. QUIC's `QuicPacketWindow` is 256 bits (48 bytes, a span of 192 packets) per packet number space, and its
  largest gives `quic_decode_packet_number` its expected number: a packet 192 behind a later one was declared lost by
  RFC 9002 long before (three later packets, or 9/8 of a round trip), so dropping it costs at most the retransmission
  already under way. quiche keeps 128 bits; msquic, ngtcp2 and picoquic keep the ranges they acknowledge. The receive
  path is: header protection off, decode against the window, `check` (a duplicate costs no AEAD open, admission stage
  S4's one open per datagram), open, `accept`; a key update keeps the space's window.
- **Single-use identifiers: `ReplayFilter`.** Random, authenticated 16-byte identifiers used once in any order over a
  lifetime: QUIC NEW_TOKEN tokens (S13) and the tickets a server that accepts 0-RTT opens (S17, RFC 8446 section 8.1). The split-block
  Bloom filter by issue period S13 describes, generalized from it unchanged: its user checks that the period covers
  the identifier's lifetime and its issuers' clock skew (`QuicTokenKeys::validate` refuses a filter shorter than the
  NEW_TOKEN lifetime or `kQuicTokenClockSkew` as `NotReady`).
- **Scope.** A record covers one receiver; the servers of a fleet share none, so an identifier one server accepted is
  accepted once more by each other (RFC 8446 section 8's zones). That is why early data carries only operations their
  schema declares idempotent, fleet-wide, and why a ticket's single use in one server's filter is defence in depth
  beside the provider's freshness check (RFC 8446 section 8.3: AWS-LC refuses early data whose ticket age is more than
  60 seconds off), not the guarantee.

Evidence: `test_replay.cpp` (windows of 128, 256 and 1,024 bits against the restated contract over twelve random walks
of 4,000 in-order, reordered, duplicated, replayed and far-jumping numbers, from 0, 2^40 and just below 2^62; the
span's last remembered and first Stale number; a ring position reused by a number one word on; a jump past the whole
ring; the number range's extremes and QUIC's last packet number; checks that record nothing; clear; the filter's
periods, slot reuse after a long silence, refusals and false positives at capacity), `test_quic.cpp` (600 packets
sealed and header-protected, received with reordering, every seventh duplicated and every fiftieth replayed 300 packets
later: each opened once, every duplicate and replay dropped before its AEAD, and a forged packet claiming a number far
ahead opened, refused and leaving the window where it was), `test_quic_token.cpp` (a filter shorter than the clock
skew refused for short-lived tokens), the `crd-fuzz-crypto-quic` window mode (every verdict, the largest and `any()`
against the restated contract for each window size over hostile operation sequences; three seeds), and five source
mutations, each caught (the Stale bound off by one, passed words left set, a Duplicate accepted, the clearing one word
short, the filter's period not covering the skew); the four window mutations by the fuzz corpus too.

<a id="early-data"></a>
### Early data

**S17. Early data (0-RTT)** ([tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp), RFC 9846 sections
2.3, 4.2.10 and 8; RFC 9001 section 4.6; threat T07) is the provider's 0-RTT under Cerid's rules, off unless both ends
set `TlsConfig::early_data`:

- **Tickets say what they allow.** A server that accepts early data issues tickets whose early_data extension allows
  `kTlsEarlyDataBytes` (14,336, the provider's, below one record: 0-RTT data never comes near a key's usage limit) over
  records and any amount (`kQuicEarlyDataBytes`, 0xffffffff, RFC 9001 section 4.6.1) over QUIC. A client keeps each
  ticket as Cerid's ticket record (a version byte, that allowance read off the NewSessionTicket, the server's QUIC
  transport parameters and the provider's session), and a record that does not parse is never offered. A client
  offers 0-RTT on a ticket that allows it; `send_early()` sends whole messages within the allowance or nothing.
- **Single-use tickets.** Every ticket carries its issue time, sealed with the session state; a server that accepts
  early data must hold a `ReplayFilter` whose period covers its ticket lifetime and the provider's freshness window
  (`kTlsMaxTicketAgeSkew`), so a ticket the provider would still take is never too old for the filter. The ticket's
  identifier is the first 16 bytes of its salt (random, and authenticated by the opening: the salt derives the
  sealing key), filed by the sealed issue time; a ticket the filter has seen, or a false positive (0.1% at
  capacity), is ignored and the handshake runs in full (`Replayed`), so an attacker's replay of a captured first
  flight gets no early data from that server, and the full handshake's ticket allows 0-RTT again. The scope is one
  server (RFC 8446 section 8's zones): another server of a fleet accepts the same replay once, which is why only
  operations their schema declares idempotent run on early data.
- **The provider's refusals.** The provider refuses early data whose client's ticket age is more than 60 seconds
  from the server's (`Stale`, RFC 8446 section 8.3), after a HelloRetryRequest, on another application protocol,
  from a server without early data and under another early data context (`Rejected`; over QUIC the provider's
  comparison, over records Cerid's sealed digest, [S18](#message-schemas)); the resumption stands. A client learns
  only `Rejected`; its early bytes are gone, and it sends them again after the handshake. The provider forgets every
  value it predicted, so `complete()` reads the group, AEAD, peer key and protocol again.
- **Replayable bytes reach the caller marked.** A server's early bytes come only from `read_early()`, never from
  `read()`, before the handshake ends: the handshake's checks (the pins of a resumed client included) run when the
  ServerHello is sent, before the first early byte reaches the caller. `read_early()` peeks before it reads: bytes the
  provider returns while the handshake still runs are early ones, and a peek that ran into the client's Finished
  completes the handshake and leaves the first 1-RTT bytes pending for `read()`.
- **QUIC.** The Early level has the client's 0-RTT secret only (a client's from `init()` while `Offered`, a server's
  once it accepted; zeroed on a client its server refused), handed out once by `derive_keys` like every other. A
  server names its early data context at `init()`: the transport parameters a client remembers and the application
  protocol's settings, encoded the same way on every connection (RFC 9000 section 7.4.1); the provider compares it
  byte for byte with the ticket's. A client's ticket record keeps the server's transport parameters from the ticket's
  connection, which `remembered_transport_parameters()` gives the transport for its 0-RTT packets; the transport
  (4.2a) checks that the accepted connection's parameters lower none of them.

Evidence: `test_tls_early.cpp` (14 cases: early bytes read as they come, after the Finished, one byte at a time, none
at all and with the first 1-RTT bytes beside the Finished; a replayed first flight refused by its server and accepted
by another of the fleet; a ticket offered twice refused, the bytes sent again and the next ticket used; ticket ages 30
and 57 seconds off accepted, 63, 120 and 3,600 refused, and a ticket from the future offered to no one; a
HelloRetryRequest, another protocol and a server without early data refusing it; early data off unless both ends
configure it; the allowance exact; the pins known before the first early byte; a changed early record failing with
bad_record_mac; every configuration refusal; one context and filter on four threads; over QUIC the 0-RTT keys of both
ends agreeing in versions 1 and 2 with the remembered parameters, another context, a HelloRetryRequest and a replayed
Initial refusing it, and the context's refusals), `crd-fuzz-crypto-tls`'s early data modes over records and QUIC (a
client that offered and a server that accepted, each fed hostile bytes after a real resumption), `tls_interop.py`
against OpenSSL 4.0.3 (both ways over records accepted and refused with the bytes sent again, a HelloRetryRequest both
ways, both ways over QUIC with the 0-RTT secrets' probes equal, and another early data context refusing it), and
twelve source mutations, each caught.

<a id="message-schemas"></a>
### Message schemas

**S18. Server-authorized message schemas**
([capability.hpp](../../engine/foundation/crypto/include/crd/crypto/capability.hpp), D5, threat T07; the user's order
of 2026-11-01: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED!"). What an
operation bit of a grant means, and whether it may run on replayable bytes, is decided by the audience's issuers,
never by a client:

- **The artefact.** Magic `CRDSCHEM`, a format byte with an N-1 reader, the session type (`ScopeKind`), the schema's
  16-byte identifier (which NET.3's generated messages name), its revision (major for incompatible changes, minor for
  compatible additions), the audience, the issue time, the policy generation the issuer held, the declared operations
  (a 64-bit set: what the bits of `Grant::operations` mean) and the idempotent ones (a subset: a second run changes
  nothing), then the signature list. 78 bytes before the signatures; decoding is exact (`TrailingBytes`), bounded
  (`kMaxSchemaBytes`, every signature at its bound) and allocation-free, every malformed field refused with its own
  error; issuing refuses a kind outside its set, a zero identifier or audience, no operation, or an idempotent
  operation not declared (`BadField`) before anything is signed.
- **Verification** (`CapabilityAuthority::verify_schema`) runs in this order: the configuration, the decoding, the
  audience and kind (`WrongAudience`), the issue time (`NotYetValid` past `kMaxIssueSkew`), the revocation generation
  (`RevocationStale` without a current one; `Revoked` when issued before its cut-off or under an older policy
  generation than it honours), the hybrid issuer set covering the issue time (`UntrustedIssuer`), the policy snapshot
  (`AlgorithmRefused`) and every required issuer's signature. The verified schema (`AuthorizedSchema`) carries the
  SHA-256 of its signed bytes: signatures are not part of it, so the same content signed again has the same digest
  and any change of content another. A service verifies its schema again under each revocation generation it
  applies, as it verifies tokens at every renewal.
- **The verdict** (`authorize_operation`, allocation-free, constant in the operations): `OtherSchema` for a
  capability of another audience or session type, `Undeclared` for no operation or one the schema does not declare,
  `Replayable` for early data asking for an operation it does not declare idempotent (a delivery other than
  `Established` is judged as replayable), `NotGranted` when the capability does not hold every operation on the
  object, else `Admitted`. The schema judges before the grant, so a client learns nothing about its grant from an
  early refusal. A client asks `AuthorizedSchema::early()` before it sends an operation with `send_early()`.
- **Early data is bound to the schema.** A server that accepts early data names its early data context at every
  session's `init()`, over records too (`TlsSession::init`, at most `kTlsMaxEarlyDataContextBytes`; missing,
  oversized or given where early data is not accepted is `BadConfig`): the schema's digest and any setting a replayed
  operation must see unchanged. Over QUIC the provider compares the context with the ticket's (RFC 9000 section
  7.4.1). Over records Cerid seals the context's SHA-256 into every ticket beside the issue time; a ticket of another
  context turns early data off for that handshake only (the provider's per-connection switch, set while the ticket
  is opened and restored as soon as the 0-RTT decision is made, so the connection's own tickets allow it again) and
  spends no `ReplayFilter` entry: the resumption stands, its early data is `Rejected`, and the client sends its
  operations again after the handshake under the new schema. A schema change therefore never lets an operation run
  on early data that the new schema does not declare idempotent, even for a client that still holds the old schema.
  (AWS-LC's `SSL_SESSION_copy_without_early_data` cannot serve here: its copy drops the ticket's age_add, and a
  ticket without it resumes nothing.)
- **The authority on early data** (`CapabilityAuthority::verify_early`). The provider exposes no early exporter, so
  early data presents only what the resumed ticket's handshake proved: the peer key, known before the first early
  byte (S17). A Peer token over a resumed, mutually authenticated session is verified there; a Proof token is
  `Unbound` in early data and is presented after the handshake, as is any token on a session that accepted no early
  data. Fleet-wide, a replay reaches each server once (S16's scope); only idempotent operations run on it.

Evidence: `test_capability.cpp`'s schema cases (the issued size and every decoded field; the digest equal to the
SHA-256 of the signed bytes, equal for a second signing and different for another revision; every malformed field
refused before signing and on decoding, every proper prefix `Truncated`, the format, kind, operations, idempotent
set, trailing byte, unsigned and oversized lists; tokens, revocation generations and schemas refusing each other by
magic; the verification order through each refusal, with every 97th byte flipped refused; a table of 15 operation
verdicts, both other-schema cases and the names; and over a real resumed, mutually authenticated TLS session: nothing
presented before early data is accepted, a Peer token admitting idempotent operations in early data and refusing a
replayable one, a Proof token `Unbound` there and admitted after the handshake, and a schema change refusing the old
ticket's early data while resuming, the new ticket allowing it under the new verdicts), `test_tls_early.cpp` (a
records ticket of another context resuming without early data, spending no filter entry, and the next ticket
allowing it under the new context and refused under the old; the records context's refusals), the
`crd-fuzz-crypto-capability` schema modes (decode against a restated layout; a generated life against a model of the
verification order, the restated verdict rule and a flipped byte), `tls_interop.py` (a Cerid server under another
schema's digest refusing OpenSSL 4.0.3's early data over records while resuming), and source mutations, each
caught.

<a id="wire-formats"></a>
### Wire formats

**S26. Generated bit-packed wire formats** (NET.3, threats T07, T09 and T11; [P-8](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md#p-8);
the user's order of 2026-11-01: "AS SECURE AND FAST AND COMPACT AS POSSIBLE"). Every message of every session type comes
from one authored schema through `scripts/gen-wire-schemas.py`; nothing hand-writes a decoder.

- **The model.** A schema is a JSON document (`cerid-wire-schema/1`, `engine/foundation/net/schemas/`): its 16-byte S18
  identifier, authored once and stable across revisions; its session type; its revision; its operations; its channels with
  the design's declared budgets; enums, structs and messages. It is the model REFLECT.2's producers converge on (the user's
  order: a network row never waits for a later system). `transport` is the one session type below S18: its messages
  present the capabilities, so no capability authorizes them, and it declares no operation.
- **The layout.** Fields follow each other with no tags, most significant bit first (`containers.wire-bits`): bool, uint of
  1 to 64 bits, range (the offset from its minimum, in the bits its span takes), varuint and zigzag varsint (groups of
  a declared 1 to 32 bits behind a continuation bit, the shortest form only), enum, f32, f64 (finite unless declared
  otherwise, so a NaN never reaches a simulation), hesap-net's bounded float and smallest-three quaternion, bytes, text
  (UTF-8, checked) and list of any type. Optional fields spend one bit. Bytes and text are byte-aligned with their padding
  checked, so they can be viewed. One message has one encoding: a padding bit, a trailing byte, an overlong integer or a
  range's unused code is refused, so a decoded message re-encodes to its input.
- **Revisions.** A message carries no revision: the session announces the minor it writes (`RevisionAnnounce`, zero bytes
  per message). A minor only appends fields to a message or to a struct declared `extensible` (aligned, its length in
  bytes, its body padded), never to a plain struct, whose elements carry no overhead. A reader takes the default of a
  field its writer's minor lacks, and skips what a newer writer appended; an encoder asked for an older minor refuses a
  set field that minor lacks rather than dropping it. The digest of every minor's layout is recorded in the schema's
  `history` and checked: any change to a recorded revision (width, order, bound, default) fails the generator and needs
  a new major; a rename does not.
- **Bounds decided before code exists.** The generator refuses recursion, nesting past 8, a zero-sized element, a message
  above its channel's `max_message_bytes` and a duplicate identifier. It publishes each message's minimum and maximum
  size, its channel and its operation bit (`MessageDesc`) for admission, and the fields of the S18 artefact
  (`WireSchema`), so the signed schema and the code cannot disagree on what exists.
- **Decoding.** Linear in the bytes, no allocation: every element takes at least one bit, so a claimed count cannot
  multiply work. Bytes and text are spans of the input, and a list keeps where its validated elements lie and re-reads
  them when walked; its validating pass runs each element codec's generated `check`, the decode's verdict without the
  floats and quaternions it would rebuild (hesap-net's `bounded_valid` and `smallest_three_valid` share the decode's
  arithmetic, so the verdicts agree on every word). A list given storage (`WireList::into`, kept across decodes and
  through struct fields) is decoded once into it as it is validated; the elements are the caller's only when the whole
  message is accepted, and a list longer than the storage is viewed as without it (2026-11-08).
- **Admission** is [S27](#frames).
- **Evidence.** Each schema gets a generated fuzz target. The generator's reference interpreter, which shares no code
  with the C++ it emits, computes known answers: `test_wire.cpp` holds the generated encoder and decoder to its bytes and
  its verdicts on damaged messages at every minor, older and newer writers included.

Later slices of [NET.3](../ROADMAP.md#slice-net.3): [baseline deltas](#baseline-deltas), [the integrity hash](#integrity-hash),
[padding](#padding), [the state-hash exchange](#state-hashes) and [coded bodies](#body-coding) with the tool that trains their models landed, and the serializer budget board ([2026-11-08](../bench/2026-11-08-net-3-serializers.md)); a cheaper validating pass, cheaper deltas (word runs halved their cost on 2026-11-08) and two more peers remain. crd-net does not link crd-hesap-net, which is not a packaged library: a target holding
a schema with quantised fields links `crd-net-quantized`, whose header sits outside crd-net's installed headers.

<a id="frames"></a>
### Frames and admission

**S27. Frames, stages S6 and S7, quotas and reassembly** (NET.3, threats T07, T09 and T11;
[frame.hpp](../../engine/foundation/net/include/crd/net/frame.hpp),
[admission.hpp](../../engine/foundation/net/include/crd/net/admission.hpp),
[reassembly.hpp](../../engine/foundation/net/include/crd/net/reassembly.hpp)). No socket and no session here: 4.2a's
transport hands these functions a stream's or a datagram's plaintext after S4 and S5, and the time.

- **The layout.** Every number is the wire formats' 7-bit varuint, shortest form only. A carrier names its route once: the
  channel's number in the session's flat table of its schemas' channels, times four, plus the form (one byte for up to 32
  channels). A stream is that route, then `[message][length][object?][hash?][expanded?][body]` frames. A datagram is the route, then
  one message running to its end (form Whole, no length), two or more sized frames (Listed: one is Whole's, so one message
  has one encoding), or one fragment (`[message][total][object?][hash?][expanded?][chunk][sequence][index][piece]`). The object is
  there only when the message has an operation bit, the hash as [S29](#integrity-hash) says, the expanded size as
  [S32](#body-coding) says. The length counts everything
  after itself, so a reader can step over a message a newer writer added, whose description it lacks; a length shorter
  than the object and hash it was read past is `Malformed`.
- **Order.** A stream or datagram refusal costs at most one header read: route and carrier, the principal's datagrams, the
  message's description by binary search, its revision and channel, the declared length against its least and most (a newer
  writer may pass this reader's most, never the channel's). Then S6 and S7.
- **S6.** The channel's messages and bytes, the principal's messages and bytes, the capability's operations and bytes, each
  a virtual-scheduling gate; then the principal's pending messages; then, for a frame that really arrives in pieces, its
  length from the principal's reassembly quota. Nothing is spent until all of it holds, so a refusal here can be retried
  and says when (`retry_at_ns`). A rate of zero is none at all. A gate is one 64-bit next-arrival time: exact on a simulated
  clock and in integer nanoseconds, a message larger than the credit passes a clear gate and pays by the wait.
- **S7.** Everything has been charged: refused traffic is not free. A schema above S18 needs its verified artefact (checked
  against the generated identifier, kind and major at `bind()`) and a current capability, or `NoCapability`, then
  `authorize_operation` with the message's bit, the object and the `Delivery`. A transport schema runs below S18 only for
  a message with no operation on a control channel; anything else is `Undeclared`. Nothing has read a field of the body, and
  on a refusal nothing will: it gives back its pending place and reservation, and no buffer ever existed. The session
  withdraws a capability that expires or is revoked (`set_capability(nullptr)`); a renewal keeps what was spent.
- **Actions.** Every verdict is counted and none answers the peer. A reliable channel cannot lose what it declared: a limit
  holds its reader back (`Wait`, nothing consumed, so flow control reaches the sender), broken framing or a broken protocol
  ends the session (`Close`, GoAway ProtocolError), and an S7 refusal or a newer message is stepped over (`Skip`, by its
  length, with no buffer). An unreliable channel only drops. A refused stream is the stream's own end, not the session's.
<a id="quotas"></a>
- **Quotas.** One `PrincipalQuota` per principal across its sessions and loops, every counter atomic: streams, datagrams,
  messages, bytes, reassembly bytes, pending messages, the capability's budget and its objects. Priorities keep headroom:
  a limit leaves a channel the whole at priority 0 and an eighth less for each step down, so lower classes fill first and
  overload sheds them first. Priority 0 keeps its own channel gates but the principal's other traffic
  cannot close it (it is charged all the same): a session at its budget can still renew its capability. `SendBudget` is the
  sender's side of the same rule: over its share a reliable message waits and an unreliable one is shed.
- **Reassembly.** A frame all in hand is a view of the input, copying nothing. Only one that arrives in pieces takes a
  buffer, of exactly its declared length, from an injected allocator's `try_allocate`, after S6 and S7: a refused message
  costs no memory, and an allocator's refusal holds the reader back. A header split across reads waits in 29 bytes of carry
  (its numbers have bounded widths). Fragments carry their message's whole description in each piece, so any may arrive
  first, and hold to one shape: at least two pieces, at most 256, a chunk of at least 64 bytes, each piece exactly a
  chunk or what is left, so overlaps do not exist and a slot is a fixed bitmap. A slot pool bounds the messages in pieces;
  the oldest gives way to a newer one, a timeout gives it up, a State channel's newest message supersedes every older one, and
  a window of 64 sequences keeps a finished or given-up message from opening again. Every piece must agree with its first
  (message, object, total, chunk, delivery) or it is dropped; a repeat is written once.
- **Revisions.** A transport schema is readable before its announcement, at its first minor, which is how
  `RevisionAnnounce` travels (so it is always written at minor 0); a schema above S18 reads nothing before its announcement,
  and a second announcement that differs ends the session.
- **Evidence.** `test_admission.cpp`: known bytes of every header form; each refusal at its boundary with what it spends; a
  stream at every split against the generated session control messages and their decoders, zero copies when whole; a
  hostile path (loss to 30%, duplication, reordering) delivering each message intact at most once; quotas under threads.
  `crd-fuzz-net-admission`: a restated layout and rule decide every verdict and frame, splits read as whole does, every budget
  comes back. Source mutations, each caught ([session](../sessions/2026-11-07-net-3-admission.md)).

<a id="baseline-deltas"></a>
### Baseline deltas

**S28. What changed since a message both sides hold** (NET.3, threats T07, T09 and T12;
[delta.hpp](../../engine/foundation/net/include/crd/net/delta.hpp)). A message the schema declares `delta` travels as the
fields that differ from a baseline. The codec works on encoded bytes, not values: the baseline is the very bytes the receiver
holds, so a quantised field compares by its code and is never quantised twice, unchanged fields need no re-encoding, and a
receiver rebuilds one ordinary encoding of the message that the generated decoder then validates and views, so a delta adds no
second decoder and no lifetime on a baseline. The generator writes one table per message and one interpreter (`delta_encode`,
`delta_apply`) walks them. No allocation. Each table row also names its run: the siblings from it on, of one revision, whose
encodings always take the same bits (fixed-width values, plain structs and fixed-count lists of them; never a varint, whose
width a hostile baseline's continuation bit would change). A run is compared, copied and skipped as 64-bit words: the
writer finds the first bit that differs and codes the fields before it as their zero bits, the reader counts the leading
zero change bits and copies those fields' bits from the baseline in one move. The layout is unchanged, so are the vectors.

- **The layout.** Each field a writer's revision has gets one changed bit before it, in field order, so a newer writer's
  fields land after everything an older reader knows and revisions compose as they do for messages. A changed optional then
  says whether it is present, and a value that appears travels whole. A changed fixed-width field is its new code, a changed
  single bit nothing more (its bit is its news), a changed varint its value, bytes and text their whole field. A field declaring
  `delta_group` sends the difference instead: wrapped to the field's width and folded so a small step either way is a small
  number, in groups of that many bits. A plain struct recurses. An extensible struct aligns and carries its delta's length, so an
  older reader steps over what it does not know.
- **Lists.** By position: the count, a changed bit per shared element, then new elements whole. With `delta_key` the list is a
  map: the head field of a plain struct, a uint, range or varuint, ascends strictly (encoders and decoders refuse anything else,
  `KeyOrder`). A merge walks both sides: `0` keep, `10` changed (the key costs nothing), `110` insert, `111` remove, and once
  either side runs out what is left needs no operation. An insertion must stand below the next old key and above everything
  already passed, so a removal followed by re-inserting the key has no encoding.
- **One delta has one encoding.** A field said to have changed that has not is `Unchanged`, at every kind and depth; every
  number is shortest-form and every padding bit zero. A delta of nothing is legal and is the bits it takes to say so. A reader
  older than its writer takes what it knows and ignores the rest, and its result decodes at the writer's revision.
- **Hostile input.** Work is linear in the baseline and the delta: every element costs a delta bit or consumes baseline. What
  comes out passes the generated decoder before anything keeps it. Output is held to the message's most by the fixed room it is
  written into, and [S32](#body-coding) charges what a small delta expands to.
- **The frame's body.** `[sequence, 16 bits][distance back to the baseline, varuint, zero for a whole message][hash of the whole
  message, 32 bits][delta or message]`. A delta message's description bounds this body, so admission (S27) needs no new rule;
  `write_frame` and `decode_frame` refuse a delta message, whose body only `DeltaWriter` and `DeltaReader` read.
- **Baselines.** `BaselineStore` keeps encoded messages by sequence in memory fixed at construction: a ring of bytes and a table
  of slots, the oldest given up. `DeltaWriter` (one per peer and stream) sends what changed since the newest message the peer
  acknowledged that the store still holds, or the whole message when there is none or the delta is no smaller, which also bounds
  how long it spends: past the message's size it gives up. A server encodes a tick once and one store serves every client.
  `DeltaReader` drops what is not newer than what it has, so a repeat or an overtaken delta costs nothing. `BaselineAck` on
  session control names what it holds; an acknowledgement of something never written, or older than the newest, changes nothing.
- **Recovery needs no message.** A writer only ever uses a baseline its peer acknowledged and the reader acknowledges only what it
  kept, so loss, reordering and repeats leave the two agreeing on some baseline; if the reader has since given it up it says
  `MissingBaseline` while its acknowledgement of its newest message moves the writer on, or the writer has given that one up too
  and sends the message whole.
- **Integrity.** The hash is taken over the whole message at the writer and checked on what the reader rebuilt, so it catches a
  writer's memory fault or a baseline that differs, which the AEAD tag cannot see (`Mismatch`: nothing delivered or kept). A
  reader older than its writer cannot rebuild the fields it lacks, so it validates what it rebuilt instead.
- **Evidence.** The generator's reference interpreter computes values, not bits, and shares no code with the C++: bytes and
  verdicts for every message at every revision, damaged deltas and rebuilt messages included. `test_delta.cpp` spells each form's
  exact bits and each refusal, and runs writers and readers on a path with up to 70% loss, repeats and reordering.
  `crd-fuzz-wire-delta-test`: a rebuilt message's delta is the input, and any two messages' delta rebuilds the second.
  `crd-fuzz-net-delta-stream`: any bodies, and a script of deliveries ([session](../sessions/2026-11-07-net-3-baseline-deltas.md)).

S29 to S31 hold to `test_frame_integrity.cpp` and `test_state_hash.cpp`, and to `crd-fuzz-net-admission`'s restated layout and
`crd-fuzz-net-state-hash` ([session](../sessions/2026-11-07-net-3-integrity-padding.md)).

<a id="integrity-hash"></a>
### The plaintext integrity hash

**S29. What the AEAD tag cannot see** (NET.3, threat T12; [P-15](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md#p-15);
[frame.hpp](../../engine/foundation/net/include/crd/net/frame.hpp)). The tag proves a packet arrived as it was sealed. A sender's
memory or pipeline fault is sealed as valid, so a frame also carries a hash taken at serialisation and checked after decryption.

- **The field.** 32 bits, most significant first, after the object: the low half of XXH3-64 over the body, seeded with
  `(message << 32) ^ object`, so a fault in the header's numbers shows too. Its width is fixed, so `write_frame` encodes behind a
  header of known size and fills it in after. A fragment carries its whole message's hash in every piece, like the rest of
  the message's description; a piece that names another is `FragmentConflict`.
- **Who has one.** `frame_shape()` decides from the channel and the message. A channel has it unless it declares
  `"integrity": false`, which only a Media channel may (the generator refuses any other): a codec's bitstream conceals its own
  errors, and 4 bytes are a large share of a 60-byte voice frame. A delta message has none: [S28](#baseline-deltas)'s body hashes
  the whole message it rebuilds, which also covers the delta.
- **When.** After S7, once the whole body is here: a view of the input, a stream's buffer at its last byte, a fragmented message
  at its last piece. Nothing has decoded it. It costs the XXH3 of bytes already admitted and charged.
- **What a mismatch does.** `IntegrityFailed`, counted, with the frame's pending place, reservation and buffer given back and
  nothing delivered. An unreliable channel drops it, and a Listed datagram reads on, its lengths having framed the rest. A
  reliable channel closes (GoAway ProtocolError): it cannot lose a message its sender believes delivered.

<a id="padding"></a>
### Padding

**S30. What a packet's size says** (NET.3, threat T15; [frame.hpp](../../engine/foundation/net/include/crd/net/frame.hpp)). Every
channel states `padding_block` in its schema (the generator refuses a channel without one, and anything but zero or at least
16). Traffic pads to the next multiple, so lengths inside a block look alike. What a block is worth depends on the class; these
are the values a schema starts from:

| Class | Block | Why |
|---|---|---|
| Control | 128 | Renewals and generation handshakes are rare, and their sizes name what happened |
| Transaction | 256 | The size of an edit or an idempotency record says what was edited |
| Event | 64 | Chat lengths |
| Input | 32 | Which inputs a player pressed, at small cost per packet |
| Media | 32 | Variable-rate voice sizes leak speech; constant-rate Opus needs none |
| Bulk | 4096 | An asset's size identifies it |
| State | 0 | Bandwidth dominates, and a snapshot's size says little the server does not already show |

- **A path that controls its packets** (QUIC's PADDING frames, TLS record padding, raw UDP) pads each packet to `padded_size()`,
  with the largest block among the channels whose frames it holds, at most the path's room. It costs no reader anything and
  covers what the packet shares with acknowledgements and other channels. [4.2a](../ROADMAP.md#slice-4.2a)'s packet assembly calls it.
- **A path that does not** (a browser's WebTransport, WebSocket and data channels: [no path control](#browsers)) pads in band, and
  a native reader reads it the same on every path.
  - A datagram: `pad_datagram()` puts `route(Padded)[count][count zeros]` before the datagram's own route and frames, moving them
    behind it, towards the same size. `open_datagram` takes it off, so the frames below it change nothing. It is refused
    (`BadPadding`) on a channel that declares no block, with a set bit, a count of two blocks or more, or an inner route that is
    Padded again or names another channel.
  - A stream: `write_stream_padding()` writes message zero, which no schema has (identifiers start at one), as
    `[0][length][length zeros]`, bringing the bytes about to be flushed to a multiple. The reader checks the zeros in pieces and
    steps over them without a buffer; a set bit or a length of two blocks closes the stream. On a channel that declares no block
    message zero stays `UnknownMessage`.
- **Padding is not free.** Its bytes are charged to the channel and the principal: a stream's frame as it arrives, held back like
  any frame when a gate is closed, and a datagram's prefix after its checks, so the gates close the frame behind it. A reader
  never requires padding: it protects the sender's own traffic.

<a id="state-hashes"></a>
### State hashes

**S31. Finding the first difference** (NET.3, threat T12; [state_hash.hpp](../../engine/foundation/net/include/crd/net/state_hash.hpp)).
Peers of a lockstep or rollback session report what they confirmed with `StateHash{step, hash, chain}` on session control.
[4.2c](../ROADMAP.md#slice-4.2c)'s policies supply the hashes and choose the steps; what a divergence does is
[HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3)'s. `StateHashExchange` compares them, in memory fixed at construction.

- **The chain.** `chain = XXH3-64(step, hash little-endian; seed: the chain before)`, from zero. One message confirms everything
  recorded before it, so a report may be sparse: agreement at a step means agreement on every step up to it.
- **Order.** A report and its step arrive in either order, and a peer ahead of this side waits in a fixed ring, the oldest giving way
  (counted), since a later chain covers it. A report of a step this side gave up or never recorded is `NotHeld` and changes
  nothing; a repeat or a step going back is `Stale`. Reports cost at most one binary search each, and each report is compared once.
- **The verdict.** The first difference is said exactly once, as `Diverged`, naming the newest step agreed, the step that showed it
  and both hashes: the hashes differ at that step, or only the chain does, so a step in between did. Nothing is compared after it,
  while records still produce reports so the peer sees it too. `restart()` starts both sides over after a resynchronisation.
- **Evidence.** `test_state_hash.cpp`, and `crd-fuzz-net-state-hash`: an unbounded restatement decides every verdict, chain and
  divergence of any script of records and honest or damaged reports.

<a id="body-coding"></a>
### Coded bodies

**S32. Dictionaries and entropy coding, under caps** (NET.3, threats T09 and T15;
[coding.hpp](../../engine/foundation/net/include/crd/net/coding.hpp),
[entropy.hpp](../../engine/foundation/compression/include/crd/compression/entropy.hpp),
[zstd.hpp](../../engine/foundation/compression/include/crd/compression/zstd.hpp)). A channel may declare models, and its bodies
then travel coded when that pays. The codecs are crd-compression's (capability `compression.codecs`); crd-net binds them to
channels.

- **What a schema says.** `models` on a channel: from which minor (`since`), the kind (`dictionary` or `entropy`), the XXH3-64 of
  the model's source and `max_ratio`. They join their revision's layout digest, so both ends of a revision hold the same model.
  Both ends code with the newest model of the lower of their two revisions: it costs nothing on the wire, and an older peer is
  never sent what it cannot read. A product ships a better model as a new minor. `bind()` refuses a session that does not hold
  every declared model, by hash and kind, with a coder to use them. A channel with no model at a revision has no coded frames at it.
- **One message at a time, against a fixed model.** Nothing is kept between messages, for three reasons. A shared window would tie
  each frame to every frame before it, and admission must be free to refuse one (S7) or step over one (a newer writer's) without
  reading it. Compressing under encryption leaks through sizes (CRIME, BREACH) when a window mixes a secret with bytes an attacker
  chose; here a window holds one message and a public model. And a session keeps no window's memory, so a million idle sessions
  cost none. What is left is an oracle inside one message, so the generator refuses models on Control and Transaction channels, which
  mix credentials and peer-chosen bytes, on Media, whose bodies are a codec's output, and on a transport schema. Padding (S30)
  counts what is sent, after coding.
- **Two kinds.** A dictionary is zstd against a dictionary both ends hold (`zstd_train_dictionary`), for what a reliable channel
  carries: frames without a magic number, content size, dictionary identifier or checksum, all memory static (`ZstdDictionary`,
  `ZstdMessageCoder`). Entropy is a range variant of asymmetric numeral systems (Duda, arXiv:1311.2540) with a model trained once
  (`EntropyTrainer`), for small bit-packed bodies: 12-bit frequencies in four contexts chosen by the byte before (zero, below 16, low
  nibble zero, other), one state of three bytes, no allocation. A coded message has one encoding: a decode must end on the starting
  state with every byte used. One state rather than an interleaved pair because a second state costs three more bytes on every
  datagram, and a snapshot's message is far too short to gain from it. On simulated snapshot deltas of 56 bytes a frame, in a scratch
  run with the model trained on other ticks: order-0 0.71, whole order-1 0.70, four contexts 0.62, and zstd with a 4 KiB dictionary
  1.08, whose tables and block headers a message this small never repays.
- **The frame.** `[expanded]` after the hash, on a channel coding at the session's revisions: what the body decodes to, or zero
  for a body stored as it is, which stays a view of the input. A writer stores a body that coding did not make smaller or that
  would pass the ratio. The hash (S29) is over the message, not the bytes that carried it, so it also covers the decode.
- **Caps before anything is reserved (T09).** `read_frame` holds a coded body to being smaller than what it declares and within
  `max_ratio` (`BadExpansion`), and what it declares to the message's least and most, a newer writer to the channel's most
  (`Undersized`, `Oversized`). That costs one header read: no gate spent, no buffer.
- **What it costs (S6).** A coded frame costs what it expands to on the channel's, the principal's and the capability's byte gates,
  so a ratio buys no budget. Its output is reserved from the principal's held bytes alongside reassembly, and its buffer of exactly
  that size is allocated after S7 passes, so a refused frame costs no memory and an allocator's refusal holds the reader back.
  Decoding must produce exactly the declared size. `BadExpansion` closes a reliable channel and drops on an unreliable one.
- **Deltas.** A delta message keeps its own body (S28), `[sequence][distance][hash][expanded][payload]`: the header's numbers
  and hash are incompressible, so only the payload is coded, whole messages too. A reader decodes nothing for a `Stale` frame or a
  `MissingBaseline` one. What a delta rebuilds cannot be declared without spending bytes on every frame, and its work is bounded by
  the message's most anyway, so `Admission::read_delta` holds a closed byte gate first (`Throttled`, the same frame read again at
  `retry_at_ns`) and charges what it did expand to afterwards, paid by the wait that follows.
- **Training a model.** `ceridc model train --kind entropy|dictionary --samples <dir> --out <model>` trains on a
  directory of messages, one per file in name order, holding exactly what the channel codes (an encoded message, or a
  delta's payload as `delta_encode` writes it): `EntropyTrainer` for entropy, `zstd_train_dictionary` into
  `--dictionary-bytes` (default 4096, from 256 to 1 MiB) for a dictionary. Every `--holdout`th file (default 5; 0 for
  none) is left out of training and measured apart, so the report says what the model does on messages it never saw.
  `ceridc model measure --model <f>` measures any model, a raw dictionary included. Both load the model through
  `BodyModel`, code every sample with `BodyCoder` exactly as a writer would (stored as it is when coding does not make
  it smaller), decode each back, and print the hash, the bytes sent against the plain bytes in each set, the widest
  ratio, and the schema's `models` entry with `max_ratio` the least power of two above that ratio (a message coded
  wider is only stored); a dictionary trained on less than ten sample bytes per byte of room, which zstd asks for, is
  reported `small-corpus`. The model is written last, whole or not at all; every refusal is typed and writes nothing.
  Samples are bounded (64 MiB a message, 1 GiB together); a run trains once, so the same samples give the same bytes.
- **Evidence.** `test_entropy.cpp`: bytes from `scripts/gen-entropy-vectors.py`, a restatement sharing no code, every state a
  one-byte message can decode from, the cost held within 1% of the model's entropy. `test_zstd_messages.cpp`. `test_coded_frames.cpp`:
  each form at every split, each cap at its boundary with what it spends, gates charged the expanded size, every byte of a coded frame
  flipped, bind's refusals, deltas on a path with loss, repeats and overtaking. `crd-fuzz-compression-messages` and
  `crd-fuzz-net-coded-frames`: a restated header rule and decode decide every verdict, every budget comes back
  ([session](../sessions/2026-11-07-net-3-coded-bodies.md)). `test_ceridc_model.cpp`: trained on the coded-frame tests'
  deltas the tool writes the model `coding_test.json` pins, byte for byte; a measured raw dictionary reports the hash
  that schema pins; sent bytes equal crd-net's coder's; training is deterministic; each refusal, and the real binary
  ([session](../sessions/2026-11-08-net-3-model-training.md)).

<a id="key-logging"></a>
### Key logging

**S19. RFC 9850 key logging exists only in non-shipping builds**
([tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp),
threat T22; RFC 9850 read 2026-11-02, register entry `tls.keylog`). A test decrypts its own captures; nothing else
may hold a session's keys:

- **The sink.** `TlsConfig::key_log` (a function and its user pointer) receives each line the provider writes for
  every session and QUIC handshake of the context, `<label> <client random> <secret>` in lower-case hex without a
  terminator, on the thread driving the session inside the call that derived the secret. The exporter secrets
  (EXPORTER_SECRET, and EARLY_EXPORTER_SECRET should a provider log it) reach it only with `key_log_exporters`, RFC
  9850 section 6's separate authorization: they yield every exporter value a channel binding or capability binding
  relies on. No environment variable turns logging on (section 6: whoever
  controls the launch would control the keys); a line is a key and never enters a diagnostic record.
- **Compiled out of shipping.** `CRD_TLS_KEY_LOG` (crd-crypto's private definition from the shipping condition) is 0
  in a shipping build: the forwarding code is not compiled, the provider's key log functions are poisoned in
  tls.cpp (`#pragma GCC poison`, which clang and clang-cl read too, and MSVC's `#pragma deprecated` under `/WX`), so
  any use fails to compile, and a
  context given a sink is refused (`BadConfig`), never silently left unlogged. `tls_key_log_compiled()` answers
  which build this is; crd-crypto-tests derives its own expectation from the shipping condition and checks it on
  every lane, the shipping lanes included (the CI guard).
- **The labels.** The provider logs CLIENT_EARLY_TRAFFIC_SECRET (a client offering early data, a server accepting
  it), both handshake traffic secrets, both application traffic secrets and EXPORTER_SECRET, with the inner
  ClientHello's random once ECH is accepted and the outer one's otherwise. It never logs EARLY_EXPORTER_SECRET,
  ECH_SECRET or ECH_CONFIG (the ECH HPKE context is the provider's), which RFC 9850 lets an implementation omit; the
  register entry records it, and the provider is not patched.

Evidence: `test_tls.cpp` (the build's expectation and every refusal; both ends' lines equal for each AEAD, the
ClientHello's random on each, the server's EncryptedExtensions and the client's first application record opened
under the logged secrets by an HKDF-Expand-Label written from the RFC, and only under them; every exporter value
derived from EXPORTER_SECRET), `test_quic_handshake.cpp` (in QUIC versions 1 and 2 and each AEAD, a packet sealed
under the keys of each logged secret opening under the reading end's keys), `test_tls_early.cpp` (the early secret
at the client and at an accepting server only), `test_tls_ech.cpp` (the inner random when accepted, the outer one at
both ends when rejected) and `tls_interop.py` (each established connection's lines equal to OpenSSL 4.0's key log
both ways, over records and QUIC, with ECH and with early data).

<a id="tls-policy"></a>
### The policy generation in sessions

**S20. Every session runs under the installation's policy generation**
([tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp), threats T04 and T05, decision D7; 2026-11-03).
The generation's rules ([policy wire](#policy-wire)) are enforced where keys are agreed, not only stored:

- **The store.** `TlsConfig::policy` names the installation's `PolicyStore` and `policy_scope` the sessions'
  scope: `CeridServices` for a Cerid service's own links, `Everywhere` otherwise. A scope without a store, or one
  this build does not know, is `BadConfig`. Without a store a context uses its lists as configured (tests and tools;
  a product always names one).
- **Each session takes a snapshot when it starts** (TlsSession and QuicHandshake alike) and keeps from the context's
  groups, AEADs and peer signature algorithms those the generation does not refuse at that time in its scope, the
  preferred ones first, each kind in the configured order, set on its own provider connection. So a refused
  algorithm leaves a client's offer and a server's acceptance (a peer offering only it finds no common group, no
  common AEAD or no signature the client verifies), a server's preference follows the generation, and a ticket whose
  server key is of a refused algorithm resumes nothing (the pinned-key check reads the session's narrowed list). A
  session whose own credential is refused, or with a list left empty, is `PolicyRefused` before a byte is sent; a
  store without a usable generation refuses everything (freshness, decision 3 of the policy wire).
- **Running sessions** keep their keys until their next key update (the live swap of [agility](#agility)). At that
  moment, a stream session's own update (`update_keys`, or the one `send()` makes before its records, checked
  before any record so a refused send writes nothing) and the peer's KeyUpdate (`read()`), the session checks its
  negotiated group, AEAD, peer key algorithm and credential against the store again (`check_policy`) and, when one
  is refused, queues close_notify and is Closed with `PolicyRefused`. QUIC has no TLS KeyUpdate: its transport
  calls `check_policy` before each `QuicPacketKey::update` and closes the connection on `PolicyRefused` (4.2a).
- **HPKE and MLS suites** follow the same generation: ECH's by [S21](#tls-ech-policy) and a group's cipher suite by
  [M14](#mls-policy).

Evidence: `test_tls_policy.cpp` (a server's preference following the generation; a preferred AEAD leading a
client's offer; a refused group out of a client's offer and a server's acceptance; refused AEADs and a refused server key; every `PolicyRefused` before a byte, a
refused credential serving again under the next generation and an empty store; Cerid service contexts refusing
classical-only key exchange while other contexts accept it, and the scope's configuration refusals; a ticket of a
refused server key not resuming; a generation applied mid-session reaching it at its own key update, at the update
`send()` makes and at the peer's, and an allowed one changing nothing; a QUIC handshake under the policy and its
transport's check), `test_policy.cpp` and `crd-fuzz-crypto-policy` (scopes, format 2 and the N-1 reader).

<a id="tls-ech-policy"></a>**S21. ECH's HPKE suites run under the policy generation**
([tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp), threats T04 and T15, decision D7; 2026-11-03). A suite is judged as a whole: `PolicySnapshot::status` over its
algorithms is Refused when one is, Preferred when all are, Accepted otherwise (an HPKE KEM is named as decision H2
names it, so DHKEM(X25519) is X25519: classical-only key exchange).

- **A client** given ECH configurations (its list or a server's retry configurations) hands the provider, at each
  session's start, only those of version 0xfe0d whose KEM it implements (DHKEM(X25519, HKDF-SHA256)) and whose every
  cipher suite it could pick (HKDF-SHA256 with AES-128-GCM, AES-256-GCM or ChaCha20-Poly1305) the generation allows in
  the context's scope: the provider, not Cerid, picks among a configuration's suites, so one refused suite drops the
  configuration. The preferred configurations lead, each kind in the list's order. A client left with none is
  `PolicyRefused` before a byte, as it would never send a hello without ECH; so once classical-only key exchange is
  refused for Cerid services, a service client given ECH configurations starts nothing until its provider's ECH takes
  a post-quantum KEM.
- **A server** cannot steer the suite a client picks from what it publishes, so a server context whose ECH keys (the
  previous ones included) publish a refused algorithm runs no session (`PolicyRefused`), as one with a refused
  credential does. `TlsEchKey::init` therefore takes the AEADs a key publishes (one to three, AES-128-GCM and
  ChaCha20-Poly1305 by default) and writes the ECHConfig itself; `hpke_codepoints` and `hpke_suite_of` map a suite to
  the HPKE registries and back.

Evidence: `test_tls_ech.cpp` (a key's AEAD list in its ECHConfig and its refusals; a client skipping a configuration
with one refused suite, leading with a preferred one and keeping the list's order; a Cerid service client refused
where another client offers ECH; every AEAD or the KDF refused; retry configurations filtered, refused and accepted;
a server publishing a refused AEAD refused and one publishing allowed ones serving) and `test_policy.cpp` (the combined
status).

<a id="hpke"></a>
### Hybrid public key encryption

**H1. HPKE is RFC 9180's construction over the provider's primitives**
([hpke.hpp](../../engine/foundation/crypto/include/crd/crypto/hpke.hpp), `crypto.hpke`). MLS encrypts path secrets,
Welcome secrets and external initialisations with HPKE in base mode, single shot (RFC 9420 section 5.1.3), and its
suites need DHKEM over P-256 and P-384 as well as X25519. AWS-LC's HPKE has DHKEM(X25519) and ML-KEM only, and no
DeriveKeyPair, so one implementation of the RFC's steps serves every KEM: DHKEM (section 4.1) over `AgreementKey`,
its DeriveKeyPair (section 7.1.3; rejection sampling of the scalar for the NIST curves) and the key schedule
(section 5.1) over HKDF, the first message of a context over `AeadKey`. No primitive is Cerid's; the provider's own
HPKE stays inside its TLS stack for ECH.

- **Interface.** A KEM is named by its group (`X25519`, `Secp256r1`, `Secp384r1` for DHKEM(X25519, HKDF-SHA256),
  DHKEM(P-256, HKDF-SHA256) and DHKEM(P-384, HKDF-SHA384)), the KDF by an HKDF identifier and the AEAD by its own
  (`HpkeSuite`); the RFC's codepoints are generated from the register. `HpkeKey` (generate, `derive`, `from_private`
  in SerializePrivateKey form, `public_key`, `open`, `export_secret`), `hpke_seal` and `hpke_export`: SetupBaseS and
  SetupBaseR with the context's Export (section 5.3; exporter_secret from the key schedule, then LabeledExpand "sec"
  with the exporter context, 1 to 255 Nh bytes), which an MLS external joiner's init secret is (RFC 9420 section 8.3).
  Only sequence number 0 exists: MLS opens one message per context, and no consumer needs a context's later messages
  (or the PSK and authenticated modes). The encapsulation, decapsulation and key schedule are each written once and
  shared by sealing, opening and exporting.
- **Validation** (section 7.1.4). An EC encapsulation or recipient key must be an uncompressed point on the curve
  (`BadKey`); an X25519 one giving the all-zero secret is `WeakSecret`; a ciphertext that does not authenticate is
  `Forged`; every refusal zeroes the outputs. A DHKEM's DeriveKeyPair takes at least Nsk bytes.
- **Secrets and allocation.** The DH output, the shared secret, the key schedule's secret, key and nonce live on the
  stack of one call and are zeroed before it returns. A labelled input is laid out in a 256-byte inline buffer and,
  beyond it, once from the default allocator (an MLS group context as info); the buffer is zeroed on release.

Evidence: `test_hpke.cpp` against RFC 9180's `test-vectors.json` (as the pinned AWS-LC carries it): the six
base-mode vectors of the profile (DHKEM(P-256) and DHKEM(X25519) with HKDF-SHA256 and each AEAD) for DeriveKeyPair
of both key pairs, the private keys' serialization, the opening of the first message and its 18 exported values (an
empty, a one-byte and a word exporter context each), a later message refused, and the 26 other base-mode suites
refused as outside the profile; round trips over six suites (P-384 with both KDFs and both AEAD families included)
with every changed input forged; exports of 1, Nh and 255 Nh bytes agreeing on both sides of the six suites, another
context, info, length, key or KDF deriving another secret; an info beyond the inline buffer; every refusal;
`test_mls.cpp`'s crypto-basics and key-schedule vectors (DHKEM(P-384) opening and DeriveKeyPair); and
`crd-fuzz-crypto-mls`'s round trips with any bit changed.

**H2. The post-quantum KEMs** (draft-ietf-hpke-pq-05, `crypto.hpke-pq`; the hybrids of
draft-irtf-cfrg-concrete-hybrid-kems-03 over draft-irtf-cfrg-hybrid-kems-12's CG framework, `crypto.hybrid-kems`):
the post-quantum MLS suites (`mls.pq-ciphersuites`, M2) and every later HPKE consumer need them, and no provider in
the policy has them with DeriveKeyPair, so `hpke.cpp` composes them from the provider's ML-KEM (`KemKey`, seeds d ||
z), key agreement and SHA-3 (`sha3_256`, `shake256`, `crypto.sha3`), as H1 does DHKEM.

- **Names.** A KEM is named by the provider identifier of its components, so no identifier is added: `MlKem768` and
  `MlKem1024` are ML-KEM-768 and ML-KEM-1024 (0x0041, 0x0042); `X25519Mlkem768`, `Secp256r1Mlkem768` and
  `Secp384r1Mlkem1024` are MLKEM768-X25519 (X-Wing, 0x647A), MLKEM768-P256 (0x0050) and MLKEM1024-P384 (0x0051), as
  `X25519` names DHKEM(X25519, HKDF-SHA256). A TLS hybrid group under the same identifier concatenates its secrets
  (`tls.hybrid-kex`); the HPKE KEM is the combiner's construction, which only HPKE computes. ML-KEM-512 (0x0040) is
  below the policy's ML-KEM-768 floor and the single-stage SHAKE and TurboSHAKE KDFs (0x0010 to 0x0013) serve no MLS
  suite: both are refused as outside the profile.
- **Keys.** An ML-KEM's private key is its 64-byte seed and a hybrid's the 32-byte seed SHAKE256 expands into
  seed_PQ || seed_T (Group_T.Nseed: 32 for X25519, 128 for P-256, 48 for P-384), the ML-KEM key of seed_PQ and the
  scalar Group_T.RandomScalar(seed_T) (the first Nscalar-byte chunk in [1, n - 1]); `HpkeKey` keeps the expanded
  halves, which zero themselves on clear, move and destruction, and `from_private` takes the seed. DeriveKeyPair is SHAKE256's
  LabeledDerive(ikm, "DeriveKeyPair", "", Nsk) under the KEM's suite_id (draft-ietf-hpke-hpke-03 section 4.4: ikm,
  "HPKE-v1", suite_id, the label with its two-byte length, L). Its input is at least 32 bytes, the strength of every
  such KEM, not Nsk: an MLS suite of HKDF-SHA384 derives node keys from 48-byte secrets, fewer than an ML-KEM's
  64-byte Nsk (RFC 9180's "SHOULD have length at least Nsk" is a recommendation MLS itself does not meet).
- **Encap and Decap.** ML-KEM's are FIPS 203's (an encapsulation key failing the input check is `BadKey`; a
  ciphertext of the right length always decapsulates, by implicit rejection, so a changed one is `Forged` at the
  AEAD). A hybrid encapsulates to ek_PQ with ML-KEM, takes an ephemeral scalar from Group_T.Nseed fresh random bytes,
  sends ct_PQ || ct_T and keys the schedule with SHA3-256(ss_PQ || ss_T || ct_T || ek_T || label). A NIST-curve
  element or recipient half off the curve is refused explicitly (`BadKey`: a public input, the generic draft's
  section 4.2); an X25519 half is any 32 bytes and its all-zero result is the secret RFC 7748 computes, which the
  combiner binds, as X-Wing specifies (`AgreementKey::derive` leaves those zeros in its output when it reports
  `WeakSecret`): refusing it would split a group whose other members follow the specification. Nsecret is 32 bytes;
  the key schedule is RFC 9180's over the suite's HKDF.
- **Checks.** `hpke_check_public_key` is the check a long-lived key gets once when it enters shared state: the
  agreement's check for a DHKEM, FIPS 203's input check for ML-KEM (`check_kem_public_key`) and for a hybrid's
  post-quantum half, the curve check for its NIST-curve half. MLS's leaf, init and node keys go through it.
- **No derandomized encapsulation.** The draft's vectors fix the encapsulation randomness (ikmE) to check a sender
  byte for byte; Cerid offers no entry point that takes it, so no caller can reuse an ephemeral. The vectors hold the
  recipient side byte for byte (DeriveKeyPair, the serialized seeds, Decap, the combiner, the key schedule, every
  message and export), and the sealing side is held to it by round trips: every byte of `enc` reaches the
  recipient's combiner or ML-KEM decapsulation.

Evidence: `test_hpke.cpp` against draft-ietf-hpke-pq's `test-vectors.json` at its draft-ietf-hpke-pq-05 tag
(`hpke-pq-vectors` in `cmake/pins.json`): the five vectors of the profile (ML-KEM-768, ML-KEM-1024, MLKEM768-X25519,
MLKEM768-P256 and MLKEM1024-P384 with HKDF) for DeriveKeyPair, the serialized private key (z included, through
implicit rejection), the opening of the first message, the nine later messages refused and the 25 exported values,
and the eight others refused as outside the profile; round trips and exports over the five KEMs; the lengths of
every KEM; DeriveKeyPair's 32-byte floor; moves; implicit rejection; an ML-KEM key failing FIPS 203's input check;
X-Wing's all-zero element and recipient half; NIST-curve halves off the curve; the P-256 half's rejection sampling
(a seed whose first scalar chunk lies above the order); `test_provider.cpp` against FIPS 202's examples for SHA3-256
and SHAKE256; `crd-fuzz-crypto-mls`'s HPKE mode over every suite of the profile.

<a id="mls"></a>
### MLS

**M1. Cerid implements MLS** (RFC 9420; [mls.hpp](../../engine/foundation/crypto/include/crd/crypto/mls.hpp),
`mls.protocol`) over the provider interface, in `crd-crypto` beside the TLS and QUIC integration it shares keys and
policy with. The vetted implementations are Rust (OpenMLS, AWS's mls-rs) or C++ with exceptions, owning STL containers
and an OpenSSL dependency (Cisco's MLSpp); none fits the engine's allocators, error model and zero-allocation message
path, and none has the post-quantum suites behind Cerid's policy generations. MLS is a protocol over primitives the
provider already runs (HPKE per H1, HKDF, AEAD, signatures), as TLS's key schedule is over the provider's; the MLS
working group's test vectors, which OpenMLS, MLSpp and the other implementations generate and check, hold every layer
to the byte, and live interoperability with OpenMLS and MLSpp (M4) proves the whole.

**M2. The profile.** The suites whose every algorithm the provider implements: 0x0001
MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519 (mandatory to implement), 0x0002 MLS_128_DHKEMP256_AES128GCM_SHA256_P256,
0x0003 MLS_128_DHKEMX25519_CHACHA20POLY1305_SHA256_Ed25519 and 0x0007 MLS_256_DHKEMP384_AES256GCM_SHA384_P384 (the
high-assurance suite). 0x0004 to 0x0006 need X448, Ed448 or P-521, which no provider in the policy implements, and
are refused. The suites' hash is their KDF's (`kdf_hash`), as is the MAC's. The post-quantum suites of
draft-ietf-mls-pq-ciphersuites-07 (`mls.pq-ciphersuites`, pinned 2026-11-02) are in the table since 2026-11-02, over
HPKE's KEMs (H2): 0x0010 and 0x0011 (X-Wing, Ed25519; AES-128-GCM with SHA-256, AES-256-GCM with SHA-384), 0x0012
and 0x0013 (MLKEM768-P256, ECDSA P-256; the same two), 0x0014 (MLKEM1024-P384, ECDSA P-384), 0x0015 to 0x0017
(ML-KEM-768 with Ed25519 and with ECDSA P-256, ML-KEM-1024 with ECDSA P-384; AES-256-GCM, SHA-384), and 0x0020 to
0x0022 (X-Wing with ChaCha20-Poly1305 and ML-DSA-44, ML-KEM-768 with ML-DSA-65, ML-KEM-1024 with ML-DSA-87; SHA-384),
ML-DSA signing purely with an empty context and its keys in FIPS 204's encoding, as the draft says. IANA does not list
their codepoints yet, so the register keeps them unverified and the table names them through
`iana::unverified`; since 2026-11-03 the entry is implemented under the register's dated `awaiting_iana` record
(user order 2026-11-03, [registers](registers.md#awaiting-iana)), which switches to IANA's values when it lists
them. A post-quantum suite's keys and messages are an
order of magnitude larger: an HPKE public key or encapsulation of up to 1665 bytes (MLKEM1024-P384), and an ML-KEM's
encapsulation is shorter than its public key (Nenc 1088 against Npk 1184 for ML-KEM-768), so every kem_output (an
UpdatePath's, a Welcome's, an ExternalInit's) is sized by Nenc and every key by Npk; a signature public key of up to
2592 bytes and a signature of up to 4627 (ML-DSA-87). One buffer of each maximum (`kMaxNodeKeyBytes`,
`kMaxEncapsulationBytes`, `kMaxSignatureKeyBytes`, `kMaxSuiteSignatureBytes`, held to the table by a test) serves every
suite on the stack; an UpdatePath's up to 31 node keys take one allocation beside the path it writes.

**M3. Layers**, each closed against its vector file before the next builds on it:

1. The presentation language (section 2.1; `deserialization.json`): minimal variable-size vector headers up to
   2^30 - 1 (`take_varint` refuses the prefix 0b11 and every longer-than-needed form), vectors as views of the input
   and presence octets of 0 or 1, over the shared `WireReader` and `WireWriter`.
2. Array-based tree math (Appendix C; `tree-math.json`) for perfect trees of 1 to 2^31 leaves: level, width, root,
   children, parent, sibling, direct path, copath and common ancestor; any size that is not a power of two and any
   node outside the tree has no answer, so a size or an index read off the wire never reaches past the tree.
3. The labelled operations (`crypto-basics.json`): RefHash, ExpandWithLabel, DeriveSecret, DeriveTreeSecret,
   SignWithLabel, VerifyWithLabel, EncryptWithLabel and DecryptWithLabel, every label prefixed "MLS 1.0 ", every
   refusal zeroing its outputs. Labels and contexts up to 256 bytes allocate nothing.
4. The key schedule (section 8; `key-schedule.json`, `psk_secret.json`): the GroupContext's encoding,
   `EpochSecrets::advance` (a member's next epoch from the previous init secret, the commit secret and the PSK
   secret) and `join` (a joiner's from the Welcome's joiner secret), which hold the joiner, welcome, sender data,
   encryption, exporter, external, confirmation, membership, resumption, authentication and init secrets and zero the
   epoch and member secrets inside the derivation; `psk_secret` over external and resumption PreSharedKeyIDs with
   their PSKLabels (a nonce is Nh bytes, at most 65535 keys); MLS-Exporter. An epoch advances from its own init
   secret in place.
5. The secret tree and the sender data keys (sections 6.3.2 and 9; `secret-tree.json`), with the receive window of
   decision M5. Each parent's two children are derived together the first time a leaf below them is needed and the
   parent's secret is zeroed, a leaf's secret once its two ratchets hold theirs, a ratchet's once the next
   generation's is held, a key once spent or once it leaves the window (section 9.2).
6. Messages ([mls_message.hpp](../../engine/foundation/crypto/include/crd/crypto/mls_message.hpp);
   `transcript-hashes.json`, `message-protection.json`, `messages.json`): the structures a handshake message carries
   (credentials, capabilities and extensions, leaf nodes, key packages, the seven proposals, commits and update paths,
   sections 5.3, 7.2, 10, 12.1 and 12.4), decoded as views of the input with every vector header minimal, every enum
   value defined and every list a sequence of its entries, and written back byte for byte; a credential or proposal
   of a type this build does not know is its type and one opaque vector, as OpenMLS reads it. Their framing (section
   6): FramedContent, the signature over FramedContentTBS (with the GroupContext for a member and a new member
   committing), PublicMessage with a member's membership tag, and MLSMessage; the senders and content types a wire
   format admits are checked where a message is protected or verified, never where it is decoded, so every valid
   encoding round-trips. PrivateMessage (section 6.3): the next generation of the sender's handshake or application
   ratchet, a fresh random reuse guard XORed into the nonce, the content sealed under the message's own header (its
   PrivateContentAAD, so the associated data is never copied) and the sender data under the keys derived from the
   ciphertext's sample; a receiver opens the sender data, then the content under `receive_key`, checks the padding is
   all zero and only then spends the generation, so a forged or malformed message spends nothing; protecting and
   opening allocate nothing and open in place. The transcript hashes and the confirmation tag (section 8.2). Semantic
   checks of leaf nodes, key packages and commits belong to the layers below.
7. The ratchet tree and TreeKEM ([mls_tree.hpp](../../engine/foundation/crypto/include/crd/crypto/mls_tree.hpp);
   `tree-validation.json`, `tree-operations.json`, `treekem.json`; decision M7): the ratchet_tree extension decoded
   (each node of the type its position holds, the last one present, extended with blanks) and written without
   trailing blanks; resolutions and filtered direct paths (section 4.1); tree hashes held per node and recomputed
   along the nodes a change touches (7.8); parent hashes, with the original sibling tree hash computed with the
   parent's unmerged leaves blanked and dropped (7.9); the join-time checks (unmerged leaves increasing, present,
   below their parent and listed by every non-blank node between; every encryption and signature key once; every
   leaf's LeafNodeTBS signature with the group id and leaf index of update and commit leaves; every non-blank parent
   parent-hash valid relative to exactly one descendant, chains starting only at commit leaves); Add, Update and
   Remove with doubling and truncation (7.7, 12.1); merging an UpdatePath (its node count, the leaf's source and
   parent hash, a fresh leaf key and no path key held elsewhere checked before anything changes, 12.4.2); and
   TreeKEM: the committer's fresh leaf key, path secrets, node keys, signed commit leaf and HPKE ciphertexts to each
   copath child's resolution without the commit's new members under the provisional GroupContext; a receiver's
   decryption at the lowest path node above it, each derived public key checked against the path's, and the commit
   secret; a Welcome's path secret implanted at the lowest common ancestor (12.4.3.1).
8. A group and joining it ([mls_group.hpp](../../engine/foundation/crypto/include/crd/crypto/mls_group.hpp);
   `welcome.json`, `passive-client-welcome.json` and the Welcomes and GroupInfos of `messages.json`; decisions M8 and
   M9): the group's extensions (each type at most once, section 13.4; required_capabilities, section 11.1;
   external_senders, section 12.1.8.1) and the default extension and proposal types every client supports; a leaf
   node checked against its group (section 7.3: its source where it appears, an encryption key every agreement can
   use, its credential with the Authentication Service, its own credential type and non-default extensions listed,
   every GroupContext extension and required capability supported, a key-package leaf's lifetime) and the credential
   types of a whole tree (section 13.3); a key package checked against its group (section 10.1, its signatures over
   the leaf and over KeyPackageTBS) and KeyPackageRef; GroupInfo, GroupSecrets and Welcome decoded and written back,
   GroupInfoTBS signed and verified; the committer's Welcome (the GroupInfo sealed under the welcome key and nonce,
   then each new member's GroupSecrets sealed to its init key with the sealed GroupInfo as context); and a member's
   group state: a new group (section 11) and one joined from a Welcome (section 12.4.3.1).
9. Proposals and commits ([mls_group.hpp](../../engine/foundation/crypto/include/crd/crypto/mls_group.hpp),
   `mls_commit.cpp`; `passive-client-handling-commit.json`, `passive-client-random.json`; decision M10): a member's
   messages of an epoch processed (section 6's framing, the membership tag or the PrivateMessage opened in place, the
   signature under the sender's leaf, an external sender's key or a new member's key package), a proposal cached
   under its ProposalRef (section 5.2) when its sender may send its type (section 12.1.8), application data read, and
   a commit's list checked as section 12.2 says (the committer's own Update or removal, repeated Updates and Removes of
   a leaf, repeated PSKs and GroupContextExtensions, an ExternalInit, a type outside the profile), applied in section
   12.3's order to a copy of the tree with each proposal checked against the new extensions (an Update's leaf, its
   signature and new key; an Add's key package; every kept member's support of new extensions), its path checked and
   merged and decrypted under the provisional GroupContext, the new tree's keys and credential types, the key schedule
   with the PSKs and the confirmation tag: the next epoch staged, which `Group::merge` makes the group's. The
   committer's side builds the same list (the cached proposals section 12.2 prefers and its own by value), the path
   when required or asked for, the commit signed and protected, the new epoch's GroupInfo signed and the Welcome with
   each joiner's path secret and every PSK. Proposals and application data are sent too (`propose`, `propose_update`,
   `protect`), and a member's own Update's key is kept until a commit adopts the leaf.
10. a. External joins ([mls_group.hpp](../../engine/foundation/crypto/include/crd/crypto/mls_group.hpp),
    `mls_commit.cpp`; no working-group vector holds one, so `test_mls_external.cpp` holds the joiner and the members
    to each other; decision M11): a member's GroupInfo of its epoch signed on demand and after each commit, carrying
    the tree and, when asked, the external_pub extension (DeriveKeyPair(external_secret)'s public key); a new member
    joins from it (`Group::join_external`): the GroupInfo's tree and members checked as a Welcome's are, the
    ExternalInit's init secret exported to the external key (section 8.3), an optional Remove of its own former leaf
    (a resync) and PSKs, the leftmost free leaf (section 12.4.2) with a fresh path, the commit signed under the
    GroupInfo's GroupContext as a PublicMessage without a membership tag, the new epoch confirmed and its GroupInfo
    signed by the joiner; members process it as any commit (the sender's key the path leaf's, the list exactly one
    ExternalInit, at most one Remove and PSKs, none by reference, the leaf placed and checked, the key schedule from
    the init secret decapsulated with the epoch's external key) and every replaced leaf, by an Update, a path or a
    resync, is accepted only as its predecessor's successor (section 5.3.1's
    `AuthenticationService::valid_successor`).
    b. Reinitialization and subgroup branching
    ([mls_group.hpp](../../engine/foundation/crypto/include/crd/crypto/mls_group.hpp), `mls_commit.cpp`; no
    working-group vector holds a ReInit commit or a resumed Welcome, so `test_mls_resume.cpp` holds the creators and
    joiners to each other and to a hand-made committer; decision M12): a ReInit proposed, committed alone and merged,
    which ends the group (sections 12.1.5, 12.2 and 12.4.2); the new group of a ReInit (`Group::reinit`) or a subgroup
    (`Group::branch`) created with its first commit of Adds, the reinit or branch PSK and a path; a joiner's checks of
    a resumed group (section 12.4.3.1: the only such PSK, from the prior group it names, epoch 1, the ReInit's or the
    prior group's parameters, and the members).
    c. HPKE's post-quantum and hybrid KEMs, the precondition of the post-quantum suites (decision H2; the
    working group's draft-ietf-hpke-pq vectors), with the register pins of the four drafts and the key check every
    MLS key now passes (`hpke_check_public_key`).
    d. The post-quantum suites in the table (M2), with every kem_output sized by Nenc and the buffers sized for
    the largest keys and signatures; every behavioural suite of the layers above runs in all fifteen suites (no
    working-group vector holds a post-quantum suite).
    e. GREASE (section 13.5; decision M13).
    f. Live interoperability with MLSpp (M4, amended): the four shared suites, both libraries in each role.
    g. Live interoperability with OpenMLS (M4, amended): the three shared suites, both libraries in each role, the
    pinned Rust toolchain and OpenMLS 0.9.2's locked crates; key packages checked as section 10 says (M13).

**M5. The receive window** (RFC 9420 section 9.2 leaves its size to the application). OpenMLS's defaults: a receiver
keeps the keys of the 5 generations below the next unconsumed one of each ratchet (late and reordered messages) and
derives at most 1000 generations ahead of it, so a forged generation costs a bounded chain before the message fails
to open; both are settable per tree, up to 1024 and 65536. A key is read without changing the tree (`receive_key`)
and spent only after its message authenticated (`consume`), so a forged message spends nothing. Generation g lives in
slot g mod the window, every slot of the window holds a kept or a spent generation, and so a spent generation is
Replayed and a dropped or too distant one OutOfWindow (a new `CryptoError`) in constant time. A tree allocates once
when its epoch starts (per leaf two node secrets, two ratchets and twice the window's slots, about 0.7 KiB with the
default window) and nothing on the message path; it has at most 2^20 leaves (about 0.7 GiB), a group size no
deployment of MLS approaches and a bound on what a hostile tree size can claim. A sender's own ratchet hands out its
generations in turn (`send_key`) and is Exhausted past 2^32 - 1, when the group must start a new epoch.

**M7. Tree state and staged keys.** A RatchetTree owns each node's encoding, so the messages it came from need not
outlive it, and holds every node's tree hash and two occupancy bits (a non-blank node below, for resolutions and
filtered direct paths; a non-blank leaf below, for truncation), recomputed along the path a change touches: the root
hash is a read, and a 2^20-leaf tree changes in O(log n) hashes. Every refusal leaves a tree as it was (checks run
before anything changes; the encodings a change writes are built first); ProviderFailure empties it rather than leave
stale hashes. A commit's keys are staged: create_update_path and process_update_path write the new leaf key, the path
keys and the path and commit secrets into a PathUpdate, which PathKeys::apply takes only once the commit is confirmed
(section 12.4.2's confirmation tag), so a commit refused late changes no key; `prune` drops the keys of nodes a
proposal blanked or replaced (section 7.5's deletion). A received path's derived public keys are compared with the
path's, so a committer cannot hand members a secret that disagrees with the keys it published. Parent-hash validity
at join takes RFC 9420's top-down form: every non-blank parent valid relative to exactly one of its children's
descendants.

**M8. Validation against a group.** What RFC 9420 leaves to the application is the caller's, never read here:
the time (`LeafPolicy::now`, so a vector's or a recording's leaves stay checkable years later), the maximum total
lifetime of a key-package leaf (section 7.2's MUST: `kDefaultMaxLifetime`, OpenMLS's default key package lifetime of
twelve weeks and its one-hour margin, so the two accept each other's packages; settable per group, the working
group's vectors use a year and forever), whether a joiner checks the lifetimes of key-package leaves already in the
tree (section 7.3's RECOMMENDED: on by default; a group whose members update less often turns it off), whether a
credential binds acceptable identifiers to its key (section 5.3.1's Authentication Service, an interface the
application implements: asked for every member's credential when joining, each added member's and each external
sender's), and which pre-shared keys and groups a member holds (a `GroupStore`). A leaf's or init key's encryption
key must be one every encapsulation can use (`hpke_check_public_key`: an uncompressed point on the curve, an
X25519 key not of small order; decision H2 for the post-quantum KEMs), checked once when it enters the group, since every later commit encrypts to it and a key
that cannot be encrypted to would stop the group; the same holds for every parent key a joiner receives. Whether a
leaf's capabilities name the group's version and cipher suite is not checked: sections 7.3 and 10.1 do not ask it,
choosing the best suite all members support is the creator's duty (section 11), and the working group's suite-7
welcome key package lists suites 1 to 6 only. Default extension and proposal types are supported whether or not a
list names them. Section 13.3's credential rule is checked over the whole tree in time linear in the leaves and their
lists (each type in use found once, each member's list marked and cleared again).

**M9. Group state and joining.** A `Group` holds one member's epoch: the GroupContext's fields (owned, so the
messages they came from need not outlive them), the ratchet tree, the member's path keys, the epoch secrets, the
secret tree and the interim transcript hash. A new group's epoch 0 comes from a fresh random init secret with no
commit or PSK secret, so its epoch secret is a fresh random value (section 11); its confirmation tag is over the empty
confirmed transcript hash. Joining runs section 12.4.3.1 in its order: the Welcome opened for the member's key
package (`open_welcome`: its KeyPackageRef, the init key checked against the package, the GroupSecrets decrypted with
the sealed GroupInfo as context, the PSKs looked up in the store and named once each, the GroupInfo opened under the
welcome key and nonce, which `welcome_secret` and `welcome_key` derive before the GroupContext is known), then the
group id the member must not already hold, the tree (the one the member fetched, or the GroupInfo's ratchet_tree
extension) whose root hash is the GroupContext's, the GroupInfo's signature under its signer leaf, the tree's
join-time checks (M7) and M8's checks of every member (on the member's own leaf, which is its package's, that is
section 13.4's rule that a joiner supports every GroupContext extension; a separate check proved redundant), the
member's own leaf (found by its encryption key, then compared whole) and its private keys, the path secret implanted
at the lowest common ancestor of the signer's and the member's leaves, the epoch's secrets and the confirmation tag.
A refusal anywhere leaves the group empty. A resumption PSK of usage reinit or branch is taken only from the prior
group the joiner names, whose rules decision M12 checks. Every Welcome of one commit must seal the same GroupInfo,
since the key and nonce are the joiner secret's.

**M10. Epoch messages and commits.** A received message is processed whole into a `ReceivedMessage` that owns its
bytes (a PrivateMessage opens in place); a commit is staged, not applied: `StagedCommit` holds the next epoch (tree,
extensions, transcript hashes, secrets, secret tree), the path's keys for the member and the committed proposals'
encodings, so the application may read the proposals and apply its own rules (section 12.2 lets it) before
`Group::merge` takes it in constant work (the past epoch's resumption PSK kept, the member's path keys pruned and the
path's applied, an own Update's kept key adopted, the cache dropped). A committer stages its own commit the same way
and merges it once the delivery service accepted it; its own commit echoed back is Replayed, and a staged commit of
an epoch the group left is OutOfWindow. Nothing of the group changes before a merge but a PrivateMessage's spent
generation (section 9.2). What RFC 9420 leaves to the application is bounded per group (`GroupConfig`): at most
`pending_proposals` cached proposals per epoch (256 by default, up to 65536; Exhausted past it, so a flood of
proposals costs bounded memory), and the resumption PSKs of `resumption_epochs` past epochs (32 by default, 56
bytes each, up to 1024), which a commit's PreSharedKey proposal may inject (section 8.6): this group's own are looked
up there first, then in the store. A member's commit includes by default every cached proposal section 12.2 prefers
(a Remove over an Update of the same leaf, the latest Update of a leaf, the first of repeated Adds of one client and
of repeated PSKs, the latest GroupContextExtensions every member supports, only proposals valid on their own) and its
own by value; an Update by value would be the committer's own and is refused. A member that saw proposals commits
before it sends application data (section 12.4: `protect` is NotReady while proposals wait). A ReInit and the PSKs
of usage reinit and branch follow decision M12. A commit from a new member is an external join (decision M11).

**M11. External joins.** A new member joins by an external commit only from a GroupInfo that carries the external_pub
extension, and only a member can publish one: the key is DeriveKeyPair(external_secret) of the epoch, which no one
outside the group derives, so the GroupInfoOptions default is the tree without the key and a group is closed to
external joins until a member publishes it (`Group::group_info`, or a commit's GroupInfo, with `external_pub`). The
joiner checks the GroupInfo as a Welcome's (its signature under the signer's leaf, the tree's root hash and join-time
checks, every member's leaf), takes its confirmed transcript hash and derives the interim hash from its confirmation
tag, which only the signature vouches for (a joiner holds no confirmation key), and checks its own commit as the
members will (the list, its leaf as a commit leaf at the leftmost free leaf, the new tree's keys and credential
types), so a commit it sends is one the members take. A joiner has no epoch before its commit, so staging buys
nothing: `join_external` leaves the group at the epoch its commit begins, and when the delivery service refuses the
commit (another commit won the epoch, OpenMLS's and MLSpp's race) the application drops the group and joins again from
a newer GroupInfo; members see the losing commit as OutOfWindow. A resync (the joiner's Remove of its own former leaf)
is accepted only when the Authentication Service judges the new credential a successor of the removed leaf's (section
5.3.1's rule, which the same check now enforces for every Update and every commit path), so a stranger cannot take a
member's place by removing it; a former state that still runs processes the commit as its own removal. The external
commit always carries a path (section 12.4.3.2), its proposals are by value only, and the receiver checks the
ExternalInit's kem_output is Npk long before any key work; a kem_output no agreement can use is BadKey or WeakSecret,
and one that decapsulates to another init secret fails at the confirmation tag (Forged). The joiner's PSKs come from
its store alone (it holds no resumption secret of the group).

**M12. Reinitialization and branching** (RFC 9420 sections 11.2, 11.3, 12.1.4, 12.1.5, 12.2, 12.4.2 and 12.4.3.1). A
ReInit is a proposal like the others (a member's `propose`, or an external sender's), valid on its own when its version
is no older than the group's and it names each extension once; its cipher suite is the new group's creator's to judge,
so a ReInit to a suite or a version this build does not implement still ends the group, and `reinit` refuses it then
(Unsupported). A commit holds a ReInit only alone (section 12.2). A member's default list takes the latest valid cached
ReInit only when nothing else is committed, by reference or by value: section 12.2 prefers the other proposals, and the
ReInit is proposed again in a later epoch. Merging a ReInit's commit ends the group (`Group::ended`): it keeps the
epoch's secrets and the ReInit, sends nothing (NotReady) and takes no message (OutOfWindow), as section 12.4.2 requires,
and a ReInit's commit never publishes the external key. Any member of the ended group creates the new group
(`Group::reinit`, section 11.2's step 3), and any member of a group, ended or not, a subgroup (`Group::branch`, section
11.3; from an ended group, a way on when its ReInit names what this build cannot create): a group created as section 11
says, then its first commit (an Add per other member, the PSK of usage reinit or branch, and a path), merged at once,
since the creator is the whole group at epoch 0 and no delivery service can refuse that commit. The PSK names the prior
group at its current epoch: for a reinit, the epoch the ReInit's commit began, as section 11.2 requires; for a branch,
the epoch the member branches from, so a member that moved on is never brought into a branch of a past epoch. It carries
a fresh Nh-byte nonce (section 11.3), and its secret is the prior group's resumption secret, never the store's. A reinit
or branch key is accepted only in that first commit (section 12.1.4) and, in a Welcome, only once and only from the
prior group the joiner names (`Group::join`'s `prior`; section 12.4.3.1). The joiner then checks what section 12.4.3.1
lists: epoch 1; for a reinit, the ReInit of the prior group's last commit and its group id, version, cipher suite and
extensions; for a branch, the prior group's version and cipher suite; and the members. Membership "according to the
application" is the Authentication Service's `valid_successor`, its judgement that two credentials present one member:
each member of the new group matches a distinct member of the prior group, and for a reinit every prior member is
matched. Section 12.4.3.1 alone asks only that every prior member be in the new group; that each new member also be a
prior one is Cerid's rule, from section 11.2's "same membership", so a peer's reinitialization that adds a member is
refused (Forged) by design. The matching costs O(n·m) judgements, since the application's equivalence has no order to
sort by. The creator checks its own new group the same way before it returns the Welcome. A reinitialization may keep
the prior group's id, and then the store's repeat check spares that id: the member takes no further part in the group
the ReInit ended.

**M13. GREASE** (RFC 9420 section 13.5). The fifteen reserved values 0x0A0A to 0xEAEA (`grease`, `random_grease`; 0xFAFA
is private use). A sender puts random ones where receivers ignore unknown values: Cerid adds an empty extension of a
random GREASE type to every GroupInfo it signs (`GroupInfoOptions::grease`, on by default, none added beside an
application's own), and an application adds them to its leaves' capabilities and extensions and its key packages'
extensions, which the API takes as written (the tests' members all do), each extension of a type the leaf's capabilities
list: section 10 makes that a MUST for a key package's extensions as section 7.2 does for a leaf's. `sign_key_package`
signs a package as written (as `sign_leaf_node` does a leaf), and `check_key_package` refuses one with an unlisted
extension type (Unsupported), as OpenMLS does: a committer that let one in would split a group whose other members check
it on the Add (MLSpp's packages break the rule by construction: TP-11). A receiver gives them no special processing: an
unknown capability and a GroupInfo's unknown extension are ignored, and a leaf's or a key package's extension of a
GREASE type is accepted when the leaf's capabilities list it. Section 13.5 forbids them in a proposal's or a
credential's type and in a GroupContext's extensions, and requires a client to compare its own capabilities without
them: Cerid compares every leaf's capabilities against what a group requires or uses that way (`supports_extension`,
`supports_proposal`, `supports_credential`), so a GREASE value is never supported. A group whose context or required
capabilities name one, a GroupContextExtensions proposal adding one and a leaf whose credential type is one are refused
(Unsupported) on both sides, though the sender's own capabilities list it, and a proposal of a GREASE type is a type
this build does not implement.

<a id="mls-policy"></a>**M14. A group's cipher suite runs under the policy generation** (threat T04, decision D7,
[S20](#tls-policy); 2026-11-03). `GroupConfig::crypto_policy` names the installation's `PolicyStore` and `policy_scope`
the group's scope (CeridServices for a Cerid service's own groups); a scope without a store, or one the build does not
know, is Unsupported, and without a store (tests and tools) the suite is not checked. `check_suite_policy` judges the
suite's KEM, KDF, AEAD and signature algorithm together at the group's clock (`LeafPolicy::now`, which
`Group::set_now` moves forward only), and refuses with `PolicyRefused`:

- `create`, `join` (after the Welcome opens and names the suite), `join_external`, and so `reinit` and `branch`;
- every epoch: a commit the member makes and one it receives are staged only under an allowed suite, the group's or,
  for a ReInit, the one it names. A ReInit to an allowed suite is how a group leaves a refused one; a ReInit to a
  refused suite is refused. The current epoch's keys stay until it ends, as a TLS session's stay until its next key
  update: application messages and proposals still flow.

Evidence: `test_mls_policy.cpp` (creation, Welcome and external joins refused and allowed per scope, the scope's
configuration refusals; a member's own and a received commit refused while application data flows; a ReInit to a
refused suite refused and one to the hybrid suite ending the group, which is created again and joined under the
generation; a dated refusal reaching the group when its clock passes the date, a clock that never goes back).

**M6. Padding** (RFC 9420 section 6.3 leaves its amount to the application). A PrivateMessageContent of L bytes is
padded with Padmé (Nikitin et al., "Reducing Metadata Leakage from Encrypted Files and Communication with PURBs",
PETS 2019; `padme_padding`): with E = floor(log2 L), the top floor(log2 E) + 1 bits of L are kept and the bits below
rounded up, so a padded length reveals O(log log L) bits instead of L's O(log L), for at most 12 % more bytes (11.6 %
at worst, at 129 bytes). The sender may pad more (a call's frames to a constant size); the receiver accepts any
length of zero bytes and refuses any other byte (section 6.3.1), so padding is never a covert channel.

**M4. Interoperability** runs as TLS's does: pinned peers built in their own projects on the public-checks lane
(OpenMLS through the pinned Rust toolchain, MLSpp against the pinned OpenSSL) and a Python driver relaying MLS messages
between a Cerid group member and each peer, so Cerid creates, joins, commits, protects and processes in groups where
the others do too; the MLS working group's gRPC harness is not used.

Amended 2026-11-02, with the first peer. Each peer is a program over its own library's interface that holds several
members and speaks one line protocol (`cerid_mls_peer.cpp`'s header), so the members of one group live in both
processes and every message one library makes the other processes; `mls_interop.py` is the delivery service and reads
every message off the wire itself. **The post-quantum suites have no codepoint in common**: Cerid's are
draft-ietf-mls-pq-ciphersuites-07's (M2), MLSpp's pinned main implements an earlier assignment (0x0008 to 0x000A over
ML-KEM hybrids, with the HPKE KEM ids 0x0041, 0x0042, 0x0050, 0x0051 and X-Wing's 0x647A) and OpenMLS 0.9 provisional
ones (0x004D to 0x004F and 0x0906, and no P-256 or P-384 hybrid). A codepoint is a suite's identity on the wire and in
every hash, so no peer agrees through an alias without a non-standard codepoint in Cerid's table: the post-quantum
suites interoperate once the peers take the draft's or IANA's codepoints (`mls.pq-ciphersuites` tracks both), and until
then interoperability runs on the RFC 9420 suites each peer shares with Cerid (MLSpp: 0x0001, 0x0002, 0x0003, 0x0007;
OpenMLS's RustCrypto provider: 0x0001, 0x0002, 0x0003, P-384 being only in its post-quantum feature). A third-party
peer's deviation is registered once and the driver keeps every other oracle (MLSpp:
[TP-9](../third-party-defects.md#tp-9), [TP-11](../third-party-defects.md#tp-11); OpenMLS:
[TP-10](../third-party-defects.md#tp-10)). TP-10 makes OpenMLS refuse every GREASE leaf and package extension and
MLSpp puts one on each of its leaves, so the two peers never share a group: each meets Cerid alone.

MLSpp (`crd-crypto-mls-interop-mlspp`, `CRD_BUILD_MLS_VS_MLSPP` on the complete-tier `linux-gcc-public-checks`
lane; MLSpp built by its own CMake against the pinned OpenSSL 4.0): in each shared suite a group created by each
library, with handshake messages public and private, whose roles alternate between the two, so that each library
adds members by value with the tree in the Welcome, commits an Update and an Add by reference for a joiner who
fetches the tree, commits an Update and a Remove by reference (the removed member told so), an external PSK and an
Add without a path, an empty commit, external joins from a GroupInfo with the tree and without it, and a resync;
after every epoch all members report the same epoch, authenticator, exporter, tree hash and roster and read each
other's application data. Every message is first delivered with each authenticated field forged (signature,
membership and confirmation tags, a PrivateMessage's sender data and content: refused, the group unchanged), an
application message again (refused as replayed) and one of the past epoch (refused); a committer refuses a key
package of an endless lifetime (decision M8) and one of another suite. The driver reads every key package, Welcome
(each entry addressed by the KeyPackageRef it computes) and GroupInfo off the wire, GREASE included, and every key
package's extensions listed by its capabilities (section 10).

OpenMLS (`crd-crypto-mls-interop-openmls`, `CRD_BUILD_MLS_VS_OPENMLS` on the same lane): `crd-openmls-peer`, a Rust
program over OpenMLS 0.9.2 and its RustCrypto provider (each member with its own provider and storage, OpenMLS keying a
group's state by its id), linted by Clippy and built by `cargo build --locked` with the pinned Rust toolchain
(`tools/rust-linux-x64`, `scripts/install-rust.py`) against the committed Cargo.lock (`tools/openmls-crates`: every
crate from crates.io at the SHA-256 it names), with CARGO_HOME inside the build tree and the peer's own warnings
errors. The same group script and
hostile deliveries in each of the three suites, each library creating, handshakes public and private (forged content
included: OpenMLS 0.9.2 spends no generation on it), the refusals of the endless and foreign key packages, and
TP-10's reproduction: 18 cases. OpenMLS's group configuration fixes a group's outgoing handshake format, so the peer
sets it before each proposal and commit and reads both (its mixed policies); its external commit removes the leaf of
the joiner's signature key, so a resync joiner takes over its former member's signer and credential, and Cerid accepts
the one commit that removes that leaf and adds the same key in a new one.

Evidence: `test_mls_schedule.cpp` (every epoch of the key-schedule vectors in the four suites, five each: the
GroupContext, the eleven secrets advanced in place and joined from the joiner secret, the external key and the
exporter; the 44 psk_secret vectors of 0 to 10 keys; the 12 secret-tree vectors' sender data keys and every leaf's
generations 0 and 15 of both ratchets received in order, received newest first in reverse leaf order and sent; the
window against a restatement of section 9: late, spent, dropped and too-distant generations, the bounds at their
edges, every other ratchet untouched after one leaf opened its path, a tree of one leaf, a window of none, moves and
every refusal; a resumption key's PSKLabel restated; the epoch's refusals and that each input reaches its outputs),
`test_mls.cpp` (the suites; the 14 deserialization headers decoded, sized and written back; the malformed
headers and presence octets; the 10 tree-math trees of 1 to 512 leaves node by node; direct paths, copaths and common
ancestors against their definitions for every pair of nodes of trees up to 64 leaves and at 2^31 leaves; every
crypto-basics vector of the four suites with fresh signatures and encryptions, and the refusal of the three outside;
DeriveKeyPair of the 20 external keys of the key-schedule vectors; every refusal), `test_provider.cpp` (`kdf_hash`
against FIPS 180-4's examples and `kdf_mac` against Wycheproof's 348 HMAC-SHA-256 and HMAC-SHA-384 vectors),
`test_mls_message.cpp` (every transcript-hashes vector of the four suites: the commit's AuthenticatedContent written
back, its confirmation tag, both hashes and the input restated; every message-protection vector: each public message
verified and protected again, byte for byte under Ed25519, application data refused as a public message, each private
message opened, replayed and protected again into a fresh tree; the 300 messages vectors' key packages, the seven
proposal bodies, commits, public and private messages, Welcomes and GroupInfos decoded and written back;
PrivateMessage against a restatement of section 6.3 in both directions, with in-place opening, the padding rule,
truncated and non-minimal contents, every single-byte change of a message, replay, another epoch's keys and a leaf
past the tree; the framing rules of section 6 for every sender, content type and wire format; public messages from
every sender with their tags and signatures bound to the context, the wire format and each field; the decoders'
refusals; Padmé against its definition restated for every length below 70,000), `test_mls_tree.cpp` (the 56
tree-validation vectors of the profile: each tree decoded and written back, every node's resolution and tree hash,
the join-time checks; the 5 tree-operations vectors; the 44 treekem vectors: each member's private state checked
against the tree, every update path parent-hash valid, merged to its tree hash and decrypted by every other member to
its path secret and commit secret, then written again by its sender and merged and decrypted by the others; model
groups in each suite that add, update, remove and commit at random, every member's tree, commit secret and held keys
compared with TreeKEM's invariant at every epoch and each joiner validating the tree and implanting its Welcome path
secret; doubling, truncation and trailing blanks; the decoder's, the join-time checks', the merge's, the decryption's,
the tree changes' and the path keys' refusals, each leaving the tree unchanged), `test_mls_group.cpp` (every welcome
vector of the profile: the Welcome decoded and written back, its key package checked, the Welcome opened with the
init key, the GroupInfo verified under the signer's key and its epoch's confirmation tag; every
passive-client-welcome vector of the profile joined to its epoch authenticator, the tree given or carried, external
PSKs held; the messages vectors' Welcomes and GroupInfos written back; groups a creator starts in every suite and
joiners enter from real key packages, with and without a path, PSKs and the tree in the GroupInfo, every member
secret, the tree, the keys and the interim hash compared with the committer's; every refusal of a join, each leaving
the group empty, among them a hostile tree whose root key is a small-order point under valid parent hashes and
signatures; a group's creation; section 7.3's leaf checks in order, 13.3's credential types, 10.1's key package
checks and KeyPackageRef; extension lists, capabilities, required capabilities and external senders),
`test_mls_commit.cpp` (every passive-client-handling-commit vector of the profile, 52 vectors and 104 epochs with
proposals by value and by reference, external and resumption PSKs, Adds, Updates, Removes and GroupContextExtensions,
and the passive-client-random vector's 200 epochs, each epoch's authenticator reached, each proposal refused as
Replayed when sent again and each commit as OutOfWindow in the next epoch; groups of Cerid members in every suite as
public and private messages: four joiners from one Welcome, an Update, a Remove of a member that processes its own
removal and a PSK by reference, an empty commit, a partial commit with an Add and a resumption PSK and the tree
fetched, GroupContextExtensions with required_capabilities and a Remove by value, application data each way and
replayed, every member at the same epoch, authenticator, tree and transcript; two listed external senders and a new
member proposing, their proposals committed by reference; every refusal of sections 6, 12.1,
12.2 and 12.4.2 by a member's own framing of a hostile list, the group unchanged after each and still taking a valid
commit; the sending, committing and merging refusals; the proposal bound and the resumption PSK ring),
`test_mls_external.cpp` (groups of Cerid members in every suite that new members join externally from a published
GroupInfo with and without its tree (fetched beside it), with a PSK, one after another as the tree doubles, a member
resyncing into its own former leaf and another whose former state still runs and processes its own removal, two
joiners racing from one GroupInfo (the loser refused as OutOfWindow and joining again), the joiners then committing
and welcoming, application data each way, every member at the same epoch, authenticator, tree and transcript; a
member's GroupInfo with the external key only when asked, the key DeriveKeyPair(external_secret)'s, its context,
tag and signature, and its refusals; every refusal of the joiner (24 faults of the GroupInfo, the tree, the external
key and its own commit, among them a resync of another client's leaf and a joiner holding a member's signature key),
the joiner left empty; every refusal of a member (no ExternalInit or two, an Add, Update, GroupContextExtensions or
ReInit beside it, two Removes, a short kem_output, a proposal by reference, a blank Remove, no path, repeated and
unknown PSKs, another signer, a kem_output no agreement can use, another init secret at the confirmation tag, a
refused credential, a resync the Authentication Service refuses as a successor, the commit in the next epoch), the
group unchanged; the reserved leaf, the joining path made and merged to the same tree and the joiner's keys started),
`test_mls_resume.cpp` (groups in every suite that a ReInit ends, proposed by a member, and that another member than the
committer reinitializes into the next suite with new extensions, the group id changed or kept with the store naming it;
subgroups branched from a group that goes on, one of the creator alone; the new groups' members joining with their prior
groups, agreeing with the creator, exchanging data and committing; a ReInit committed alone: a cached ReInit left out
for a Remove or a by-value proposal, a by-value ReInit beside a cached proposal refused, the latest valid cached ReInit
chosen (an external sender's over a member's and an older version's), a cached branch PSK never committed, and a
ReInit's commit refusing the external key; every send of an ended group NotReady and a message to it OutOfWindow; every
refusal of a creator (a prior group no ReInit ended, an empty one, itself, a stranger, a missing member, a duplicated
one, a key package of another suite, a ReInit of a suite or version outside this build); every refusal of a joiner
against a hand-made creator (the ReInit's group id, cipher suite and extensions, epoch 2, a missing member, a stranger,
a duplicate, no prior group, another one (one at the same epoch among them), a prior ReInit of a later version, a past
epoch, a live prior group, two resuming PSKs, a branch in another suite or past epoch 1 or with a stranger, a known
group id, the joining group as its own prior), each left empty),
`crd-fuzz-crypto-mls-commit` (any message, and the honest proposal, commit and external commit changed in any byte,
refused with the member unchanged or accepted only as the honest one; the published GroupInfo changed in any byte
refused by a joiner left empty, the intact one joined and its commit taken; the honest ReInit's commit changed in any
byte, refused or ending the group only intact; the resumed Welcome changed in any byte, refused with the joiner empty
or joining only the honest new epoch), `crd-fuzz-crypto-mls-group` (every group decoder's acceptance
written back, extension lists against a restatement, and an honest Welcome changed in any byte refused with the group
empty or joining only the honest epoch),
`crd-fuzz-crypto-mls-tree` (accepted
trees written back exactly, resolutions and filtered direct paths against section 4.1 restated, tree changes against a
model with the held hashes equal to a fresh decode's, and a changed update path refused unchanged or decrypting only
to the honest commit secret), `crd-fuzz-crypto-mls-message`
(every decoder's acceptance written back, private messages changed in any byte or hostile refused with nothing
spent, public messages verified exactly when unchanged) and `crd-fuzz-crypto-mls` (headers against a restatement of section 2.1.2, tree math
against its invariants, HPKE round trips with any bit changed, hostile openings and signatures, derivations, and
sequences of reads, spends and sends on a secret tree against a model of the window and section 9 restated).
Since layer 10d the behavioural suites run in all fifteen suites and the fuzz targets' honest groups are made in each
(both wire formats for `crd-fuzz-crypto-mls-commit`); `test_mls.cpp` restates the table and holds the four buffer
maxima to its largest key, encapsulation, signature key and signature; every test member's capabilities, leaf and key
package carry GREASE, and `test_mls_external.cpp` checks the fifteen values, random draws of each, the GroupInfo's
GREASE extension (none when asked not to or beside the application's own), joins from a Welcome and an external
commit with GREASE throughout, and the refusals of a GREASE GroupContext extension, GroupContextExtensions proposal and
credential type.

<a id="negotiation"></a>
## Version and extension negotiation

Every Cerid wire protocol (session control, game, project, media control, crash upload, agent, service and relay
allocation) carries in its handshake:

- a protocol identifier and a version (major: incompatible; minor: compatible additions);
- an extension list of (identifier, length, value) entries; an unknown extension is ignored, never fatal;
- GREASE identifiers from a reserved pattern, sent by every build including shipping (RFC 8701 practice; every TLS
  ClientHello too), with the QUIC bit greased per RFC 9287 and compatible version negotiation per RFC 9368 in
  [4.2a](../ROADMAP.md#slice-4.2a)'s QUIC transport;
- transcript binding: the negotiated version, extensions and algorithms are covered by the handshake transcript, so
  a downgrade is detected (T05).

Schemas evolve by adding fields with new identifiers; identifiers are never reused, and readers accept version N-1.
Long-lived artefacts (replays, crash packs, qlog and pcapng captures, world journals) carry schema versions with N-1
readers (OPS.2 owns the N-1 bundle reader). NET-STD implements the mechanism and its register.

<a id="negotiation-wire"></a>
### The negotiation messages

`crd-net` implements this in [negotiation.hpp](../../engine/foundation/net/include/crd/net/negotiation.hpp)
(NET-STD, 2026-10-31). Every integer is big-endian; the protocol identifiers and the GREASE values come from the
[standards register](registers.md#cerid-identifiers) through the generated
[register_ids.hpp](../../engine/foundation/net/include/crd/net/register_ids.hpp).

| Message | Layout |
|---|---|
| Offer (client) | kind `1` (u8), format (u8), protocol (u16), version count (u8), each version major and its highest minor (u16, u16), extension count (u8), each extension identifier, length and value (u16, u16, bytes) |
| Answer (server) | kind `2`, format, protocol, the selected major and minor (u16, u16), the server's available versions (count and entries as above), the accepted extensions with the server's values |

- **Selection.** The server picks the highest major both lists hold; the minor is the lower of the two highest minors
  (minors are compatible additions). It keeps the offered extensions it knows, in offer order, and answers only those.
- **Ignore-unknown.** A major or an extension a receiver does not know is skipped in both directions. An answer may
  carry an extension the client does not know (a newer server's addition); only an extension the client knows and
  did not offer is refused (`UnsolicitedExtension`), because the client deliberately left it out.
- **GREASE.** Every offer and answer, in every build, holds one GREASE version major and two distinct GREASE
  extensions (0 to 7 value bytes) from the reserved `0x?A?A` values, their values and positions drawn from 24 bytes of
  caller entropy (the CSPRNG in the transport, the seeded stream in NET-SIM, so a simulated run is replayable). A
  policy may not list a GREASE value; a selected GREASE major is refused.
- **Downgrade.** The answer lists the server's available versions (the RFC 9368 pattern). The client compares them
  with the offer it really sent: a higher common major than the selection means the offer was stripped in flight
  (`Downgrade`); a selection the available list contradicts is `InconsistentAnswer`. The in-band check is only as good
  as the answer's integrity, so the handshake binds both messages: `write_transcript` gives the 8-byte label
  `crdneg01`, the offer's length (u32), the offer, the answer's length and the answer, which NET.2 folds into its
  transcript hash. A change to either message in flight makes the two sides' transcripts differ.
- **Bounds.** At most 8 versions and 32 extensions per message (GREASE included, so a policy has at most 7 majors and
  30 extensions), 1024 bytes per extension value and 4096 bytes per message. Decoding is exact: a short field, a count
  or a value above its bound, a repeated major or identifier and a byte after the last field are each refused with
  their own error, and nothing allocates (decoded values view the received bytes).
- **Format.** The message layout has its own format byte. A build writes `kNegotiationFormat` and reads every format
  from `kNegotiationFormatOldest` on; when layout 2 lands, the oldest readable stays 1 (N-1) and an answer is written
  in the offer's layout.

<a id="agility"></a>
## Crypto agility

- **Policy generations.** The accepted algorithm set is a signed policy generation: accepted key exchange groups,
  signature algorithms, AEADs and minimum protocol versions, each with effective and expiry dates. Generations are
  signed by offline, hardware-held policy keys with a hybrid signature, numbered monotonically and never applied out of
  order; a client refuses a generation older than the newest it has accepted.
- **Swapped live** between connections: established sessions keep their keys until their next key update; new
  sessions use the new generation. A live swap never weakens authentication: a generation that adds an algorithm is a
  reviewed release, while one that removes an algorithm may be pushed at once.
- **Migration windows** state when an algorithm becomes preferred, accepted only, and refused; an **offline grace
  period** lets a disconnected client use its last generation for a bounded time before networked features require
  an update.
- **Remote disable**: a signed generation that removes a broken algorithm takes effect at the next connection, with
  priority distribution through the update channel (T19).
- **Dated refusal of classical-only key exchange**: first for Cerid services, and everywhere by 2030, aligned with
  NIST IR 8547 (final on 2026-10-27: every quantum-vulnerable public-key algorithm, whatever its strength, is
  deprecated after 2030 and disallowed after 2035); NET.2 sets the dates.
- RFC 7696 is the checklist; NET-STD records each item's evidence.

<a id="policy-wire"></a>
### The policy generation

`crd-crypto` implements the mechanism in [policy.hpp](../../engine/foundation/crypto/include/crd/crypto/policy.hpp)
(NET-STD, 2026-10-31); NET.2 issues the first generation, holds the offline keys and sets the dates. Algorithms are
named only by their Cerid algorithm identifiers ([registers](registers.md#cerid-identifiers), generated into
[register_ids.hpp](../../engine/foundation/crypto/include/crd/crypto/register_ids.hpp)), never by an IANA value or a
provider constant, so a provider swap or a corrected IANA value never changes a signed generation. Every integer is
big-endian.

| Part | Layout |
|---|---|
| Body (signed) | magic `CRDPOLCY` (8 bytes), format (u8, 2), generation (u64), issued, expires (u64 Unix seconds), offline grace (u32 seconds), minimum count (u8) and each protocol, major and minor (u16, u16, u16), rule count (u8) and each algorithm (u16), scope (u8) with accepted-from, preferred-from, preferred-until and refused-from (u64 each); format 1, read only, has no scope byte |
| Signatures | count (u8), each key identifier (32 bytes, the SHA-256 of the public key), algorithm (u16), length (u16) and bytes |

Every signature covers the body exactly as received. The magic separates the domain: no other Cerid signed artefact
begins with it (capability tokens begin with `CRDCAPTK`, their proofs' signed bytes with `CRDCAPPR` and revocation
generations with `CRDREVOK`, [S15](#capability-tokens), message schemas with `CRDSCHEM`, [S18](#message-schemas)), so
a signature over one never verifies as another, whatever the
provider.

Decisions (numbered for the tests and the fuzz oracle):

1. **Hybrid trust.** An installation trusts 2 to 4 keys, at least one post-quantum and one traditional signature
   algorithm (D7), and every one of them must sign every generation: a missing signature is refused before a wrong
   one is judged, in the trust set's order. Signatures by other keys are ignored, so a later build can add a key
   without breaking an older one. A signature labelled with another algorithm than its key's is refused.
2. **Monotonic numbers.** A generation below the newest one the installation ever accepted (the floor, persisted by
   the caller and passed back at start, so a restart cannot be rolled back) is refused before any signature is
   verified, which keeps a replay of old generations cheap. The current number again is `AlreadyCurrent` when its
   content is equal (signature bytes may differ, signing can be randomised) and a `Conflict` otherwise, reported only
   after its signatures verify, so a forgery never raises the conflict alarm. A newer number issued before the current
   generation is refused.
3. **Dates.** A generation is valid from issue to expiry (at most 400 days) and usable for its offline grace period
   after that (at most 90 days); one issued more than 300 seconds after the caller's clock, or already past its
   grace period, is refused. Past the grace period every algorithm is refused until a newer generation arrives, so
   networked features require an update.
4. **Windows.** An algorithm is accepted in `[accepted_from, refused_from)` and preferred in `[preferred_from,
   preferred_until)`, with `accepted_from <= preferred_from <= preferred_until <= refused_from` and a non-empty
   accepted window; outside it, or without a rule, it is refused. Dated refusals (classical-only key exchange first for
   Cerid services and everywhere by 2030) are rules whose `refused_from` NET.2 sets.
5. **Remote disable.** A generation whose rule refuses an algorithm from its own issue time, or that drops the rule,
   disables it at the next session; distribution is the update channel's (T19).
6. **The build's ceiling.** A rule takes effect only for an algorithm the build's providers implement. A generation
   therefore cannot enable anything a reviewed build does not hold (additions are reviewed releases, D7), while a
   removal reaches every build at once. Unknown identifiers decode and are ignored.
7. **Live swap.** A session takes a `PolicySnapshot` (a value) when it starts and keeps it until its next key update;
   applying a generation changes only what later snapshots hold. `PolicyStore` serialises `apply` and `snapshot`
   across threads and verifies signatures outside its lock.
8. **Bounds.** At most 16 protocol minimums, 32 rules, 8 signatures of 1 to 8192 bytes (room for SLH-DSA-128s) and
   32 KiB per generation; each algorithm and protocol at most once and each key at most once among the signatures.
   Decoding is exact (a short field, a count or a length above its bound and a byte after the last field are each
   refused with their own error) and allocates nothing. The layout has its own format byte with an N-1 reader, as the
   negotiation's.
9. **Scopes** (format 2, 2026-11-03). A rule governs every session (`Everywhere`, 0) or only Cerid service sessions
   (`CeridServices`, 1); a service session follows its algorithm's services rule where the generation has one and the
   Everywhere rule otherwise, which is how the dated refusal of classical-only key exchange reaches Cerid services
   before every other session (T04). Each algorithm has at most one rule per scope. A scope the build does not know
   decodes, stays signed and governs nothing, so a later build can add one; a format 1 rule is an Everywhere rule.
   Sessions enforce the rules as [S20](#tls-policy) states.

<a id="policy-issue"></a>
### Issuing a generation

**S22. The first generation and its issue** (NET.2, threats T04 and T19, decision D7; 2026-11-03).

- **The reviewed source.** A generation is authored as TOML
  ([generation-1.policy.toml](../../assets/crypto-policy/generation-1.policy.toml)) that names each algorithm by its
  standards register name (`algorithm_name`, generated beside the identifiers) and each bound as a day at 00:00:00
  UTC. Every key is known and every value checked; the content then passes `validate_content`, so a source the
  decoder would refuse is refused before any byte is signed. Signers sign the body the source renders, nothing else.
- **Generation 1's rules.** Every algorithm the build's providers implement has an Everywhere rule (an algorithm
  without one is refused, decision 4): X25519MLKEM768, ML-KEM-768, ML-DSA, AES-256-GCM and ChaCha20-Poly1305 are
  preferred. The dated refusals: classical-only key exchange (X25519, P-256, P-384) for Cerid services from
  2026-11-01T00:00:00Z, before any generation was issued, so no Cerid service ever accepts it, and everywhere from
  2030-01-01T00:00:00Z (NIST IR 8547's 2030 deprecation); traditional signatures (Ed25519, ECDSA) everywhere from
  2036-01-01T00:00:00Z (disallowed after 2035). A later generation can only move these dates earlier without a
  reviewed release (a removal); adding time is an addition (D7).
- **The tool.** `ceridc policy body` renders the source; each signer signs the body and writes a one-signature
  generation; `ceridc policy assemble` joins those of one body (a different body or a second signature by one key is
  refused); `ceridc policy verify` judges the result with a trust set (TOML: each key's register name and public
  key in lower-case hex), a floor and a time as `PolicyStore` would, reports each rule's status in its scope, and
  with `--source` refuses a generation whose signed bytes are not exactly the source's body. The layout, the checks
  and the verification stay crd-crypto's: the tool adds no second reader.
- **The key holders' keys** live in crd-crypto's key store, never in a loose file: `ceridc policy key` makes a
  signature key in a store (`--protector platform|tpm`, `--device tpm|pkcs11` with the PIN read from a file, never
  from the command line) or reports the one it holds under that name, algorithm and placement, with its `[[key]]`
  trust set entry; `ceridc policy sign` checks the body (`check_policy_body`) before anything is signed, signs it
  through `StoredSigner` (a device key's signature is checked by the device signer, every signature again under the
  stored public key) and writes a one-signature generation for `assemble`.
- **The issue ceremony** is the key holders' act, never an agent's: the post-quantum key (ML-DSA-65; no token holds
  one, so it is a key store file key on an offline machine under a hardware protector) and the traditional key
  (ECDSA P-256 in a TPM or a PKCS #11 token, decision K12/K14) sign the rendered body, and the trust set and the
  signed generation are committed beside the source with the bootstrap header regenerated from them
  (`python scripts/gen-policy-bootstrap.py`, then `python scripts/check-generated.py --refresh` on it; S24).

Evidence: `test_ceridc_policy.cpp` (generation 1's rules and dates decoded from signed bytes; the hybrid pair's
signatures judged current, in grace, expired, not yet issued and rolled back; a missing and a forged signature; a
one-key trust set; a source mismatch; every source, trust set and assembly refusal with its key and line; the real
binary's bytes; stored keys made, reported again and refused under another algorithm, placement or protector, signing
from the source and from the body file, joined and verified; the real binary's ceremony under the operating system's
protector), `test_register_ids.cpp` (every name present and distinct).

<a id="policy-start"></a>
**S23. A store's start: the persisted generation, then the bootstrap** (NET.2, threats T04 and T19; 2026-11-03).

- **Two inputs.** An installation saves the newest generation it accepted beside its floor; every build embeds a
  bootstrap generation (the newest one issued when it was built). `PolicyStore::start` applies the persisted one
  first, then the bootstrap, and reports each answer (`PolicyStart`; `Absent` for one it does not hold).
- **Why that order.** A bootstrap older than the persisted generation is refused as a rollback and the same one is
  already current, both harmless; a newer one supersedes it. A floor above the bootstrap therefore keeps the
  persisted generation instead of leaving an empty store, and a lost or damaged persisted file falls back to the
  bootstrap when the floor allows it.
- **Fail closed.** When neither is usable (the persisted one expired past its grace, the bootstrap below the floor)
  the store stays empty and refuses every algorithm until a newer generation is applied: freshness is never traded
  for availability.

Evidence: `test_policy.cpp` "a store starts from the persisted generation, then the bootstrap" (a fresh install, a
floor above the bootstrap, the same generation twice, a newer bootstrap, a damaged persisted file, neither usable and
nothing held).

<a id="policy-state"></a>
**S24. An installation's saved state and the build's bootstrap** (NET.2, threats T04 and T19; 2026-11-03).

- **The bootstrap** is generated, never hand-written: `scripts/gen-policy-bootstrap.py` embeds the trust set
  (`trust-set.toml`) and the newest `generation-<N>.policy` in `assets/crypto-policy/` as constexpr data
  (`policy_bootstrap()`, [policy_state.hpp](../../engine/foundation/crypto/include/crd/crypto/policy_state.hpp)); the
  `crd-crypto-policy-bootstrap-drift` CTest and `check-generated.py` hold the header to its inputs. Both stay empty
  until the issue ceremony (S22) commits them, and a build without a trust set refuses every algorithm (fail closed).
  The generator copies bytes and computes key identifiers only; crd-crypto's decoder stays the one reader (S22), and
  `crd-crypto-tests` verifies every embedded generation under its trust set, the populated shape every lane through a
  test trust set and generation 1 that `ceridc policy key`, `sign` and `assemble` made.
- **The saved state** is one durable file in the installation's private directory (`policy-state`,
  [durable_file.hpp](../../engine/foundation/platform/include/crd/platform/durable_file.hpp): checked, atomic, one
  older save kept): format 1, the floor, the newest accepted generation. `PolicyInstallation::open` raises the store's
  floor to the saved one (`PolicyStore::raise_floor`), then starts it from the saved generation and the bootstrap
  (S23) and saves when the generation or the floor changed or the newest save was damaged. So a saved floor outlives
  an expired generation: an older bootstrap cannot roll the installation back.
- **Several processes** of one installation share it: every read and save runs under the directory's lock, and
  `apply` first catches up with the floor and generation another process saved.
- **Damage and failure.** A damaged newest save falls back to the older one (a bounded rollback within the
  installation's own history); every save damaged, or a payload this build does not read, is read as none and the
  bootstrap is saved over it. Write access to the installation's own directory is outside T19's boundary (it can
  delete the file, which is a fresh install): what it can reach is the bootstrap, a signed generation within its own
  dates. A save that fails keeps the adopted generation for the run and is reported (`Unsaved`); a restart starts from
  the older save until the update channel delivers the generation again.
- **Placement:** crd-crypto, beside `PolicyStore` and the key store's durable-file pattern; `PolicyStore` itself stays
  free of I/O, so `ceridc policy verify` judges with the same code. Sessions take the installation's store when 4.2a
  brings crd-net its transport.

Evidence: `test_policy.cpp` (a raised floor; an installation saved, restarted and superseded by a newer bootstrap; two
processes sharing a floor; a saved floor outliving an expired generation; the older save recovered, every save
damaged, four unreadable payloads and a floor-only one; no directory, a file in its place and a state file that cannot
be replaced; the test bootstrap verified under the provider and a forged signature refused; the build's empty
bootstrap refusing everything), `test-repository-tools.py` `PolicyBootstrapHeaders` (the empty, trust-only and newest
generation shapes, drift and every refused input).

<a id="policy-layers"></a>
**S25. A product's policy layer over Cerid's** (NET.2, threats T04 and T19; user decision 2026-11-03, option C: "We
must grant flexibility without making our system more complicated").

- **Two layers, one mechanism.** Cerid's signed generation (its trust set, S22) is the floor of every product built on
  Cerid. A product (a studio's game, app or server) may add its own generations under its own trust set, made with the
  same `ceridc policy key`, `sign` and `assemble`; a product without one runs on Cerid's store alone. The product's
  store is an ordinary `PolicyStore` whose `PolicyConfig::base` names Cerid's store, and its `PolicyInstallation`
  saves its own state in its own directory: no second format, reader or tool.
- **Only tightening.** An algorithm is usable only while both layers accept it at that time in that scope (each layer
  resolving its own scope rule over its Everywhere rule), so the earlier refusal date wins; among what both accept the
  product's status decides (its own preference order); a version runs only when both layers' minimums allow it. A
  product rule that would accept what Cerid's layer refuses changes nothing and is reported
  (`PolicySnapshot::loosenings`, `ceridc policy verify --base-generation --base-trust`).
- **Each layer keeps its own** trust set, floor, rollback protection, expiry and grace. A layered snapshot's freshness
  is the worse layer's; either layer without a generation, or expired past its grace, refuses everything. Two layers
  at most: a base that has a base is a bad config (and a cycle of stores is never followed).
- **Sessions read the composed answer only.** `PolicyStore::snapshot()` of a product store composes both layers, so
  TLS, QUIC, ECH, MLS and capabilities change no line; a TLS session records both generations
  (`policy_generation`, `base_policy_generation`). Cerid's own key ceremony (S22) is a one-time owner action before
  the first release and does not hold NET.2's Done; tests use test keys.

Evidence: `test_policy_layers.cpp` (every tightening, every attempted loosening and its report, scopes and minimums,
either layer missing or expired, separate trust sets, floors and rollbacks, a remote disable through the base, three
layers refused, a product installation's own saved state), `fuzz_policy.cpp` mode 2 (both layers' generation
sequences against a model of the composition), `test_tls_policy.cpp` (a session under a product layer) and
`test_ceridc_policy.cpp` (the layered verify and its refusals).

<a id="path-privacy"></a>
## Paths, metadata privacy and faulty peers

- **LEO and mobile paths**: scheduled handovers (the Starlink 15-second cycle) and cellular handover are expected
  events, not loss signals; NET-PATH's congestion response and migration handle them, and NET-DEBUG replays recorded
  traces.
- **IPv6-only networks** are first-class: IPv6 first with IPv4 fallback, NAT64 and DNS64, 1280-byte MTU floor (T17).
- **Metadata privacy**: ECH (RFC 9849) on public endpoints; channel padding policies (none, bucketed sizes, constant
  rate for high-privacy calls) declared per channel and paid for in measured bandwidth; connection ID rotation on
  migration; keyed pseudonyms for session and peer identities in crash packs (T15).
- **Faulty and compromised peers**: every receiver treats peer state as untrusted input. Servers validate
  transitions; a plaintext integrity hash taken at serialisation and verified after decryption catches a sender's
  own memory fault that the AEAD tag cannot see (HW-RESILIENCE.1); lockstep and rollback exchange state hashes; a
  mismatch yields a typed error and a named culprit, never silent divergence (principle h).
- **Relays** are abuse targets: allocation is authenticated, permissions cover only the session's peers, quotas and
  accounting are per allocation, and Cerid relays sit behind DDoS fronting (T13).
- **CRA reporting duty**: the crash pool flags clusters with security impact and drafts the Article 14 reports for
  human submission; the legal side is COMPLY.3's (T23).

<a id="decisions"></a>
## Decisions

<a id="d1"></a>**D1.** `crd-net` is the one owner of sockets, transport, sessions, channels, admission, conditioning,
NAT traversal, relays' client side and capture. No other module opens a socket.

<a id="d2"></a>**D2.** Cryptography has its own foundation owner, `crd-crypto`, because its consumers are wider than
the network: SUPPLY signing, policy and rule generations, crash pack protection and the key store. `crd-net`
depends on it; it depends on no network code. NET-STD created the module with the algorithm identifiers, the
signature verification interface and the policy generations (2026-10-31); NET.2 adds the providers behind that
interface.

<a id="d3"></a>**D3.** Game and project traffic are separate session types with separate protocol identifiers, key
labels, schemas and authorisation contexts; only transport, admission and cryptography are shared.

<a id="d4"></a>**D4.** Every path that can reach a non-loopback address is authenticated and encrypted in every build;
plaintext exists only in NET-SIM's in-process virtual sockets.

<a id="d5"></a>**D5.** Capabilities are signed, expiring, attenuating tokens bound to their session; authentication
and authorisation are separate stages; 0-RTT carries only schema-declared idempotent operations.

<a id="d6"></a>**D6.** Admission is the eight stages above in that order; their numeric bounds are measured
configuration published by NET.4, not constants fixed in this design.

<a id="d7"></a>**D7.** Policy generations are monotonic and signed by offline hardware-held keys; additions are
reviewed releases, removals may be pushed at once.

<a id="hand-off"></a>
## Hand-off

| Clause | Implemented and proven by |
|---|---|
| Standards register ([contract](registers.md)), negotiation, GREASE, algorithm identifiers, the verification interface, policy generation mechanism ([wire](#policy-wire)) | [NET-STD](../ROADMAP.md#slice-net-std) |
| Legal register entries for every networked flow | [COMPLY.0](../ROADMAP.md#slice-comply.0) |
| Cryptography inventory and export classification | [COMPLY.1](../ROADMAP.md#slice-comply.1) |
| `crd-crypto` providers, the first policy generation and its offline keys, handshakes, capabilities, keys, MLS, ECH, the verified model | [NET.2](../ROADMAP.md#slice-net.2) |
| Network math (FEC, estimators, quantisation) | [HESAP-NET](../ROADMAP.md#slice-hesap-net) |
| Message schemas' authorization (the signed schema, the operation verdict and early data bound to the schema, [S18](#message-schemas)) | [NET.2](../ROADMAP.md#slice-net.2) |
| [Schema wire formats](#wire-formats) generated per message, admission stages S6 and S7 calling S18's verdict, padding policies | [NET.3](../ROADMAP.md#slice-net.3) |
| Adversarial and load evidence, fuzzing, interop, admission budgets | [NET.4](../ROADMAP.md#slice-net.4) |
| `crd-net` transport, paths, browser-facing server, stages S0 to S5, the QUIC bit greased (RFC 9287) and compatible version negotiation (RFC 9368) | [4.2a](../ROADMAP.md#slice-4.2a) |
| PMTU, ECN, pacing, congestion control, migration, offloads | [NET-PATH](../ROADMAP.md#slice-net-path) |
| ICE, STUN, TURN, MASQUE, relays, IPv6-only and NAT64 | [NET-REACH](../ROADMAP.md#slice-net-reach) |
| Determinism substrate | [4.2b](../ROADMAP.md#slice-4.2b) |
| Seeded fault simulation of every networked component | [NET-SIM](../ROADMAP.md#slice-net-sim) |
| Conditioning, capture, record and replay, packet inspector | [NET-DEBUG](../ROADMAP.md#slice-net-debug) |
| Windows and Linux qualification and deployment | [NET.5](../ROADMAP.md#slice-net.5) |
| Privacy of packs and telemetry | [COMPLY.2](../ROADMAP.md#slice-comply.2) |
| Network context in reproduction packs, the crash pool | [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1), [CRASH-FIX.2](../ROADMAP.md#slice-crash-fix.2) |
| Browser matrix | [WEB.5](../ROADMAP.md#slice-web.5) |
