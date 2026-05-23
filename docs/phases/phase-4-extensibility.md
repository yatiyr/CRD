# Phase 4 — Extensibility + Networking

**Status:** ⏳ planned — **partially superseded; see notes below.**

> **Restructured 2026-05-22.** This file's original §4.0 (scripting) and §4.1 (advanced math) are superseded:
> - **§4.0 C++ hot-reload scripting** → folded into the consolidated **`docs/phases/phase-4.0-platform.md`** (Cerid Platform Layer: reflection + command interface + scripting + agents), which unifies it with ADR-0081 (agent-native CLI/RPC/MCP) and the reflection-codegen + ECS-scripting + packaging additions. The §4.0 table below is retained for history only.
> - **§4.1 Advanced math** → **obsolete**: entirely absorbed by **Phase 3.1.6 `crd-hesap`** (the elite numerical substrate; far beyond this stub's scope).
> - **§4.2 Networking** → still live; remains here (re-home as its own phase when it nears).

## Slices

### 4.0 — C++ hot-reload scripting  *(superseded — see `phase-4.0-platform.md`; retained for history)*

| Slice | Module / Topic                       | Notes                                                                          |
| :---: | ------------------------------------ | ------------------------------------------------------------------------------ |
| 4.0a  | `crd-scripting` C++ hot-reload       | DLL reload via DynamicLibrary; suspend job system, swap DLL, resume            |
| 4.0b  | C ABI plugin boundary                | stable versioned C facade; all persistent state lives in engine-owned memory   |
| 4.0c  | scripting cookbook                   | published patterns: gameplay tick, custom layers, asset hooks                  |

### 4.1 — Advanced math  *(OBSOLETE — absorbed by Phase 3.1.6 `crd-hesap`)*

| Slice | Module / Topic                       | Notes                                                                          |
| :---: | ------------------------------------ | ------------------------------------------------------------------------------ |
| 4.1a  | `crd-math` dense numerical           | small dense solves, factorisations, least squares                              |
| 4.1b  | `crd-math` sparse + iterative        | CSR / CSC, CG, BiCGSTAB, GMRES, preconditioners                                |
| 4.1c  | `crd-math` parallel solvers          | task-graph driven via `crd-jobs`; SIMD specialization                          |
| 4.1d  | `crd-math` robust geometry           | exact predicates, clipping, hulls, intersection robustness                     |

### 4.2 — Networking  *(live)*

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
