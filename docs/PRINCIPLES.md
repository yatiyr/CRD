# Cerid engineering principles

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](ROADMAP.md); current rules: [AGENTS](../AGENTS.md).

Current architectural rules; scheduling lives only in [ROADMAP](ROADMAP.md). Departures require an explicit
decision/ADR. The [previous wording and rationale](archive/2026-09-12-orientation-history.md#docs-principles) are history.

## Product and module boundaries

Cerid is a general-purpose engine substrate. Games, simulation/robotics, medical visualization, CAD/CAM, DAWs,
scientific work and offline cinema are equal-class consumers. Public modules are separable; applications must
omit unused systems at link/package time. Dependencies go one way. Vendor types stay behind Cerid-owned interfaces.
Renderer, eylem physics, audio and product UI remain Cerid-owned implementations, not wrappers around a vendor engine.

Build vertical end-to-end slices against real consumers. Measure a workload before optimization or a major path
replacement. Correctness precedes speed. Determinism is a declared, reachable tier with appropriate replay/precision
proofs, never an unsupported blanket claim. Verification follows [BUILDING](BUILDING.md); reasoning follows [SANITY](SANITY.md).

## Authorable execution — the central contract

CHIR is high-level authoring; CEIR is canonical execution; CKIR is the device shader/kernel form
([ADR-0109](decisions/0109-ceir-chir-ckir-ownership-and-module-placement.md)). UI documents, styles, pipelines,
frame graphs and behaviours are editable assets. The host owns generic mechanisms and executes programs; it does
not encode a rendering technique in application/engine C++.

Shipping algorithms use committed, directly authorable CEIR/CKIR assets. CKIR `.ckir` node graphs round-trip through
`ckir_write`/`ckir_read`; CEIR is parse/print authorable. Domain declarations and cookers are frontends of the same
canonical IR. A bootstrap builder is allowed only to print an asset, verify it, and be deleted from the shipping path.
Declarations plus a hard-coded algorithm builder are not the final authorable form.

Every rendering technique is an authored frame graph, including shadows, GI, post, picking, overlays and UI effects.
`FrameGraphBuilder` is for tests, node editors and runtime-generated graphs; it is not the shipping source for engine
techniques. Missing expressiveness requires extending the canonical contract, never bypassing it.
Frame authoring converges through CEIR ([ADR-0127](decisions/0127-ceir-frame-dialect-and-converter.md)); preserve the
existing public runtime surface, and do not build a second UI/node scheduler beside it.

**Completion test:** edit/shadow the built-in asset from an app, cook and install it while running, observe the new
behaviour without an engine rebuild, and retain the previous working generation on failure. Delete the replaced
C++ algorithm path and prove the application still renders. Cooking alone, a parallel legacy path or an unreachable
library does not satisfy the contract. UI layout metadata must not change semantic program identity.

Human authoring uses text; runtime installs verified cooked binaries. A development/editor host may invoke cookers;
shipping source import is not an implicit fallback. Small configuration remains its explicit existing exception.

## Current ownership

- `gpu-context` owns device/program/pipeline interfaces. Backend-specific shading languages/bytecode stay inside
  providers. The former rhi/renderer/shader stack is retired ([ADR-0105](decisions/0105-retire-rhi-renderer-gpu-context-is-the-graphics-layer.md)).
- Scene owns the gameplay/simulation ECS and spatial hierarchy. UI has a separate retained `UiWorld` and distinct
  `UiNodeId`; explicit attachments integrate world-space UI. This user-chosen ADR-0107 D2 supersedes the old UI-in-scene
  clauses. The full [ADR-0107](decisions/0107-ui-2d-architecture.md) remains Proposed.
- Canvas is shared 2D composition. Cerid owns fonts/shaping/vector rendering. ImGui remains debug/recovery only.
  Real-time and offline rendering, UI and CR-D007 share asset/property/command contracts and polished visual quality.
- Generic intrusive references belong to memory; resource ownership/eviction/reload references belong to resources.
- Eylem remains Cerid-native, deterministic by construction; geometry-before-physics prerequisites remain intact.
  Current delivery order is [ADR-0129](decisions/0129-renderer-ui-editor-delivery-order.md), not an older phase schedule.
- Typed properties/commands are shared by in-process GUI, CLI and RPC/MCP. UI does not spawn a CLI per input event.
  Version schemas; preserve the ADR-0081 deprecation window of at least two minor versions for removed schemas.
  Existing commands are partial implementation, not proof of universal command coverage.
- C++ remains a first-class native/provider/authoring interface; its builders emit canonical assets. Cerid owns
  CHIR rather than embedding Lua/Python/JavaScript/GDScript/Wren as its product execution language (ADR-0108).

## Units and representation

Use `Quantity<D,T>` for physical values at public APIs, ECS/config/cooker/UI and cross-module boundaries. SI is
canonical inside the typed layer; convert authoring/display units at its edges. Precision (`f32`/`f64`) is independent
of dimension. Math/SIMD/numerical kernels and GPU/file/wire payloads use raw scalars deliberately. Bridge once at the
boundary with `.value`, `to_raw_vec`/`from_raw_vec` or strip-compute-retag. No upper-layer opt-out
([ADR-0078](decisions/0078-units-substrate-architecture.md)).

Use Cerid owning containers in engine/tools/tests; non-owning standard views and algorithms are permitted.
RAII, allocator ownership, naming and interface stability rules are in [CODING](CODING.md).

## Cross-domain product contracts

Shared assets and commands serve agents and humans. Inference and training qualify together; native/CHIR functions
share reflected effects and capabilities. Project collaboration and multiplayer retain distinct authoritative models.
Memory/task/device lifetimes, clocks/frames and trust boundaries remain explicit. Apply the
[quality contract](design/system-quality-contract.md); measured evidence defines support, not module names.
The [expanded review](research/2026-09-12-cerid-whole-system-review.md) routes future domain requirements to ROADMAP.
