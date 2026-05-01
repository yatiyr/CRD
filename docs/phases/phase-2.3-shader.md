# Phase 2.3 — `crd-shader`

**Status:** 🚧 active

`crd-shader` sits between the proven RHI/backend foundation and the future
renderer/material system. Its job is not to be "GLSL wrappers"; its job is to
provide a stable shader/effect layer with reflection, variant control, cache,
and hot-reload discipline.

## Load-bearing decisions (already made)

- **Hybrid mechanism model.**
  - permutations for structural variants
  - specialization constants for numeric variants
  - bindless / UBO / push constants for material parameters
  - dynamic branching only for cheap, low-frequency cases
- **SPIR-V is canonical IR.**
- **Reflection is core.** `spirv-reflect` drives layouts and material discovery.
- **Four-tier cache.** source → preprocessed → SPIR-V → VkPipeline, with
  `VkPipelineCache` underneath.
- **Hot-reload is early.** Atomic swap, last-good fallback, never crash the
  consumer on bad source.
- **No Vulkan / SPIR-V / GLSL types in public API.** Opaque handles only.

## Deliverables (design packet)

- `docs/phases/phase-2.3-shader/survey.md`
- `docs/phases/phase-2.3-shader/api-envelope.md`
- draft ADRs:
  - ADR-0025 — shader mechanism policy
  - ADR-0026 — shader variant key
  - ADR-0027 — reflection consumption model
  - ADR-0028 — shader cache hierarchy
  - ADR-0029 — shader hot reload
  - ADR-0030 — shader/PSO boundary
  - ADR-0031 — frontend → IR seam

## Slices

| Slice | Topic | Status | Notes |
| --- | --- | --- | --- |
| 2.3a | public envelope + opaque handles | ✅ | no backend/frontend leak in public API |
| 2.3b | frontend → IR seam + GLSL ingest | ✅ | GLSL via shaderc now, second frontend later |
| 2.3c | reflection consumption | ✅ | `spirv-reflect` drives descriptor/push/vertex metadata |
| 2.3d | variant key + mechanism policy | ✅ | typed variant key, rejection criteria, no `#define` soup |
| 2.3e | cache hierarchy | ✅ | content-addressed keys, include graph in hash |
| 2.3f | hot reload | ✅ | atomic swap, last-good fallback, observability |
| 2.3g | pipeline handoff / descriptor growth | ⏳ | what `crd-shader` produces, what RHI consumes |

Every slice must be independently shippable and keep the triangle path alive.

## Near-term execution order

1. **2.3g — pipeline handoff / descriptor growth**

This order is deliberate: it retires the biggest architectural risks early
without pretending the material/renderer side already exists.

## 2.3a shipped

Shipped in session: `docs/sessions/2026-04-29-shader-2.3a-envelope.md`

Delivered:

- `crd-shader` module scaffold
- opaque public handles (`ModuleHandle`, `EffectHandle`, `VariantHandle`)
- public metadata/value types and minimal `Effect` / `Runtime` interfaces
- minimal in-memory runtime proving effect/variant/reload observability
- tests and smoke for the public envelope

## 2.3b shipped

Shipped in session: `docs/sessions/2026-04-29-shader-2.3b-glsl-ingest.md`

Delivered:

- runtime-loaded `shaderc_shared` frontend integration
- `FrontendCompileRequest` used as the real frontend → IR seam
- GLSL source file compilation into canonical internal SPIR-V modules
- module metadata query path (`find_module`, `variant_modules`)
- tests and smoke for successful and failing GLSL ingest

## 2.3c shipped

Shipped in session: `docs/sessions/2026-04-29-shader-2.3c-reflection.md`

Delivered:

- `spirv-reflect` integration in the compile path
- descriptor binding extraction
- push-constant extraction
- vertex input extraction
- material-parameter discovery
- effect-level metadata aggregation from compiled modules

## 2.3d shipped

Shipped in session: `docs/sessions/2026-04-29-shader-2.3d-variant-key.md`

Delivered:

- deterministic structural variant key generation
- specialization values kept out of structural identity
- typed mechanism-policy helper surface
- runtime access to per-variant structural keys
- tests and smoke for key stability and policy behavior

## 2.3e shipped

Shipped in session: `docs/sessions/2026-04-30-shader-2.3e-cache-hierarchy.md`

Delivered:

- content-addressed source/preprocessed/SPIR-V key path
- include-graph-aware preprocessing
- in-memory source/preprocessed/SPIR-V caches
- on-disk SPIR-V cache
- cache-hit observability through compile diagnostics

## 2.3f shipped

Shipped in session: `docs/sessions/2026-04-30-shader-2.3f-hot-reload.md`

Delivered:

- atomic reload path
- last-good fallback on failed reload
- stable effect handle across reload
- stable variant handle lookup across reload
- tests and smoke for successful + failed reload behavior

## Decisions

- ADR-0025 — shader mechanism policy (DRAFT)
- ADR-0026 — shader variant key (DRAFT)
- ADR-0027 — reflection consumption model (DRAFT)
- ADR-0028 — shader cache hierarchy (DRAFT)
- ADR-0029 — shader hot reload (DRAFT)
- ADR-0030 — shader / PSO boundary (DRAFT)
- ADR-0031 — frontend → IR seam (DRAFT)

## Forward-fit test

Every load-bearing ADR must answer:

- Does this break when the node editor lands?
- Does this break when materials need per-instance parameter variation?
- Does this break when compute / mesh shaders / ray tracing land?
- Does this break if GLSL is swapped for Slang in two years?

## Forbidden

- permutations as ungoverned `#define` soup
- single-tier cache
- reflection deferred to "later"
- frontend coupling that forces API changes for a second frontend
- hand-rolled SPIR-V parsing when libraries exist
- a from-scratch DSL in slice 1

## Non-goals for 2.3

- material authoring UX
- scene integration
- full renderer policy
- node-editor implementation
- generalized render graph design
