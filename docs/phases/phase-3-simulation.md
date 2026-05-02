# Phase 3 — Simulation foundation

**Status:** ⏳ planned

Cerid's first move beyond pure rendering. Each slice is a module with a
public Cerid API and a backend implementation, so consumers can opt out.

## Slice order rationale

Scene/ECS (3.0) ships before Physics (3.1). Physics integration requires a
scene to sync transforms into; without a scene there is nowhere to attach
rigid bodies. All later slices (animation, audio, culling) share the same
dependency on the scene graph.

## Slices

| Slice | Module / Topic                  | Notes                                                                                |
| :---: | ------------------------------- | ------------------------------------------------------------------------------------ |
| 3.0a  | `crd-scene` graph               | hybrid: hierarchy + entity/component; UI nodes share the tree; prerequisite for 3.1+ |
| 3.0b  | entity/component storage        | SoA component arrays; hierarchical traversal kept separate                           |
| 3.0c  | scene serialization             | TOML authoring → cooked binary runtime; asset_cooker integration                     |
| 3.0d  | first real scene                | camera (FPS + orbit) + meshes via cooked assets + skybox                             |
| 3.1a  | `crd-physics` interface         | rigid body, collider, constraint, world, query API; backend-neutral                  |
| 3.1b  | `crd-physics-physx` backend     | PhysX 5.x as the first backend                                                       |
| 3.1c  | physics ↔ scene integration     | transform sync; fixed-step option; deterministic mode flag; requires Phase 3.0       |
| 3.2a  | `crd-animation` skeletal        | skeletons, clips, sampling, GPU skinning path                                        |
| 3.2b  | blend trees                     | 1D / 2D blends, additive, layer masks                                                |
| 3.2c  | inverse kinematics              | two-bone IK, FABRIK, target/pole constraints                                         |
| 3.2d  | cinematic timeline              | track-based authoring, deterministic playback, asset binding                         |
| 3.3a  | `crd-audio` graph               | low-latency mix graph, DAW-grade jitter targets                                      |
| 3.3b  | spatialization                  | HRTF / panning, occlusion hooks                                                      |
| 3.3c  | DAW-facing extensions           | sample-accurate scheduling, plugin host scaffold                                     |
| 3.4   | PBR + lighting                  | punctual lights (point/spot/directional), Cook-Torrance, IBL, CSM                    |
| 3.5   | Post-FX                         | HDR, ACES tonemap, bloom, TAA. SSAO/SSR explicitly deferred.                         |
| 3.6   | BVH-accelerated culling         | dynamic AABB tree; replaces brute-force frustum culling at scale                     |

## Decisions

- ADR-0018 — Physics architecture (PhysX backend, native later)
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0021 — Animation architecture
- ADR-0022 — Open-world streaming pipeline
