# ADR-0035 — Networking architecture principles

**Status:** Accepted  
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

A thin, platform-neutral socket abstraction:
- Reliable ordered channel (sliding window retransmission).
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
- Mode (rollback vs. extrapolation) is a per-session configuration
  parameter, not a compile-time choice.

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
- Phase 4.2e (ROS2 bridge) depends on Phase 4.2a transport and Phase 8.0
  robotics module.
- Phase 8.0c (robotics ROS2 bridge) depends on Phase 4.2e.
