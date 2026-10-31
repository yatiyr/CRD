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

Two capabilities have no owner yet and this design assigns them (decisions [D1](#d1) and [D2](#d2)): `crd-net` owns
sockets, transport, sessions, channels, admission, conditioning, NAT traversal and capture; `crd-crypto` owns the
cryptographic provider interface, the key store, the CSPRNG and keyed pseudorandom functions. Both need OWN.1
registry entries when their modules land.

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
| T03 | Post-quantum handshakes exceed the amplification limit (V1, V7) | Certificate compression (RFC 8879), raw public keys or pinned Cerid credentials for game and session servers, resumption, Retry; measured in round trips | NET.2 |
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
  issued under and the session it is bound to (an exporter-derived binding, so a stolen token is useless in another
  session). Capabilities only attenuate: a holder can derive a narrower one, never a wider one.
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
  UDP path serves LAN and headless simulation with its own reliability, but every path that can reach a
  non-loopback address is authenticated and encrypted with the same session keys; there is no plaintext mode in any
  build. NET-SIM's virtual sockets never leave the process.
- **One session model over every path.** Sessions, channels, capabilities and schemas are identical across paths;
  a session migrates between paths (QUIC migration, or a resumption on the fallback path) without the application
  seeing anything but a latency change.
- **Memory.** Connection state comes from fixed-size pools sized by configuration; steady-state packet handling makes
  no heap allocation. Reassembly and pending buffers are counted against the principal's quota.
- **Path behaviour** (PMTU, ECN, pacing, congestion control, offloads, migration policy) belongs to NET-PATH;
  reachability (ICE, STUN, TURN, MASQUE, Cerid relays) to NET-REACH.

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
- **Randomness** comes only from the operating system (BCryptGenRandom, getrandom) through `crd-crypto`.
- **Keys.** Long-term keys live in hardware where available (TPM 2.0, Secure Enclave, Android Keystore, YubiKey-class
  keys), with a software fallback protected by the platform (DPAPI, the Keychain, the Secret Service). Keys and secrets
  are Restricted in every diagnostic record, zeroed on release and never logged.
- **Debug keys never ship.** RFC 9850 key logging and NET-DEBUG capture keys exist only in non-shipping builds and
  are refused at runtime when built in; a CI guard proves it (T22).
- **Export control** of everything above is [COMPLY.1](../ROADMAP.md#slice-comply.1)'s: its inventory is generated
  from the provider registry and the NET-STD algorithm entries.

<a id="negotiation"></a>
## Version and extension negotiation

Every Cerid wire protocol (session control, game, project, media control, crash upload, agent, service and relay
allocation) carries in its handshake:

- a protocol identifier and a version (major: incompatible; minor: compatible additions);
- an extension list of (identifier, length, value) entries; an unknown extension is ignored, never fatal;
- GREASE identifiers from a reserved pattern, sent by every build including shipping (RFC 8701 practice), with the
  QUIC bit greased per RFC 9287 and compatible version negotiation per RFC 9368;
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
  NIST IR 8547 once final; NET.2 sets the dates.
- RFC 7696 is the checklist; NET-STD records each item's evidence.

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
depends on it; it depends on no network code.

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
| Standards register ([contract](registers.md)), negotiation, GREASE, algorithm identifiers, policy generation mechanism | [NET-STD](../ROADMAP.md#slice-net-std) |
| Legal register entries for every networked flow | [COMPLY.0](../ROADMAP.md#slice-comply.0) |
| Cryptography inventory and export classification | [COMPLY.1](../ROADMAP.md#slice-comply.1) |
| `crd-crypto`, handshakes, capabilities, keys, MLS, ECH, the verified model | [NET.2](../ROADMAP.md#slice-net.2) |
| Network math (FEC, estimators, quantisation) | [HESAP-NET](../ROADMAP.md#slice-hesap-net) |
| Schemas, wire format, admission stages S6 and S7, padding policies | [NET.3](../ROADMAP.md#slice-net.3) |
| Adversarial and load evidence, fuzzing, interop, admission budgets | [NET.4](../ROADMAP.md#slice-net.4) |
| `crd-net` transport, paths, browser-facing server, stages S0 to S5 | [4.2a](../ROADMAP.md#slice-4.2a) |
| PMTU, ECN, pacing, congestion control, migration, offloads | [NET-PATH](../ROADMAP.md#slice-net-path) |
| ICE, STUN, TURN, MASQUE, relays, IPv6-only and NAT64 | [NET-REACH](../ROADMAP.md#slice-net-reach) |
| Determinism substrate | [4.2b](../ROADMAP.md#slice-4.2b) |
| Seeded fault simulation of every networked component | [NET-SIM](../ROADMAP.md#slice-net-sim) |
| Conditioning, capture, record and replay, packet inspector | [NET-DEBUG](../ROADMAP.md#slice-net-debug) |
| Windows and Linux qualification and deployment | [NET.5](../ROADMAP.md#slice-net.5) |
| Privacy of packs and telemetry | [COMPLY.2](../ROADMAP.md#slice-comply.2) |
| Network context in reproduction packs, the crash pool | [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1), [CRASH-FIX.2](../ROADMAP.md#slice-crash-fix.2) |
| Browser matrix | [WEB.5](../ROADMAP.md#slice-web.5) |
