# crd-resources

General-purpose resource substrate: typed payloads loaded from cooked binary packs, held under
intrusive reference counts, with synchronous (v1c) and async (v1d) delivery. Plus `tools/asset_cooker/`
— a separate CLI that ingests source assets and emits runtime-consumed binary packs.

**Phase 2.6 COMPLETE** — v1a, v1b, v1c, v1d, v1e, v1f, v1g shipped.

Depends on: `crd-core`, `crd-log`, `crd-memory`, `crd-containers`, `crd-platform`.  
Does NOT depend on: `crd-rhi`, `crd-shader`, `crd-renderer` (loader-registry pattern keeps this low in
the graph — a DAW build can link `crd-resources` without any GPU code). ADR-0036.

## Status

| Slice | Ships | Status |
|---|---|---|
| v1a | `ResourceId` (UUID v4/v5), CRDR container, `ResourceManager` shell, `manifest_dump` CLI | ✅ |
| v1b | Cooker CLI (`cook` sub-command), zstd per-chunk compression, `crd-cooker` static lib | ✅ |
| v1c | `RefCounted<T>`, `ResourceHandle<T>`, `load_sync<T>`, cycle detection, `smoke_resources` | ✅ |
| v1d | `crd-platform::AsyncFile`, `load_async<T>`, `wait_ready()` fiber-cooperative, load coalescing | ✅ |
| v1e | `ShaderResourceLoader`, `MaterialResourceLoader`, `compile_glsl()`, GLSL/material cooker handlers, `smoke_resources_render` | ✅ |
| v1f | Hot-reload: PACK-file mtime polling, atomic payload swap, last-good preservation, `subscribe_reload`/`unsubscribe_reload`, `poll_hot_reload`, `reload_mount_now` | ✅ |
| v1g | `load_streamed<T>`, 2Q LRU eviction, memory budget (`set_memory_budget`), `pin`/`unpin`, `load_streamed` via `AsyncFile`, re-issue on eviction | ✅ |

## Core concepts

### ResourceId — 128-bit hybrid UUID

```cpp
ResourceId id = ResourceId::mint_random();           // UUID v4; written to .meta sidecar at first import
ResourceId id = ResourceId::from_content(span);      // UUID v5; same bytes → same id (content-addressing)
ResourceId id = ResourceId::parse("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx");
crd::containers::String s = id.to_string(alloc);     // 36-char hyphenated lowercase hex
```

The id lives in the `.meta` sidecar next to the source file in VCS. It is stable across renames. At
runtime the cooked manifest is the sole authority — `.meta` files are never shipped.

### CRDR — cooked binary container

All cooked artifacts (per-resource files and the manifest) share one format: 32-byte LE header + N
typed chunks. Each chunk carries a FourCC, optional per-chunk zstd compression (chunk flag bit 0), and
payload zero-padded to 16-byte alignment. Chunks are sorted by FourCC at write time for determinism.

Key FourCCs: `CRDR` (container magic), `PACK` (manifest type), `MFST` (manifest entry table), `STRP`
(string pool), `DEPS` (dependency list), `BLOB` (generic payload), `SHDR` (shader), `MATR` (material).

### ResourceHandle<T> — one-pointer atomic refcount

```cpp
ResourceHandle<BlobResource> h = rm.load_sync<BlobResource>(id);

if (h.state() == LoadState::Ready)
{
    const BlobResource* res = h.get();  // non-null; stable for lifetime of handle
}

// Copy is one atomic increment; drop all copies and the payload is released (non-permanent block)
// or stays in the manager table (permanent block). No manual free.
```

`sizeof(ResourceHandle<T>) == sizeof(pointer)` on all platforms. The intrusive refcount lives in
`ResourceControlBlock`, not in the handle itself.

### LoadState

| State | Meaning |
|---|---|
| `Unloaded` | null handle; never requested |
| `Queued` | submitted to loader queue (v1d) |
| `Loading` | loader job running (v1d) |
| `Ready` | payload valid; `get()` returns non-null |
| `Placeholder` | soft fallback from `load_placeholder()`; `get()` non-null |
| `Failed` | hard failure; `get()` returns nullptr |

### ILoader — typed resource loader

```cpp
class ILoader
{
public:
    virtual ~ILoader() = default;
    [[nodiscard]] virtual crd::u32 type_fourcc()    const noexcept = 0;
    [[nodiscard]] virtual crd::u32 loader_version() const noexcept = 0;
    [[nodiscard]] virtual void* load(const LoadContext& ctx) = 0;
    [[nodiscard]] virtual void* load_placeholder(const LoadContext& /*ctx*/) { return nullptr; }
    virtual void unload(void* payload) noexcept = 0;
};

struct LoadContext
{
    ResourceId                       id;
    crd::containers::ConstSpan<u8>   bytes;    // entire cooked artifact bytes
    ResourceManager*                 manager;  // for transitive load_sync of deps
    crd::memory::IAllocator*         allocator;
};
```

One loader per FourCC. Register at startup before any mounts. Transitive deps: call
`ctx.manager->load_sync<DepType>(dep_id)` from inside `load()` — cycle detection fires automatically
if there is a circular dependency.

`load_placeholder()` is optional. Override it to return a magenta-checker texture, an error material,
a silence buffer — any typed fallback that lets the engine keep running after a broken artifact.

## Typed loaders (v1e)

### `ShaderResourceLoader` — `crd-shader`

Handles `type='SHDR'` artifacts. Registered by calling `crd::shader::register_shader_loader(rm)` at startup.

Reads the first SPVV/SPVF/SPVC chunk from the artifact to determine shader stage, copies the raw SPIRV bytes into `ShaderResource::spirv`, then runs spirv-reflect to populate `descriptor_bindings`, `push_constants`, and (for vertex stage) `vertex_attributes`. The loader owns a `MallocAllocator` and placement-new constructs `ShaderResource` from it.

```cpp
#include <crd/shader/shader_resource_loader.hpp>

// at startup:
crd::shader::register_shader_loader(&rm);

// then:
auto h = rm.load_sync<crd::shader::ShaderResource>(shader_id);
const crd::shader::ShaderResource* s = h.get();
// s->stage, s->spirv, s->descriptor_bindings, s->push_constants, s->vertex_attributes
```

### `MaterialResourceLoader` — `crd-renderer`

Handles `type='MATR'` artifacts. Registered by calling `crd::renderer::register_material_loader(rm)` at startup. Requires `ShaderResourceLoader` to also be registered.

Reads the 32-byte META chunk (two serialised `ResourceId` pairs: vert hi/lo, frag hi/lo), calls `ctx.manager->load_sync<ShaderResource>(id)` for each, and constructs a `MaterialResource` holding both handles. Transitive loads are safe because the manager mutex is not held during loader dispatch.

```cpp
#include <crd/renderer/material_resource_loader.hpp>

// at startup (register shader loader first):
crd::renderer::register_material_loader(&rm);

// then:
auto h = rm.load_sync<crd::renderer::MaterialResource>(mat_id);
const crd::renderer::MaterialResource* m = h.get();
const crd::shader::ShaderResource* vert = m->vertex_shader.get();
const crd::shader::ShaderResource* frag = m->fragment_shader.get();
```

### `compile_glsl()` — shaderc wrapper

Free function in `crd-shader` usable without the full shader runtime:

```cpp
#include <crd/shader/compile.hpp>

crd::shader::CompileResult r = crd::shader::compile_glsl(
    crd::shader::Stage::Vertex,
    crd::containers::StringView(src, len),
    "debug_name",
    &alloc);

if (r.ok)
{
    // r.spirv contains the SPIRV bytes
}
```

Returns `CompileResult::ok = false` when shaderc is unavailable (graceful degradation).

## Hot-reload (v1f)

```cpp
// Callback type: fired after a successful reload. Called on the poll/reload-now thread, outside
// the manager mutex. `new_generation` matches handle.generation() after the swap.
using ReloadCallback = void (*)(ResourceId id, crd::u32 new_generation, void* user);

// Subscribe for a specific resource. Returns a token for unsubscribe.
crd::u32 subscribe_reload(ResourceId id, ReloadCallback cb, void* user);

// Unsubscribe by token. No-op if invalid.
void unsubscribe_reload(ResourceId id, crd::u32 token);

// Call once per frame from the main thread. Checks all mounted PACKs for mtime changes.
// Reloads after debounce_ms elapses. Returns the number of payloads successfully swapped.
// Old payloads freed at the START of the next poll/reload-now call (one-frame grace period).
crd::usize poll_hot_reload(crd::u32 debounce_ms = 200U);

// Force-reload all loaded resources from a mounted pack, bypassing mtime check.
// Useful in tests and external tools. Returns the number of resources successfully reloaded.
crd::usize reload_mount_now(MountId id);
```

**Contract for raw-pointer holders:** `get()` returns a raw `const T*`. Callers must not cache
this pointer past the next `poll_hot_reload` or `reload_mount_now` call. The one-frame grace
period guarantees the old payload is alive for the rest of the current frame.

**Atomicity:** `ResourceControlBlock::payload` is `std::atomic<void*>`. Writers (hot-reload) do
`payload.exchange(new, acq_rel)` then `state.store(Ready, release)`. Readers (`get()`) do
`state.load(acquire)` then `payload.load(acquire)`. This prevents a data race between a reader
mid-`get()` and a concurrent reload.

## Memory budget + 2Q eviction (v1g)

```cpp
// Set a soft memory ceiling. Eviction runs after each successful load that exceeds the budget.
// Victims chosen by 2Q (Johnson & Shasha 1994): A1in FIFO first, then Am LRU.
// Default: unlimited (~0ULL).
void set_memory_budget(crd::u64 bytes);

// Bytes currently tracked against the budget (uses blob_size as proxy). Thread-safe.
[[nodiscard]] crd::u64 current_memory_use() const noexcept;

// Pinned resources are never evicted, even at refcount 0.
// Reference-counted: each pin() must be paired with exactly one unpin().
// Safe to call before or after the resource is loaded.
void pin  (ResourceId id);
void unpin(ResourceId id);

// Like load_async but reads the artifact via crd::platform::AsyncFile inside the job fiber.
// LoadContext::stream_file is set for the loader; bytes span is empty.
// Requires crd::jobs to be initialised.
template <typename T>
[[nodiscard]] ResourceHandle<T> load_streamed(ResourceId id);
```

**2Q policy summary (Johnson & Shasha 1994):**
- New resources → `A1in` (FIFO probationary queue).
- When `A1in` evicts a resource → id moves to `A1out` ghost (bounded at 256 entries; no payload).
- When a resource in `A1out` is reloaded → promoted to `Am` (LRU main queue); generation bumped.
- Eviction preference: A1in front before Am front; pinned/active entries skipped.

**Re-issue:** evicted blocks stay in `m_handles` with `state = Unloaded`. `load_sync`/`load_async`/
`load_streamed` re-issue without allocating a new control block; `generation` is incremented.

## Public API (v1d surface)

```cpp
namespace crd::resources {

class ResourceManager
{
public:
    explicit ResourceManager(crd::memory::IAllocator* a);
    ~ResourceManager();  // unloads and frees all permanent blocks

    // Registration — call before any mount
    void register_loader(std::unique_ptr<ILoader> loader);

    // Mounts — last mount wins on UUID collision
    [[nodiscard]] MountId mount_manifest(crd::containers::StringView path);
    void                  unmount(MountId id);

    // Synchronous load — blocks the calling thread; safe from non-fiber contexts
    template<typename T>
    [[nodiscard]] ResourceHandle<T> load_sync(ResourceId id);

    // Asynchronous load — submits a job; returns immediately with Queued (or Ready if cached).
    // Coalesces with in-flight loads for the same id (no duplicate I/O).
    // Requires crd::jobs to be initialised.
    template<typename T>
    [[nodiscard]] ResourceHandle<T> load_async(ResourceId id);

    // Diagnostics
    [[nodiscard]] crd::usize loader_count()    const noexcept;
    [[nodiscard]] crd::usize mount_count()     const noexcept;
    [[nodiscard]] crd::usize entry_count()     const noexcept;
    [[nodiscard]] crd::usize handle_count()    const noexcept;
    [[nodiscard]] crd::usize in_flight_count() const noexcept;
};

} // namespace crd::resources
```

### `wait_ready()` — fiber-cooperative block

```cpp
// On ResourceHandle<T> (and ResourceHandleBase):
LoadState wait_ready();
```

Atomically claims the `load_counter` stored in the control block and calls `crd::jobs::wait()`. The calling fiber suspends (releasing the OS thread to the scheduler) until the load job signals completion. Safe to call from any worker fiber or non-fiber thread (falls back to `yield`-spin on the main thread). Also safe on sync-loaded handles — no counter is stored; the terminal-state fast path returns immediately.

### `crd::platform::AsyncFile`

```cpp
namespace crd::platform {
class AsyncFile
{
public:
    [[nodiscard]] static AsyncFile open(crd::containers::StringView path);
    [[nodiscard]] bool              is_open() const noexcept;
    [[nodiscard]] u64               size()    const noexcept;
    // Returns nullptr if offset+size > file size.
    [[nodiscard]] crd::jobs::Counter* read_async(u64 offset, crd::containers::Span<u8> dst);
};
} // namespace crd::platform
```

## RefCounted<T> (crd-memory prerequisite)

`crd::memory::RefCounted<T>` lives in `engine/memory/include/crd/memory/ref_counted.hpp`. Include it
directly — it is NOT in `memory.hpp` to avoid PCH cost in unrelated translation units.

```cpp
struct MyBlock : public crd::memory::RefCounted<MyBlock>
{
    // ...
protected:
    ~MyBlock() = default;  // protected: can't be stack-destroyed while refs > 0
};

MyBlock* b = /* allocate */;
b->add_ref();                          // relaxed increment
crd::u32 refs_left = b->release();    // [[nodiscard]]; acq_rel decrement; free if 0
```

## Synchronous load flow

```
load_sync<T>(id)
  ↓ cache hit?        → add_ref, return existing block
  ↓ cycle?            → make_failed_block (non-permanent), log Error
  ↓ not in manifest?  → make_failed_block, log Error
  ↓ no loader?        → make_failed_block, log Error
  ↓ mount missing?    → make_failed_block, log Error (internal)
  ↓ file read fail?   → make_failed_block, log Error
  ↓ loader->load()    → payload non-null? → permanent=true, state=Ready, cache in m_handles
  ↓ null payload
    load_placeholder() → non-null? → permanent=true, state=Placeholder, cache
    null                           → permanent=false, state=Failed (NOT cached)
```

All error paths return a non-null block with `state = Failed`. A null block is never stored in a handle.

## Cycle detection

Thread-local visiting stack (64 entries max, depth-first). Pushed before `loader->load()`, popped after.
A transitive `load_sync` call that finds its own id already on the stack logs an error and returns a
Failed block for that sub-load. The parent load then also fails (its dep is Failed).

v1c limitation (still open): thread-local storage is per-OS-thread. Fiber migration across a suspend
point inside `loader->load()` would corrupt the visiting stack. Safe in v1d (loaders in async jobs do
not suspend mid-`load()`; transitive `load_sync` calls are synchronous and complete before the fiber
can migrate). A full fix requires passing a per-load visit set in `LoadContext` instead of TLS.

## Cook pipeline

`tools/asset_cooker/` is a separate executable (ADR-0013). Never linked into engine binaries.

```
asset_cooker  cook          --root <source-dir>  --out <pack.crdr>
asset_cooker  manifest_dump  <pack.crdr>
```

Incremental rebuild: `cook_key = fnv1a64(source_bytes) ^ handler_version`. Cached in
`.cook_cache/<uuid>.key`. A match skips the artifact; a mismatch re-cooks and updates the cache.
`cook.log.toml` records every decision (cooked / skipped / why) for CI determinism auditing.

Two-pass PACK assembly: pass 1 builds the CRDR manifest section to measure its byte size; pass 2
recomputes real `blob_offset` values and appends artifact bytes after the CRDR section.

## Module layout (shipped files)

```
engine/resources/
  include/crd/resources/
    resource_id.hpp          ← v1a
    crdr.hpp                 ← v1a + v1b (compression added)
    loader.hpp               ← v1a (ILoader, LoadContext)
    load_state.hpp           ← v1c
    resource_control_block.hpp  ← v1c + v1d (load_counter field)
    resource_handle.hpp      ← v1c + v1d (wait_ready decl; release_block non-inline)
    resource_manager.hpp     ← v1a shell, v1c extended, v1d (load_async, m_in_flight, mutex)
    log_channel.hpp          ← v1a
    resources.hpp            ← umbrella
  src/
    resource_id.cpp
    crdr.cpp
    resource_handle.cpp      ← v1d (release_block + wait_ready impl)
    resource_manager.cpp     ← v1c impl + v1d (AsyncLoadCtx, LoadJobFn, load_async_impl, run_load_job)
    log_channel.cpp
    detail/sha1.hpp          ← vendored, UUID v5

engine/platform/
  include/crd/platform/
    async_file.hpp           ← v1d
  src/
    async_file.cpp           ← v1d

tools/asset_cooker/
  include/crd/cooker/
    cook_handler.hpp         ← v1b
    cook_command.hpp         ← v1b
  src/
    main.cpp
    cook_command.cpp
    manifest_dump.cpp
    cook_handlers/
      blob_passthrough.cpp
      glsl.cpp               ← v1e (.glsl → type='SHDR' artifact)
      material.cpp           ← v1e (.mat.toml → type='MATR' artifact)

engine/shader/
  include/crd/shader/
    shader_resource_loader.hpp  ← v1e
    compile.hpp                 ← v1e (compile_glsl free function)
  src/
    shader_resource_loader.cpp  ← v1e
    compile.cpp                 ← v1e

engine/renderer/
  include/crd/renderer/
    material_resource_loader.hpp  ← v1e
  src/
    material_resource_loader.cpp  ← v1e

engine/memory/include/crd/memory/
  ref_counted.hpp            ← v1c (not in memory.hpp umbrella)

engine/platform/include/crd/platform/
  filesystem.hpp             ← read_file_range() added in v1c

runtime/examples/
  smoke_resources_render.cpp ← v1e
```

## Session logs

- [v1a — ResourceId + CRDR + ResourceManager shell](../sessions/2026-05-03-resources-v1a.md)
- [v1b — Cooker CLI + zstd](../sessions/2026-05-03-resources-v1b.md)
- [v1c — RefCounted + ResourceHandle + load_sync](../sessions/2026-05-03-resources-v1c.md)
- [v1d — AsyncFile + load_async + wait_ready](../sessions/2026-05-04-resources-v1d.md)
- [v1e — ShaderResourceLoader + MaterialResourceLoader + smoke](../sessions/2026-05-04-resources-v1e.md)
- [v1f — Hot-reload: mtime polling, atomic payload swap, callbacks](../sessions/2026-05-04-resources-v1f.md)
- [v1g — load_streamed, 2Q eviction, memory budget, pinning](../sessions/2026-05-04-resources-v1g.md)

## Long-term direction

- **Phase 2.6 COMPLETE** — all v1a–v1g slices shipped. crd-resources is a full resource substrate:
  typed loaders, sync + async + streamed load, hot-reload, 2Q LRU eviction, memory budget, pinning.
- Phase 3.0 `crd-scene` registers a `SceneLoader` into the same registry — the resource substrate
  becomes the single load path for all engine asset types.
- Phase 2.7 `TextureResource` + `MeshResource` + glTF import will be the first consumers of the
  full substrate (async streaming + eviction) for real runtime data.
