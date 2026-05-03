# Session — 2026-05-03: Phase 2.6 v1c — RefCounted<T> + ResourceHandle<T> + load_sync<T>

**Status:** SHIPPED  
**Tests after:** 420/420 win-debug, 420/420 win-relwithdebinfo, 420/420 win-asan, 420/420 win-clang-cl, 420/420 win-tidy, 417/417 win-release  
**Duration:** one session

---

## What shipped

### `crd::memory::RefCounted<T>` (`engine/memory/include/crd/memory/ref_counted.hpp`)

CRTP intrusive reference-count base (ADR-0014 prerequisite, called out in the Phase 2.6 design):

```cpp
template <typename Derived>
class RefCounted
{
public:
    void add_ref() noexcept
    {
        m_refs.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] crd::u32 release() noexcept
    {
        const crd::u32 prev = m_refs.fetch_sub(1, std::memory_order_acq_rel);
        CRD_ASSERT_MSG(prev > 0U, "RefCounted::release() on object with zero refcount");
        return prev - 1U;
    }

    [[nodiscard]] crd::u32 use_count() const noexcept
    {
        return m_refs.load(std::memory_order_relaxed);
    }

protected:
    ~RefCounted() = default;   // prevents stack destruction with live refs

private:
    std::atomic<crd::u32> m_refs{1};
};
```

Design choices:
- `release()` is `[[nodiscard]]` — callers must inspect the returned count to know whether they should free.
- Protected destructor prevents accidental destruction from a base pointer while refs > 0.
- `add_ref()` uses `relaxed` (only needs atomicity, not ordering — the refcount itself carries no data).
- `release()` uses `acq_rel` — the decrement must be an acquire-release fence so that any cleanup done by the caller after `release()` returns 0 is correctly ordered relative to the last user.
- NOT added to `memory.hpp` umbrella to avoid PCH invalidation in every translation unit that includes memory.

Tests in `tests/memory/test_memory.cpp` (12 new):
- Starts at 1.
- `add_ref` increments count.
- `release` decrements and returns new count (each CHECK wrapped in a local `const auto r = ...` block to avoid MSVC C4834 on `[[nodiscard]]`).
- 4-thread concurrent stress: each thread does 25 `add_ref` then 25 `release`. A bump to 2 at start ensures the count never transiently hits 0 during parallel ops. Final count must equal 2.

### `ResourceControlBlock` (`engine/resources/include/crd/resources/resource_control_block.hpp`)

Inherits `RefCounted<ResourceControlBlock>`. Fields:

| Field | Type | Notes |
|---|---|---|
| `id` | `ResourceId` | which resource this block represents |
| `type_fourcc` | `u32` | loader FourCC |
| `permanent` | `bool` | false = freed when refs → 0; true = outlives all handles |
| `generation` | `atomic<u32>` | bumped on successful hot-reload (v1f) |
| `state` | `atomic<LoadState>` | Unloaded / Queued / Loading / Ready / Placeholder / Failed |
| `payload` | `void*` | loader-allocated object; null until Ready/Placeholder |
| `loader` | `ILoader*` | back-pointer for `unload()` call in destructor |
| `alloc` | `IAllocator*` | allocator used to free this block itself |

**`permanent` flag semantics:** The flag divides blocks into two lifetimes:
- `permanent = true` — set after a successful `load()` or `load_placeholder()`. The block is inserted into `m_handles` and lives until `ResourceManager::~ResourceManager()` calls `unload()`, destructs, and frees it.
- `permanent = false` (default) — set only by `make_failed_block()` (all hard-failure paths). These blocks are NOT inserted into `m_handles`. When the last `ResourceHandle` drops, `release_block()` sees refs == 0 and frees the block immediately.

### `LoadState` enum (`engine/resources/include/crd/resources/load_state.hpp`)

```cpp
enum class LoadState : crd::u8
{
    Unloaded,     // handle is null / never requested
    Queued,       // submitted but not yet dispatched (v1d)
    Loading,      // loader running (v1d)
    Ready,        // payload valid
    Placeholder,  // soft fallback from load_placeholder()
    Failed,       // hard failure; get() == nullptr
};
```

### `ResourceHandleBase` + `ResourceHandle<T>` (`engine/resources/include/crd/resources/resource_handle.hpp`)

`ResourceHandleBase` (non-template) — holds one `ResourceControlBlock*`:
- Default-constructed: `m_block = nullptr`.
- Copy ctor / copy assign: calls `m_block->add_ref()` on the new block.
- Move ctor / move assign: steals the pointer, sets source to null.
- Destructor: calls `release_block()`.
- `state()`: returns `LoadState::Unloaded` if `m_block == nullptr`, otherwise `m_block->state.load(acquire)`.
- `is_ready()`: `state() == LoadState::Ready`.
- `id()`, `generation()`: forward to block.
- `wait_ready()`: v1c no-op (returns current state). Will be fiber-cooperative in v1d.
- `release_block()`: calls `m_block->release()`; if result == 0 AND `!m_block->permanent`, calls `m_block->loader->unload(payload)`, then `~ResourceControlBlock()`, then `m_alloc->deallocate(block)`.

`ResourceHandle<T>` (template, inherits base):
- `get() const noexcept` — returns `static_cast<const T*>(m_block->payload)` when state is Ready or Placeholder; otherwise `nullptr`.
- `sizeof(ResourceHandle<T>) == sizeof(pointer)` — one pointer on any platform.

### Thread-local cycle detection

```cpp
constexpr crd::usize kMaxVisitDepth = 64;
thread_local ResourceId  tl_visiting[kMaxVisitDepth];
thread_local crd::usize  tl_visit_count = 0;
```

`visiting_contains(id)`, `visiting_push(id)` (asserts < 64), `visiting_pop()` (asserts > 0) live in the anonymous namespace of `resource_manager.cpp`. The cycle check fires before dispatching to `loader->load()`, with a push before and pop after:

```
visiting_push(id)
payload = loader->load(ctx)    // ctx.manager allows transitive load_sync
visiting_pop()
```

v1c limitation documented in-source: thread-local storage is per-OS-thread; fiber migration across `await` points would invalidate the stack. Safe in v1c (all loads are synchronous; no fiber suspend mid-load). Marked for revisit in v1d.

### `load_sync_impl` — synchronous load implementation

Error hierarchy (all non-null returns):
1. **Cache hit** — `m_handles.find(id)` returns existing block; `add_ref()`, return.
2. **Cycle** — `visiting_contains(id)` → `make_failed_block()`, log Error.
3. **Not in manifest** — `m_live.find(id) == nullptr` → `make_failed_block()`, log Error.
4. **No loader** — `m_loaders.find(type_fourcc) == nullptr` → `make_failed_block()`, log Error.
5. **Mount missing** — `find_mount(entry->mount_id) == nullptr` → `make_failed_block()`, log Error (internal consistency check).
6. **File read fail** — `read_file_range(...)` returns false → `make_failed_block()`, log Error.
7. **Loader success** — `loader->load(ctx)` returns non-null; block is `permanent = true`, inserted into `m_handles`, state = Ready.
8. **Loader fail + placeholder** — `load()` returns null, `load_placeholder(ctx)` returns non-null; block is `permanent = true`, state = Placeholder.
9. **Hard failure** — both `load()` and `load_placeholder()` return null; block stays `permanent = false`, state = Failed.

`make_failed_block()` (file-scope static):
```cpp
static ResourceControlBlock* make_failed_block(crd::memory::IAllocator* alloc, ResourceId id)
{
    void* raw = alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
    auto* block = new (raw) ResourceControlBlock();
    block->id    = id;
    block->alloc = alloc;
    // permanent = false (default) → freed by the last handle when refs drops to 0
    block->state.store(LoadState::Failed, std::memory_order_release);
    return block;
}
```

### `read_file_range` (`engine/platform/include/crd/platform/filesystem.hpp`)

New `crd::platform::fs` function:

```cpp
[[nodiscard]] bool read_file_range(
    const Path& path,
    crd::u64 offset,
    crd::u64 size,
    crd::containers::Array<crd::u8>& out) noexcept;
```

Uses `std::ifstream` with `std::ios::binary`, `seekg` to the byte offset, then `read` into the pre-resized `Array`. Returns `false` if the file can't be opened, seek fails, or the read is short. Avoids `cstdio` leaking into `resource_manager.cpp`.

### `ResourceManager` destructor + `handle_count()`

Destructor iterates all permanent blocks in `m_handles`:
```cpp
for (auto it = m_handles.begin(); it != m_handles.end(); ++it)
{
    ResourceControlBlock* block = it.value();
    if (block->payload && block->loader)
        block->loader->unload(block->payload);
    block->~ResourceControlBlock();
    m_alloc->deallocate(block);
}
```

`handle_count() const noexcept` → `m_handles.size()` (diagnostics).

### `smoke_resources.exe`

New `runtime/examples/smoke_resources.cpp`. End-to-end path in a single executable:
1. Assembles a PACK file in memory (two-pass: pass 1 measures CRDR section size, pass 2 writes real `blob_offset`).
2. Writes the pack to a temp file, mounts it.
3. `load_sync<BlobResource>(blob_id)` — custom loader wraps raw bytes.
4. Asserts `state() == Ready`, `res->bytes.size() == 5`, bytes match `{0x01, 0x02, 0x03, 0x04, 0x05}`.
5. Deletes the temp file and exits 0.

Prints: `smoke_resources: OK — BlobResource loaded and verified`

### Tests (`tests/resources/test_resource_manager.cpp` additions)

8 new v1c tests (appended to the existing 6 from v1a):
- **load_sync round-trip** — BlobResource loads Ready, bytes match.
- **refcount stability (1000 copies)** — copy handle 1000 times, all copies drop; block still in `m_handles`, `handle_count() == 1` (permanent block not freed).
- **cache hit** — second `load_sync` returns same block pointer, `use_count()` shows 2.
- **unknown id** — `load_sync` for unmounted id returns `state() == Failed`.
- **hard failure** — loader that always returns nullptr → `state() == Failed`, `get() == nullptr`.
- **placeholder** — loader whose `load()` returns nullptr but `load_placeholder()` returns a value → `state() == Placeholder`, `get() != nullptr`.
- **transitive dependency** — a `ChainedLoader` that calls `ctx.manager->load_sync<BlobResource>(dep_id)` inside `load()`; both parent and dep end up `Ready`.
- **cycle detection** — two resources each declaring the other as a dep; both end up `Failed`, cycle logged as Error.

---

## Issues encountered and resolved

1. **MSVC C4834 on `[[nodiscard]] release()` inside `CHECK(...)`**  
   MSVC 14.50 warns when a `[[nodiscard]]` return value is "discarded" even within a Catch2 `CHECK(expr)` macro (due to how Catch2 wraps the expression in a temporary). Fix: `{ const auto r = w.release(); CHECK(r == expected); }` for assertion uses; `static_cast<void>(w.release())` for explicit discards in concurrent tests.

2. **`crd::containers::String` not formattable by `std::format`**  
   `CRD_LOG_*` macros forward to `std::format`. `String` has no `std::formatter<>` specialization. Fix: use `id.to_string(m_alloc).c_str()` (returns `const char*`, which `std::format` handles as `%s`-style), or simplify log messages to omit the id string entirely for error paths that fire rarely.

3. **`mount->pack_path.as_view()` does not exist**  
   `crd::containers::String` has `operator std::string_view()` but no `as_view()` method. Fix: pass `mount->pack_path` directly to `crd::platform::fs::Path`'s constructor, which accepts `StringView`; `String` implicitly converts.

4. **`load_sync` returning `nullptr` → `state() == Unloaded` (not `Failed`)**  
   Initial design had `load_sync_impl` return `nullptr` on error, and the handle default-constructed to Unloaded. Tests expected `Failed`. Fix: all error paths now call `make_failed_block()` — the function always returns a non-null block with `state = Failed`. A null block is never stored in a handle.

5. **Two-pass PACK assembly in tests**  
   The test helper `write_pack_with_artifacts()` must replicate the cooker's two-pass logic: pass 1 builds the CRDR section to measure its size; pass 2 recomputes `blob_offset = crdr_section_size` and appends artifact bytes after. Without pass 1, blob_offset would be wrong and `read_file_range` would read garbage.

6. **clang-tidy naming suggestion for test-local `const`**  
   win-tidy suggested renaming a local `const u8 kDepByte = ...` to `k_dep_byte` (snake_case for local constants). This did not block the build (clang-tidy note, not error) and was not changed — the `kCamelCase` convention for all constants is intentional per CLAUDE.md.

---

## Proposed commit message

```
feat(resources): Phase 2.6 v1c — RefCounted<T> + ResourceHandle<T> + load_sync<T>

Adds crd::memory::RefCounted<T> (CRTP intrusive refcount, add_ref/release/use_count,
protected dtor) in ref_counted.hpp; not added to memory.hpp umbrella to avoid PCH churn.

ResourceControlBlock inherits RefCounted; carries id, type_fourcc, permanent flag,
atomic generation/state, payload, loader, and alloc. permanent=false blocks are freed
by the last handle dtor; permanent=true blocks live in m_handles until manager shutdown.

ResourceHandleBase (non-template) holds one pointer; release_block() in dtor handles
the permanent/non-permanent split. ResourceHandle<T> typed wrapper adds get() casting
payload to const T* when state is Ready or Placeholder. sizeof(ResourceHandle<T>) == sizeof(pointer).

load_sync_impl: checks cache -> cycle -> manifest -> loader -> mount -> file read ->
dispatch to loader->load(), then load_placeholder() on failure. make_failed_block()
covers all error paths (always returns non-null, non-permanent Failed block). Thread-local
visiting stack [64] for cycle detection (v1c limitation: fiber-migration-unsafe; revisit v1d).

Adds read_file_range() to crd::platform::fs (seekg + read into Array<u8>).
Adds smoke_resources.exe: assemble PACK in-memory, mount, load_sync<BlobResource>,
verify 5-byte payload, exit 0.

12 new RefCounted tests + 8 new load_sync tests. Total: 420 tests.
All 6 configs green: debug/relwithdebinfo/asan/clang-cl/tidy 420/420, release 417/417.
```
