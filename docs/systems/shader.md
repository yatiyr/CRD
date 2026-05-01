# crd-shader

Backend-neutral shader/effect envelope above RHI and below the future
renderer/material system. `crd-shader` exists so the engine can talk about
effects, variants, metadata, hot reload, and caching without leaking GLSL,
SPIR-V, or Vulkan types through the public API.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| 2.3a | public envelope + opaque handles | ✅ |
| 2.3b | frontend → IR seam + GLSL ingest | ✅ |
| 2.3c | reflection consumption | ✅ |
| 2.3d | variant key + mechanism policy | ✅ |
| 2.3e | cache hierarchy | ✅ |
| 2.3f | hot reload | ✅ |
| 2.3g | pipeline handoff / descriptor growth | ✅ |

## Core decisions

- No Vulkan / SPIR-V / GLSL types in the public API.
- SPIR-V is the canonical IR, and 2.3b now proves the first real lowering path.
- Reflection is core later, but 2.3a/2.3b only prove the public envelope and the
  runtime seam.
- A second frontend (e.g. node editor) must be able to plug in without
  consumer-side API changes.

## What ships today

- Opaque public handles:
  - `ModuleHandle`
  - `EffectHandle`
  - `VariantHandle`
- Public metadata/value types:
  - `Stage`
  - `ParameterClass`
  - `PassType`
  - `AlphaMode`
  - `RenderPath`
  - `ParameterDesc`
  - `DescriptorBindingDesc`
  - `PushConstantRangeDesc`
  - `VertexAttributeLayoutDesc`
  - `VariantKey`
  - `VariantRequest`
  - `SpecializationValue`
  - `CompileDiagnostics`
  - `ReloadEvent`
- Public interfaces:
  - `Effect`
  - `Runtime`
- Minimal runtime implementation:
  - `create_effect()`
  - `find_effect()`
  - `find_module()`
  - `request_variant()`
  - `is_variant_ready()`
  - `variant_modules()`
  - `reload_effect()`

## What ships today (2.3b — frontend → IR seam + GLSL ingest)

- real GLSL textual frontend path
- runtime-loaded `shaderc_shared` integration through Cerid's own platform
  dynamic-library layer (no static CRT mismatch)
- frontend modules described through Cerid-owned `FrontendCompileRequest`
- internal canonical IR artifact stored as SPIR-V words
- variant requests now compile frontend modules into canonical IR modules
- module metadata is queryable without exposing code bytes publicly

This slice still intentionally stops short of reflection, cache, and hot
reload. It proves the frontend→IR seam, not the full shader system.

## What ships today (2.3c — reflection consumption)

- `spirv-reflect` is now wired into the compile path
- compiled modules own reflected metadata for:
  - descriptor bindings
  - push constant ranges
  - vertex attribute requirements
  - material-parameter discovery
- effects now aggregate reflection-derived metadata from their compiled modules
- the frontend→IR seam is no longer abstract only; it has a real consumer

Still intentionally outside this slice:

- cache hierarchy
- hot reload behavior beyond the observable envelope
- variant-key policy enforcement
- PSO/layout handoff details beyond the reflected metadata surface

## What ships today (2.3d — variant key + mechanism policy)

- deterministic structural `VariantKey` generation from `VariantRequest`
- typed structural axes encoded in the key:
  - pass type
  - skinned/static
  - alpha mode
  - render path
- specialization values are kept out of the structural key by design
- public mechanism-policy helper surface:
  - `VariantAxis`
  - `Mechanism`
  - `MechanismDecision`
  - `decide_mechanism()`
- runtime variants now retain and expose their structural key

This slice still intentionally stops short of cache and hot reload. It locks in
the structural-variant rule before cache identity is built on top of it.

## What ships today (2.3e — cache hierarchy)

- explicit key tiers in diagnostics:
  - `SourceKey`
  - `PreprocessedKey`
  - `SpirvKey`
- local-include preprocessing with include-graph participation in the
  preprocessed/SPIR-V identity path
- in-memory caches for:
  - source text
  - preprocessed text
  - SPIR-V words
  - module handle reuse by SPIR-V key
- on-disk SPIR-V cache under `cache/shaders/`
- compile diagnostics now expose cache hit/miss behavior without leaking the
  backend implementation details into consumers

This is still intentionally not the full cache story:

- no Vulkan `VkPipelineCache` integration yet
- no hot-reload invalidation policy yet
- no persistent cache metadata/index file yet

## What ships today (2.3f — hot reload)

- effect reload is now real and observable
- reload compiles a fresh replacement off to the side
- live state only swaps if the full compile/reflection/cache path succeeds
- failed reload preserves the last-good live state
- `ReloadEvent` now has real meaning:
  - `succeeded`
  - `using_last_good`
- existing `EffectHandle` remains stable across reload
- existing variant handles remain queryable across reload

This slice still intentionally stops short of the final PSO/layout boundary.

## What ships today (2.3g — pipeline handoff / descriptor growth)

- backend-neutral `VariantPipelineDesc`
- `Runtime::describe_variant()`
- handoff includes:
  - compiled module usage list
  - normalized descriptor bindings
  - normalized push constant ranges
  - vertex attribute requirements
- descriptor visibility is merged across stages at the handoff surface
- `crd-shader` now explicitly owns the shader-side inputs to PSO creation,
  while `crd-rhi` continues to own native pipeline objects

## How to use it

```cpp
auto runtime = crd::shader::create_runtime();

crd::shader::EffectDesc desc;
desc.name = crd::containers::String("basic");
desc.source_path = crd::containers::String("shaders/basic.shader");

auto effect = runtime->create_effect(desc);

crd::shader::CompileDiagnostics diagnostics;
crd::shader::VariantCompileRequest request;
request.effect = effect;
request.variant.pass_type = crd::shader::PassType::MainColor;
request.variant.render_path = crd::shader::RenderPath::Forward;

auto variant = runtime->request_variant(request, diagnostics);
auto modules = runtime->variant_modules(variant);
auto* module0 = runtime->find_module(modules[0]);

auto descriptors = module0->descriptor_bindings();
auto push_constants = module0->push_constants();
auto vertex_attributes = module0->vertex_attributes();
auto key = runtime->variant_key(variant);
auto source_key = diagnostics.source_key;
auto spirv_key = diagnostics.spirv_key;
```

## Long-term direction

- 2.3b now proves the real frontend → IR seam and GLSL ingest.
- 2.3c now proves reflection-driven metadata ownership.
- 2.3d now proves structural variant identity and mechanism policy helpers.
- 2.3e now proves the first real cache hierarchy.
- 2.3f now proves atomic reload + last-good fallback.
- 2.3g now proves the PSO/layout handoff boundary.
- The material system and renderer will eventually consume this layer, not raw
  backend shader objects.
