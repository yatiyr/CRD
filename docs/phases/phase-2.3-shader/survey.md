# Phase 2.3 Shader Survey

## Filament

- **Steal:** aggressively data-driven material/shader pipeline boundaries and
  clear separation between authoring-time metadata and runtime shader state.
- **Avoid:** overfitting the design to one material language/toolchain shape.

## bgfx

- **Steal:** strong backend insulation and pragmatic shader permutation control.
- **Avoid:** a toolchain that is so custom that future frontends feel bolted on.

## The Forge

- **Steal:** explicit pipeline ownership and practical real-engine shader build
  workflow discipline.
- **Avoid:** backend-centric leakage into consumer-facing surfaces.

## Granite

- **Steal:** Vulkan-first realism, SPIR-V-centric thinking, and respect for
  reflection-driven layout derivation.
- **Avoid:** designing too close to one backend such that the public API stops
  being truly backend-agnostic.

## Unreal

- **Steal:** serious handling of variants, cache, and hot-reload observability.
- **Avoid:** permutation explosion without a sharply written mechanism policy.

## Unity SRP

- **Steal:** separation of high-level rendering policy from lower shader
  pipeline infrastructure.
- **Avoid:** treating shader variants as a dumping ground for every axis.

## Slang

- **Steal:** frontend/IR seam discipline and long-horizon language portability.
- **Avoid:** coupling the public API to today's textual frontend choice.

## Khronos stack

- **Steal:** use existing ecosystem pieces where they are strongest:
  `shaderc`, `spirv-reflect`, `VkPipelineCache`, `SPIR-V`.
- **Avoid:** reimplementing parsers/reflection infrastructure for ego reasons.

## Design commitments restated

- Structural axes become permutations only when they actually change shader
  structure or pipeline compatibility.
- Numeric axes become specialization constants when they are compile-time
  numeric decisions rather than material identity.
- Material parameters live in resource-binding/push-constant space, not in the
  permutation key unless they truly affect structure.
- SPIR-V is the canonical internal artifact and the seam between frontends and
  backend/cache.
- Reflection is non-optional and drives layouts rather than merely validating
  hand-authored descriptors after the fact.
- Hot-reload and cache identity are first-class design concerns, not polish.

## Explicit non-goals for Phase 2.3

- building a custom Cerid DSL in the first slice
- implementing the node editor itself
- locking in a material authoring workflow
- solving renderer policy and scene binding at the same time
- optimizing for exotic shader stages before the core path is stable
