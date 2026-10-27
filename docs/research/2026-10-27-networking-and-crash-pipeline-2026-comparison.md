# Cerid networking and the crash-to-fix pipeline: the 2026 state of the art

<!-- doc-role: reference -->
> Technical reference and proposals; verify dated claims against current contracts/source. Live slice state exists
> only in [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md). Agent entry: [START_HERE](../../START_HERE.md).

**Date:** 2026-10-27. **Status:** Recorded research. Every proposed row below is a proposal for the user, not an
accepted change. Nothing in [ROADMAP](../ROADMAP.md) was edited by this research.

## Question, method and evidence boundary

What do the best protocols, libraries, engines, services and papers ship in 2026 for networking (transport, security,
game netcode, large worlds, real-time media, collaboration, testing, operations, browsers and specialised links) and for
the path from a crash on a user's machine to a reviewed fix, how do Cerid's rows compare, and what must change so that
Cerid misses nothing today and stays current for decades? The bar is the user's standing order: the proper, complete,
world-class solution, elegant and not bloated, never the "simple" or "modern only" option.

- **Scope against earlier research.** [Part 2, area 7](2026-10-26-beyond-games-2026-comparison.md#area-7-networking)
  surveyed netcode models, transports, anti-cheat, fleets and CRDTs in one page. Its proposals were adopted on
  2026-10-27 while this research ran ([Part 2 adoption](../sessions/2026-10-27-research-adopt-part-2.md)): 4.2c,
  COLLAB.1, 4.2e and NET carry research amendments, and [NET-SERVER](../ROADMAP.md#slice-net-server) and
  [ANTICHEAT](../ROADMAP.md#slice-anticheat) are rows. This document builds on those rows instead of proposing them
  again. Part 3's SEC.1, SUPPLY.1-4 and its AGENT.3 guardrail amendment, not yet rows, are marked "(P3)"
  ([Part 3](2026-10-26-building-with-cerid-2026-comparison.md#gaps-and-proposed-roadmap-changes)).
  GPU crash dumps belong to [GPU-FORENSICS](../ROADMAP.md#slice-gpu-forensics) (Part 1) and are only linked here.
- **Cerid side.** Read, not run: the [ROADMAP](../ROADMAP.md) rows NET.1-NET.5, 4.2a-4.2e, NET-SIM, NET-DEBUG,
  NETCRASH, NET-SCALE, NET-LIVE, NET, COLLAB.1-5, CRASH-FIX.1-4, OPS.1-3, AGENT.3, WEB.5, HW-RESILIENCE.1-4 and DIAG.5,
  DIAG.9 as of 2026-10-27; the two user-decision sessions of 2026-10-27
  ([network foundation](../sessions/2026-10-27-user-decision-network-foundation-window.md),
  [crash to fix](../sessions/2026-10-27-user-decision-crash-to-fix-pipeline.md));
  [ADR-0035](../decisions/0035-networking-architecture.md), [ADR-0063](../decisions/0063-eylem-determinism-contract.md),
  [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md) and
  [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md); and greps of `engine/` on 2026-10-27. A grep
  hit is a source fact, not a runtime or quality claim. No build, test, benchmark or CI ran for this document.
- **Outside side.** Web searches and fetched pages read on 2026-10-27. Sources are listed at the end as N-numbers
  (Parts 1-3 use W, B and C). **(K)** marks a source given from prior knowledge and not re-read today; its current
  version or status is uncertain. "Uncertain" means the sources disagree or were secondary only.
- **No downloads** were made and no software ran. Comparisons that need one are in
  [Needs a download](#needs-a-download).
- **Order numbers** below are the ROADMAP's first column as of 2026-10-27 (OWN 110, NET.1 111 to NETCRASH 122,
  AUD-2 123, AGENT.3 301, CRASH-FIX.3 394, OPS.1 530, WEB.5 627, 4.2c 856, NET-SERVER 858, ANTICHEAT 859 to
  COLLAB.2 865), read after the Part 2 adoption renumbered the table to 973 rows. Another agent edits the ROADMAP
  concurrently; placement is by anchor, the number is a hint.

## The principles every area is judged by

The first six are those of Parts 1-3; (g) and (h) come from ADR-0063, ADR-0133 and the 2026-10-26 hardware decision.

- **(a) Runtime changeability.** Replication rules, server logic, conditioning profiles, congestion-control parameters,
  matchmaking rules and fault schedules are authored in CHIR/CEIR and swapped live between ticks with a version
  handshake, staged commit and last-good generation. Security policy changes only through signed, authorised
  generations; a live swap never weakens authentication.
- **(b) crd-hesap is the computational base.** Erasure-code and FEC math, bandwidth and jitter estimation, clock-offset
  filters, interpolation and extrapolation for lag compensation and dead reckoning use crd-hesap. Hashing,
  checksums and general compression belong to their foundation modules ([OWN.1](../ROADMAP.md#slice-own.1)).
- **(c) CHIR is the Cerid language** for commands, bots, scripted network timelines and agent actions.
- **(d) Strict ownership.** `crd-net` owns sockets, transport, sessions, conditioning, NAT traversal and capture;
  codecs belong to the media rows, crypto primitives to vetted providers behind an owned interface (ADR-0130 item 3),
  replay to [4.2b](../ROADMAP.md#slice-4.2b) and DIAG. Nothing duplicates them.
- **(e) Wide hardware.** SSE2 x64 floor with runtime dispatch (checksums, AEAD, FEC kernels dispatch by level), older
  NICs and kernels without offloads, Windows/Linux first, macOS, mobile and web next.
- **(f) Budgets from peers.** Beat the best peer (msquic/quiche throughput per core, GGPO rollback cost, Iris bandwidth
  per player, LiveKit capacity per node, FoundationDB-class simulation coverage); parity only with recorded evidence.
- **(g) Determinism and replay.** ADR-0063 determinism; every networked failure replays from a seed or a recorded
  session through DIAG.
- **(h) Graceful on faulty hardware and hostile networks.** A bad NIC, a corrupting router, a bit flip before
  encryption or a lying peer yields a typed error, a retry or a named culprit, never a crash or silent bad state.

<a id="highest-impact-gaps"></a>
## Executive summary: the most important gaps

Cerid's networking is entirely plan today: no `crd-net` exists in source. The plan is unusually strong in intent (a
NETCRASH window before AUD-2, simulation testing as a gate, built-in conditioning, post-quantum hybrids, MLS, formal
verification, guarded fix agents). What it lacks is the layer of specifics that decides whether such a system stays
correct and current for decades. Ranked by how much later work each gap blocks:

| Rank | Gap | Cerid today | Proposal |
|---:|---|---|---|
| 1 | No standards register, negotiation or GREASE contract; no re-research cadence | Missing | NET-STD (new) |
| 2 | Path layer unspecified: PMTUD, ECN, pacing, congestion control, offloads | 4.2a clause | NET-PATH (new) |
| 3 | NAT traversal only in Later NET-LIVE; P2P games, DAW, collab need it | NET-LIVE | NET-REACH (new) |
| 4 | PQ pins: RFC 10024, RFC 9881, MTC; PQ certs against the amplification limit | NET.2 intent | NET.2 amend |
| 5 | Network math has no hesap owner; FEC is Hamming(7,4) only | `hesap-comms` | HESAP-NET (new) |
| 6 | NET-SIM lacks storage faults, fault points, swarm tests, history checkers | NET-SIM | NET-SIM amend |
| 7 | NET-DEBUG lacks trace links, AQM/ECN, NAT types, IPv6-only, MTU blackholes | NET-DEBUG | NET-DEBUG amend |
| 8 | No interop runner, differential tests or state learning against QUIC peers | NET.4 fuzzing | NET.4 amend |
| 9 | Clock sync (NTP/PTP-class) unowned; tick sync, music, VPROD need it | Missing | NET-TIME (new) |
| 10 | Crash pool: no CRA Article 14 workflow, pseudonymisation or grouping spec | Missing | CRASH-FIX.2 amend |
| 11 | Crash packs lack network context and client-server trace correlation | Missing | CRASH-FIX.1, OPS.1 |
| 12 | Fix agent lacks behavioural diff, fuzz re-run, independent critic | Partial | CRASH-FIX.3/.4 amend |
| 13 | Media: SCReAM v2/L4S, Opus DRED, SFrame keys, WHIP/WHEP, MoQ, cascading | Clauses | NET-LIVE amend |
| 14 | Browser server side: WebTransport endpoint, HTTP/2 fallback, draft churn | WEB.5 | 4.2a, WEB.5 amend |
| 15 | Large worlds: overload mode, authority leases, event-sourced persistence | NET-SCALE | NET-SCALE amend |

Next in impact: kernel-free anti-cheat with attestation (ANTICHEAT amendment), fleets with QUIC-LB, Agones and
Open Match 2 (NET-SERVER amendment), the meaning of HW-RESILIENCE.1's packet checksum under AEAD, CRC32 copies in
five modules, and crash-data prompt-injection corpus items for AGENT.3.

<a id="cerid-today"></a>
## Cerid today: what the networking and crash rows build on

Stated once so the comparisons are read fairly.

- **No `crd-net` in source** (no socket, Winsock or `sendto` hit under `engine/`). Every networking cell below is a
  plan, not shipped capability.
- **hesap-comms** (`engine/numerics/hesap-comms`) is a physical-layer kit: carrier and timing recovery, modulation,
  OFDM, equalizer, pulse shaping, framing with a preamble detector and Hamming(7,4) as "the hook for stronger codes",
  and `channel.hpp` channel models (AWGN, Rayleigh and Rician fading, carrier offset) on a seeded Philox stream. It has
  no packet erasure code (Reed-Solomon, RaptorQ, XOR parity) and no network estimators. The seeded channel models are
  the right owner pattern for NET-DEBUG's loss models.
- **Checksums and hashing.** `containers/hash.hpp` has FNV-1a only; CRC32 is implemented separately in `perf` bundles,
  `resources` PNG and ZIP code and `hesap-tensor` ZIP I/O. HW-RESILIENCE.1 needs one owned, dispatched checksum family.
- **Diagnostics are strong and done.** [DIAG.5a](../ROADMAP.md#slice-diag.5a) and DIAG.5b (Windows and Linux crash
  capture), [DIAG.5d](../ROADMAP.md#slice-diag.5d) (versioned, checksummed, atomically published bundles with exact
  RSDS symbol identities and unloaded-module generations), [DIAG.9a](../ROADMAP.md#slice-diag.9a) (run records and
  replay), [DIAG.9b](../ROADMAP.md#slice-diag.9b) (flight recorder, fault injection) and
  [DIAG.9c](../ROADMAP.md#slice-diag.9c) (minimisation, observer-effect checks, rr/TTD routes). This is the base for
  CRASH-FIX.1 and NET-DEBUG capture, and it is better than most engines ship.
- **Determinism contract.** ADR-0063 and ADR-0035's layered design (transport, determinism substrate, sync, sessions)
  match industry practice; ADR-0035's "mode is a per-session configuration, not a compile-time choice" already embodies
  principle (a).

<a id="area-1-transport"></a>
## 1. Transport

### State of the art 2026

- **QUIC core is stable and unchanged since 2023:** RFC 8999-9002 (v1), RFC 9221 (datagrams), RFC 9287 (greasing the
  QUIC bit), RFC 9368 (compatible version negotiation), RFC 9369 (QUIC v2), RFC 9114 (HTTP/3), RFC 9204 (QPACK)
  [N83](#n83). No QUIC WG RFC was published in 2025-2026; the activity is in extensions.
- **Extensions in flight (QUIC WG, October 2026)** [N83](#n83): multipath QUIC (-21) in the RFC Editor queue (RFC Editor
  state "In Progress" since 2026-05-20; the WG page shows it awaiting final approval) [N01](#n01); reliable stream
  resets with partial delivery (RESET_STREAM_AT, -11) in the RFC Editor queue, a dependency of WebTransport [N02](#n02);
  ACK frequency (-14) has consensus but expired waiting for write-up; QUIC address discovery (-01, August 2026) for
  reflexive addresses per path [N06](#n06); QUIC-LB routable connection IDs (-22, 2026-10-23); extended key update, QMux
  and receive timestamps as WG documents. NAT traversal over QUIC is not a WG document: n0's iroh documents its
  `REACH_OUT`-based scheme as an informational individual draft (July 2026) built on Seemann's earlier draft and on
  address discovery and multipath [N03](#n03).
- **MASQUE.** CONNECT-UDP (RFC 9298) and CONNECT-IP (RFC 9484) (K) [N84](#n84); "Proxying bound UDP in HTTP"
  (connect-udp-listen, -16, August 2026) extends RFC 9298 to peer-to-peer UDP and was at IESG telechat in August 2026
  [N10](#n10). It makes an HTTP/3 proxy a standards-based TURN alternative.
- **Congestion control.** CUBIC remains the default almost everywhere (K). BBRv3 is still out of tree in Linux (Google's
  v3 branch, forward-ported by distributions); mainline has BBR v1 [N04](#n04). L4S (RFC 9330 architecture, RFC 9331
  ECN protocol, experimental) is deployed by Comcast and supported by Apple platforms; TCP Prague's coexistence results
  are mixed [N05](#n05). For real-time media, libwebrtc now carries a SCReAM v2 implementation (with documented
  deviations from the IETF text) beside Google Congestion Control (GCC) [N49](#n49).
- **Path MTU.** DPLPMTUD (RFC 8899) is the method for datagram transports; QUIC requires 1200-byte minimum and probes
  upward (K) [N85](#n85).
- **Kernel paths and offloads.** Linux 6.15 added io_uring zero-copy receive (header split and NIC queue configuration
  by the user; dmabuf and GPU-memory areas follow) [N08](#n08); UDP GSO/GRO and `MSG_ZEROCOPY` are older (K).
  Windows has USO (send segmentation, Windows 10 2004+) and URO (receive coalescing, Windows 11 24H2, NDIS 6.89)
  [N09](#n09), and
  RIO and IOCP (K); MsQuic uses these and an XDP path (K). AF_XDP and DPDK are the Linux kernel-bypass options (K).
- **Mobile and satellite.** QUIC connection migration and NAT rebinding are in RFC 9000 (K). Starlink shows a
  deterministic 15-second reconfiguration cycle with latency spikes of about 140 ms and 75 ms at its boundaries
  [N11](#n11); median latency fell to about 80 ms in 2024 with a bimodal distribution and loss above 10% in some
  countries [N12](#n12); handover-aware QUIC (StarQUIC) completes transfers up to 35% faster by not backing off at
  scheduled handovers [N13](#n13).
- **Reference implementations.** msquic, quiche, ngtcp2, quinn, picoquic, s2n-quic, lsquic (K) [N86](#n86); the QUIC
  interop runner tests them pairwise (K) [N87](#n87).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| QUIC v1/v2, datagrams, HTTP/3 | RFC 9000/9369/9221/9114 [N83](#n83) | Planned: [4.2a](../ROADMAP.md#slice-4.2a) |
| Raw UDP and TCP/TLS paths | GameNetworkingSockets, ENet (K) | Planned: 4.2a |
| Multipath, address discovery, reliable reset | QUIC WG drafts [N01](#n01) [N06](#n06) | Missing |
| Congestion control incl. L4S, BBRv3, delay-based | CUBIC, BBRv3, Prague, SCReAM v2 [N49](#n49) | NET.3 word |
| DPLPMTUD, ECN validation, pacing | RFC 8899, RFC 9000 (K) | NET.3 says "MTU" |
| Offloads and kernel paths | io_uring ZC Rx, GSO/GRO, USO/URO, RIO, XDP [N08](#n08) [N09](#n09) | Missing |
| Connection migration, NAT rebinding, handover | RFC 9000; StarQUIC [N13](#n13) | NET-DEBUG emulates rebinding only |
| MASQUE proxying incl. bound UDP | RFC 9298, connect-udp-listen [N10](#n10) | Missing |

### Gaps in area 1

- **A path layer.** DPLPMTUD with 1280-byte IPv6 floors, ECN marking and validation (RFC 9000 section 13.4), pacing,
  GSO/GRO/USO/URO and io_uring/RIO fast paths with a plain `sendmsg`/`WSASendTo` fallback on every kernel, connection
  migration and handover handling (scheduled-handover awareness for LEO), and a pluggable congestion-control interface
  (NewReno, CUBIC, BBRv3, Prague for L4S, and delay-based real-time controllers shared with NET-LIVE). Each controller
  is an authored, versioned policy swapped per connection (principle a), its estimators in hesap (principle b).
- **Extension tracking.** Multipath, address discovery, ACK frequency, receive timestamps, reliable reset and QUIC-LB
  are drafts or queued RFCs; each needs a pinned revision, a negotiation flag and an upgrade path, which no row owns.
- **Peers as budgets.** Handshakes per second, throughput per core, p99 added latency under 1% loss, memory per
  connection, measured against msquic and quiche in the same harness (principle f).

<a id="area-2-security"></a>
## 2. Security

### State of the art 2026

- **Post-quantum key exchange is standardised and default.** RFC 10024 (August 2026) defines X25519MLKEM768,
  SecP256r1MLKEM768 and SecP384r1MLKEM1024 hybrids for TLS 1.3 and calls X25519MLKEM768 widely deployed [N14](#n14).
  OpenSSL 3.5 LTS (April 2025) offers X25519MLKEM768 and X25519 key shares by default; OpenSSL 4.0 (2026-04-14) removed
  engines and SSLv3 and added ECH [N23](#n23).
- **Post-quantum signatures are not yet in the Web PKI.** RFC 9881 (October 2025) specifies ML-DSA in X.509
  [N16](#n16). Because an ML-DSA-44 signature is about 2.4 KB against 64 bytes for P-256, Let's Encrypt and Chrome
  favour Merkle Tree Certificates: Let's Encrypt plans staging in late 2026 and production in 2027 (June 2026 post), the
  IETF PLANTS draft is at -00 [N17](#n17) [N18](#n18). A Chrome quantum-resistant root store around Q3 2027 is reported
  by secondary sources only (uncertain).
- **Deprecation timeline.** NIST IR 8547 (initial public draft, November 2024) deprecates 112-bit RSA and ECC after 2030
  and disallows them after 2035; it was still a draft in mid-2026 while US federal guidance already plans on its dates
  (uncertain) [N22](#n22). NSA CNSA 2.0 sets similar dates for national-security systems (K) [N88](#n88).
- **ECH** is RFC 9849 (dated March 2026 on the RFC Editor page; a mirror PDF says November 2025, uncertain)
  [N15](#n15). **SSLKEYLOGFILE** is RFC 9850 (Informational, December 2025), with ECH labels, and "MUST NOT be used in a
  production system" [N62](#n62).
- **MLS** (RFC 9420) is in production: GSMA RCS Universal Profile 3.0 (March 2025) uses MLS, RCC.16 v3.0 (February
  2026) adds encrypted video calls, and cross-platform encrypted RCS between iPhone and Android began in beta in May
  2026 (secondary) [N19](#n19); Webex encrypts conference calls with MLS (2023 claim) [N20](#n20). The MLS
  architecture is RFC 9750 (K) [N89](#n89). SFrame (RFC 9605) derives media keys from an MLS exporter [N43](#n43).
- **Verified implementations.** libcrux ML-KEM is verified with hax and F* and ships in Firefox, OpenSSH and Signal
  [N24](#n24); AWS-LC uses mlkem-native with formal correctness proofs [N25](#n25); HACL* provides verified C (K).
  Protocol models in Tamarin and ProVerif analysed TLS 1.3 before publication (K) [N90](#n90).
- **Implementations keep failing at logic, not crypto.** NDSS 2026's MerCuriuzz found 14 unknown logic flaws across 16
  QUIC implementations [N26](#n26); QUIC-Fuzz found 10 new vulnerabilities and 2 CVEs in six servers [N27](#n27);
  QUICtester's state learning found 55 errors in 19 servers including a specification ambiguity that gave an easy DoS
  [N29](#n29); OSTIF audited ngtcp2 and nghttp3 in 2025 [N28](#n28).
- **Agility.** RFC 7696 (BCP 201) sets algorithm-agility guidelines; RFC 8701 GREASE keeps extension points working by
  sending reserved values (K) [N91](#n91). RFC 8879 compresses certificate chains (K) [N92](#n92).
- **Regulation.** The EU Cyber Resilience Act's Article 14 reporting duty applies since 2026-09-11: early warning within
  24 hours of awareness of an actively exploited vulnerability, notification within 72 hours, final report within 14
  days of a fix, through ENISA's Single Reporting Platform (no API at launch); the other CRA obligations apply from
  2027-12-11 [N21](#n21).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| TLS 1.3, hybrid PQ key exchange | RFC 10024 [N14](#n14); OpenSSL [N23](#n23) | [NET.2](../ROADMAP.md#slice-net.2) |
| PQ signatures and certificate strategy | RFC 9881, MTC [N16](#n16) [N17](#n17) | NET.2 "PQ-ready signatures" |
| ECH and metadata privacy | RFC 9849 [N15](#n15) | Missing |
| Group E2EE | MLS in RCS and Webex [N19](#n19) [N20](#n20) | Planned: NET.2 |
| Formal verification | Tamarin/ProVerif models; HACL*, libcrux, mlkem-native | Planned: NET.2 model in CI |
| Logic and state fuzzing | MerCuriuzz, QUIC-Fuzz, QUICtester [N26](#n26) [N29](#n29) | NET.4 coverage fuzzing |
| Debug key logging | RFC 9850 [N62](#n62) | NET-DEBUG debug-build key logs |
| Vulnerability reporting duty | CRA Article 14 [N21](#n21) | Missing (SEC.1 (P3) proposes) |

### Gaps in area 2

- **Pin the standards.** NET.2 should name RFC 10024's groups (X25519MLKEM768 preferred, SecP384r1MLKEM1024 for
  high-assurance deployments), RFC 9881 ML-DSA for Cerid's own service certificates and code-signing chains, and track
  MTC for public endpoints, with classical-only refused after a dated policy switch tied to NIST IR 8547.
- **PQ cost inside QUIC.** Larger chains collide with QUIC's three-times anti-amplification limit before address
  validation (K); the design needs certificate compression (RFC 8879), Retry or address-validation tokens, resumption,
  and raw public keys or pinned Cerid-issued credentials for game and session servers, measured in round trips.
- **Implementation-level assurance.** The Tamarin/ProVerif model must be bound to the code: state machine generated
  from one CHIR/C++ description or checked by state learning against the model; verified primitives (HACL*, libcrux,
  aws-lc) chosen as providers, never written in-house.
- **Hardware keys and rotation.** TPM 2.0, Secure Enclave, Android Keystore and YubiKey-class keys behind the provider
  interface with software fallback (principle e); rotation and revocation rehearsed in NET-SIM.
- **Debug keys never ship.** RFC 9850 key logging and NET-DEBUG capture keys are compiled out of shipping builds and
  refused at runtime when built in, with a CI guard (mirrors [SCRIPT.4](../ROADMAP.md#slice-script.4)'s allowlists).
- **CRA readiness** for every networked product Cerid builds: the crash pool must classify security-relevant clusters
  and produce the 24-hour, 72-hour and 14-day reports (area 9).

<a id="area-3-netcode"></a>
## 3. Game netcode

### State of the art 2026

- **Models.** Rollback with input delay (GGPO, MIT since 2019, and its successors) for fighting and fast P2P games (K)
  [N93](#n93); server-authoritative prediction, reconciliation and lag compensation (Overwatch GDC 2017, Valve's
  Source networking model, Valorant's 128-tick servers) (K) [N94](#n94); priority and budget systems for many entities
  (Halo: Reach GDC 2011) (K) [N94](#n94); predicted rigid-body physics (Rocket League GDC 2018) (K) [N94](#n94);
  deterministic lockstep for RTS (K). Counter-Strike 2 keeps 64-tick servers but timestamps inputs between ticks
  ("sub-tick"); player complaints persist, and Valve's own technical description was not found (uncertain) [N95](#n95).
- **Engines.** Unreal Iris moved from experimental to beta in 5.7, compiled in but off by default, opt-in beside the
  legacy system and Replication Graph, which Epic does not plan to deprecate; Iris support for Mover and Network
  Prediction is in Epic's main branch but users on 5.8.2 still ask for compatibility (uncertain) [N30](#n30). Unity's
  Netcode for Entities has a 6.x line for Unity 6.7 (a forum post names 1.14.0 on 2026-07-23; the version labels
  disagree) and Multiplayer Play Mode runs up to four players on one device [N33](#n33) [N60](#n60). Photon's current
  Unity SDK is Fusion 2 with Fusion 3 announced as next generation; Quantum is at v3 (deterministic predict and roll
  back) [N33](#n33). Godot ships scene replication (`MultiplayerSynchronizer`, `MultiplayerSpawner`) with rollback via
  add-ons (K). Valve GameNetworkingSockets offers reliable and unreliable messages over UDP with Steam Datagram Relay
  for DDoS protection and routing (K) [N96](#n96).
- **Determinism across CPUs and compilers.** Box2D v3 and Jolt document cross-platform determinism with strict float
  rules (Part 1) (K); fixed-point (Photon Quantum) remains the conservative choice (K).
- **Anti-cheat.** Kernel anti-cheat is still the norm and is tightening: Battlefield 6 (Javelin) and Black Ops 7
  (Ricochet) require Secure Boot and TPM 2.0; Apex Legends blocks Linux and Steam Deck [N32](#n32). Microsoft's Windows
  endpoint security platform moves antivirus and EDR to user mode, and Microsoft reportedly works with game developers
  on non-kernel anti-cheat; no 2026 shipping date or vendor change was found (uncertain) [N31](#n31).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| Fixed-step substrate, input log, snapshots | GGPO, Quantum (K) | Planned: [4.2b](../ROADMAP.md#slice-4.2b) |
| Rollback, prediction, reconciliation | GGPO, Overwatch, Rocket League (K) | [4.2c](../ROADMAP.md#slice-4.2c) |
| Lag compensation, sub-tick inputs | Valve, Riot (K) [N95](#n95) | 4.2c names lag compensation |
| Relevancy, priority budgets, delta compression | Iris [N30](#n30), Halo: Reach (K) | NET.3 deltas; 4.2c policies |
| Multi-client local testing | Unity MPPM, 4 players [N60](#n60) | [NET-DEBUG](../ROADMAP.md#slice-net-debug) |
| Desync detection | Quantum checksums (K) | [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3) state hashes |
| Anti-cheat | Server authority; kernel AC [N32](#n32) | Later: [ANTICHEAT](../ROADMAP.md#slice-anticheat) |

### Gaps in area 3

- **4.2c's amended text needs the technique list** so that "selectable authored replication policies" is testable: input
  delay plus rollback with a declared maximum rollback window; sub-tick input timestamps; lag compensation with bounded
  rewind (a declared maximum), hit validation against the rewound state and favour-the-shooter limits; delta compression
  against the last acknowledged baseline per client; authored quantisation precision per property; priority accumulators
  with per-connection byte budgets; predicted physics with error smoothing; deterministic lockstep with per-tick state
  hashes naming the diverging peer (HW-RESILIENCE.3). History buffers and interpolation kernels come from `hesap-interp`
  (principle b); policies are CHIR generations swapped between ticks (principle a).
- **Cross-machine determinism evidence.** Lockstep and rollback across Windows/Linux and MSVC/Clang/GCC need the
  ADR-0063 float rules (no FMA contraction, fixed FTZ/DAZ, no `-ffast-math`) proven by a cross-lane replay corpus in
  NET-SIM, not only on one lane.
- **Anti-cheat without a Cerid kernel driver.** The ANTICHEAT row should say Cerid never ships a kernel
  component (the Windows resiliency direction and Linux/Proton both argue against it), uses server authority and
  replay-based statistical detection first, offers optional platform attestation (TPM measured boot, Play Integrity,
  App Attest) with honest degradation where absent, and keeps vendor SDK adapters optional.

<a id="area-4-large-worlds"></a>
## 4. MMO and large worlds

### State of the art 2026

- **Server meshing is still unfinished in the industry.** Star Citizen's Alpha 4.10 shipped "quasi-dynamic server
  meshing" as instancing; territory subdivision of the persistent universe has not shipped, a "Claims" system that
  assigns space to servers was in development in September 2026, internal tests asked for 200 servers, and the CTO
  reported up to 400 players in one area working better than one server per universe (secondary coverage) [N34](#n34).
- **SpatialOS lessons.** Several SpatialOS games were cancelled or sunset (Worlds Adrift 2019, Lazarus, Mavericks) and a
  2019 engine-licence dispute put every SpatialOS-Unity game at risk overnight [N38](#n38). The lesson is not that
  distributed simulation fails but that a world's authority, persistence and mesh must not depend on one external
  platform. Hadean and Improbable's later direction were not verified today (uncertain).
- **Graceful overload.** EVE Online's time dilation slows a crowded system to as little as 10% speed instead of
  dropping state, and remains the reference for honest overload [N37](#n37).
- **Persistence.** Large worlds keep an authoritative store with an event log and periodic snapshots (event sourcing),
  entity authority leases with fencing tokens so a lost server cannot write after its lease, and exactly-once handoff
  of entities between servers (K). Amazon's New World used a grid of servers with cross-server handoff on AWS (K,
  uncertain).
- **Fleets and matchmaking.** Agones v1.61.0 (24 September, year inferred as 2026) on Kubernetes 1.34-1.36 with
  Counters and Lists, fleet autoscaling fixes and the player-allocation filter removed in 1.59 [N35](#n35). Open Match 2
  is in public preview as one horizontally scaled `om-core` service with gRPC APIs [N36](#n36). QUIC-LB (-22) lets load
  balancers route by encrypted connection ID across migration [N83](#n83).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| Partitioning, handoff, re-sharding | Star Citizen QDSM [N34](#n34) | [NET-SCALE](../ROADMAP.md#slice-net-scale) |
| Overload behaviour | EVE time dilation [N37](#n37) | Missing |
| Persistent authoritative store | Event log plus snapshots, leases (K) | NET-SCALE "survives server loss" |
| Fleets, matchmaking | Agones [N35](#n35), Open Match 2 [N36](#n36) | [NET-SERVER](../ROADMAP.md#slice-net-server) |
| Connection-ID routing | QUIC-LB [N83](#n83) | Missing |

### Gaps in area 4

- **NET-SCALE acceptance needs mechanisms, not only outcomes:** authority leases with fencing, exactly-once entity
  handoff, an event-sourced world journal with snapshots, static and dynamic partitioning both supported (dynamic is
  what nobody has finished), a declared overload mode (time dilation per region with client notification) and all of
  it checked for linearizability of accepted actions under NET-SIM partitions and server kills.
- **Own the mesh.** The SpatialOS lesson argues for the mesh, persistence and authority in `crd-net` and Cerid's own
  services, with cloud and orchestration as adapters (NET-SERVER).
- **Fleet specifics for NET-SERVER:** Agones and Open Match 2 adapters, QUIC-LB connection IDs, region selection
  by measured RTT beacons, drain-and-swap server updates through live CEIR generations, relay fronting for DDoS, and a
  cost model per concurrent player.

<a id="area-5-real-time-media"></a>
## 5. Real-time communication and networked music

### State of the art 2026

- **WebRTC.** The encoded-transform API (`RTCRtpScriptTransform`) is Baseline since October 2025; the March 2026 W3C
  working draft adds SFrame sender and receiver transform types [N48](#n48). libwebrtc carries GCC and a SCReAM v2
  implementation [N49](#n49); SCReAM v2 supports L4S and warns it can be starved by TCP Prague (K) [N49](#n49).
- **E2EE in calls.** SFrame (RFC 9605) encrypts whole frames so SFUs route without access, with keys exported from MLS;
  a W3C issue proposes an MLS key-management interface for browsers [N43](#n43). Webex uses MLS [N20](#n20).
- **Codecs.** AV1 in Teams since late 2023 (screen sharing since March 2024, 63% less bandwidth than H.264)
  [N45](#n45); VP9 and AV1 SVC let an SFU drop layers per receiver (K). Opus deep redundancy (DRED) is an mlcodec WG
  draft (-07, August 2026) that updates RFC 6716 [N40](#n40); Opus also has in-band LBRR redundancy, and Opus 1.5
  shipped deep packet-loss concealment (K). Neural speech codecs near 1 kbps with about 30 ms latency are research
  (LRAC challenge 2025-2026), not deployed in mainstream calls [N50](#n50).
- **Transports beyond RTP/UDP.** Media over QUIC transport is at -20 (August 2026), still an I-D with a WG milestone of
  December 2026 for the IESG; Cloudflare runs a MoQ relay as a technical preview since August 2025 and its open-source
  relay targets older drafts [N39](#n39). RTP over QUIC is an experimental AVTCORE draft with SDP signalling at -00
  [N42](#n42).
- **Ingest and egress.** WHIP is RFC 9725 (March 2025); WHEP has no RFC yet (uncertain) [N41](#n41). LL-HLS remains the
  fallback for very large audiences (K) [N97](#n97).
- **SFUs and capacity.** LiveKit fits each room on one node [N44](#n44); Zoom's Video SDK documents up to 5,000 users
  per session while a marketing page says 1,000 (uncertain) [N46](#n46); mediasoup, Janus, Jitsi and Pion are the open
  peers (K) [N98](#n98).
- **Loss recovery and audio processing.** NACK/RTX, ULPFEC and FlexFEC (RFC 8627), RED (RFC 2198), NetEQ-class jitter
  buffers, AEC3 echo cancellation, RNNoise and DeepFilterNet noise suppression (K) [N99](#n99).
- **Networked music performance.** Musicians need roughly 25-30 ms mouth-to-ear for natural ensemble playing; JackTrip
  reaches below 50 ms on low-cost hardware over research and good residential networks [N47](#n47); SonoBus and
  SoundJack are peers (K). On LANs, AES67 interoperates over PTP (IEEE 1588-2019, 802.1AS gPTP), with Dante
  (proprietary) and AVB/Milan as the installed base (K) [N100](#n100).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| SFU with simulcast and SVC | LiveKit, mediasoup, Zoom [N44](#n44) | Later: [NET-LIVE](../ROADMAP.md#slice-net-live) |
| Media congestion control | GCC, SCReAM v2 with L4S [N49](#n49) | NET-LIVE "congestion control" |
| FEC, RED, DRED, deep PLC | RFC 8627, RFC 2198, Opus DRED [N40](#n40) | NET-LIVE "FEC and NACK" |
| Group E2EE | SFrame with MLS keys [N43](#n43) | NET-LIVE "through MLS" |
| Ingest, egress, broadcast | WHIP RFC 9725, WHEP, LL-HLS, MoQ [N39](#n39) [N41](#n41) | NET-LIVE "live streaming" |
| Capacity beyond one node | Cascaded SFUs (K) | NET-LIVE "thousand-participant webinars" |
| Networked music, clock sync | JackTrip [N47](#n47), PTP/AES67 [N100](#n100) | NET-LIVE "clock alignment" |

### Gaps in area 5

- **NET-LIVE should name its mechanisms:** GCC and SCReAM v2 with L4S feedback; simulcast plus AV1/VP9 SVC with the
  AV1 dependency-descriptor header; NACK/RTX, FlexFEC, RED and Opus DRED/deep PLC; an adaptive jitter buffer; SFrame
  with MLS-derived keys; WHIP and WHEP; MoQ relays for webinar and broadcast fan-out with LL-HLS fallback; cascaded SFUs
  so a room spans nodes and regions (beyond LiveKit's one-node room); recording and server-side compositing; captions
  through the ML rows.
- **A music mode with its own budgets:** sub-25 ms one-way on a metro link, 2.5 ms Opus frames or uncompressed PCM,
  PTP/gPTP hardware timestamps on LANs and a drift-compensating resampler from `hesap-dsp`, AES67 interoperability
  (licence decision for Dante left to the user). Clock sync lives in NET-TIME, not inside the media row.
- **FEC math in hesap.** FlexFEC parity, Reed-Solomon and fountain codes belong to `hesap-comms` (HESAP-NET), not to the
  media rows.

<a id="area-6-collaboration"></a>
## 6. Collaboration and sync

### State of the art 2026

- **CRDT libraries.** Automerge 3.0 (July 2025) cut memory over tenfold with the same file format (Part 2 B43); Loro
  1.x builds on Eg-walker and changed its encoding for compatibility since late 2023 [N51](#n51); Yjs remains the
  ecosystem default by downloads (secondary) [N51](#n51). Eg-walker (EuroSys 2025) merges text by replaying an event
  graph and keeps documents small (K) [N56](#n56); Fugue gives non-interleaving text merges (K) [N101](#n101).
- **Server-authoritative designs.** Figma runs one multiplayer worker per document, rewritten in Rust (K date)
  [N55](#n55); Google Docs-style operational transformation with a central sequencer (K).
- **Sync engines.** Rocicorp Zero 1.0 (2026) replicates Postgres into SQLite in `zero-cache` and syncs query-driven row
  subsets; no SSR yet [N52](#n52). ElectricSQL rebranded as Electric with an agent focus (secondary) [N53](#n53).
  Replicache, PowerSync and Liveblocks are peers (K).
- **Access control and E2EE.** Ink & Switch's Keyhive (pre-alpha, unaudited) pairs capability-based write access with
  group key agreement, and Beelay syncs encrypted documents so the server sees ciphertext [N54](#n54); MLS is the
  standards-track group-key alternative [N89](#n89).
- **Binary assets.** Content-defined chunking (FastCDC) with content addressing deduplicates large asset revisions (K)
  [N102](#n102).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| Authoritative host with local edits | Figma workers [N55](#n55) | [COLLAB.1](../ROADMAP.md#slice-collab.1) |
| Text and property CRDTs | Automerge 3, Loro, Yjs, Eg-walker [N51](#n51) | COLLAB.1 amendment evaluates |
| Partial, query-driven sync | Zero 1.0 [N52](#n52) | Missing |
| E2EE with access control | Keyhive, MLS [N54](#n54) | Missing |
| Binary asset closure | Content-defined chunking [N102](#n102) | [COLLAB.2](../ROADMAP.md#slice-collab.2) names it |

### Gaps in area 6

- **COLLAB.1 additions:** Eg-walker and Fugue as the text algorithms to beat, hybrid logical clocks and version
  vectors for causality, query-scoped partial sync (Zero-class) for large projects, and an E2EE mode where the project
  host relays ciphertext with MLS group keys and capability grants (Keyhive-class) for teams that require it, with the
  authoritative validation of constrained graphs kept as ADR-0130 item 4 says.
- **COLLAB.2:** content-defined chunking with content addressing, resumable transfers on NET-PATH, and merge-cost and
  memory budgets against Automerge 3 and Loro.

<a id="area-7-testing"></a>
## 7. Network testing, simulation and debugging

### State of the art 2026

- **Deterministic simulation testing.** FoundationDB runs the whole cluster in one deterministic process with
  simulated network, disks and clocks, and `BUGGIFY` fault points inside production code that fire only in simulation
  (K) [N103](#n103). TigerBeetle's VOPR drives a cluster from one seeded PRNG, injects network, storage and crash faults
  with per-seed probabilities ("swarm testing") and replays any seed exactly (K) [N104](#n104) [N57](#n57). Antithesis
  runs unmodified systems inside a deterministic hypervisor with guided exploration and property assertions (etcd uses
  it; 2026 funding and adoption by Jane Street and Ethereum) [N57](#n57) [N58](#n58). turmoil runs many hosts on one
  thread with partition, repair and hold (filesystem shim unstable) [N59](#n59); madsim intercepts time and randomness
  for Rust (K) [N64](#n64); Shadow runs real Linux binaries under syscall interception, deterministically, at Tor
  scale (K) [N63](#n63).
- **Correctness checkers.** Jepsen with the Elle and Knossos checkers finds isolation and linearizability anomalies
  from operation histories under partitions and clock skew (K) [N105](#n105).
- **Emulators.** Linux `tc netem`: delay with distributions, loss (random, four-state Markov, Gilbert-Elliott),
  duplication, corruption, reordering, rate and "slot" burst models (K) [N106](#n106); clumsy on Windows through the
  WinDivert driver (admin rights) (K) [N107](#n107); Apple Network Link Conditioner (K) [N112](#n112); Toxiproxy for TCP
  (latency, bandwidth, slicer, reset, timeout toxics) (K) [N108](#n108); Comcast (wrapper over tc/pf, unmaintained) (K)
  [N125](#n125); Mahimahi's trace-driven cellular link shells and record-replay (K) [N109](#n109); ns-3 discrete-event
  simulation and Mininet emulated topologies (K) [N110](#n110). Windows ships capture (`pktmon`) but no maintained
  in-box link conditioner (K, uncertain) [N114](#n114). Measured link traces exist for Starlink (May-June 2026
  dataset) [N12](#n12).
- **Engine tools.** Unreal: per-connection network emulation profiles and `PktLag`/`PktLoss`/`PktDup`/`PktOrder`
  console variables, multi-process play-in-editor and Networking Insights (K) [N111](#n111). Unity: PlayMode Tools
  network simulator with RTT delay, jitter and drop and presets such as 4G, and Multiplayer Play Mode with up to four
  players [N60](#n60). Godot: debugger network profiler (K).
- **Tracing and capture.** qlog main schema -14 and QUIC events -13 (July 2026) are Proposed Standard drafts with a
  schema-URI registry [N61](#n61); qvis visualises them (K). SSLKEYLOGFILE is RFC 9850 [N62](#n62); pcapng's Decryption
  Secrets Block lets a capture carry its own keys (K) [N113](#n113).
- **Load.** Bot swarms with recorded or scripted inputs and distributed launchers are standard practice for MMO load
  tests (K).

### Cerid comparison: NET-SIM and NET-DEBUG against every peer

The two rows already exceed any single peer in intent: no peer combines in-process, admin-free conditioning on every
platform with seeded deterministic simulation, CHIR-authored timelines, qlog plus pcapng, and multi-peer record and
replay correlated with a flight recorder. The table lists, capability by capability, what the best peers have and what
the rows must add to be better than all of them.

| Capability | Best peers | NET-SIM / NET-DEBUG today | Must add |
|---|---|---|---|
| Seeded schedules, virtual clock, sockets | FDB, VOPR, turmoil | NET-SIM has it | Run each seed twice |
| Storage and fsync faults, torn writes | FDB, VOPR | Missing | Simulated disk owned by platform I/O |
| In-code fault points | FDB `BUGGIFY` | Missing | Sim-only fault seams, compiled out |
| Swarm (per-seed fault mix) | VOPR, Antithesis | Missing | Fault probabilities drawn per seed |
| Guided exploration | Antithesis | Missing | Coverage-guided seed selection |
| Invariants, linearizability, liveness | Jepsen Elle, Antithesis | Replay only | History and liveness checks |
| Seed shrinking | VOPR, DIAG.9c | DIAG.9c minimises | Minimise fault schedule and seed |
| Real third-party binaries | Shadow, Antithesis | Out of scope | Emulation lane, not simulation |
| Loss models incl. bursty | netem, clumsy | NET-DEBUG has GE | Markov 4-state, slot bursts |
| Trace-driven links | Mahimahi, Starlink data | Missing | Replay cellular, Wi-Fi, LEO traces |
| Queues, AQM, ECN | netem plus qdiscs | "queue limits" | FQ-CoDel, DualQ L4S, ECN marking |
| MTU change, blackholes | netem, NLC | "MTU changes" | ICMP filtering, PMTU blackholes |
| NAT types, rebinding, CGNAT | Mininet setups | "NAT rebinding" | Cone, symmetric, timeouts, hairpin |
| IPv6-only, NAT64/DNS64 | NLC (K) | Missing | IPv6-only and NAT64 profiles |
| UDP blocked, proxies | Toxiproxy (TCP) | Missing | Forced fallback to TCP/TLS, H2 |
| Clock skew and jumps | Jepsen, FDB | NET-SIM "clock skew" | Per-peer drift, steps, leap smear |
| Per peer, channel, direction | Unreal, clumsy | NET-DEBUG has it | - |
| No admin, all platforms | Unreal, Unity (in-engine) | NET-DEBUG has it | - |
| Live change from tools | Unreal console | NET-DEBUG has it | - |
| qlog, qvis | quiche, msquic | NET-DEBUG qlog | Pin -14; schema URI; editor view |
| pcapng with keys | Wireshark, RFC 9850 | pcapng plus key logs | Embed DSB; compiled out of shipping |
| Replication cost per property | Networking Insights | Inspector named | Per-property cost, priority view |
| Multi-client launcher | Unity MPPM (4) | Cluster launcher, bots | Windows plus headless N, one editor |
| Bot swarm at scale | MMO load tools | CHIR bots | Distributed launchers, thousands |
| Multi-peer record and replay | none in engines | NET-DEBUG has it | - |
| Field link conditions to lab | none | Missing | Crash packs carry link summary |

### Gaps in area 7

- **NET-SIM additions:** simulated storage (fsync, torn and lost writes, latency) for the crash pool, persistence and
  collaboration hosts; sim-only fault seams in engine code (the existing `crd-core` fault seam is the owner) that
  compile to nothing in shipping; swarm testing with per-seed fault mixes; coverage-guided seed selection; history
  checkers for linearizability and session guarantees (Elle-class) plus liveness after partitions heal; every seed run
  twice with trace comparison (determinism self-check); minimisation of failing schedules through DIAG.9c; a
  cross-lane replay corpus (Windows/Linux, MSVC/Clang/GCC) for lockstep and rollback determinism.
- **NET-DEBUG additions:** trace-driven link replay (Mahimahi format, Starlink and cellular traces), queue disciplines
  with AQM and ECN marking (FQ-CoDel, DualQ for L4S), MTU blackholes, NAT behaviour types and CGNAT timeouts, IPv6-only
  with NAT64/DNS64, UDP-blocked networks that force fallbacks, per-peer clock drift and steps; qlog pinned to -14 with
  its schema URI; pcapng with an embedded Decryption Secrets Block; a per-property replication cost and priority view;
  distributed launchers for bot swarms of thousands. Loss models are hesap-stats distributions on the existing seeded
  Philox stream, the same pattern as `hesap-comms/channel.hpp` (principle b).
- **A separate emulation lane** for third-party peers (other QUIC stacks, TURN servers, browsers) under NET-DEBUG
  conditioning, because they cannot run inside NET-SIM's deterministic world; NET.4's interop testing uses it.

<a id="area-8-observability"></a>
## 8. Observability and operations

### State of the art 2026

- **OpenTelemetry.** Traces, metrics and logs are stable; profiles reached public alpha in March 2026 (Collector
  v0.148.0) [N65](#n65). W3C Trace Context carries trace identity across services (K) [N115](#n115); Part 3 records MCP
  carrying it in `_meta`.
- **Operations practice.** SLOs with error budgets, regional failover, autoscaling with warm buffers (Agones fleets
  [N35](#n35)), incident runbooks and cost per concurrent user are standard (K). Privacy rules (area 9) apply to logs
  and traces as much as to crash data.

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| Traces, metrics, logs export | OpenTelemetry [N65](#n65) | [OPS.1](../ROADMAP.md#slice-ops.1); DIAG traces |
| Client-server trace correlation | W3C Trace Context [N115](#n115) | Missing |
| Continuous profiling | OTel profiles (alpha) [N65](#n65) | DIAG captures; no export |
| Distributed deployments | OTel Collector pipelines (K) | [OPS.3](../ROADMAP.md#slice-ops.3) |
| SLOs, incident response | SRE practice (K) | Missing |

### Gaps in area 8

- **OPS.1 should export DIAG to OTLP** (traces, metrics, logs; profiles when stable) with secret-safe and pseudonymised
  attributes at the source, and `crd-net` sessions should carry a W3C trace context in their handshake so a client
  crash, a server trace and a relay log join on one identity.
- **SLOs and runbooks** for every Cerid-operated service (crash pool, TURN/relay fleet, matchmaking, project host),
  with burn-rate alerts and documented incident response; cost per active user measured.

<a id="area-9-crash-pipeline"></a>
## 9. Crash reporting and the crash-to-fix pipeline

### State of the art 2026

- **Capture.** Crashpad (out-of-process handler, minidumps) replaced Breakpad, which BugSplat labels deprecated
  [N76](#n76) [N116](#n116). Windows Error Reporting with `LocalDumps`, Apple `.ips` crash reports and MetricKit, and
  Android tombstones are the OS paths (K) [N119](#n119). DirectX Dump Files (`.dxdmp`) capture GPU state on TDR with up
  to 2 MB of custom game data, collectable by the game or through Watson; retail support is expected in fall 2026
  [N80](#n80) (owned by [GPU-FORENSICS](../ROADMAP.md#slice-gpu-forensics)).
- **Services.** Sentry (native SDK with Crashpad, console crash reporting), Backtrace (now Sauce Labs Error Reporting,
  acquired 2021), BugSplat, Bugsnag and Firebase Crashlytics (K for the last two) [N66](#n66) [N76](#n76). Symbol
  servers use the Microsoft SymSrv layout and debuginfod on Linux (K) [N118](#n118).
- **Grouping.** Sentry groups by normalised in-app frames with fingerprint and stack-trace rules (K) [N117](#n117);
  research uses learned similarity (TraceSim and S3M from JetBrains, 2020-2022) [N75](#n75).
- **Replay and reproduction.** rr and Pernosco on Linux, WinDbg Time Travel Debugging (TTD.exe recorder, elevated)
  and Undo LiveRecorder (Undo raised $37M in 2026) [N77](#n77) [N78](#n78) [N79](#n79); DIAG.9c already routes rr and
  TTD. Games use deterministic input replays (K).
- **Agentic fixes.** Sentry Seer (formerly Autofix) is generally available on paid plans with a vendor-reported 94.5%
  root-cause accuracy and 38,000+ issues fixed [N66](#n66) and added local-development and code-review debugging in
  January 2026 [N67](#n67). GitHub's agentic autofix for code-scanning alerts reached GA on 2026-10-15: it explores the
  code, fixes, reruns CodeQL to confirm the alert closes and opens a draft pull request with its validation steps
  [N70](#n70). GitHub's cloud agent pushes only to `copilot/` branches, sits behind a firewall allowlist, cannot approve
  or merge, needs a human to approve workflow runs, and filters hidden characters against prompt injection
  [N71](#n71). Google DeepMind's CodeMender (October 2025) validates with static and dynamic analysis, differential
  testing, fuzzing, SMT solvers, an LLM critique and an LLM judge for functional equivalence, and every patch is human
  reviewed; 72 fixes upstreamed [N68](#n68). Since July 2026 OSS-Fuzz pairs with CodeMender for C/C++ memory-safety
  bugs, honours each project's policy on AI-generated code, tests each fix in isolation (compiles, crash resolved,
  functional tests pass) and keeps Google engineers reviewing during the beta [N69](#n69).
- **How good are "passing" fixes?** On SWE-bench Verified, 7.8% of patches counted correct fail the developers' full
  tests and 29.6% of plausible patches behave differently from the ground truth (PatchDiff, ICSE 2026) [N72](#n72);
  32.67% of "resolved" issues in SWE-Bench+ leaked the solution through the issue text [N73](#n73). Aggregators report
  2026 SWE-bench Verified scores near 95%, which with contamination claims makes it unusable as a quality signal
  (uncertain) [N74](#n74).
- **Privacy law.** Pseudonymised data stays personal data under GDPR; EDPB Guidelines 01/2025 recommend keyed one-way
  functions (MACs) or encryption with sufficient entropy for pseudonymisation [N120](#n120). Consent for telemetry also
  falls under ePrivacy rules (K). CRA Article 14 adds vulnerability reporting duties [N21](#n21).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| Crash capture, dumps, bundles | Crashpad, WER [N116](#n116) | Done: DIAG.5a, DIAG.5b, DIAG.5d |
| Reproduction pack with replay | Input replays; rr/TTD (K) | [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1) |
| GPU crash dumps | DirectX Dump Files [N80](#n80) | Planned: GPU-FORENSICS |
| Collector, symbols, grouping | Sentry [N66](#n66) [N117](#n117) | [CRASH-FIX.2](../ROADMAP.md#slice-crash-fix.2) |
| Hardware-fault separation | None comparable | Planned: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2) |
| Fix agent, guardrails | CodeMender [N68](#n68), GitHub [N71](#n71) | [CRASH-FIX.3](../ROADMAP.md#slice-crash-fix.3) |
| Fix quality measurement | PatchDiff [N72](#n72) | [CRASH-FIX.4](../ROADMAP.md#slice-crash-fix.4) seeded fleet |
| Pseudonymisation, consent, retention | EDPB 01/2025 [N120](#n120) | CRASH-FIX.1 "declared policy" |
| Vulnerability reporting | CRA Article 14 [N21](#n21) | Missing |

### Gaps in area 9

- **CRASH-FIX.1 additions:** the network context of a crash (session and peer identities as keyed pseudonyms, the qlog
  tail, a link-condition summary NET-DEBUG can replay, the W3C trace identity), DirectX Dump Files custom data carrying
  the pack identity so the two join, OS crash formats (Apple `.ips`, Android tombstones) when MAC and mobile arrive,
  and consent tiers (minimal stack-only, standard, full memory) with per-tier redaction tests.
- **CRASH-FIX.2 additions:** a two-level grouping (an exact signature over normalised in-app frames and fault class,
  then learned similarity for near-duplicates) with reviewable merges and splits; a SymSrv and debuginfod symbol store
  with source indexing; a public, abuse-resistant ingest endpoint qualified under NET.4 (rate limits, size limits,
  authorisation before decode); keyed-MAC pseudonymisation per EDPB 01/2025, retention and deletion on request; EU data
  residency; and a CRA workflow that flags clusters with security impact and drafts the 24-hour, 72-hour and 14-day
  reports for human submission (ENISA's platform has no API; SEC.1 (P3) owns disclosure policy).
- **CRASH-FIX.3 additions (match CodeMender, exceed it on evidence):** after the failing test passes, a behavioural
  differential run of patched against unpatched builds on the reproduction, recorded replays and the fuzz corpus
  (PatchDiff-class), a fuzz campaign seeded from the crash input, the sanitizer lanes, a minimal-diff rule, and an
  independent critic in a separate context that receives only provenance-tagged pack content; respect a third-party
  project's AI-contribution policy before any upstream submission; draft pull requests whose workflows need human
  approval and a network allowlist (the GitHub pattern).
- **CRASH-FIX.4 additions:** report false-fix rate by behavioural divergence, not "tests pass"; never cite SWE-bench
  Verified as evidence; compare against CodeMender's published validation stages and Seer's claimed accuracy with
  Cerid's own seeded fleet.

<a id="area-10-web"></a>
## 10. Web and browser networking

### State of the art 2026

- **WebTransport** is in every current major browser since Safari 26.4 (Baseline date 2026-03-24, secondary), while the
  protocol is still draft -16 (July 2026) and Chrome and Safari still send the legacy `:protocol` token instead of
  `webtransport-h3` (secondary) [N02](#n02) [N07](#n07). WebTransport over HTTP/2 is a separate draft for networks that
  block UDP (K) [N123](#n123).
- **WebSocket** (RFC 6455) bootstraps over HTTP/2 (RFC 8441) and HTTP/3 (RFC 9220) (K) [N124](#n124).
- **WebRTC data channels** remain the only browser-to-browser peer-to-peer channel, with ICE for NAT traversal (K)
  [N121](#n121); encoded transforms and WebCodecs give media control (area 5) [N48](#n48).
- **Direct Sockets** (raw TCP and UDP) exist only for Isolated Web Apps, which ship only on ChromeOS; a June 2026 change
  split permissions into `local-network` and `loopback-network`; Gecko and WebKit have not committed [N126](#n126).
- **Browser limits** (K): no raw UDP, no ECN or DSCP control, no socket options, connection limits per origin, and
  `serverCertificateHashes` for short-lived self-signed WebTransport certificates in development.

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| Browser client channel | WebTransport [N07](#n07) | [WEB.5](../ROADMAP.md#slice-web.5) qualifies |
| Fallback without UDP | WebSocket, WebTransport over HTTP/2 [N123](#n123) | 4.2a "WebSocket fallback" |
| Browser peer-to-peer | WebRTC data channels [N121](#n121) | NET-LIVE "WebRTC interoperability" |
| Server side for browsers | HTTP/3 servers with WebTransport (K) | Not named |
| Raw sockets in the browser | Direct Sockets, ChromeOS only [N126](#n126) | Correctly not assumed |

### Gaps in area 10

- **The server side is a `crd-net` deliverable:** an HTTP/3 endpoint with WebTransport sessions, WebSocket over
  HTTP/1.1, HTTP/2 and HTTP/3, and WebTransport over HTTP/2 when the draft settles, all mapped onto the same session
  and channel model as native clients, with the draft revision negotiated and both `:protocol` spellings accepted.
- **WEB.5 qualification** should cover the browser matrix on Chrome, Edge, Firefox and Safari, UDP-blocked networks,
  WebRTC data channels for browser peer-to-peer sessions over NET-REACH, and service-worker offline queues for
  collaboration; Direct Sockets is never required.

<a id="area-11-specialised"></a>
## 11. Specialised links, emerging networks and staying current

### State of the art 2026

- **Robotics.** ROS 2 Lyrical Luth (2026-05-22) is an LTS release supported to May 2031; its default middleware is
  Fast DDS (`rmw_fastrtps_cpp`) and Zenoh (`rmw_zenoh_cpp`) is Tier 1 on all platforms [N81](#n81). The
  [4.2e](../ROADMAP.md#slice-4.2e) row's research amendment already targets Lyrical and native RMW participation.
- **Deterministic networking.** IEEE 802.1 TSN handles layer 2 and IETF DetNet layer 3 (RFC 9023, RFC 9037); the DetNet
  WG targeted an enhanced data plane for July 2026, and 3GPP Release 18 supports DetNet [N82](#n82). Linux exposes
  `SO_TXTIME`, the `etf` and `taprio` qdiscs and hardware timestamps (K).
- **Industrial and aerospace.** OPC UA (including PubSub over TSN), MAVLink 2 and CCSDS space links are the interop
  targets (K); [AUTOPILOT](../ROADMAP.md#slice-autopilot) covers MAVLink.
- **LEO and cellular.** Starlink's scheduled handovers and bimodal latency (area 1) [N11](#n11) [N12](#n12); 5G-Advanced
  adds L4S and DetNet support, and edge compute moves servers within a few milliseconds of players (K, partly from
  [N82](#n82)). Wi-Fi 7 (802.11be) multi-link operation reduces tail latency (K).
- **Embedded.** picoquic, ngtcp2 and quiche are used on constrained targets; QUIC on microcontrollers is research (K)
  [N86](#n86).

### Cerid comparison

| Capability | Best 2026 reference | Cerid |
|---|---|---|
| ROS 2 interop | Lyrical, Fast DDS, Zenoh [N81](#n81) | Later: 4.2e (amended) |
| Time-sensitive links and clock sync | TSN, DetNet, PTP [N82](#n82) [N100](#n100) | Missing (NET-TIME proposed) |
| LEO and mobile awareness | StarQUIC [N13](#n13) | NET-DEBUG "satellite" profile |
| Embedded transport | picoquic, ngtcp2 (K) | [EMBED](../ROADMAP.md#slice-embed) does not name networking |

### Gaps in area 11

- **Clock sync as a shared service** (NET-TIME): NTP-class offset and drift estimation over `crd-net` with hesap
  filters, PTP and gPTP hardware timestamps where the NIC supports them, `SO_TXTIME` launch-time scheduling on Linux,
  and a published clock-quality value every consumer reads (4.2c tick sync, NET-LIVE music mode, VPROD genlock, digital
  twins, 4.2e).
- **Embedded builds of the transport core** for EMBED targets (freestanding, no heap after start, bounded queues) are
  a later amendment to EMBED once 4.2a exists; recorded here, not proposed as a row.

<a id="staying-current"></a>
### What is likely to change through 2030 and later

| When (expected) | Change | Cerid exposure |
|---|---|---|
| 2026-2027 | Multipath QUIC and reliable stream reset become RFCs [N83](#n83) | NET-PATH, WebTransport |
| 2026-2027 | WebTransport RFC; browsers move to `webtransport-h3` [N02](#n02) | 4.2a server endpoint, WEB.5 |
| 2027 | MoQ transport to the IESG (milestone December 2026) [N39](#n39) | NET-LIVE broadcast |
| 2027 | Merkle Tree Certificates in the Web PKI (Let's Encrypt production) [N17](#n17) | NET.2 public endpoints |
| 2027-12-11 | Full EU CRA obligations [N21](#n21) | Every shipped Cerid product |
| 2026-2028 | qlog, ACK frequency, Opus DRED, QUIC-LB RFCs [N61](#n61) [N40](#n40) | NET-DEBUG, NET-LIVE, NET-SERVER |
| 2030 | NIST deprecates 112-bit RSA/ECC (draft) [N22](#n22) | NET.2 policy switch |
| 2035 | NIST disallows quantum-vulnerable algorithms (draft) [N22](#n22) | Classical-only refused |
| Open | BBRv3 in mainline Linux; L4S on more access networks [N04](#n04) [N05](#n05) | NET-PATH controllers |
| ~2030 | IMT-2030 (6G) and later Wi-Fi generations (K) | NET-DEBUG profiles |

### Staying current through the years: the design rules

1. **Negotiate everything.** Every Cerid wire protocol carries a version and an extension list in its handshake;
   unknown extensions are ignored, never fatal; each side sends GREASE values (RFC 8701 practice, RFC 9287 for QUIC)
   in every build so peers and middleboxes that ossify are caught in testing, not in the field [N91](#n91).
2. **Crypto agility by registry.** Algorithms are identified, never hard-coded; hybrids are the default; the accepted
   set is a signed policy generation (principle a) with dated migration windows, an offline grace period and a remote,
   signed disable for a broken algorithm; RFC 7696 is the checklist [N91](#n91).
3. **Pluggable providers behind owned interfaces:** crypto (OpenSSL 3.5 LTS or 4.x, AWS-LC, BoringSSL, verified
   primitives), congestion controllers, NAT-traversal strategies, codecs, fleet and matchmaking adapters. Swapping a
   provider never changes the wire format.
4. **A standards register in the repository** (NET-STD): every RFC and draft Cerid implements, its revision, codepoints,
   implementation status, interop evidence and deprecation date; a CI guard fails when a pinned draft has expired in
   the committed snapshot or a pinned RFC is marked obsoleted, so staleness becomes a failing check.
5. **Continuous interop evidence:** a nightly interop run against the reference QUIC stacks and the browser matrix,
   with results stored as evidence, not opinion.
6. **Versioned long-lived artefacts:** replays, crash packs, qlog and pcapng captures and world journals carry schema
   versions with N-1 readers (OPS.2 already owns the N-1 bundle reader).
7. **Scheduled re-research:** a light scan after each IETF meeting (three a year) and a full re-run of this document
   once a year or when a pinned item changes status, each run proposing roadmap amendments for the user, never editing
   rows on its own; any download it needs is listed for approval.

<a id="gaps-and-proposed-roadmap-changes"></a>
## Gaps and proposed roadmap changes

Proposals only; the user approves before any [ROADMAP](../ROADMAP.md) edit. Positions keep the user's 2026-10-27 order:
foundation additions go inside the NETCRASH window (after OWN, before AUD-2), application layers stay after the
systems they need and after NET.5, and the fix agent stays after AGENT.3. Every Requires names a row that sits earlier
in the table. "(P3)" marks a Part 3 proposal that is not yet a row. The full text of each proposal follows the
table.

| # | Row | New/Amend | Position | Requires | Proposal |
|---:|---|---|---|---|---|
| 1 | NET-STD | New | After NET.1 (111) | NET.1 | [P-1](#p-1) standards register, negotiation, GREASE |
| 2 | HESAP-NET | New | After NET.2 (112) | OWN.1 | [P-2](#p-2) network math in hesap |
| 3 | NET-PATH | New | After 4.2a (115) | 4.2a, HESAP-NET | [P-3](#p-3) path layer and congestion control |
| 4 | NET-REACH | New | After NET-PATH | NET-PATH, NET.2 | [P-4](#p-4) NAT traversal and relays |
| 5 | NET-TIME | New | Before 4.2c (856) | NET.5, HESAP-NET | [P-5](#p-5) cross-machine clock sync |
| 6 | NET.1 | Amend | In place (111) | OWN | [P-6](#p-6) threat model and negotiation design |
| 7 | NET.2 | Amend | In place (112) | NET.1 | [P-7](#p-7) PQ pins, ECH, keys, verified providers |
| 8 | NET.3 | Amend | In place (113) | NET.2, HESAP-NET | [P-8](#p-8) schema wire format, decode budgets |
| 9 | NET.4 | Amend | In place (114) | NET.3 | [P-9](#p-9) interop, differential, state learning |
| 10 | 4.2a | Amend | In place (115) | NET.4, NET-STD | [P-10](#p-10) browser-facing server endpoint |
| 11 | NET-SIM | Amend | In place (117) | 4.2a, 4.2b | [P-11](#p-11) storage faults, swarm, checkers |
| 12 | NET-DEBUG | Amend | In place (118) | NET-SIM | [P-12](#p-12) trace links, AQM, middleboxes |
| 13 | NET.5 | Amend | In place (119) | NET-PATH, NET-REACH | [P-13](#p-13) qualification envelope |
| 14 | NETCRASH | Amend | In place (122) | NET.5, CRASH-FIX.2 | [P-14](#p-14) names the new window rows |
| 15 | HW-RESILIENCE.1 | Amend | In place (095) | SIMD-DISPATCH | [P-15](#p-15) checksums under AEAD |
| 16 | CRASH-FIX.1 | Amend | In place (120) | DIAG, HW-RESILIENCE.2 | [P-16](#p-16) network context, consent tiers |
| 17 | CRASH-FIX.2 | Amend | In place (121) | CRASH-FIX.1, NET.5 | [P-17](#p-17) grouping, symbols, CRA |
| 18 | CRASH-FIX.3 | Amend | In place (394) | CRASH-FIX.2, AGENT.3 | [P-18](#p-18) behavioural validation |
| 19 | CRASH-FIX.4 | Amend | In place (395) | CRASH-FIX.3 | [P-19](#p-19) false-fix measurement |
| 20 | AGENT.3 | Amend | In place (301) | AGENT.2 | [P-20](#p-20) crash-data injection corpus |
| 21 | OPS.1 | Amend | In place (530) | DIAG | [P-21](#p-21) OTLP export, trace context, SLOs |
| 22 | WEB.5 | Amend | In place (627) | WEB.4 | [P-22](#p-22) browser network matrix |
| 23 | 4.2c | Amend | In place (856) | NET.5, NET-TIME | [P-23](#p-23) technique list |
| 24 | 4.2d | Amend | In place (857) | NET.5 | [P-24](#p-24) backend adapters on NET-REACH |
| 25 | NET-SCALE | Amend | In place (860) | 4.2c, 4.2d | [P-25](#p-25) leases, journal, overload mode |
| 26 | NET-LIVE | Amend | In place (861) | NET.5, NET-TIME | [P-26](#p-26) media mechanisms, music mode |
| 27 | 4.2e | Amend | In place (862) | NET.5 | [P-27](#p-27) ROS 2 and time-sensitive links |
| 28 | NET | Amend | In place (863) | NET-TIME | [P-28](#p-28) parent closes on NET-TIME |
| 29 | COLLAB.1 | Amend | In place (864) | NET.5, DOC | [P-29](#p-29) algorithms, partial sync, E2EE |
| 30 | COLLAB.2 | Amend | In place (865) | COLLAB.1 | [P-30](#p-30) chunked asset sync, budgets |
| 31 | NET-SERVER | Amend | In place (858) | 4.2d | [P-31](#p-31) fleets, QUIC-LB, regions |
| 32 | ANTICHEAT | Amend | In place (859) | NET.4, 4.2c, NET-SERVER | [P-32](#p-32) no kernel driver, attestation |

Totals: 5 new rows and 27 amendments to existing rows.

### Proposal texts

<a id="p-1"></a>
**P-1. NET-STD (new, after NET.1).** Standards register and protocol evolution contract. A machine-readable register
beside the module registry lists every protocol, extension and algorithm Cerid implements or plans (RFC or draft and
its revision, IANA codepoints, owning module, implementation state, interop evidence, deprecation date) and a watch
list (multipath QUIC, reliable reset, address discovery, ACK frequency, receive timestamps, QUIC-LB, WebTransport,
connect-udp-listen, MoQ transport, RTP over QUIC, qlog, Opus DRED, MTC, NIST IR 8547). Every Cerid wire protocol carries
a version and extension negotiation with ignore-unknown semantics and sends GREASE values in every build. A CTest
fails when a pinned draft has expired or a pinned RFC is obsoleted in the committed register snapshot. A scheduled
re-research (after each IETF meeting, and a full annual run) proposes amendments for the user; snapshot updates that
need a download are listed for approval. NET.5 and every later networked row cite the register entries they implement.

<a id="p-2"></a>
**P-2. HESAP-NET (new, after NET.2; owner crd-hesap).** Network and media math in hesap, by module: packet erasure
codes in `hesap-comms` (XOR/FlexFEC parity per RFC 8627, Reed-Solomon over GF(2^8) with dispatched SIMD kernels and an
SSE2 floor, RaptorQ-class fountain codes per RFC 6330 [N122](#n122), interleavers); bandwidth, RTT, jitter and one-way
delay-trend estimators (Kalman and trendline filters as in GCC) and clock-offset and drift filters in `hesap-stats`;
interpolation, extrapolation and dead-reckoning kernels and rewind history sampling in `hesap-interp`; quantisation
helpers (bounded floats, smallest-three quaternions) as numeric kernels. Bit-exact oracles against reference
implementations, SSE2 and dispatched paths byte-identical, budgets against libwebrtc and the reference codes. `crd-net`,
NET-LIVE and 4.2c consume it; nothing reimplements it.

<a id="p-3"></a>
**P-3. NET-PATH (new, after 4.2a).** The path layer of `crd-net`: DPLPMTUD (RFC 8899) with a 1280-byte IPv6 floor and
blackhole detection; ECN marking, validation and reporting; pacing; a pluggable, versioned congestion-control interface
with NewReno, CUBIC, BBRv3 and Prague (L4S) for bulk and game traffic and the delay-based real-time controllers shared
with NET-LIVE, each an authored policy chosen per connection and swappable live; connection migration, NAT rebinding
and scheduled-handover awareness for LEO links; fast paths for UDP GSO/GRO, io_uring (including zero-copy receive where
the NIC supports header split), Windows USO/URO and RIO, each with a portable fallback chosen at runtime and reported;
multipath QUIC and address discovery behind NET-STD flags until their RFCs land. Estimators come from HESAP-NET.
Budgets: throughput per core, added p99 latency under loss and reordering, and handshake rate against msquic and
quiche, on old NICs without offloads as well as current ones.

<a id="p-4"></a>
**P-4. NET-REACH (new, after NET-PATH).** Reachability for every product class, not only calls: ICE with STUN and TURN
(RFC 8445, RFC 8489, RFC 8656) [N121](#n121), QUIC address discovery and QUIC NAT traversal (iroh-class, behind
NET-STD flags), MASQUE CONNECT-UDP and bound-UDP proxying as a standards-based relay, Cerid-operated relays with DDoS
fronting in the style of Steam Datagram Relay, IPv6-first with IPv4 fallback, NAT64/DNS64 and CGNAT handling,
relay-cost accounting, and authenticated relay allocation under NET.2 so relays cannot be abused as open proxies.
Qualified under NET-DEBUG's NAT-type profiles and NET-SIM. Consumed by 4.2d lobbies, COLLAB, DAW sessions and NET-LIVE.

<a id="p-5"></a>
**P-5. NET-TIME (new, directly before 4.2c; Later).** Cross-machine time: NTP-class offset and drift estimation over
`crd-net` sessions with HESAP-NET filters, PTP (IEEE 1588-2019) and gPTP (802.1AS) with hardware timestamps where
available and software timestamps otherwise, `SO_TXTIME` launch-time scheduling on Linux, and one published
clock-quality value (offset bound, drift, source) that every consumer reads: 4.2c tick sync, NET-LIVE music mode, VPROD
genlock, digital twins and 4.2e. Qualified with NET-SIM clock skew, steps and leap smear and against PTP reference
hardware where the user approves it.

<a id="p-6"></a>
**P-6. NET.1 (amend).** The threat model adds metadata privacy (ECH, padding, traffic analysis of voice and game
channels), PQ-specific denial of service (large handshakes against the amplification limit), relays as abuse targets,
LEO and mobile paths, IPv6-only networks, compromised or faulty peers sending plausible state (principle h), and the CRA
reporting duty. The design records the version and extension negotiation and the crypto-agility policy whose
mechanisms NET-STD and NET.2 implement.

<a id="p-7"></a>
**P-7. NET.2 (amend).** Pin RFC 10024 hybrid groups (X25519MLKEM768 preferred, SecP384r1MLKEM1024 for high assurance),
RFC 9881 ML-DSA for Cerid-issued service and code-signing certificates, MTC tracking for public web endpoints, and a
dated policy that refuses classical-only key exchange first for Cerid services and by 2030 everywhere, aligned with
NIST IR 8547 once final. Keep handshakes within QUIC's amplification limit with certificate compression (RFC 8879),
address-validation tokens and resumption, and raw public keys or pinned Cerid credentials for game and session servers.
Support ECH (RFC 9849) on public endpoints. Bind the Tamarin/ProVerif model to the code by generating the handshake
state machine from one description or checking it by state learning against the model. Choose verified primitives
(HACL*, libcrux, AWS-LC's mlkem-native) behind the provider interface. Hardware-backed keys (TPM 2.0, Secure Enclave,
Android Keystore) with software fallback. RFC 9850 key logging and all capture keys are absent from shipping builds,
enforced by a CI guard.

<a id="p-8"></a>
**P-8. NET.3 (amend).** Wire formats are generated from the shared reflection and schema model (ADR-0130 item 1) with
versioned fields, bounds checked zero-copy decoding and a generated fuzzer per schema; authorisation and size limits
run before any decode; quantisation and FEC math come from HESAP-NET; general compression (dictionary compression for
reliable streams, entropy coding for snapshots) comes from the foundation compression owner. Requires HESAP-NET.

<a id="p-9"></a>
**P-9. NET.4 (amend).** Add the QUIC interop runner against msquic, quiche, ngtcp2, quinn and picoquic in NET-DEBUG's
emulation lane; differential testing of Cerid's transport against them on identical schedules; state-machine learning
against the NET.2 model (QUICtester-class) and logic-bug fuzzing (MerCuriuzz and QUIC-Fuzz class) beside coverage
fuzzing; continuous fuzzing of every parser in CI; after the independent review, a public vulnerability disclosure
policy and bug bounty under SEC.1 (P3).

<a id="p-10"></a>
**P-10. 4.2a (amend).** The browser-facing server is part of `crd-net`: an HTTP/3 endpoint with WebTransport sessions
(accepting both `:protocol` spellings, draft revision negotiated through NET-STD), WebSocket over HTTP/1.1, HTTP/2
(RFC 8441) and HTTP/3 (RFC 9220), and WebTransport over HTTP/2 once its draft settles, all mapped onto the same session
and channel model as native clients. Path behaviour moves to NET-PATH. Requires NET-STD.

<a id="p-11"></a>
**P-11. NET-SIM (amend).** Add simulated storage faults (fsync loss, torn and lost writes, latency) through the
platform I/O owner, sim-only fault seams on the existing `crd-core` fault seam that compile out of shipping, swarm
testing with per-seed fault mixes, coverage-guided seed selection, history checkers for linearizability and session
guarantees plus liveness after healing, a double run of every seed with trace comparison, minimisation of failing
fault schedules through DIAG.9c, and a cross-lane determinism corpus. Peers: FoundationDB, VOPR, Antithesis, Jepsen.

<a id="p-12"></a>
**P-12. NET-DEBUG (amend).** Add trace-driven links (Mahimahi-format cellular, Wi-Fi and Starlink traces with the
15-second handover cycle), four-state Markov and slot burst loss, queue disciplines with AQM and ECN marking (FQ-CoDel,
DualQ L4S), MTU blackholes, NAT behaviour types (full cone to symmetric, hairpinning, CGNAT timeouts), IPv6-only with
NAT64/DNS64, UDP-blocked networks, per-peer clock drift and steps; qlog pinned to draft -14 with its schema URI and an
editor view; pcapng with an embedded Decryption Secrets Block; a per-property replication cost and priority view; an
emulation lane for third-party peers; distributed launchers for bot swarms of thousands. Loss and delay models use
hesap-stats on the seeded Philox stream. Peers add Mahimahi, Networking Insights and Unity MPPM.

<a id="p-13"></a>
**P-13. NET.5 (amend).** Requires NET-PATH and NET-REACH (NET-STD through 4.2a). The qualification envelope includes
IPv6-only with NAT64, CGNAT, UDP-blocked networks with fallback, a 1280-byte MTU, a recorded LEO trace, mobile
handover by migration, and old NICs without offloads; budgets against msquic and quiche in the same harness.

<a id="p-14"></a>
**P-14. NETCRASH (amend).** The window text lists NET-STD, HESAP-NET, NET-PATH and NET-REACH beside the existing rows;
its Requires is unchanged because NET.5 requires them.

<a id="p-15"></a>
**P-15. HW-RESILIENCE.1 (amend).** Define the network checksum under encryption: the AEAD tag already detects
corruption in flight, so the added check is a plaintext integrity hash computed at serialisation and verified after
decryption (it catches a sender's own memory or pipeline fault that AEAD would seal as valid), plus state hashes for
lockstep and rollback. One foundation-owned, dispatched checksum family (CRC32C with SSE4.2 and carry-less multiply
paths and a portable path, and a 64-bit fast hash) replaces the CRC32 copies in `perf`, `resources` and `hesap-tensor`
under OWN.1.

<a id="p-16"></a>
**P-16. CRASH-FIX.1 (amend).** Packs carry network context (session and peer identities as keyed pseudonyms, the qlog
tail, a link-condition summary NET-DEBUG can replay, the W3C trace identity), the DirectX Dump Files custom-data
link to the pack identity through GPU-FORENSICS, OS crash formats when MAC and mobile arrive, and consent tiers
(minimal, standard, full memory) with redaction tests per tier and keyed-MAC pseudonymisation per EDPB 01/2025.

<a id="p-17"></a>
**P-17. CRASH-FIX.2 (amend).** Two-level grouping (exact normalised signature, then learned similarity for near
duplicates) with reviewable merges and splits; a SymSrv and debuginfod symbol store with source indexing; an
abuse-resistant ingest endpoint qualified under NET.4; retention, deletion on request and EU data residency; a CRA
Article 14 workflow that flags clusters with security impact and drafts the 24-hour, 72-hour and 14-day reports for
human submission through ENISA's platform; OTLP export of pool metrics.

<a id="p-18"></a>
**P-18. CRASH-FIX.3 (amend).** After the failing test passes, the contract requires a behavioural differential run of
patched against unpatched builds on the reproduction, recorded replays and the fuzz corpus, a fuzz campaign seeded from
the crash input, the sanitizer lanes and a minimal-diff check; an independent critic in a separate context receives
only provenance-tagged pack content; third-party projects' AI-contribution policies are honoured before upstream
submission; pull requests are drafts whose CI needs human approval, and the agent runs behind a network allowlist.

<a id="p-19"></a>
**P-19. CRASH-FIX.4 (amend).** Measure false fixes by behavioural divergence (PatchDiff-class), not by passing tests;
never cite SWE-bench Verified as evidence; compare the stages and rates with CodeMender's published validation and
Seer's claimed accuracy on Cerid's seeded fleet.

<a id="p-20"></a>
**P-20. AGENT.3 (amend).** The hostile-input corpus adds crash-data vectors: instructions in log lines, exception
messages, file and module names, symbol names, peer and player names, chat text, user-written bug descriptions and
asset metadata, each delivered through a crash pack to the fix agent.

<a id="p-21"></a>
**P-21. OPS.1 (amend).** DIAG traces, metrics and logs export over OTLP (profiles when the signal is stable) with
attributes pseudonymised at the source; `crd-net` handshakes carry a W3C trace context so client, server and relay
records join; every Cerid-operated service has SLOs, burn-rate alerts and an incident runbook.

<a id="p-22"></a>
**P-22. WEB.5 (amend).** Qualify browser networking on Chrome, Edge, Firefox and Safari: WebTransport across draft
revisions, WebSocket and HTTP/2 fallbacks on UDP-blocked networks, WebRTC data channels over NET-REACH for browser
peer-to-peer sessions, service-worker offline queues for collaboration; Direct Sockets is never required.

<a id="p-23"></a>
**P-23. 4.2c (amend, extends its 2026-10-27 research amendment).** The selectable policies are specified by technique:
input delay plus rollback with a declared maximum window, sub-tick input timestamps, lag compensation with bounded
rewind and hit validation, delta compression against each client's last acknowledged baseline, authored quantisation per
property, priority accumulators with per-connection byte budgets, predicted physics with error smoothing, and lockstep
with per-tick state hashes that name the diverging peer; history and interpolation from HESAP-NET; tick sync from
NET-TIME; a cross-lane determinism corpus under NET-SIM. Requires NET-TIME.

<a id="p-24"></a>
**P-24. 4.2d (amend).** Lobby and session adapters (Steamworks with Steam Datagram Relay, Epic Online Services and
platform services) sit on NET-REACH for peer-to-peer and relay paths; Cerid's own lobby and matchmaking stub stays
backend-neutral.

<a id="p-25"></a>
**P-25. NET-SCALE (amend).** Mechanisms: authority leases with fencing tokens, exactly-once entity handoff, an
event-sourced world journal with snapshots, static and dynamic partitioning, and a declared overload mode (time
dilation per region with client notification); linearizability of accepted actions checked under NET-SIM partitions
and server kills; the mesh, persistence and authority stay in Cerid, with clouds and orchestrators as adapters. Peers
add EVE Online's time dilation; Star Citizen's 2026 state is recorded as quasi-dynamic only.

<a id="p-26"></a>
**P-26. NET-LIVE (amend).** Name GCC and SCReAM v2 with L4S, simulcast and AV1/VP9 SVC with the dependency descriptor,
NACK/RTX, FlexFEC, RED, Opus DRED and deep PLC, adaptive jitter buffers, SFrame with MLS-derived keys, WHIP and WHEP,
MoQ relays with LL-HLS fallback for webinars, cascaded SFUs across nodes and regions, recording and compositing; a music
mode with sub-25 ms one-way metro budgets, 2.5 ms Opus frames or PCM, NET-TIME clocks, a `hesap-dsp` drift resampler and
AES67 interoperability. NAT traversal comes from NET-REACH and FEC math from HESAP-NET. Requires NET-TIME.

<a id="p-27"></a>
**P-27. 4.2e (amend, beside its 2026-10-27 research amendment).** Lyrical LTS facts (Fast DDS default, Zenoh Tier 1);
DDS Security interop; time-sensitive links (TSN-aware scheduling through `SO_TXTIME` and NET-TIME) for robots and
digital twins.

<a id="p-28"></a>
**P-28. NET (amend).** The parent also requires NET-TIME, beside NET-SERVER and ANTICHEAT.

<a id="p-29"></a>
**P-29. COLLAB.1 (amend, extends its 2026-10-27 research amendment).** Evaluate Eg-walker and Fugue beside Automerge 3,
Yjs and Loro; hybrid logical clocks and version vectors; query-scoped partial sync (Zero-class); an E2EE mode where the
host relays ciphertext with MLS group keys and capability grants (Keyhive-class), while constrained graphs stay
transactional.

<a id="p-30"></a>
**P-30. COLLAB.2 (amend).** Binary asset closure through content-defined chunking with content addressing and
resumable transfers on NET-PATH; merge-cost and memory budgets against Automerge 3 and Loro.

<a id="p-31"></a>
**P-31. NET-SERVER (amend).** Agones and Open Match 2 adapters, QUIC-LB connection IDs, region
selection by measured RTT beacons, drain-and-swap updates through live CEIR generations, relay fronting for DDoS,
autoscaling with warm buffers, SLOs and a cost model per concurrent player.

<a id="p-32"></a>
**P-32. ANTICHEAT (amend).** Cerid ships no kernel component. Server authority and validation
first, statistical and replay-based detection on 4.2b replays, optional platform attestation (TPM measured boot, Play
Integrity, App Attest) with honest degradation where absent (Linux and Proton players are not locked out by Cerid
itself), optional vendor SDK adapters, a privacy policy and an appeal path.

<a id="needs-a-download"></a>
## Needs a download (user approval)

Not downloaded. Sizes were not checked (that would need at least a request to each host); each item needs separate
user approval before it is fetched.

- **QUIC peers** msquic, quiche, ngtcp2, quinn, picoquic: NET.4 differential and interop testing, NET-PATH budgets.
  https://github.com/microsoft/msquic ; https://github.com/cloudflare/quiche ; https://github.com/ngtcp2/ngtcp2 ;
  https://github.com/quinn-rs/quinn ; https://github.com/private-octopus/picoquic
- **QUIC interop runner**: NET.4. https://github.com/quic-interop/quic-interop-runner
- **Crypto providers** OpenSSL 3.5 LTS or 4.0, AWS-LC, BoringSSL, libcrux, HACL*: NET.2 provider evaluation.
  https://openssl-library.org ; https://github.com/aws/aws-lc ; https://boringssl.googlesource.com/boringssl ;
  https://github.com/cryspen/libcrux ; https://github.com/hacl-star/hacl-star
- **Tamarin and ProVerif**: NET.2 model checking in CI. https://tamarin-prover.com ;
  https://bblanche.gitlabpages.inria.fr/proverif/
- **MLS implementations** OpenMLS, MLSpp: NET.2 interop. https://github.com/openmls/openmls ;
  https://github.com/cisco/mlspp
- **Wireshark** (pcapng with DSB, QUIC dissection) and **qvis** (qlog): NET-DEBUG oracles.
  https://www.wireshark.org ; https://github.com/quiclog/qvis
- **Emulators** clumsy, Toxiproxy, Mahimahi, ns-3, Mininet: NET-DEBUG peers. https://github.com/jagt/clumsy ;
  https://github.com/Shopify/toxiproxy ; http://mahimahi.mit.edu ; https://www.nsnam.org ; https://mininet.org
- **Link traces**: the 2026 Starlink per-packet dataset (IEEE DataPort) and Mahimahi cellular traces for NET-DEBUG
  profiles. https://ieee-dataport.org/documents/leo-access-link-traces-measurements-commercial-starlink-terminal
- **Simulation peers** Shadow, turmoil, madsim, FoundationDB, TigerBeetle (VOPR): NET-SIM comparison.
  https://github.com/shadow/shadow ; https://github.com/tokio-rs/turmoil ; https://github.com/madsim-rs/madsim ;
  https://github.com/apple/foundationdb ; https://github.com/tigerbeetle/tigerbeetle
- **Jepsen and Elle**: NET-SIM history checkers. https://github.com/jepsen-io/jepsen ; https://github.com/jepsen-io/elle
- **Game netcode peers** GGPO, GameNetworkingSockets; Unreal Engine (Iris), Unity Netcode for Entities, Godot: 4.2c
  budgets. https://github.com/pond3r/ggpo ; https://github.com/ValveSoftware/GameNetworkingSockets
- **Fleet peers** Agones and Open Match 2 (local Kubernetes cluster): NET-SERVER.
  https://github.com/googleforgames/agones ; https://github.com/googleforgames/open-match2
- **Media peers** libwebrtc (GCC, SCReAM v2, NetEQ, AEC3), LiveKit, mediasoup, Pion, Opus 1.5+ with DRED, RNNoise,
  DeepFilterNet, a MoQ relay: NET-LIVE budgets. https://webrtc.googlesource.com/src ;
  https://github.com/livekit/livekit ;
  https://github.com/versatica/mediasoup ; https://github.com/pion/webrtc ; https://opus-codec.org ;
  https://github.com/xiph/rnnoise ; https://github.com/Rikorose/DeepFilterNet ; https://github.com/cloudflare/moq-rs
- **Music peers** JackTrip, SonoBus: NET-LIVE music mode. https://github.com/jacktrip/jacktrip ;
  https://github.com/sonosaurus/sonobus
- **Robotics** Zenoh and Fast DDS: 4.2e. https://github.com/eclipse-zenoh/zenoh ; https://github.com/eProsima/Fast-DDS
- **Sync peers** Zero, Keyhive (beside Part 2's Automerge, Yjs and Loro): COLLAB.1.
  https://github.com/rocicorp/mono ; https://github.com/inkandswitch/keyhive
- **Crash peers** Crashpad, Sentry self-hosted and its symbolicator: CRASH-FIX.2 comparison.
  https://chromium.googlesource.com/crashpad/crashpad ; https://github.com/getsentry/self-hosted
- **Observability** OpenTelemetry C++ SDK and Collector: OPS.1. https://github.com/open-telemetry/opentelemetry-cpp ;
  https://github.com/open-telemetry/opentelemetry-collector
- **PTP reference** linuxptp (and, separately, PTP-capable NIC hardware, a purchase): NET-TIME.
  https://linuxptp.nwtime.org (K)

<a id="sources"></a>
## Sources

Read 2026-10-27 (search results or fetched pages) unless marked (K). Dates are publication dates as shown by the
source. "Secondary" marks coverage, not the primary document.

<a id="n01"></a>N01. IETF, multipath QUIC draft -21 (2026-03-17), RFC Editor "In Progress" from 2026-05-20:
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/).

<a id="n02"></a>N02. IETF, WebTransport over HTTP/3 draft -16 (July 2026; requires HTTP/3 datagrams, capsules and
RESET_STREAM_AT): [draft](https://datatracker.ietf.org/doc/html/draft-ietf-webtrans-http3-16).

<a id="n03"></a>N03. F. Bruynooghe (n0), QUIC NAT traversal for iroh, individual informational draft -00 (July 2026):
[draft](https://datatracker.ietf.org/doc/html/draft-bruynooghe-n0-quic-nat-traversal-00).

<a id="n04"></a>N04. bbr-dev list, BBRv3 on newer kernels (March 2025; mainline still BBR v1):
[thread](https://groups.google.com/g/bbr-dev/c/4vwZGw0nxdQ/m/zb-hfJjuAwAJ).

<a id="n05"></a>N05. IETF 123 TSVWG minutes (2025-07-23; Comcast L4S status, Apple support); RFC 9330, RFC 9331:
[minutes](https://datatracker.ietf.org/meeting/123/materials/minutes-123-tsvwg-202507231400-00).

<a id="n06"></a>N06. M. Seemann, C. Huitema, QUIC address discovery draft -01 (2026-08-15):
[draft](https://www.ietf.org/archive/id/draft-ietf-quic-address-discovery-01.html).

<a id="n07"></a>N07. WebRTC.ventures, WebTransport is now Baseline (April 2026; secondary):
[article](https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/).

<a id="n08"></a>N08. Linux kernel, io_uring zero copy Rx (6.15):
[docs](https://kernel.org/doc/html/v6.15/networking/iou-zcrx.html).

<a id="n09"></a>N09. Microsoft, UDP RSC offload (URO, Windows 11 24H2) and UDP segmentation offload (USO):
[URO](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload);
[USO](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-segmentation-offload-uso-).

<a id="n10"></a>N10. D. Schinazi, A. Singh, Proxying bound UDP in HTTP, draft -16 (2026-08-24):
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-masque-connect-udp-listen/).

<a id="n11"></a>N11. Statistical study of Starlink end-to-end latency at 500 Hz (January 2026 preprint):
[arXiv 2601.08439](https://arxiv.org/abs/2601.08439).

<a id="n12"></a>N12. IFIP Networking 2025, Starlink latency 2022-2024 from RIPE Atlas and Cloudflare Radar:
[paper](https://dl.ifip.org/db/conf/networking/networking2025/1571125226.pdf); IEEE DataPort, Starlink access-link
traces (May-June 2026):
[dataset](https://ieee-dataport.org/documents/leo-access-link-traces-measurements-commercial-starlink-terminal).

<a id="n13"></a>N13. StarQUIC, LEONET 2024:
[paper](https://onlineacademiccommunity.uvic.ca/starlink/wp-content/uploads/sites/8876/2024/09/leonet24-victor.pdf).

<a id="n14"></a>N14. RFC 10024, PQ/T Hybrids for TLS 1.3 (August 2026):
[RFC](https://www.rfc-editor.org/rfc/rfc10024.html).

<a id="n15"></a>N15. RFC 9849, TLS Encrypted Client Hello (dated March 2026 on the RFC Editor page):
[RFC](https://www.rfc-editor.org/rfc/rfc9849.html).

<a id="n16"></a>N16. RFC 9881, ML-DSA in X.509 (October 2025): [info](https://www.rfc-editor.org/info/rfc9881).

<a id="n17"></a>N17. Let's Encrypt, post-quantum certificates roadmap (2026-06-03):
[post](https://letsencrypt.org/2026/06/03/pq-certs).

<a id="n18"></a>N18. IETF PLANTS, Merkle Tree Certificates draft -00 (2026):
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-plants-merkle-tree-certs/).

<a id="n19"></a>N19. IETF blog, RCS adopts MLS (2025): [post](https://www.ietf.org/blog/rcs-adopts-mls/); GSMA RCC.16
v3.0 (February 2026): [spec](https://media.gsma.com/assets/2026/rcs/RCC.16+v3.0.pdf); Infosecurity, encrypted RCS
between iPhone and Android (2026; secondary):
[news](https://infosecurity-magazine.com/news/apple-google-rcs-end-to-end).

<a id="n20"></a>N20. 37C3, RFC 9420 or how to scale end-to-end encryption with MLS (December 2023; Webex in production):
[talk](https://app.media.ccc.de/v/37c3-12064-rfc_9420_or_how_to_scale_end-to-end_encryption_with_messaging_layer_security).

<a id="n21"></a>N21. EU Cyber Resilience Act reporting from 2026-09-11 (secondary: PwC, Ireland's NCSC):
[PwC](https://www.pwc.ch/en/insights/regulation/eu-cyber-resilience-act.html); [NCSC](https://ncsc.gov.ie/cra/).

<a id="n22"></a>N22. NIST IR 8547 initial public draft (2024-11-12): [CSRC](https://csrc.nist.gov/pubs/ir/8547/ipd);
draft status in mid-2026 from secondary coverage (uncertain).

<a id="n23"></a>N23. OpenSSL 4.0 final release (2026-04-14):
[post](https://openssl-library.org/post/2026-04-14-openssl-40-final-release/); OpenSSL NEWS (3.5 default key shares):
[NEWS](https://fossies.org/linux/openssl/NEWS.md).

<a id="n24"></a>N24. Cryspen, Verifying libcrux's ML-KEM: [post](https://cryspen.com/post/ml-kem-verification/).

<a id="n25"></a>N25. OpenSSL Conference 2025, High-assurance post-quantum cryptography:
[slides](https://archive.openssl-conference.org/2025/presentations/Karthik_Bharg_OpenSSL__High-Assurance_Post-Quantum_Cryptography.pdf).

<a id="n26"></a>N26. NDSS 2026, Identifying logical vulnerabilities in QUIC implementations (MerCuriuzz):
[paper](https://www.ndss-symposium.org/ndss-paper/identifying-logical-vulnerabilities-in-quic-implementations/).

<a id="n27"></a>N27. QUIC-Fuzz (March 2025 preprint): [arXiv 2503.19402](https://arxiv.org/abs/2503.19402).

<a id="n28"></a>N28. OSTIF, nghttp3 and ngtcp2 audits complete (2025):
[post](https://ostif.org/nghttp3-ngtcp2-audits-complete/).

<a id="n29"></a>N29. QUICtester, ASIA CCS 2025: [arXiv 2505.12690](https://arxiv.org/abs/2505.12690).

<a id="n30"></a>N30. Epic, Introduction to Iris:
[docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-iris-in-unreal-engine); forum,
[5.7 Iris strategy](https://forums.unrealengine.com/t/5-7-iris-strategy/2699685) and
[Iris on 5.8.2](https://forums.unrealengine.com/t/requesting-an-eta-on-iris-mover-network-prediction-plugin-compatibility-ue-5-8-2/2841863).

<a id="n31"></a>N31. Pureinfotech, Microsoft removes antivirus from the Windows kernel (2025; secondary):
[article](https://pureinfotech.com/microsoft-removes-antivirus-windows-kernel/).

<a id="n32"></a>N32. TheSixthAxis, Battlefield 6 and Black Ops 7 need Secure Boot and TPM 2.0 (2025-08-07):
[article](https://www.thesixthaxis.com/2025/08/07/battlefield-6-and-black-ops-7-both-need-secure-boot-and-tpm-2-0-for-pc-anti-cheat/).

<a id="n33"></a>N33. Unity, Netcode for Entities manual (6.7):
[docs](https://docs.unity3d.com/Packages/com.unity.netcode@6.7/manual/index.html); Photon documentation index:
[index](https://doc.photonengine.com/llms.txt).

<a id="n34"></a>N34. Massively OP, Star Citizen CTO on server meshing (2026-02-06):
[article](https://massivelyop.com/2026/02/06/star-citizen-cto-outlines-progress-on-server-meshing/); Letter from the
Chairman (2026-08-27): [wiki](https://starcitizen.tools/Comm-Link:Letter_from_the_Chairman_-_2026-08-27) (secondary).

<a id="n35"></a>N35. Agones releases (v1.61.0, 24 September):
[releases](https://github.com/googleforgames/agones/releases).

<a id="n36"></a>N36. Open Match 2 repository (public preview): [repo](https://github.com/googleforgames/open-match2).

<a id="n37"></a>N37. PC Gamer, EVE Online's time dilation (K date):
[article](https://www.pcgamer.com/uk/eve-onlines-mad-time-dilation-tech-beats-lag).

<a id="n38"></a>N38. Massively OP, Improbable and Unity's SpatialOS dispute (2019-01-10):
[article](https://massivelyop.com/2019/01/10/improbable-and-unitys-spat-over-spatialos-threatens-dozens-of-mmos-including-worlds-adrift/).

<a id="n39"></a>N39. IETF, MoQ transport draft -20 (2026-08-31):
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/);
moq.dev, first CDN (2025-08-21): [post](https://moq.dev/blog/first-cdn/); Cloudflare moq-rs:
[repo](https://github.com/cloudflare/moq-rs).

<a id="n40"></a>N40. IETF mlcodec, Opus DRED draft -07 (2026-08-04):
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-mlcodec-opus-dred/).

<a id="n41"></a>N41. RFC 9725, WHIP (March 2025): [RFC](https://www.rfc-editor.org/rfc/rfc9725.html).

<a id="n42"></a>N42. IETF AVTCORE, RTP over QUIC draft -14:
[draft](https://datatracker.ietf.org/doc/html/draft-ietf-avtcore-rtp-over-quic); SDP for RoQ -00:
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-avtcore-sdp-roq/).

<a id="n43"></a>N43. RFC 9605, SFrame (August 2024): [RFC](https://www.rfc-editor.org/rfc/rfc9605.html); W3C WebRTC
issue on MLS key management (June 2025):
[log](https://lists.w3.org/Archives/Public/public-webrtc-logs/2025Jun/0052.html).

<a id="n44"></a>N44. LiveKit, self-hosting benchmarks:
[docs](https://docs.livekit.io/transport/self-hosting/benchmark.md).

<a id="n45"></a>N45. AOMedia, Microsoft Teams AV1 story:
[page](https://aomedia.org/av1-adoption-showcase/microsoft-story).

<a id="n46"></a>N46. Zoom, Video SDK documentation: [docs](https://developers.zoom.us/docs/video-sdk/).

<a id="n47"></a>N47. Low-latency JackTrip framework for music education (2025):
[arXiv 2505.00550](https://arxiv.org/pdf/2505.00550).

<a id="n48"></a>N48. W3C, WebRTC Encoded Transform working draft (2026-03-19):
[WD](https://www.w3.org/TR/2026/WD-webrtc-encoded-transform-20260319/); MDN, `RTCRtpScriptTransformer`:
[MDN](https://developer.mozilla.org/docs/Web/API/RTCRtpScriptTransformer/readable).

<a id="n49"></a>N49. libwebrtc, SCReAM v2 implementation differences:
[doc](https://webrtc.googlesource.com/src/+/HEAD/modules/congestion_controller/scream/g3doc/implementation_diff.md);
Ericsson SCReAM: [repo](https://github.com/EricssonResearch/scream/).

<a id="n50"></a>N50. Cisco LRAC workshop 2026 (low-resource audio codecs):
[site](https://crowdsourcing.cisco.com/lrac-workshop/2026/).

<a id="n51"></a>N51. Loro, JS/Wasm benchmarks: [docs](https://loro.dev/docs/performance); PkgPulse, Yjs vs Automerge vs
Loro (2026; secondary): [guide](https://www.pkgpulse.com/guides/yjs-vs-automerge-vs-loro-crdt-libraries-2026).

<a id="n52"></a>N52. InfoQ, Zero version 1 (June 2026): [news](https://infoq.com/news/2026/06/zero-version-1).

<a id="n53"></a>N53. DevToolLab, local-first apps and sync engines guide (July 2026; secondary):
[guide](https://devtoollab.com/blog/local-first-apps-crdt-sync-engine-guide-2026).

<a id="n54"></a>N54. Ink & Switch, Keyhive lab notebook: [notebook](https://inkandswitch.com/keyhive/notebook/04).

<a id="n55"></a>N55. Figma, Rust in production at Figma (K date):
[post](https://www.figma.com/blog/rust-in-production-at-figma/).

<a id="n56"></a>N56. (K) J. Gentle, M. Kleppmann, Collaborative text editing with Eg-walker, EuroSys 2025:
[arXiv 2409.14252](https://arxiv.org/abs/2409.14252).

<a id="n57"></a>N57. Antithesis, deterministic simulation testing:
[docs](https://antithesis.com/docs/resources/deterministic_simulation_testing/); etcd, autonomous testing with
Antithesis (2025): [post](https://etcd.io/blog/2025/autonomus_testing_with_antithesis/).

<a id="n58"></a>N58. Fortune, Antithesis (2026-03-23):
[article](https://fortune.com/2026/03/23/antithesis-janestreet-ethereum-will-wilson-foundationdb-software).

<a id="n59"></a>N59. Tokio, Announcing turmoil (2023-01-03):
[post](https://tokio.rs/blog/2023-01-03-announcing-turmoil); [docs](https://docs.rs/turmoil).

<a id="n60"></a>N60. Unity, PlayMode Tools window:
[docs](https://docs.unity3d.com/Packages/com.unity.netcode@1.9/manual/playmode-tool.html); Multiplayer Play Mode:
[docs](https://docs.unity3d.com/Packages/com.unity.multiplayer.playmode@1.3/manual/index.html).

<a id="n61"></a>N61. IETF, qlog main schema -14 (July 2026):
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-quic-qlog-main-schema/); QUIC events -13:
[draft](https://datatracker.ietf.org/doc/html/draft-ietf-quic-qlog-quic-events-13).

<a id="n62"></a>N62. SSLKEYLOGFILE draft -05, published as RFC 9850 (December 2025):
[draft](https://www.ietf.org/archive/id/draft-ietf-tls-keylogfile-05.html);
[RFC 9850 diff](https://ftp.nic.ad.jp/rfc/authors/rfc9850-diff.html).

<a id="n63"></a>N63. (K) Shadow simulator: [site](https://shadow.github.io).

<a id="n64"></a>N64. (K) madsim: [repo](https://github.com/madsim-rs/madsim).

<a id="n65"></a>N65. Polar Signals, OpenTelemetry profiling goes alpha (2026-03-26):
[post](https://blog.polarsignals.com/blog/posts/2026/03/26/opentelemetry-profiling-goes-alpha).

<a id="n66"></a>N66. Sentry, Seer is generally available:
[changelog](https://sentry.io/changelog/seer-sentrys-ai-debugger-is-generally-available).

<a id="n67"></a>N67. Business Wire, Sentry adds local development and code review debugging to Seer (2026-01-27):
[release](https://www.businesswire.com/news/home/20260127739891/en/Sentry-Adds-Local-Development-and-Code-Review-Debugging-to-Seer).

<a id="n68"></a>N68. Google DeepMind, Introducing CodeMender (2025-10-06):
[post](https://deepmind.google/blog/introducing-codemender-an-ai-agent-for-code-security/).

<a id="n69"></a>N69. Google, From finding to fixing: OSS-Fuzz with CodeMender (2026-07-29):
[post](https://blog.google/security/from-finding-to-fixing-reducing-maintainer-burden-with-automated-patches/).

<a id="n70"></a>N70. GitHub, agentic autofix for code scanning alerts GA (2026-10-15):
[changelog](https://github.blog/changelog/2026-10-15-agentic-autofix-for-code-scanning-alerts-general-availability).

<a id="n71"></a>N71. GitHub, risks and mitigations for Copilot cloud agent:
[docs](https://docs.github.com/en/copilot/concepts/agents/cloud-agent/risks-and-mitigations).

<a id="n72"></a>N72. Wang, Pradel, Liu, Are "solved issues" in SWE-bench really solved correctly? (ICSE 2026):
[arXiv 2503.15223](https://arxiv.org/abs/2503.15223).

<a id="n73"></a>N73. SWE-Bench+ (2024): [arXiv 2410.06992](https://arxiv.org/abs/2410.06992).

<a id="n74"></a>N74. MorphLLM, SWE-Bench Pro leaderboard (2026; secondary):
[page](https://www.morphllm.com/swe-bench-pro).

<a id="n75"></a>N75. JetBrains et al., TraceSim (2020):
[arXiv 2009.12590](https://arxiv.org/abs/2009.12590); S3M (2021): [arXiv 2103.10526](https://arxiv.org/abs/2103.10526).

<a id="n76"></a>N76. BugSplat, Breakpad (deprecated):
[docs](https://docs.bugsplat.com/introduction/getting-started/integrations/cross-platform/breakpad); Sauce Labs,
Backtrace acquisition (2021):
[post](https://saucelabs.com/blog/backtrace-acquisition-adds-error-monitoring-to-sauce-labs).

<a id="n77"></a>N77. Microsoft, Time Travel Debugging overview:
[docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/time-travel-debugging-overview).

<a id="n78"></a>N78. Wikipedia, Undo (company) (2026 funding; secondary):
[page](https://en.wikipedia.org/wiki/Undo_(company)).

<a id="n79"></a>N79. ICSME 2025 technical briefing, debugging with rr and Pernosco:
[session](https://conf.researchr.org/details/icsme-2025/icsme-2025-technical-briefings/3/Hands-On-Technical-Briefing-Advanced-Debugging-With-rr-And-Pernosco-).

<a id="n80"></a>N80. Microsoft DirectX blog, console-level developer tools on Windows (2026-03-12):
[post](https://devblogs.microsoft.com/directx/directx-bringing-console-level-developer-tools-to-windows/).

<a id="n81"></a>N81. ROS 2, Release Lyrical Luth:
[docs](https://docs.ros.org/en/lyrical/Releases/Release-Lyrical-Luth.html); release announcement:
[discourse](https://discourse.openrobotics.org/t/ros-2-lyrical-luth-released/55021).

<a id="n82"></a>N82. IETF DetNet working group: [about](https://datatracker.ietf.org/wg/detnet/about/); RFC 9023:
[info](https://rfc-editor.org/info/rfc9023).

<a id="n83"></a>N83. IETF QUIC WG documents (fetched 2026-10-27):
[documents](https://datatracker.ietf.org/wg/quic/documents/).

<a id="n84"></a>N84. (K) RFC 9298, Proxying UDP in HTTP; RFC 9484, Proxying IP in HTTP:
[RFC 9298](https://www.rfc-editor.org/rfc/rfc9298.html); [RFC 9484](https://www.rfc-editor.org/rfc/rfc9484.html).

<a id="n85"></a>N85. (K) RFC 8899, Packetization Layer Path MTU Discovery for datagram transports:
[RFC](https://www.rfc-editor.org/rfc/rfc8899.html).

<a id="n86"></a>N86. (K) msquic, quiche, ngtcp2, quinn, picoquic, s2n-quic, lsquic project repositories (see
[Needs a download](#needs-a-download)).

<a id="n87"></a>N87. (K) QUIC interop runner: [results](https://interop.seemann.io).

<a id="n88"></a>N88. (K) NSA, Commercial National Security Algorithm Suite 2.0:
[CNSA 2.0](https://media.defense.gov/2022/Sep/07/2003071834/-1/-1/0/CSA_CNSA_2.0_ALGORITHMS_.PDF).

<a id="n89"></a>N89. (K) RFC 9420, MLS protocol; RFC 9750, MLS architecture:
[RFC 9420](https://www.rfc-editor.org/rfc/rfc9420.html); [RFC 9750](https://www.rfc-editor.org/rfc/rfc9750.html).

<a id="n90"></a>N90. (K) Cremers et al., A comprehensive symbolic analysis of TLS 1.3 (CCS 2017):
[project](https://tls13tamarin.github.io/TLS13Tamarin/).

<a id="n91"></a>N91. (K) RFC 7696 (BCP 201), algorithm agility; RFC 8701, GREASE; RFC 9287, greasing the QUIC bit:
[RFC 7696](https://www.rfc-editor.org/rfc/rfc7696.html); [RFC 8701](https://www.rfc-editor.org/rfc/rfc8701.html).

<a id="n92"></a>N92. (K) RFC 8879, TLS certificate compression: [RFC](https://www.rfc-editor.org/rfc/rfc8879.html).

<a id="n93"></a>N93. (K) GGPO (MIT licence since 2019): [repo](https://github.com/pond3r/ggpo).

<a id="n94"></a>N94. (K) GDC talks: T. Ford, Overwatch gameplay architecture and netcode (2017); D. Aldridge, I shot you
first: networking the gameplay of Halo: Reach (2011); J. Cone, It IS rocket science! The physics of Rocket League
detailed (2018); Valve, Source multiplayer networking:
[wiki](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking).

<a id="n95"></a>N95. Prima Games, Counter-Strike 2 tick rate changes explained (secondary):
[article](https://primagames.com/tips/counter-strike-2-tick-rate-changes-explained).

<a id="n96"></a>N96. (K) Valve GameNetworkingSockets: [repo](https://github.com/ValveSoftware/GameNetworkingSockets).

<a id="n97"></a>N97. (K) RFC 8216 HTTP Live Streaming and Apple's low-latency extension:
[RFC 8216](https://www.rfc-editor.org/rfc/rfc8216.html).

<a id="n98"></a>N98. (K) mediasoup, Janus, Jitsi Videobridge and Pion project repositories (see
[Needs a download](#needs-a-download)).

<a id="n99"></a>N99. (K) RFC 8627 FlexFEC; RFC 2198 RED; libwebrtc NetEQ and AEC3; RNNoise; DeepFilterNet:
[RFC 8627](https://www.rfc-editor.org/rfc/rfc8627.html); [RFC 2198](https://www.rfc-editor.org/rfc/rfc2198.html).

<a id="n100"></a>N100. (K) IEEE 1588-2019 (PTP), IEEE 802.1AS (gPTP), AES67, Dante and AVB/Milan:
[AES67](https://www.aes.org/publications/standards/search.cfm?docID=96).

<a id="n101"></a>N101. (K) M. Weidner, M. Kleppmann, The art of the Fugue (2023):
[arXiv 2305.00583](https://arxiv.org/abs/2305.00583).

<a id="n102"></a>N102. (K) W. Xia et al., FastCDC, USENIX ATC 2016:
[paper](https://www.usenix.org/conference/atc16/technical-sessions/presentation/xia).

<a id="n103"></a>N103. (K) FoundationDB, simulation and testing (BUGGIFY):
[docs](https://apple.github.io/foundationdb/testing.html).

<a id="n104"></a>N104. (K) TigerBeetle, VOPR and simulation testing:
[docs](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/internals/vopr.md).

<a id="n105"></a>N105. (K) Jepsen and the Elle checker: [Jepsen](https://jepsen.io);
[Elle](https://github.com/jepsen-io/elle).

<a id="n106"></a>N106. (K) Linux `tc-netem` manual: [man page](https://man7.org/linux/man-pages/man8/tc-netem.8.html).

<a id="n107"></a>N107. (K) clumsy (WinDivert-based): [site](https://jagt.github.io/clumsy/).

<a id="n108"></a>N108. (K) Shopify Toxiproxy: [repo](https://github.com/Shopify/toxiproxy).

<a id="n109"></a>N109. (K) Mahimahi: [site](http://mahimahi.mit.edu).

<a id="n110"></a>N110. (K) ns-3 and Mininet: [ns-3](https://www.nsnam.org); [Mininet](https://mininet.org).

<a id="n111"></a>N111. (K) Epic, network emulation and Networking Insights in Unreal Engine:
[docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-network-emulation-in-unreal-engine).

<a id="n112"></a>N112. (K) Apple, Network Link Conditioner (Xcode Additional Tools):
[developer downloads](https://developer.apple.com/download/all/).

<a id="n113"></a>N113. (K) IETF OPSAWG, PCAP Now Generic (pcapng) with the Decryption Secrets Block:
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-opsawg-pcapng/).

<a id="n114"></a>N114. (K) Microsoft, Packet Monitor (pktmon):
[docs](https://learn.microsoft.com/en-us/windows-server/networking/technologies/pktmon/pktmon).

<a id="n115"></a>N115. (K) W3C Trace Context: [Recommendation](https://www.w3.org/TR/trace-context/).

<a id="n116"></a>N116. (K) Crashpad: [project](https://chromium.googlesource.com/crashpad/crashpad).

<a id="n117"></a>N117. (K) Sentry, issue grouping:
[docs](https://docs.sentry.io/concepts/data-management/event-grouping/).

<a id="n118"></a>N118. (K) Microsoft symbol server protocol and debuginfod:
[SymSrv](https://learn.microsoft.com/en-us/windows/win32/debug/symbol-servers-and-symbol-stores);
[debuginfod](https://sourceware.org/elfutils/Debuginfod.html).

<a id="n119"></a>N119. (K) Apple crash reports and MetricKit; Android tombstones; Windows Error Reporting LocalDumps:
[WER](https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps).

<a id="n120"></a>N120. EDPB, Guidelines 01/2025 on pseudonymisation (January 2025):
[news](https://edpb.europa.eu/news/news/2025/edpb-adopts-pseudonymisation-guidelines-and-paves-way-improve-cooperation_en).

<a id="n121"></a>N121. (K) RFC 8445 ICE, RFC 8489 STUN, RFC 8656 TURN, RFC 8838 Trickle ICE:
[RFC 8445](https://www.rfc-editor.org/rfc/rfc8445.html).

<a id="n122"></a>N122. (K) RFC 6330, RaptorQ forward error correction:
[RFC](https://www.rfc-editor.org/rfc/rfc6330.html).

<a id="n123"></a>N123. (K) IETF WEBTRANS, WebTransport over HTTP/2:
[datatracker](https://datatracker.ietf.org/doc/draft-ietf-webtrans-http2/).

<a id="n124"></a>N124. (K) RFC 6455 WebSocket, RFC 8441 (HTTP/2), RFC 9220 (HTTP/3):
[RFC 9220](https://www.rfc-editor.org/rfc/rfc9220.html).

<a id="n125"></a>N125. (K) Comcast network condition simulator: [repo](https://github.com/tylertreat/comcast).

<a id="n126"></a>N126. Chrome, Direct Sockets for Isolated Web Apps:
[docs](https://developer.chrome.com/docs/iwa/direct-sockets);
Blink-dev PSA on permission policies (June 2026):
[thread](https://groups.google.com/a/chromium.org/g/blink-dev/c/5cSZjgnJelk/m/K56ic8m_AgAJ).
