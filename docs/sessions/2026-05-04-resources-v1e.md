# 2026-05-04 — Phase 2.6 v1e: ShaderResourceLoader + MaterialResourceLoader + end-to-end cooked render smoke

## What shipped

### ShaderResourceLoader (`crd-shader`)

`engine/shader/include/crd/shader/shader_resource_loader.hpp` + `src/shader_resource_loader.cpp`.
`ShaderResourceLoaderImpl` (private, in anonymous namespace) inherits `crd::resources::ILoader`, handles
`kFourCC_SHDR`, version 1.

`load()` path:
1. Parse artifact via `crdr_read()`.
2. Find the first SPVV/SPVF/SPVC chunk — chunk presence determines `Stage`.
3. Allocate `ShaderResource` via placement new using internal `MallocAllocator`.
4. `memcpy` SPIRV bytes into `res->spirv`.
5. Call `reflect_into()` which runs spirv-reflect to populate `descriptor_bindings`, `push_constants`,
   and (vertex stage only) `vertex_attributes`.

`unload()` calls the destructor then `m_alloc.deallocate()`.

Public registration entry point:
```cpp
// engine/shader/include/crd/shader/shader_resource_loader.hpp
namespace crd::shader {
void register_shader_loader(crd::resources::ResourceManager* rm);
}
```

**Clang-cl fix:** The initial draft contained a `to_parameter_class_local` helper (maps
`SpvReflectDescriptorType` → `ParameterClass`) that `reflect_into` never called. MSVC `/W4 /WX`
silently accepts unused statics in anonymous namespaces; clang-cl's `-Werror,-Wunused-function`
treats them as errors. Removed the function entirely — it was premature work for when
`DescriptorBindingDesc` gains a `ParameterClass` field.

### MaterialResourceLoader (`crd-renderer`)

`engine/renderer/include/crd/renderer/material_resource_loader.hpp` + `src/material_resource_loader.cpp`.
`MaterialResourceLoaderImpl` handles `kFourCC_MATR`, version 1.

META chunk is exactly 32 bytes:
```
bytes  0– 7: vert_id.hi  (u64 LE)
bytes  8–15: vert_id.lo  (u64 LE)
bytes 16–23: frag_id.hi  (u64 LE)
bytes 24–31: frag_id.lo  (u64 LE)
```

`load()` extracts the two `ResourceId` values, calls `ctx.manager->load_sync<ShaderResource>()` for
each, then placement-new constructs `MaterialResource` (the two `ResourceHandle<ShaderResource>` fields
are move-assigned). Transitive `load_sync` is safe: the resource manager mutex is not held during
loader dispatch.

Public registration entry point:
```cpp
// engine/renderer/include/crd/renderer/material_resource_loader.hpp
namespace crd::renderer {
void register_material_loader(crd::resources::ResourceManager* rm);
}
```

### `compile_glsl()` free function (`crd-shader`)

`engine/shader/include/crd/shader/compile.hpp` + `src/compile.cpp`.

```cpp
struct CompileResult
{
    bool                             ok;
    crd::containers::Array<crd::u8>  spirv;
    crd::containers::String          error_message;
};

CompileResult compile_glsl(Stage stage,
                            crd::containers::StringView src,
                            crd::containers::StringView debug_name,
                            crd::IAllocator* alloc);
```

Wraps shaderc. Returns `ok = false` when shaderc is unavailable — callers can skip gracefully. Used by
the `[shaderc]`-tagged test case and the GLSL cooker handler.

### Cooker handlers

`tools/asset_cooker/src/cook_handlers/glsl.cpp`: registered for `.glsl` extension. Calls `compile_glsl()`,
wraps result in a `CrdrWriter` with the appropriate FourCC chunk (SPVV/SPVF/SPVC based on stage suffix
in the filename: `.vert.glsl`, `.frag.glsl`, `.comp.glsl`). Returns `type='SHDR'`.

`tools/asset_cooker/src/cook_handlers/material.cpp`: registered for `.mat.toml` extension. Reads the
TOML, extracts `vertex_shader` and `fragment_shader` source paths, looks up their `ResourceId` values
from adjacent `.meta` sidecars, serialises both UUIDs into a 32-byte block, writes a `CrdrWriter`
with `kFourCC_META` chunk. Returns `type='MATR'`.

### `smoke_resources_render`

`runtime/examples/smoke_resources_render.cpp`. Builds SHDR + MATR artifacts in-memory (no on-disk
cook required), mounts them as a single PACK, calls `rm.load_sync<MaterialResource>(matr_id)`,
asserts `handle.state() == LoadState::Ready`, prints SPIRV byte counts, exits 0.

Output:
```
smoke_resources_render: OK — MaterialResource loaded with vert+frag SPIRV (vert=1040 bytes, frag=572 bytes)
```

## Tests

`tests/resources/test_shader_material_loaders.cpp` — 6 `TEST_CASE` entries:

| Tag | What |
|---|---|
| `[resources][shader][v1e]` | Vertex SHDR round-trip (SPVV chunk → Stage::Vertex, SPIRV bytes match) |
| `[resources][shader][v1e]` | Fragment SHDR round-trip (SPVF chunk → Stage::Fragment) |
| `[resources][shader][v1e]` | Missing SPIRV chunk → `LoadState::Failed` |
| `[resources][material][v1e]` | Material loads + resolves transitive shader deps; verifies `handle_count() == 3` and same payload pointer returned from cache |
| `[resources][material][v1e]` | Missing META chunk → `LoadState::Failed` |
| `[resources][shader][v1e][shaderc]` | Real SPIRV round-trip via `compile_glsl()`; verifies `vertex_attributes` non-empty; gracefully skips if shaderc unavailable |

## Six-configuration results

| Config | Result |
|---|---|
| win-debug | 435/435 ✅ |
| win-relwithdebinfo | 435/435 ✅ |
| win-release | 432/432 ✅ |
| win-asan | 435/435 ✅ |
| win-clang-cl | 435/435 ✅ |
| win-tidy | 435/435 ✅ |

win-release is 3 fewer (debug-only `FiberState` tests gated by `#if CRD_ENABLE_ASSERTS`).

## Decisions made

- **`ShaderResource` stores raw SPIRV + re-derived reflection.** spirv-reflect runs at load time, not
  at cook time. This keeps the CRDR artifact format simple (one SPIRV chunk, no serialised reflection
  blob) and avoids a schema dependency on spirv-reflect's internal layout. The cost is trivial for
  the sizes involved.

- **`to_parameter_class_local` removed.** The function was written speculatively for a future
  `DescriptorBindingDesc::parameter_class` field. Keeping dead code triggers clang-cl's warning-as-error
  policy and adds noise. It will be written again when `DescriptorBindingDesc` actually gains the field.

- **META chunk is 32 raw bytes, not TOML.** Keeps the loader allocation-free and branchless for the
  common path. The 32-byte layout is documented in this session log and in the loader source.

- **`compile_glsl()` is a separate free function, not a method on the shader runtime.** The cooker
  and loader tests need GLSL→SPIRV without constructing a `Runtime`. Keeping it separate avoids a
  transitive dependency on the runtime's internal caches.
