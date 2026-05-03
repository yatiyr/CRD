# crd-resources

General-purpose resource substrate: typed payloads loaded from cooked binary packs, held under
intrusive reference counts, with synchronous (v1c) and async (v1d) delivery. Plus `tools/asset_cooker/`
— a separate CLI that ingests source assets and emits runtime-consumed binary packs.

**Phase 2.6 in progress** — v1a, v1b, v1c shipped.

Depends on: `crd-core`, `crd-log`, `crd-memory`, `crd-containers`, `crd-platform`.  
Does NOT depend on: `crd-rhi`, `crd-shader`, `crd-renderer` (loader-registry pattern keeps this low in
the graph — a DAW build can link `crd-resources` without any GPU code). ADR-0036.

## Status

| Slice | Ships | Status |
|---|---|---|
| v1a | `ResourceId` (UUID v4/v5), CRDR container, `ResourceManager` shell, `manifest_dump` CLI | ✅ |
| v1b | Cooker CLI (`cook` sub-command), zstd per-chunk compression, `crd-cooker` static lib | ✅ |
| v1c | `RefCounted<T>`, `ResourceHandle<T>`, `load_sync<T>`, cycle detection, `smoke_resources` | ✅ |
| v1d | `crd-platform::AsyncFile`, `load_async<T>`, `wait_ready()` fiber-cooperative | 🔜 |
| v1e | `ShaderResourceLoader`, `MaterialResourceLoader`, end-to-end cooked render smoke | 🔜 |
| v1f | Hot-reload (file watcher, atomic payload swap, last-good preservation) | 🔜 |
| v1g | Streaming, 2Q LRU eviction, memory budget, pinning | 🔜 |

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

## Public API (v1c surface)

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

    // Diagnostics
    [[nodiscard]] crd::usize loader_count() const noexcept;
    [[nodiscard]] crd::usize mount_count()  const noexcept;
    [[nodiscard]] crd::usize entry_count()  const noexcept;
    [[nodiscard]] crd::usize handle_count() const noexcept;
};

} // namespace crd::resources
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

v1c limitation: thread-local storage is per-OS-thread. Fiber migration across a suspend point inside
`loader->load()` would corrupt the stack. Safe in v1c (all synchronous; no suspension). Revisit in v1d
when `load_async` runs loaders on fiber-scheduled jobs.

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
    resource_control_block.hpp  ← v1c
    resource_handle.hpp      ← v1c
    resource_manager.hpp     ← v1a shell, v1c extended
    log_channel.hpp          ← v1a
    resources.hpp            ← umbrella
  src/
    resource_id.cpp
    crdr.cpp
    resource_manager.cpp
    log_channel.cpp
    detail/sha1.hpp          ← vendored, UUID v5

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

engine/memory/include/crd/memory/
  ref_counted.hpp            ← v1c (not in memory.hpp umbrella)

engine/platform/include/crd/platform/
  filesystem.hpp             ← read_file_range() added in v1c
```

## Session logs

- [v1a — ResourceId + CRDR + ResourceManager shell](../sessions/2026-05-03-resources-v1a.md)
- [v1b — Cooker CLI + zstd](../sessions/2026-05-03-resources-v1b.md)
- [v1c — RefCounted + ResourceHandle + load_sync](../sessions/2026-05-03-resources-v1c.md)

## Long-term direction

- v1d adds `crd::platform::AsyncFile` (IOCP on Windows, io_uring on Linux) and `load_async<T>`. `wait_ready()` becomes fiber-cooperative: suspends the calling fiber, releases the OS thread.
- v1e wires `ShaderResourceLoader` and `MaterialResourceLoader` for an end-to-end cooked render path.
- v1f adds dev-mode file watching and atomic hot-reload with generation-bump notification.
- v1g closes the streaming story: `load_streamed<T>` for large blobs, 2Q LRU eviction, memory budget, pinning.
- Phase 3.0 `crd-scene` registers a `SceneLoader` into the same registry — the resource substrate becomes the single load path for all engine asset types.
