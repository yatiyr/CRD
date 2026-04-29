# crd-shader

Backend-neutral shader/effect envelope above RHI and below the future
renderer/material system. `crd-shader` exists so the engine can talk about
effects, variants, metadata, hot reload, and caching without leaking GLSL,
SPIR-V, or Vulkan types through the public API.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| 2.3a | public envelope + opaque handles | ✅ |
| 2.3b | frontend → IR seam + GLSL ingest | ⏳ |
| 2.3c | reflection consumption | ⏳ |
| 2.3d | variant key + mechanism policy | ⏳ |
| 2.3e | cache hierarchy | ⏳ |
| 2.3f | hot reload | ⏳ |
| 2.3g | pipeline handoff / descriptor growth | ⏳ |

## Core decisions

- No Vulkan / SPIR-V / GLSL types in the public API.
- SPIR-V will be the canonical IR, but 2.3a does not implement ingestion yet.
- Reflection is core later, but 2.3a only proves the public envelope and the
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
- Minimal in-memory runtime implementation:
  - `create_effect()`
  - `find_effect()`
  - `request_variant()`
  - `is_variant_ready()`
  - `reload_effect()`

This runtime does **not** compile shaders yet. It only proves the shape and
ownership of the public envelope.

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
```

## Long-term direction

- 2.3b adds the real frontend → IR seam and GLSL ingest.
- 2.3c brings in reflection-driven metadata ownership.
- 2.3d–g add the real variant/cache/hot-reload/PSO growth.
- The material system and renderer will eventually consume this layer, not raw
  backend shader objects.
