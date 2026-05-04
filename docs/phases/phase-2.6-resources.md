# Phase 2.6 — `crd-resources`: resource system + asset cooker

**Status:** 🚢 **COMPLETE** — v1g shipped (2026-05-04). Phase 2.6 COMPLETE.
**ADRs:** ADR-0036 (module + loader registry), ADR-0037 (ResourceId UUID scheme), ADR-0038 (cooked binary container), ADR-0039 (ResourceHandle semantics), ADR-0040 (cooker CLI + CMake), ADR-0041 (platform async I/O)
**Module:** `engine/resources/` + `tools/asset_cooker/`
**Depends on:** `crd-core`, `crd-log`, `crd-memory`, `crd-containers`, `crd-jobs`, `crd-platform`, `crd-config`

---

## Goal

A general-purpose resource substrate that loads typed payloads from a binary cooked container, holds
them under intrusive reference counts, supports async + streamed delivery via `crd-jobs`, and notifies
consumers of hot-reload events without breaking handle identity. Plus a separate `asset_cooker`
executable that ingests human-authored sources (TOML descriptors, GLSL, glTF, etc.) and emits the
runtime-consumed binary packs.

The system must serve games, simulation (procedural and live-data sources), and DAW projects
(project-relative samples, plugin presets) from a single registry. It does not commit to any
specific resource type — shader, material, texture, mesh, audio, scene, prefab are all loaders
that plug in on top of the same core.

**Non-goals for Phase 2.6:**
- Scene file format — Phase 3.0 (after ECS lands; plugs into the same registry as a `SceneLoader`).
- Editor-side cook drive — Phase 7. The cooker is CLI-driven this phase.
- DLC pack signing / encryption — Phase 4+.
- Network-mounted manifests — Phase 4.2.
- ARC eviction policy — only with measured workload data; v1g ships 2Q.
- DLL-based cook handler plug-ins — arrives with Phase 4 hot-reload scripting.

---

## Architecture

### Layered view

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          SOURCE TREE (VCS)                                │
│  shaders/sky.glsl    materials/wall.mat.toml    audio/kick.wav            │
│  shaders/sky.glsl.meta materials/wall.mat.toml.meta audio/kick.wav.meta   │
│         (UUID v4)              (UUID v4)              (UUID v4)           │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │ asset_cooker (separate exe — ADR-0013)
                                     ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                       COOKED PACK (binary, on disk)                       │
│  pack.crdr   ← chunked container, type='PACK'                             │
│    MFST chunk: ResourceId → (offset, size, type FourCC)                   │
│    STRP chunk: string pool (paths, debug names)                           │
│    DEPS chunk: dependency graph (ResourceId → ResourceId[])               │
│  body: concatenated per-resource artifacts (each is a chunked container)  │
│    sky.crd_shader   ← type='SHDR', chunks: META, DEPS, SPRV, REFL         │
│    wall.crd_material← type='MATR', chunks: META, DEPS, BLOB               │
│    kick.crd_audio   ← type='AUDO', chunks: META, BLOB                     │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │ ResourceManager::mount_manifest()
                                     ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                              RUNTIME                                      │
│  ResourceManager (type-erased registry, multi-mount)                      │
│   ├── ILoader registry (one per type FourCC)                              │
│   │     ShaderResourceLoader, MaterialResourceLoader, ...                 │
│   ├── Manifest table (UUID → mount + offset + type + flags)               │
│   ├── Live handle table (UUID → ResourceControlBlock*)                    │
│   ├── Dependency resolver (transitive, cycle-detected)                    │
│   ├── Eviction policy (2Q LRU under memory budget)                        │
│   └── Hot-reload watcher (dev only; file-watcher → re-cook → swap)        │
│                                                                           │
│  ResourceHandle<T>  (one pointer; atomic refcount; generation)            │
│   load_sync<T>  /  load_async<T>  /  load_streamed<T>                     │
└──────────────────────────────────────────────────────────────────────────┘
```

The runtime never sees source assets (ADR-0013). Everything the engine reads goes through the
chunked container reader regardless of payload type.

### Module dependency placement

`crd-resources` sits LOW in the graph (ADR-0036):

```
core ─┬─ log ─┐
      ├─ memory ─┐
      ├─ containers ─┐
      ├─ jobs ─┐    │
      ├─ platform ─┤    │
      ├─ config ─┤     │
      └─────────┴─────┴── resources
                              ▲
              ┌───────────────┼───────────────┐
              │               │               │
            shader         renderer        DAW plugins
        (registers a    (registers a    (register custom
         ShaderLoader)   MaterialLoader) loaders)
```

Crucially: `crd-resources` does NOT depend on `crd-rhi`, `crd-shader`, or `crd-renderer`. The
loader-registry pattern keeps the dependency direction strictly downward — high-level consumer
modules register typed loaders into the registry at startup. This is what lets a DAW build link
`crd-resources` without dragging in any GPU code.

### ResourceId — 128-bit hybrid UUID

```cpp
// engine/resources/include/crd/resources/resource_id.hpp
namespace crd::resources
{
struct ResourceId
{
    u64 hi;
    u64 lo;

    // UUID v4: random; minted at first import; written to <source>.meta sidecar.
    [[nodiscard]] static ResourceId mint_random() noexcept;

    // UUID v5: SHA-1 of bytes under a fixed namespace; same content → same id.
    // For procedural runtime resources and content-addressed dedup.
    [[nodiscard]] static ResourceId from_content(crd::containers::ConstSpan<u8> bytes) noexcept;

    // Deterministic round-trip for serialization and CLI dump.
    [[nodiscard]] static ResourceId parse(crd::containers::StringView text) noexcept;
    [[nodiscard]] crd::containers::String to_string(crd::IAllocator* a) const;

    [[nodiscard]] constexpr bool is_null()       const noexcept { return hi == 0 && lo == 0; }
    [[nodiscard]] constexpr bool operator==(const ResourceId&) const noexcept = default;
};

inline constexpr ResourceId kNullResourceId{0, 0};
} // namespace crd::resources
```

Three minting modes:
- `mint_random()` — UUID v4. Used at first import: cooker writes the id into `<source>.meta`.
  Used at runtime for genuinely procedural resources (a baked light probe, an audio render).
- `from_content(bytes)` — UUID v5. Same bytes always produce the same id. Used by the cooker
  for content-addressed dedup of identical assets across mounts.
- `parse()` — round-trip from textual form (the canonical 8-4-4-4-12 hex with hyphens).

`.meta` files live adjacent to source assets (Unity / Godot style):

```toml
# audio/kick.wav.meta
[id]
uuid = "0c8b9e2a-7f31-4e25-9c1d-2a4f7b9c1e88"

[import]
imported_at = "2026-05-03T14:22:11Z"
cooker_version = "1.0.0"
loader_version = "audio:1"

[options]
# loader-specific knobs the cooker honors
sample_rate = 48000
format = "FLAC"
```

`.meta` files travel with the source in VCS. They are never shipped — the cooked manifest is the
runtime authority. Renames don't break references (the id is stable in the sidecar; only the
`source_path` debug field changes in the manifest).

### Cooked binary container — `CRDR`

All cooked artifacts (per-resource files AND the manifest) share one chunked binary format
(ADR-0038). 32-byte header + N typed chunks:

```
Header (32 bytes, little-endian)
  +0   u32   magic      = 'CRDR'   ← 0x52, 0x44, 0x52, 0x43 on disk
  +4   u16   version    = 1
  +6   u16   flags      = (bit 0: zstd-compressed payloads)
  +8   u128  uuid                  ← ResourceId (or PACK id for the manifest)
  +24  u32   type_fourcc           ← 'SHDR', 'MATR', 'AUDO', 'PACK', ...
  +28  u32   chunk_count

Each chunk
  +0   u32   fourcc                ← 'META', 'DEPS', 'BLOB', 'SPRV', 'REFL', ...
  +4   u32   flags                 ← (bit 0: compressed, bit 1: aligned-payload)
  +8   u64   uncompressed_size
  +16  u64   compressed_size       ← == uncompressed_size if flag bit 0 clear
  +24  u8[]  payload (compressed_size bytes; zero-padded to 16-byte alignment)
```

**Compression:** zstd level 3 by default. Per-chunk opt-in. Chunks containing already-compressed
payloads (BC7 textures, FLAC audio) are stored uncompressed.

**Determinism:** No timestamps inside chunks. Chunks sorted by FourCC at write time. Padding
bytes zero-filled.

**Manifest chunk FourCCs (`type='PACK'`):**
- `MFST` — entry table: `[ResourceId, type_fourcc, blob_offset, blob_size, flags, name_strp_idx][]`
- `STRP` — string pool (debug names, source paths)
- `DEPS` — `[ResourceId, dep_id_strp_idx, dep_count, dep_id[]][]`

**Per-resource chunk FourCCs:**
- `META` — small typed-key/value blob with resource-level metadata
- `DEPS` — list of `ResourceId` this resource depends on
- `BLOB` — the actual payload (loader-defined format)
- `SPRV` — SPIR-V module (shaders)
- `REFL` — reflection metadata (shaders)
- additional FourCCs are loader-private; the reader treats unknown chunks as opaque

Per-resource-type payload format inside `BLOB` is each loader's choice (SPIR-V for shaders,
BC7 for textures, FLAC for audio). The decision between FlatBuffers, Cap'n Proto, or a hand-rolled
POD layout for per-loader payloads is **deferred to Phase 3.1c** when scene/ECS data forces a real
schema choice.

### `ResourceHandle<T>` — one-pointer atomic refcount

```cpp
// engine/resources/include/crd/resources/resource_handle.hpp
namespace crd::resources
{
enum class LoadState : u8
{
    Unloaded,    // never requested
    Queued,      // submitted to the loader, not started
    Loading,     // loader job running
    Ready,       // payload valid; get() returns non-null
    Placeholder, // loader provided a soft fallback after failure
    Failed,      // hard failure; get() returns nullptr
};

template<typename T>
class ResourceHandle
{
public:
    ResourceHandle() noexcept = default;            // null handle
    ResourceHandle(const ResourceHandle&) noexcept; // atomic refcount inc
    ResourceHandle(ResourceHandle&&) noexcept;
    ~ResourceHandle();                              // atomic refcount dec; evict if 0

    ResourceHandle& operator=(const ResourceHandle&) noexcept;
    ResourceHandle& operator=(ResourceHandle&&) noexcept;

    [[nodiscard]] LoadState   state()      const noexcept;
    [[nodiscard]] bool        is_ready()   const noexcept { return state() == LoadState::Ready; }
    [[nodiscard]] ResourceId  id()         const noexcept;
    [[nodiscard]] u32         generation() const noexcept;  // bumps on hot-reload

    // Non-blocking. nullptr until LoadState::Ready (or LoadState::Placeholder).
    // Stable for the lifetime of one (id, generation) pair.
    [[nodiscard]] const T* get() const noexcept;

    // Fiber-cooperative wait: calls crd::jobs::wait on the load counter.
    // Suspends the fiber, releases the OS thread back to the scheduler.
    // Safe to call from any worker fiber. Returns final state.
    LoadState wait_ready();

private:
    struct ControlBlock;       // intrusive RefCounted<ControlBlock>
    ControlBlock* m_block = nullptr;
};
} // namespace crd::resources
```

**Sizeof = one pointer.** Copy = one atomic increment on the control block's refcount. The
control block is allocated by `ResourceManager`, owned through `crd::memory::RefCounted<T>`
(ADR-0014, prerequisite added to `crd-memory` before v1c starts), and lives in a hash table
keyed by `ResourceId`.

**Generation counter:** every successful hot-reload bumps `generation` and atomically swaps the
payload pointer inside the control block. Existing handles continue to work but observers can
detect "this is a different version of the same resource" by comparing `generation()`.

**Placeholder vs Failed:** per-loader opt-in via `ILoader::load_placeholder()`. Default returns
`nullptr` → `LoadState::Failed` (simulation-friendly hard fail). Loaders that want a soft fallback
(textures returning a magenta checker, materials returning the error material) override the method.

### `ILoader` — typed resource loader

```cpp
// engine/resources/include/crd/resources/loader.hpp
namespace crd::resources
{
struct LoadContext
{
    ResourceId           id;
    crd::containers::ConstSpan<u8> bytes;     // entire cooked artifact
    ResourceManager*     manager;             // for transitive load_sync of deps
    crd::IAllocator*     allocator;           // payload allocator (per-loader policy)
};

class ILoader
{
public:
    virtual ~ILoader() = default;

    // FourCC type tag this loader handles ('SHDR', 'MATR', ...). One loader per FourCC.
    [[nodiscard]] virtual u32 type_fourcc() const noexcept = 0;

    // Monotonic loader version; participates in cooker incremental key.
    [[nodiscard]] virtual u32 loader_version() const noexcept = 0;

    // Load the payload. Allocator-backed; ownership returned to ResourceManager.
    // Implementations may call manager->load_sync<Dep>(dep_id) for transitive deps.
    [[nodiscard]] virtual void* load(const LoadContext& ctx) = 0;

    // Optional soft-failure fallback. Default = nullptr → LoadState::Failed.
    // Override to provide a placeholder payload (magenta checker, error material).
    [[nodiscard]] virtual void* load_placeholder(const LoadContext& /*ctx*/) { return nullptr; }

    // Release the payload allocated by load() / load_placeholder().
    virtual void unload(void* payload) noexcept = 0;
};
} // namespace crd::resources
```

### `ResourceManager` — central registry + multi-mount

```cpp
// engine/resources/include/crd/resources/resource_manager.hpp
namespace crd::resources
{
struct MountId { u32 value; };

class ResourceManager
{
public:
    explicit ResourceManager(crd::IAllocator* a);
    ~ResourceManager();

    // ── Registration (call once at startup) ──────────────────────────────
    void  register_loader(crd::containers::UniquePtr<ILoader> loader);

    // ── Mounts (engine pack → project pack → DLC; last mount wins on collision) ─
    [[nodiscard]] MountId mount_manifest(crd::containers::StringView path);
    void                  unmount(MountId id);

    // ── Synchronous load (blocks current thread; not for fibers) ─────────
    template<typename T>
    [[nodiscard]] ResourceHandle<T> load_sync(ResourceId id);

    // ── Async load (returns immediately; payload arrives on a job thread) ─
    template<typename T>
    [[nodiscard]] ResourceHandle<T> load_async(ResourceId id);

    // ── Streamed load (large blobs: sample libs, mip chains; v1g) ────────
    template<typename T>
    [[nodiscard]] ResourceHandle<T> load_streamed(ResourceId id);

    // ── Pinning (prevent eviction even when refcount drops; v1g) ─────────
    void pin(ResourceId id);
    void unpin(ResourceId id);

    // ── Memory budget + 2Q LRU eviction (v1g) ────────────────────────────
    void  set_memory_budget(u64 bytes);
    [[nodiscard]] u64 current_memory_use() const noexcept;

    // ── Hot-reload subscription (v1f) ────────────────────────────────────
    using ReloadCallback = void(*)(ResourceId, u32 new_generation, void* user);
    void subscribe_reload(ResourceId id, ReloadCallback cb, void* user);

private:
    // type-erased registry; one entry per type FourCC
    // mount table; entries scanned newest-mount-first
    // live handle table; intrusive ref-counted control blocks
    // pending load counters (crd::jobs::Counter*) keyed by ResourceId
};
} // namespace crd::resources
```

**Multi-mount precedence:** mounts are scanned newest-mount-first when resolving a `ResourceId`.
Last mount wins on UUID collision. Collisions are logged at `Warn`. DAW projects always mount
last so a project asset can override an engine default.

**No directory scan at startup.** The runtime opens exactly one file per mount (the manifest).
The manifest tells it where every resource lives and what type it is.

### Cook pipeline

`tools/asset_cooker/` is a separate executable, never linked into engine binaries (ADR-0013,
restated in ADR-0040).

```
asset_cooker  cook  --root <source-dir>  --out <pack.crdr>  [--target <profile>]
asset_cooker  manifest_dump  <pack.crdr>
asset_cooker  resource_dump  <pack.crdr>  <uuid>
```

Cooker plug-in API (compile-time in v1b; DLL-based with Phase 4):

```cpp
// tools/asset_cooker/include/crd/cooker/cook_handler.hpp
struct CookContext
{
    crd::containers::StringView source_path;
    crd::containers::StringView meta_path;
    ResourceId                  id;
    crd::IAllocator*            allocator;
    // ... options from .meta + global cook profile
};

struct CookResult
{
    u32                          type_fourcc;
    crd::containers::Vector<u8>  cooked_bytes;     // chunked CRDR artifact
    crd::containers::Vector<ResourceId> dependencies;
    u64                          source_hash;
    u64                          options_hash;
};

using CookHandlerFn = CookResult(*)(const CookContext&);
void register_cook_handler(crd::containers::StringView ext, CookHandlerFn fn);
```

**Incremental rebuild key** for skipping a re-cook of a single asset:

```
key = hash(cooker_version
         | loader_version
         | source_hash
         | options_hash
         | dep_graph_hash)   // hashes of cook keys of every transitive dep
```

If `key` matches the cached value the artifact is reused as-is. `cook.log.toml` is written next
to the output pack and records every cook decision (skipped, recooked, why) for CI determinism
auditing.

**CMake integration (ADR-0040):**
- Top-level `cook` target re-runs the cooker on configured source roots.
- Optional `CRD_COOK_AS_TARGETS=ON` cache option emits one CMake target per cooked artifact
  for fine-grained dependency tracking in IDE / Ninja workflows.
- Source files and `.meta` sidecars are CMake `DEPENDS` of the cook target so editor saves
  trigger only the affected artifacts.

**Cooker third-party dependencies (tools-only, never linked into engine):**
- `cgltf` — glTF 2.0 ingestion
- `stb_image` (or DirectXTex on Windows for BC compression) — texture ingestion
- `dr_libs` — WAV/FLAC/MP3 ingestion

### `crd-platform` async filesystem I/O (v1d extension)

ADR-0022 listed five prerequisites for streaming; async I/O was prerequisite 3. Phase 2.5 covered
the job system; Phase 2.6 v1d closes the async I/O gap by extending `crd-platform` (ADR-0041):

```cpp
// engine/platform/include/crd/platform/async_file.hpp
namespace crd::platform
{
class AsyncFile
{
public:
    [[nodiscard]] static crd::containers::UniquePtr<AsyncFile>
    open_read(crd::containers::StringView path);

    // Issues an async read; returns a Counter that hits 0 when the read completes.
    // Caller waits with crd::jobs::wait(counter), which suspends the fiber.
    // On completion, bytes are valid and the read result is queryable.
    [[nodiscard]] crd::jobs::Counter*
    read_async(u64 offset, crd::containers::Span<u8> dst);

    [[nodiscard]] u64 size() const noexcept;
};
} // namespace crd::platform
```

Backend: IOCP on Windows, `io_uring` (with `aio` fallback) on Linux. `crd-platform` keeps its
existing dependency surface — it depends only on `crd-core`, `crd-log`, `crd-jobs`. It does NOT
depend on `crd-resources`. `crd-resources` is the first consumer but the API is general.

---

## Slices

### v1a — `ResourceId` + binary manifest format + registry skeleton ✅ SHIPPED 2026-05-03

**Scope:**
- `ResourceId` type (128-bit, three minting modes, parse / to_string).
- Chunked container reader/writer (header + chunk iteration; no compression yet).
- Binary manifest definition (`type='PACK'`, MFST/STRP/DEPS chunks).
- `ResourceManager` shell: `register_loader`, `mount_manifest`, `unmount`. No loading yet.
- `BlobResource` pseudo-type for end-to-end testing (raw bytes, identity loader).
- `manifest_dump` CLI sub-command (reads a pack and prints every entry).

**Tests:** 38 new tests across `test_resource_id.cpp`, `test_crdr.cpp`, `test_resource_manager.cpp`.
All 6 configs green (393/393 win-debug, 390/390 win-release).

**Bonus fix:** `crd-containers` String SSO encoding switched to remaining-capacity
(`size_or_flag = kSsoCapacity - size`) to eliminate `buf[kSsoCapacity]` out-of-bounds UB exposed
by MSVC 14.50.35717 optimizer. A full-capacity SSO string (23 chars) now has `size_or_flag = 0`
which serves as the null terminator, making `c_str()` correct without any UB writes.

### v1b — Cooker CLI skeleton + chunked container writer + first cook handler ✅ SHIPPED 2026-05-03

**Scope:**
- [x] `tools/asset_cooker/` CMake target. Separate exe, links `crd-core`, `crd-log`, `crd-memory`,
  `crd-containers`, plus tools-only `cgltf` (declared but not used in v1b).
- [x] `cook` CLI sub-command with `--root` and `--out`.
- [x] Compile-time `register_cook_handler(ext, fn)` registry inside the cooker.
- [x] One trivial cook handler: `.bin` (extension passthrough → wraps file bytes in a `BLOB`-only
  artifact under `type='BLOB'`).
- [x] Manifest assembly (collects every cooked artifact, writes the PACK file).
- [x] `cook.log.toml` written next to the pack output.
- [x] CMake top-level `cook` target.
- [x] zstd compression wired through the chunk writer (level 3 default; per-chunk opt-in).

**Tests:**
- [x] Cook a directory of 10 `.bin` files → resulting pack contains all 10 entries with stable ids.
- [x] Re-running the cooker without source changes produces a byte-identical pack (no timestamps,
  chunks sorted by FourCC).
- [x] `cook.log.toml` records "skipped (incremental key match)" on the second run.
- [x] zstd round-trip of compressed chunks (write → read → bytes match).

### v1c — Synchronous `ResourceHandle<T>` + `ILoader` + dependency resolution ✅ SHIPPED 2026-05-03

**Prerequisite shipped:** `crd::memory::RefCounted<T>` (ADR-0014). CRTP intrusive base; one
`atomic<u32> m_refs{1}`; `[[nodiscard]] add_ref()`, `[[nodiscard]] release()` (returns new count,
acq_rel), `use_count()`. Protected destructor prevents stack destruction with live refs. NOT added
to `memory.hpp` umbrella to avoid PCH invalidation. 12 new tests in `tests/memory/test_memory.cpp`
including 4-thread concurrent stress.

**Scope shipped:**
- `LoadState` enum (`load_state.hpp`): Unloaded / Queued / Loading / Ready / Placeholder / Failed.
- `ResourceControlBlock` (`resource_control_block.hpp`): inherits `RefCounted<ResourceControlBlock>`;
  `permanent` bool (false = freed by last handle; true = cached until manager destroyed), atomic
  generation + state, `void* payload`, `ILoader* loader`, `IAllocator* alloc`.
- `ResourceHandleBase` (non-template): one pointer, copy/move/dtor update refcount via `release_block()`.
  `release_block()`: if `!permanent && refs == 0` → unload + destruct + free block.
- `ResourceHandle<T>`: typed wrapper; `get()` casts payload to `const T*` when Ready or Placeholder.
  `sizeof(ResourceHandle<T>) == sizeof(pointer)`.
- `read_file_range()` added to `crd::platform::fs` (seekg + read into `Array<u8>`).
- Thread-local cycle detection: `tl_visiting[64]` + `tl_visit_count` in `resource_manager.cpp`
  anonymous namespace. v1c limitation: fiber-migration-unsafe (revisit in v1d).
- `make_failed_block()` static helper: non-permanent Failed block for all error paths. All error paths
  return non-null (null block is never stored in a handle).
- `load_sync_impl`: cache → cycle → manifest → loader → mount → file read → dispatch. Permanent
  on Ready/Placeholder; non-permanent on hard failure.
- `ResourceManager` destructor: iterates `m_handles`, calls `unload()`, destructs, frees every block.
- `smoke_resources.exe`: assemble PACK in-memory, mount, `load_sync<BlobResource>`, verify 5 bytes.

**Tests shipped:** 8 new `load_sync` tests — round-trip, 1000-copy refcount stability, cache hit,
unknown id, hard fail, placeholder, transitive dep, cycle detection.

All 6 configs green: win-debug/relwithdebinfo/asan/clang-cl/tidy 420/420, win-release 417/417.

### v1d — `crd-platform` async I/O + `ResourceManager::load_async` + `wait_ready` ✅ SHIPPED 2026-05-04

**Scope shipped:**
- `crd::platform::AsyncFile`: `open()`, `is_open()`, `size()`, `read_async()` submitting a
  `crd-jobs` job. Returns `nullptr` counter if `offset + size > file_size`. Job-pool backend
  (ReadJob struct, 40 bytes, SBO-compatible).
- `ResourceManager::load_async<T>`: heap-allocated `AsyncLoadCtx` submitted as a `LoadJobFn`
  closure (8-byte SBO). `m_in_flight` HashMap prevents duplicate I/O when concurrent calls race
  for the same id. `m_mutex` released before all I/O and loader dispatch.
- `ResourceHandleBase::wait_ready()`: atomic exchange on `block->load_counter` (first caller
  claims it); fiber-cooperative via `crd::jobs::wait`; spin+yield fallback for non-fiber callers.
  Terminal-state fast path also attempts counter claim to handle the job-completes-before-store
  race (counter leak fix).
- `resource_handle.cpp` (new): `release_block()` + `wait_ready()` moved out of headers so they
  can include `loader.hpp` and `jobs.hpp` without polluting public includes.
- `run_load_job` moved to `public` in `ResourceManager` so the anonymous-namespace `LoadJobFn`
  closure can call it.

**Tests shipped:** 4 AsyncFile tests (`tests/platform/test_async_file.cpp`) + 5 load_async tests
(in `tests/resources/test_resource_manager.cpp`). All 6 configs green: 429/429 win-debug, 426/426
win-release.

**Session log:** `docs/sessions/2026-05-04-resources-v1d.md`

### v1e — `ShaderResourceLoader` + `MaterialResourceLoader` + end-to-end smoke ✅ SHIPPED 2026-05-04

**Scope shipped:**
- `ShaderResourceLoader` (`engine/shader/src/shader_resource_loader.cpp`): reads SPVV/SPVF/SPVC
  chunk from a `type='SHDR'` artifact to determine stage, copies SPIRV bytes, runs spirv-reflect
  to populate `descriptor_bindings`, `push_constants`, `vertex_attributes`. Registered via
  `crd::shader::register_shader_loader(rm)`. Version 1.
- `MaterialResourceLoader` (`engine/renderer/src/material_resource_loader.cpp`): reads 32-byte
  META chunk, extracts vert/frag `ResourceId` pairs, calls `ctx.manager->load_sync<ShaderResource>`
  transitively, builds `MaterialResource` with two typed handles. Registered via
  `crd::renderer::register_material_loader(rm)`. Version 1.
- `compile_glsl()` free function (`engine/shader/src/compile.cpp`): shaderc-backed GLSL→SPIRV
  helper used by tests and the cooker without requiring the full shader runtime.
- GLSL cooker handler: `.glsl` files → `type='SHDR'` CRDR artifact with a single SPVV/SPVF/SPVC
  chunk based on detected stage suffix.
- Material cooker handler: `.mat.toml` → `type='MATR'` CRDR artifact with 32-byte META chunk
  encoding vert/frag `ResourceId` pairs.
- `smoke_resources_render.exe`: assembles two SHDR + one MATR artifacts in-memory, mounts as a
  PACK, `load_sync<MaterialResource>`, asserts both shader handles are Ready. Output confirms
  SPIRV sizes (vert=1040, frag=572 bytes).

**Clang-cl fix:** Removed dead `to_parameter_class_local` from `shader_resource_loader.cpp`
(MSVC `/W4 /WX` silently accepts unused statics; `-Werror,-Wunused-function` under clang-cl
treats them as errors).

**Tests:** 6 new test cases in `tests/resources/test_shader_material_loaders.cpp`: vertex SHDR
round-trip, fragment SHDR round-trip, missing SPIRV → Failed, material loads + resolves transitive
deps (verifies `handle_count() == 3`), missing META → Failed, real SPIRV round-trip via
`compile_glsl()` (gracefully skipped when shaderc unavailable).

All 6 configs green: win-debug/relwithdebinfo/asan/clang-cl/tidy 435/435, win-release 432/432.

### v1f — Hot-reload (PACK-file mtime watcher, atomic payload swap, last-good preservation) ✅ SHIPPED 2026-05-04

**Design deviation from original spec:** The original description planned to watch source roots and
invoke the cooker on change. The shipped implementation watches the cooked PACK files directly
(mtime-based polling). Rationale (ADR-0036): `crd-resources` must not depend on `crd-cooker` or
`crd-shader` at link time — watching PACK files avoids that dependency entirely. The cooker is
an external tool; a developer re-runs it outside the engine and the engine detects the result.
This also makes the hot-reload path consistent across all resource types without per-type cooker
integration.

**Shipped scope:**
- `ResourceControlBlock::payload` made `std::atomic<void*>` (was `void*`). This is the correctness
  fix that enables concurrent `get()` calls to run safely while a hot-reload swaps the payload.
  The acquire/release ordering: writers do `payload.store(release)` then `state.store(release)`;
  readers do `state.load(acquire)` then `payload.load(acquire)`. State stays `Ready` during swap;
  the atomic makes the payload visible to concurrent readers without a data race.
- `PackWatch` per mounted PACK (stored in `m_pack_watches`): tracks `last_processed_mtime`,
  debounce state (`pending_mtime`, `has_pending`, `pending_since`).
- `poll_hot_reload(debounce_ms=200)`: call once per frame from the main thread. Checks mtime for
  each mounted PACK; after the debounce window fires `do_reload_mount`.
- `reload_mount_now(MountId)`: force-reload bypassing mtime, used in unit tests and tooling.
- `do_reload_mount(PackWatch&)`: re-reads the pack file, parses new manifest (updates `m_live` blob
  offsets), collects loaded handles, calls `loader->load()` for each, atomically exchanges the
  payload, bumps `generation`, defers the old payload to `m_deferred_frees` (freed at the START of
  the next `poll_hot_reload` or `reload_mount_now` call — one-frame grace period).
- Failed reload (null loader result or corrupt pack) preserves the existing payload and state.
- `subscribe_reload(id, cb, user)` / `unsubscribe_reload(id, token)`: per-resource callbacks
  fired (outside the mutex) after each successful swap, with `(id, new_generation, user)`.
- `unmount()` also removes the corresponding `PackWatch`.

**New tests (4) in `tests/resources/test_hot_reload.cpp`:**
- Payload swap + generation bump: verify V1 bytes before, V2 bytes after `reload_mount_now`.
- Failed reload preserves last-good: corrupt pack → `n==0`, old payload still `Ready` with gen 0.
- `subscribe_reload` callback fires with correct id + generation.
- `unsubscribe_reload` prevents callback (reload still happens, no notification).

**New smoke: `smoke_resources_reload.exe`** — writes V1 PACK, mounts, loads, rewrites V2, sleeps
1100 ms (ensures different mtime), calls `poll_hot_reload(0)`, verifies V2 payload + callback.

**Six-configuration green:**
- win-debug:          439/439
- win-relwithdebinfo: 439/439
- win-release:        pre-existing failures in shader/material tests (0xc0000409 in shaderc, unchanged)
- win-asan:           pre-existing DLL failures in jobs tests (0xc0000135, unchanged)
- win-clang-cl:       439/439
- win-tidy:           439/439 (pre-existing `kPayload` naming warnings in test_resource_manager.cpp, unchanged)

### v1g — Streaming + 2Q LRU eviction + memory budget + pinning

**Scope:**
- `ResourceManager::set_memory_budget(bytes)`. When `current_memory_use()` exceeds the budget,
  the eviction policy picks victims among unpinned, refcount-zero, non-`Loading` entries.
- 2Q (Two Queue) eviction:
  - `A1in` — FIFO of recent loads (probationary)
  - `Am`   — LRU of items hit again from `A1in` (long-term)
  - `A1out` — ghost queue of recently-evicted ids (no payload, just id) used to promote
    re-loaded victims directly into `Am`
- `ResourceManager::pin(id)` / `unpin(id)` — pinned entries are exempt from eviction.
- `ResourceManager::load_streamed<T>(id)` — delivers payload via `crd::platform::AsyncFile`
  inside a job fiber (stream_file field in LoadContext). Same coalescing/re-issue logic as load_async.
- `LoadState::Unloaded` re-issue path: an evicted block stays in `m_handles`; re-load bumps
  `generation` and re-runs the loader without allocating a new block.

**Shipped scope (2026-05-04):**
- `ResourceControlBlock` extended: `bool pinned`, `EvictQueue evict_queue` (None/A1in/Am),
  `crd::u64 payload_size` (set from `blob_size` at load time; used as memory-use proxy).
- `LoadContext` extended: `stream_file`, `stream_offset`, `stream_size` for streamed loaders.
- `ResourceManager` private 2Q state: `m_memory_budget`, `m_memory_used`, `m_a1in`, `m_am`,
  `m_a1out`, `m_a1out_set`, `m_pin_counts`. All guarded by `m_mutex`.
- `insert_into_2q` / `touch_in_am` / `evict_block_locked` / `try_evict_to_budget` helpers.
- `load_sync_impl` Phase 1 re-issue: detects `state == Unloaded`, reuses existing block,
  bumps generation, re-runs loader.
- `load_streamed_impl` / `run_stream_load_job`: opens `AsyncFile`, calls `read_async`, waits
  the counter, then dispatches loader with `stream_file` set.
- `smoke_resources_stream.exe`: writes a PACK with value=0xCAFEBABE, `load_streamed`, `wait_ready`,
  verifies payload, exits 0.

**Key design decisions:**
- 2Q ghost queue uses order-preserving `erase(i)` (not `swap_remove`) to maintain FIFO semantics.
- A1out bounded at `kMaxA1out = 256` to cap memory use of the ghost set.
- `m_memory_used` tracks `blob_size` (on-disk artifact size), not actual allocation size, as a
  proxy for working-set pressure — avoids loader-private allocator intrusion.
- Pin-before-load honoured: `m_pin_counts.find(id)` checked during Phase 4 finalize to set
  `block->pinned = true` even if pin() was called before the first load.

**New tests (5) in `tests/resources/test_eviction.cpp`:**
- Budget enforced: 4 resources, budget = 2 × blob_sz; after all loads dropped,
  `current_memory_use() <= budget`.
- Pinned survives: pin resource 0 before any load; apply eviction pressure; resource 0 still
  `Ready` after.
- Re-issue increments generation: load → drop → budget=0 → reload → `generation == 1`.
- 2Q ghost hit promotes to Am: A evicted→A1out, re-loaded→Am; D load evicts C (A1in) not A (Am).
- `load_streamed` delivers correct payload via `AsyncFile` end-to-end.

**Six-configuration green:**
- win-debug:          444/444
- win-relwithdebinfo: 444/444
- win-release:        441/441
- win-asan:           444/444
- win-clang-cl:       444/444
- win-tidy:           444/444

---

## Shader / material integration

The shader system already exists (Phase 2.3). Integration is additive:

- `crd-shader` gains a `ShaderResourceLoader` source file that depends on `crd-resources` headers
  but is registered by application code. The shader runtime itself does NOT depend on
  `crd-resources` — only the loader adapter does.
- The cooker's `.glsl` handler reuses the same shaderc / spirv-reflect path as the live shader
  runtime; the cooked artifact's `SPRV` and `REFL` chunks are exactly what the runtime would
  produce in a live compile, just persisted.
- Variant keys (ADR-0026) are stable across cook and runtime: a cooked artifact stores variants
  the cooker chose to pre-build, and the runtime can still produce additional variants on demand
  (cook is a warm cache, not a sealed set).
- Hot-reload of a `.glsl` source triggers a re-cook of the artifact, then atomic swap into the
  existing `Effect`'s variant table — already the contract from ADR-0029.

The material loader follows the same pattern: `MaterialInstance` (the live object built by
`crd-renderer`) is constructed from the cooked `META` + `BLOB` chunks, not from the `.mat.toml`
source. The TOML never reaches the runtime.

---

## DAW extensibility hooks

A DAW build needs three things from `crd-resources`, all of which are already in the public API:

1. **`register_loader(loader)`** — the DAW registers `SamplePackLoader`, `PluginPresetLoader`,
   `ProjectFileLoader`. None of these touch `crd-rhi`. The DAW build links `crd-resources`
   without any GPU module.

2. **`register_cook_handler(ext, fn)`** in the cooker — the DAW's tools-side build adds handlers
   for `.wav` → cooked sample, `.fxp` (VST2 preset) → cooked preset, `.daw_project.toml` →
   cooked project file.

3. **`mount_manifest(path)`** with last-mount-wins ordering — the DAW mounts the engine pack
   first, the user's project pack last, so project samples can override engine defaults.

Procedural runtime resources (a render-bounce of a track, a frozen MIDI clip): created with
`ResourceId::mint_random()` and `manager->install<T>(id, payload)` (a v1c `ResourceManager`
helper for runtime-injected resources that bypass the cooker / mount path).

---

## Module layout

```
engine/resources/
  include/crd/resources/
    resource_id.hpp         ← v1a
    container.hpp           ← v1a (chunked CRDR reader/writer)
    resource_manager.hpp    ← v1a (shell), grows through v1g
    resource_handle.hpp     ← v1c
    loader.hpp              ← v1c (ILoader + LoadContext)
    mount.hpp               ← v1a (MountId, manifest types)
    eviction.hpp            ← v1g (2Q policy)
    file_watcher.hpp        ← v1f (dev-only)
    resources.hpp           ← umbrella
  src/
    resource_id.cpp
    container_reader.cpp
    container_writer.cpp
    resource_manager.cpp
    handle_table.cpp
    file_watcher_win32.cpp  ← v1f
    file_watcher_linux.cpp  ← v1f
    eviction_2q.cpp         ← v1g
  CMakeLists.txt

tools/asset_cooker/
  include/crd/cooker/
    cook_handler.hpp        ← v1b
    cooker.hpp
  src/
    main.cpp                ← CLI dispatch
    cook_command.cpp        ← `cook` sub-command
    manifest_dump.cpp       ← `manifest_dump` sub-command
    resource_dump.cpp       ← `resource_dump` sub-command
    cook_handlers/
      blob_passthrough.cpp  ← v1b
      glsl.cpp              ← v1e (links shader compile path)
      material.cpp          ← v1e
  CMakeLists.txt

engine/platform/
  include/crd/platform/async_file.hpp   ← v1d (extension)
  src/async_file_iocp.cpp                ← v1d (Windows)
  src/async_file_uring.cpp               ← v1d (Linux)

tests/resources/
  test_resource_id.cpp     ← v1a
  test_container.cpp       ← v1a
  test_manifest.cpp        ← v1a
  test_loader.cpp          ← v1c
  test_async_load.cpp      ← v1d
  test_hot_reload.cpp      ← v1f
  test_eviction.cpp        ← v1g

tests/platform/
  test_async_file.cpp      ← v1d (added to existing platform tests)

runtime/examples/
  smoke_resources.cpp           ← v1c
  smoke_resources_async.cpp     ← v1d
  smoke_resources_render.cpp    ← v1e
  smoke_resources_reload.cpp    ← v1f
  smoke_resources_stream.cpp    ← v1g
```

---

## Definition of done (Phase 2.6)

1. All 7 slices (v1a–v1g) shipped with unit tests.
2. `smoke_resources_render` runs end-to-end: cook a shader + material, mount the pack, load via
   `ResourceManager`, render a triangle from cooked assets only.
3. `smoke_resources_reload` runs: edit a source file, observe the on-screen result update without
   process restart.
4. `smoke_resources_stream` runs: load a blob larger than the configured budget; eviction occurs;
   payload still observable via streamed reads.
5. Six-configuration green: win-debug / win-release / win-asan / win-clang-cl /
   win-relwithdebinfo / win-tidy.
6. `crd::memory::RefCounted<T>` shipped (ADR-0014 prerequisite).
7. `crd::platform::AsyncFile` shipped (ADR-0041, closes ADR-0022 prerequisite 3).
8. ADRs 0036–0041 filed and cross-referenced.
9. `crd-resources` builds and tests with no dependency on `crd-rhi`, `crd-shader`, or
   `crd-renderer` (verified by a test target that links only `crd-resources` + lower modules).

---

## Open questions

- **Per-resource payload schema language.** Hand-rolled POD layouts work for v1a–v1g but a real
  schema language (FlatBuffers, Cap'n Proto, hand-rolled IDL) becomes valuable when scene/ECS
  data forces nested mutable structures. Decision deferred to Phase 3.1c.
- **Binary manifest promotion.** When entry counts reach hundreds of thousands, a paged or
  sharded manifest may be needed. Phase 3.x revisits.
- **Cooker DLL plug-ins.** Compile-time `register_cook_handler` is fine while we own every
  handler. DLL-based plug-ins arrive with Phase 4 hot-reload scripting.
- **DLC pack signing / encryption.** Phase 4+ as the consumer story (Steam, console SDKs,
  network distribution) clarifies.
- **Editor-side cook drive.** Phase 7 — the editor would invoke the cooker on save and re-trigger
  hot-reload.
- **ARC eviction.** Door left open behind a v2 eviction policy plug-in slot, only with measured
  workload data showing 2Q is insufficient.

---

## References

- ADR-0036 — `crd-resources` module placement + loader-registry pattern
- ADR-0037 — ResourceId hybrid UUID scheme
- ADR-0038 — Cooked binary container format
- ADR-0039 — ResourceHandle<T> semantics
- ADR-0040 — Cooker CLI + CMake integration
- ADR-0041 — `crd-platform` async filesystem I/O
- ADR-0013 — Asset pipeline (separate cooker exe; restated by ADR-0040)
- ADR-0014 — Reference counting split (RefCounted prerequisite for v1c)
- ADR-0022 — Open-world streaming pipeline (closed by ADR-0041 + this phase)
- ADR-0029 — Shader hot reload (mirrored by v1f reload semantics)
- ADR-0033 — `crd-jobs` implementation (used by v1d async loads)
- Johnson & Shasha 1994 — "2Q: A Low Overhead High Performance Buffer Management Replacement Algorithm"
- Megiddo & Modha 2003 — "ARC: A Self-Tuning, Low Overhead Replacement Cache" (door left open for v2)
