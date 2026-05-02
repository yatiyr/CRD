# Phase 8 — Domain Modules

**Status:** ⏳ planned

Cerid-native toolkits that unlock specialized vertical domains without
forcing all users to carry the weight. Each module is opt-in at link time.
Phase 8 validates the "general-purpose substrate" claim made in the engine's
identity statement by shipping production-grade tools for verticals beyond
games.

## Prerequisites

- Physics + scene (Phase 3) for robotics simulation loop
- Networking + determinism (Phase 4.2) for digital-twin sync and ROS2 bridge
- Advanced math base (Phase 4.1) for SE(3), sparse solvers, autodiff substrate
- Editor (Phase 7) for cinematic sequencer and procgen authoring tools

## Slices

### 8.0 — Robotics substrate

| Slice | Module / Topic               | Notes                                                                          |
| :---: | ---------------------------- | ------------------------------------------------------------------------------ |
| 8.0a  | `crd-robotics` kinematics    | URDF import, forward/inverse kinematics, SE(3) rigid body tree                 |
| 8.0b  | `crd-robotics` control       | PID/MPC control primitives, joint torque/velocity commands                     |
| 8.0c  | `crd-robotics` ROS2 bridge   | bidirectional crd-net ↔ ROS2 topic/service adapter (depends on Phase 4.2e)     |
| 8.0d  | `crd-robotics` sim loop      | deterministic fixed-step loop with sensor noise injection; digital-twin sync   |

### 8.1 — Aerospace and simulation tooling

| Slice | Module / Topic               | Notes                                                                          |
| :---: | ---------------------------- | ------------------------------------------------------------------------------ |
| 8.1a  | `crd-aerospace` flight model | rigid body + aerodynamics (lookup-table lift/drag coefficients), atmosphere model |
| 8.1b  | `crd-aerospace` telemetry    | time-tagged sensor data streams, replay, live digital-twin sync via Phase 4.2  |

### 8.2 — Advanced math extensions

These extend the Phase 4.1 math module with domain-specific structures.

| Slice | Module / Topic               | Notes                                                                          |
| :---: | ---------------------------- | ------------------------------------------------------------------------------ |
| 8.2a  | `crd-math` SE(3) extensions  | Lie group arithmetic: exp/log maps, adjoint, twists, screws; required by 8.0   |
| 8.2b  | `crd-math` autodiff          | forward-mode dual numbers + reverse-mode tape; JAX-style gradient API; useful for control and ML |

### 8.3 — Cinematic production tools

| Slice | Module / Topic               | Notes                                                                          |
| :---: | ---------------------------- | ------------------------------------------------------------------------------ |
| 8.3a  | `crd-cinematic` sequencer    | frame-accurate timeline, EDL import/export, clip and take metadata             |
| 8.3b  | `crd-cinematic` camera rig   | dolly/crane/handheld motion presets, lens metadata, depth-of-field authoring   |
| 8.3c  | `crd-cinematic` render farm  | offline job dispatch via `crd-jobs`; tile-based progressive accumulation       |

### 8.4 — Procedural generation

| Slice | Module / Topic               | Notes                                                                          |
| :---: | ---------------------------- | ------------------------------------------------------------------------------ |
| 8.4a  | `crd-procgen` terrain        | heightmap synthesis (Perlin/Simplex/domain-warp), hydraulic erosion simulation |
| 8.4b  | `crd-procgen` rules          | L-system grammar, Wave Function Collapse, modular-kit layout solver            |
| 8.4c  | `crd-procgen` geometry       | Marching Cubes, SDF booleans, dual contouring, procedural mesh APIs            |

## Decisions

(none yet — slices will produce ADRs as they're designed)
