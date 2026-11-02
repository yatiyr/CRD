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
- **Randomness** comes only through `crd-crypto`: the provider's DRBG seeded from the operating system
  (BCryptGenRandom, getrandom) with CPU jitter as a second source ([P2](#provider)).
- **Keys.** Long-term keys live in hardware where available (TPM 2.0, Secure Enclave, Android Keystore, YubiKey-class
  keys), with a software fallback protected by the platform (DPAPI, the Keychain, the Secret Service). Keys and secrets
  are Restricted in every diagnostic record, zeroed on release and never logged.
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
  and exported link-only beside `Cerid::crypto`. BoringSSL (no releases), OpenSSL 3.5 (a Perl-driven build) and the
  verified libraries (HACL*, libcrux: no TLS stack) are the alternatives the provider interface keeps open.
- **P2. Randomness** is the provider's NIST SP 800-90A CTR_DRBG, seeded from the operating system's source with CPU
  jitter entropy as a second independent source and reseeded on fork; no other generator exists in the engine.
- **P3. Assembly on every lane.** AWS-LC's own assembly (AES-NI, VAES, AVX2 and AVX-512 paths, ARMv8 crypto) runs on
  Linux and Windows x64; Windows x64 assembles with the pinned NASM, acquired by the configure like every package.
  Its Windows ARM64 assembly needs clang's assembler, so `cl.exe` ARM64 builds take the provider's portable C until
  that lane assembles with clang (a named remainder of NET.2). Its above-floor code runs only behind its own CPU
  capability checks, which the ISA isolation guard reviews ([ADR-0134](../decisions/0134-runtime-cpu-dispatch.md)).
- **P4. Encodings.** ML-DSA keys and signatures as FIPS 204 encodes them, signed purely with an empty context and
  hedged; Ed25519 as RFC 8032; ECDSA public keys as uncompressed SEC 1 points and signatures as strict DER
  Ecdsa-Sig-Value (the TLS 1.3 and X.509 forms; BER and malleated forms are refused); X25519 as RFC 7748 with an
  all-zero secret refused (`WeakSecret`); EC agreement peers and ECDSA keys as uncompressed points on the curve (the
  invalid-curve defence; the form byte is checked by Cerid, because AWS-LC also decodes the hybrid form 0x06/0x07 at
  that length). On a forged opening the provider zeroes the output (`EVP_AEAD_CTX_open` clears it on every failure,
  and the tests hold any provider to it). Seeds: ML-DSA's 32-byte xi, Ed25519's private key, the ECDSA scalar in
  [1, n - 1].
- **P5. Symbols.** The package exports AWS-LC's libcrypto with its own symbol names, so a consumer that also links
  OpenSSL or BoringSSL sees duplicate symbols; prefixing them (`BORINGSSL_PREFIX`) is a named remainder of NET.2.
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

The oracle is Project Wycheproof (C2SP/wycheproof, pinned as a test package and read at run time): every AES-GCM,
ChaCha20-Poly1305, HKDF, X25519, ECDH P-256 and P-384, Ed25519, ECDSA and ML-DSA vector (keygen from seed, verify,
sign-then-verify), every ML-KEM-768 and ML-KEM-1024 vector (keys from seeds, decapsulation with implicit rejection,
encapsulation with the modulus check) and every SipHash-2-4 vector, with RFC 8032's deterministic signatures, FIPS
180-4's SHA-256 example and the refusal of every vector outside Cerid's profile (other nonce, tag and key lengths,
compressed points, context strings, expanded ML-KEM decapsulation keys).

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
  `Forged`; every refusal zeroes the outputs. DeriveKeyPair takes at least Nsk bytes.
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
are refused. The suites' hash is their KDF's (`kdf_hash`), as is the MAC's. The post-quantum suites (an ML-KEM or
X-Wing KEM with ML-DSA signatures, draft-ietf-mls-pq-ciphersuites) join when the register pins their draft and HPKE
carries the KEM.

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
    c. Next: GREASE (section 13.5) and the post-quantum suites, with live OpenMLS and MLSpp interop (M4).

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
key must be one every agreement can use (`check_agreement_public_key`: an uncompressed point on the curve, an X25519
key not of small order), checked once when it enters the group, since every later commit encrypts to it and a key
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
| Body (signed) | magic `CRDPOLCY` (8 bytes), format (u8), generation (u64), issued, expires (u64 Unix seconds), offline grace (u32 seconds), minimum count (u8) and each protocol, major and minor (u16, u16, u16), rule count (u8) and each algorithm (u16) with accepted-from, preferred-from, preferred-until and refused-from (u64 each) |
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
| Schema wire formats generated per message, admission stages S6 and S7 calling S18's verdict, padding policies | [NET.3](../ROADMAP.md#slice-net.3) |
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
