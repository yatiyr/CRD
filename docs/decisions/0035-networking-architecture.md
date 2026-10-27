# ADR-0035 — Networking architecture principles

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Status:** Accepted; amended 2026-10-27 ([below](#amendment-2026-10-27-the-foundation-window-and-the-layers-above))  
**Date:** 2026-05-02  
**Tags:** arch, networking, determinism, simulation

## Context

Cerid must eventually support network-connected scenarios across all its
target verticals:

- **Games** — authoritative server + client prediction + rollback netcode
  for low-latency action games; lockstep for strategy/simulation.
- **Robotics / aerospace** — sensor data streams, digital-twin live sync,
  ROS2 topic/service interop.
- **Collaborative tools** — real-time co-editing of scenes, shared DAW
  sessions over LAN or cloud.

Networking requirements diverge sharply between verticals. A game rollback
layer is useless for a ROS2 bridge. A session/lobby API is irrelevant to
a headless simulation node. A naive monolithic networking module would force
all consumers to carry weight they do not need.

## Decision

**Layered, determinism-first networking (Phase 4.2).**

### Layer 1 — Transport (`crd-net`, Phase 4.2a)

~~A thin, platform-neutral socket abstraction:~~ A platform-neutral transport, QUIC with unreliable datagrams first
(struck and amended 2026-10-27, see [the amendment](#amendment-2026-10-27-the-foundation-window-and-the-layers-above)):
- Reliable ordered channel ~~(sliding window retransmission)~~ (QUIC streams; the raw UDP path keeps its own).
- Unreliable unordered channel (state snapshots, sensor feeds, position updates).
- No dependency on scenes, job system, or any other Cerid module. The
  transport compiles and runs standalone. All higher layers are optional
  consumers of this layer.

### Layer 2 — Determinism substrate (Phase 4.2b)

A fixed-step simulation loop with:
- **Input log**: timestamped input events serialized to disk and/or memory.
- **Snapshot serialization**: full engine state captured to a binary blob
  at a given tick. Snapshot format is module-declared; each module
  registers a serialize/deserialize pair.
- **Deterministic replay**: given initial snapshot + input log → identical
  output on replay, regardless of wall-clock timing.

This layer is **prerequisite for every higher-level networking feature**:
rollback netcode re-simulates diverged ticks; digital-twin sync diffs
successive snapshots; replay-based debugging uses the same path as
production networking.

### Layer 3 — Client-server sync + rollback (Phase 4.2c)

Standard client-server model:
- Server is authoritative — it owns the canonical simulation state.
- Clients run ahead with local prediction; server sends authoritative
  corrections when client and server states diverge.
- **Rollback (Ggpo-style)**: when a correction arrives for tick T, the
  client rolls back to T, re-applies correct inputs, and re-simulates
  forward. Suitable for action / fighting game scenarios.
- **Extrapolation mode**: server sends state at reduced rate; client
  dead-reckons. Suitable for strategy games, simulation viewers, and
  digital-twin dashboards where eventual consistency is acceptable.
- ~~Mode (rollback vs. extrapolation) is a per-session configuration
  parameter, not a compile-time choice.~~ Struck 2026-10-27: replication is a set of selectable, authored policies,
  still chosen per session and never at compile time
  ([amendment](#amendment-2026-10-27-the-foundation-window-and-the-layers-above)).

### Layer 4 — Session / lobby API (Phase 4.2d)

Backend-neutral session management:
- `crd-net-steam` adapter (Steamworks P2P and relay sessions).
- `crd-net-raw` adapter (raw UDP; LAN, headless sim, direct IP).
- ROS2 bridge is a separate optional adapter (`crd-net-ros2`, Phase 4.2e)
  that maps ROS2 topics/services onto the Layer 1 transport.

## Consequences

**Good:**
- Deterministic simulation is useful even without networking: debugging,
  CI replay, simulation validation, and automated test oracles all use it.
- Transport layer is reused across games, robotics, digital twins, and
  DAW sessions without modification.
- Rollback and replay share the same fixed-step substrate — no
  architectural duplication.
- Layered design allows shipping Layer 1 early without committing to a
  session API.

**Bad / costs:**
- Determinism requires discipline across all simulation modules: no
  system-clock reads in logic, no thread-scheduler-dependent ordering,
  floats must use consistent FTZ/DAZ flags across client and server.
  This is a per-module compliance cost.
- Full rollback netcode is architecturally simple but per-game tuning
  (rollback window depth, input-delay budgets) is significant work that
  lives in Phase 4.2c and game-specific code, not in the engine.
- ROS2 bridge (4.2e) requires ROS2 to be installed on the target machine;
  it is an optional adapter, not a required dependency.

## Dependencies

- Phase 3.0 (scene/ECS) and Phase 3.1 (physics) must expose deterministic
  snapshot serialization before Phase 4.2b can include them in the
  full-engine snapshot.
- Phase 4.2e (ROS2 bridge) depends on Phase 4.2a transport ~~and Phase 8.0
  robotics module~~ (struck 2026-10-27: 4.2e follows NET.5, and the robotics rows consume it).
- ~~Phase 8.0c (robotics ROS2 bridge) depends on Phase 4.2e.~~ Struck 2026-10-27: 8.0c folds into 4.2e, the one
  ROS 2 bridge.

## Amendment 2026-10-27: the foundation window and the layers above

The user decided on 2026-10-27 ([decision](../sessions/2026-10-27-user-decision-network-foundation-window.md)) that
Cerid gets one robust, secure network foundation used by every product class: games from duels to massively
multiplayer worlds, collaborative editors, DAW live sessions, Zoom-class calls, the crash pool and agent hubs. The
2026-10-26 research ([area 7](../research/2026-10-26-beyond-games-2026-comparison.md#gaps-in-area-7)) adds the layers
above it ([adoption](../sessions/2026-10-27-research-adopt-part-2.md)).

**The window.** [NETCRASH](../ROADMAP.md#slice-netcrash) runs directly after OWN and before AUD-2: NET.1 to NET.4,
4.2a, 4.2b, NET-SIM, NET-DEBUG and NET.5 (Windows and Linux), with the crash pool rows. Every later networked row
builds on it; browser channels qualify under WEB.5.

**Transport (4.2a).** QUIC (RFC 9000) with unreliable datagrams (RFC 9221) is the primary transport, with raw UDP and
TCP/TLS paths; browsers use WebTransport with a WebSocket fallback. Layer 1 stays standalone (no scene or job
dependency).

**Security baseline (NET.1 to NET.4).** A threat model covering every product class; authenticated encrypted sessions
with hybrid post-quantum key exchange (X25519 with ML-KEM-768) and post-quantum-ready signatures with classical
fallback; MLS (RFC 9420) for end-to-end encrypted groups that servers relay but cannot read; mutual authentication on
every service link; key rotation, revocation and hardware-backed keys where available; a formally verified handshake
and session state machine (model checked in CI); vetted cryptographic providers only. No expensive decode happens
before authorization. Continuous fuzzing of every parser and protocol state machine, measured flood and amplification
defence, and an independent security review before the first networked release.

**Deterministic simulation (NET-SIM)** is a required gate for every layer and every networked row: components run
with virtual clocks and sockets under injected loss, delay, reordering, duplication, partitions, clock skew, crashes and
restarts over millions of seeded schedules, and a failure replays exactly from its seed. The determinism substrate
(4.2b) and this simulator share the replay path.

**Conditioning and debugging (NET-DEBUG)** live inside `crd-net`, with no OS driver or admin rights: per peer,
channel and direction latency, jitter, random and bursty loss, duplication, reordering, corruption, bandwidth and
queue limits, MTU changes, NAT rebinding and outages; named profiles and seeded CHIR timelines changed at runtime; a
local cluster launcher with bot clients; qlog and pcapng capture correlated with DIAG; multi-peer record and replay with
a packet and replication inspector.

**Replication (Layer 3, 4.2c)** is a set of selectable, authored policies: lockstep, rollback, authoritative snapshot
interpolation with prediction and reconciliation, lag compensation, and interest management with priority budgets.
Rules and server logic are authored CEIR/CHIR; a server takes a new rule generation between ticks with a version
handshake. Compression, filtering and lag-compensation math use crd-hesap.

**Above the session API (Layer 4, 4.2d).**
- NET-SERVER: headless dedicated servers, fleet orchestration, matchmaking and backend adapters, regions and
  autoscaling; 4.2d keeps the backend-neutral session API and its local stubs.
- ANTICHEAT: server validation first, statistical and replay detection, user-mode client integrity, optional vendor
  adapters with Linux and Proton support, a privacy policy. It follows 4.2c and NET-SERVER.
- NET-SCALE: sharded persistent worlds with seamless handoff and re-sharding on NET-SERVER's fleets.
- NET-LIVE: Zoom-class real-time media (selective forwarding, NAT traversal, MLS group calls, WebRTC interop) on the
  audio graph and media rows, with its forwarding servers deployed through NET-SERVER.
- COLLAB: the authoritative project host on the same foundation; Automerge 3, Yjs and Loro are evaluated for text and
  independent properties, while graphs and topology stay transactional (ADR-0130).
- ROS 2: 4.2e is the one bridge.

**Consequences.** Every networked row carries the NET-SIM gate and the NET-DEBUG tooling as acceptance; the security
baseline costs a formal model, a fuzzing budget and an external review before the first networked release.
