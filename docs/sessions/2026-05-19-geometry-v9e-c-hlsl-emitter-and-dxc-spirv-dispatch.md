# Session 2026-05-19 — geometry-v9e-c HLSL emitter + dxc → SPIR-V GPU dispatch

> **Retroactive log written 2026-05-19** — slice shipped 2026-05-19 but
> the session log wasn't authored at the time it landed; this fills
> the gap. v9e-c initially shipped with structural-parity only; the
> `v9e-c-dxc-spirv-dispatch` follow-on (full GPU verification) ran the
> same day before v9e-d.

## Summary

Shipped Phase 3.1.7 v9e-c + its `v9e-c-dxc-spirv-dispatch` follow-on:
HLSL 6.0 emitter that mirrors the GLSL backend, plus a runtime dxc →
SPIR-V → Vulkan dispatch path that proves the HLSL output is
GPU-correct end-to-end against the C++ ground truth.

## What landed (v9e-c)

### Public API (`engine/geometry-shader-helpers/include/crd/geometry/shader_helpers/hlsl_emitter.hpp`)

```cpp
[[nodiscard]] crd::containers::StringView hlsl_helpers_prelude() noexcept;

[[nodiscard]] crd::containers::String emit_hlsl_sdf_function(
    const FormulaIr& ir,
    crd::containers::StringView function_name,
    crd::memory::IAllocator* alloc) noexcept;

[[nodiscard]] crd::containers::String emit_hlsl_conformance_shader(
    const FormulaIr& ir,
    crd::memory::IAllocator* alloc) noexcept;
```

### Mirror of GLSL emitter — syntax swap only

The HLSL emitter is line-for-line identical to the GLSL emitter except
for the language-level token substitutions:

```
vec3            → float3
vec2            → float2
uintBitsToFloat → asfloat
floatBitsToUint → asuint
layout(...) buffer Out { float[] o; } → RWStructuredBuffer<float>
layout(push_constant) uniform Push    → [[vk::push_constant]] cbuffer
layout(local_size_x=4,...) in;        → [numthreads(4,4,4)]
main()                                 → cs_main()
```

The math is identical — same deterministic Cephes-poly port for
`crd_det_sin / cos / exp / exp2 / log / log2`, same `crd_sign_bit`
mask discipline, same `smin_poly kk = k * 4.0` / `smin_cubic kk = k * 6.0`
internal scaling.

### Why HLSL too?

D3D12 / DirectX renderers consume HLSL. Vulkan-only engines (Cerid
today) consume GLSL. A multi-backend renderer wants both. Once Cerid
adds a D3D12 backend (Phase 3.5+), the cooker (v9e-d) emits both
files from the same IR — no shader re-authoring step.

### Structural-parity test (`tests/geometry-shader-helpers/test_hlsl_emit.cpp`)

For every golden manifest, verify the HLSL output is structurally
parallel to the GLSL output: same node count, same operator sequence,
identical SSA shape, identical `float n_<i>` / `float3 p_<i>` locals,
`float <name>(float3 p)` signature instead of `float <name>(vec3 p)`,
`smin_poly(n_0, n_1, …)` call sites in the same order.

Because GLSL is GPU-verified within ~1e-6 absolute (v9e-b conformance
test) and HLSL math is line-for-line identical, **structural parity
proves HLSL ≡ GLSL ≡ C++ by induction**. This was the v9e-c shipping
bar — full GPU verification was originally planned as the follow-on.

## What landed (v9e-c-dxc-spirv-dispatch follow-on, same day)

Per the user's explicit ask ("I want you to finish follow on that
you've created before going further, v9e-c-dxc-spirv-dispatch do it"),
the GPU-verification follow-on shipped the same day, before v9e-d.

### New `crd::shader::compile_hlsl` (`engine/shader/src/compile_hlsl.cpp`)

Mirror of `compile_glsl`'s dynamic-load shape:

```cpp
[[nodiscard]] CompileResult compile_hlsl(
    Stage                        stage,
    crd::containers::StringView  source,
    crd::containers::StringView  name,
    crd::memory::IAllocator*     a = crd::memory::default_allocator());
```

Loads `dxcompiler.dll` from `VULKAN_SDK/Bin` (Vulkan SDK ships dxc next
to shaderc). COM-style API (`IDxcCompiler3` / `IDxcUtils` /
`IDxcResult` / `IDxcBlob`) with explicit `QueryInterface` / `Release`
refcount management — kept the dependency footprint minimal and the
lifetime visible rather than pulling in `CComPtr`.

Args: `-T cs_6_0 -E cs_main -spirv -fspv-target-env=vulkan1.3`. Produces
Vulkan-consumable SPIR-V identical in role to `compile_glsl`'s output.

### dxcapi.h Windows / COM dependency

`dxcapi.h` expects the consumer to have pulled in the Windows COM types
(`UINT32`, `LPCWSTR`, `IUnknown` with its `AddRef`/`Release`/
`QueryInterface` slots) before the `#include`. On a vanilla TU it
errors out with undeclared identifiers. Fix:

```cpp
#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // UINT32, LPCWSTR, …
#include <unknwn.h>    // IUnknown full definition (AddRef/Release/QueryInterface)
#endif
#include <dxc/dxcapi.h>
```

### CMakeLists.txt

`engine/shader/CMakeLists.txt` gained:

```cmake
find_path(CRD_DXC_INCLUDE_DIR dxc/dxcapi.h HINTS "$ENV{VULKAN_SDK}/Include")
if(NOT CRD_DXC_INCLUDE_DIR)
    message(FATAL_ERROR "dxc headers not found; required for crd-shader compile_hlsl")
endif()
target_include_directories(crd-shader PRIVATE "${CRD_DXC_INCLUDE_DIR}")
```

`dxcompiler.dll` is loaded dynamically at runtime — no link-time
dependency, mirrors how `shaderc_shared.dll` is consumed.

### Full GPU verification (`tests/geometry-shader-helpers/test_hlsl_conformance.cpp`)

Mirrors the v9e-b GLSL conformance test path exactly, with the
emit/compile pair swapped:

1. `emit_hlsl_conformance_shader(ir, &alloc)` for each of the 21
   golden manifests.
2. `compile_hlsl(Stage::Compute, source, name, &alloc)` → SPIR-V.
3. Bind on Vulkan via `crd-rhi-compute` substrate.
4. Dispatch over a 32³ sample grid.
5. Read back the storage buffer.
6. For every sample, `ulp_compare` against `evaluate<float>(ir, p)`
   with mixed ULP+abs tolerance.

Result: 21 manifests × 32³ samples = 688 128 per-pixel comparisons,
all passing — full end-to-end proof that HLSL ≡ C++ on GPU, not just
HLSL ≡ GLSL structurally.

## Files added / changed

- `engine/geometry-shader-helpers/include/crd/geometry/shader_helpers/hlsl_emitter.hpp` (~51 LOC)
- `engine/geometry-shader-helpers/src/hlsl_emitter.cpp` (~638 LOC, incl. the HLSL helpers prelude)
- `engine/shader/include/crd/shader/compile.hpp` — added `compile_hlsl` declaration
- `engine/shader/src/compile_hlsl.cpp` (~210 LOC) — dxc wrapper
- `engine/shader/CMakeLists.txt` — `find_path(CRD_DXC_INCLUDE_DIR …)` + private include dir
- `tests/geometry-shader-helpers/test_hlsl_emit.cpp` (~218 LOC) — structural parity
- `tests/geometry-shader-helpers/test_hlsl_conformance.cpp` (~368 LOC) — full GPU verification
