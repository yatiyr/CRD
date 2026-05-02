# Phase 4 — Extensibility + Networking

**Status:** ⏳ planned

Cerid stops being "the engine" and becomes "the substrate other people
build on." Phase 4 also adds the networking layer that unlocks multi-user,
multi-node, and connected-simulation use cases.

## Slices

### 4.0 — C++ hot-reload scripting

| Slice | Module / Topic                       | Notes                                                                          |
| :---: | ------------------------------------ | ------------------------------------------------------------------------------ |
| 4.0a  | `crd-scripting` C++ hot-reload       | DLL reload via DynamicLibrary; suspend job system, swap DLL, resume            |
| 4.0b  | C ABI plugin boundary                | stable versioned C facade; all persistent state lives in engine-owned memory   |
| 4.0c  | scripting cookbook                   | published patterns: gameplay tick, custom layers, asset hooks                  |

### 4.1 — Advanced math

| Slice | Module / Topic                       | Notes                                                                          |
| :---: | ------------------------------------ | ------------------------------------------------------------------------------ |
| 4.1a  | `crd-math` dense numerical           | small dense solves, factorisations, least squares                              |
| 4.1b  | `crd-math` sparse + iterative        | CSR / CSC, CG, BiCGSTAB, GMRES, preconditioners                                |
| 4.1c  | `crd-math` parallel solvers          | task-graph driven via `crd-jobs`; SIMD specialization                          |
| 4.1d  | `crd-math` robust geometry           | exact predicates, clipping, hulls, intersection robustness                     |

### 4.2 — Networking

| Slice | Module / Topic                       | Notes                                                                          |
| :---: | ------------------------------------ | ------------------------------------------------------------------------------ |
| 4.2a  | `crd-net` transport layer            | UDP socket abstraction; reliable ordered + unreliable unordered channels; platform-neutral; standalone (no scene/job dependency) |
| 4.2b  | deterministic simulation substrate   | fixed-step loop, input log (timestamped + serializable), snapshot serialization, deterministic replay from snapshot + log |
| 4.2c  | client-server sync + rollback        | authoritative server, client-side prediction, Ggpo-style rollback netcode for action scenarios; extrapolation mode for tolerant scenarios |
| 4.2d  | session / lobby API                  | backend-neutral: `crd-net-steam` adapter, `crd-net-raw` (UDP/LAN), peer discovery, matchmaking stubs |
| 4.2e  | ROS2 bridge                          | bidirectional `crd-net` ↔ ROS2 topic/service adapter; prerequisite for Phase 8.0 robotics digital-twin |

## Decisions

- ADR-0034 — C++ hot-reload DLL scripting as primary scripting mechanism
- ADR-0035 — Networking architecture principles (layered, determinism-first)
