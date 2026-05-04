# 2026-05-04 — Phase 2.6 v1f: hot-reload (atomic swap, mtime watching, callbacks)

## What shipped

### Atomic `payload` in `ResourceControlBlock`

`engine/resources/include/crd/resources/resource_control_block.hpp`

`payload` promoted from `void*` to `std::atomic<void*>`:

```cpp
// Atomic so hot-reload can swap the payload concurrently with get() calls.
// Writers: exchange(acq_rel) then state.store(release); readers: state.load(acquire) then payload.load(acquire).
std::atomic<void*> payload{nullptr};
```

State stays `Ready` during a hot-reload swap — the acquire/release chain on `state` alone does not protect the payload pointer, so making it atomic is the correct fix for the data race. Readers (`get()`) do `payload.load(acquire)` and see either the old or new pointer, both of which are valid live objects during the swap window.

All existing `payload` read/write sites updated:
- `resource_handle.hpp` `get()`: `b->payload.load(std::memory_order_acquire)`
- `resource_handle.cpp` `release_block()`: atomic load before unload, atomic store of null after
- `resource_manager.cpp` `load_sync_impl` Phase 4 + `run_load_job` finalize + `run_load_job` free path: `store(release)`

### Hot-reload public API

`engine/resources/include/crd/resources/resource_manager.hpp`

```cpp
using ReloadCallback = void (*)(ResourceId id, crd::u32 new_generation, void* user);

// Subscribe/unsubscribe to reload events for a specific resource.
crd::u32   subscribe_reload(ResourceId id, ReloadCallback cb, void* user);
void       unsubscribe_reload(ResourceId id, crd::u32 token);

// Poll all mounted packs for mtime changes; debounce_ms delays action for partially-written files.
// Returns the number of resources reloaded.
crd::usize poll_hot_reload(crd::u32 debounce_ms = 200U);

// Force-reload a specific mount immediately, bypassing mtime detection.
// Used in unit tests to avoid filesystem timing issues.
crd::usize reload_mount_now(MountId id);
```

### Internal hot-reload structures

Added to `ResourceManager` private section:

```cpp
struct ReloadSub  { ReloadCallback cb = nullptr; void* user = nullptr; crd::u32 token = 0U; };
struct PackWatch
{
    crd::u32                              mount_id;
    crd::containers::String               pack_path;       // absolute path to .pack file
    crd::i64                              last_processed_mtime = 0;
    crd::i64                              pending_mtime        = 0;
    bool                                  has_pending          = false;
    std::chrono::steady_clock::time_point pending_since{};
    PackWatch(crd::memory::IAllocator* a, crd::u32 id,
              crd::containers::StringView path, crd::i64 initial_mtime);
};
struct DeferredFree { void* payload = nullptr; ILoader* loader = nullptr; };

crd::u32                                                                   m_next_reload_token = 1U;
crd::containers::HashMap<ResourceId, crd::containers::Array<ReloadSub>>   m_reload_subs;
crd::containers::Array<PackWatch>                                          m_pack_watches;
crd::containers::Array<DeferredFree>                                       m_deferred_frees;
```

`PackWatch` is added at `mount_manifest()` success, removed at `unmount()` (swap-remove by mount_id).
`DeferredFree` entries are drained at the start of each `poll_hot_reload()` / `reload_mount_now()` call,
giving a one-frame grace period before old payloads are freed. Raw-pointer holders that live longer than
one frame must use `ResourceHandle`.

### `do_reload_mount` — the core reload path

Private helper called by both `poll_hot_reload` and `reload_mount_now`:

1. **Re-read pack file** from disk into a fresh buffer.
2. **Parse new manifest** under lock; update `m_live[id]` offsets for known resources so subsequent
   `load_sync` calls use the new positions.
3. **Collect reload tasks** (block pointer + loader + new offset) then drop the lock.
4. **Reload outside lock**: for each task, call `loader->load(ctx)` with the new chunk data.
   - If `load()` returns null (failed compile, corrupt data, missing dependency), skip the swap —
     old payload stays intact (last-good preservation).
5. **Atomic swap** for successful reloads:
   ```cpp
   void* old_payload = task.block->payload.exchange(new_payload, std::memory_order_acq_rel);
   const crd::u32 new_gen = task.block->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
   task.block->state.store(LoadState::Ready, std::memory_order_release);
   if (old_payload != nullptr)
       m_deferred_frees.push_back(DeferredFree{old_payload, task.loader});
   ```
6. **Fire callbacks**: `m_reload_subs` entries for each reloaded id are copied to a local
   `Array<ReloadSub>` before firing, so callbacks can safely call `subscribe_reload` /
   `unsubscribe_reload` without invalidating the iteration.

### Design deviation: mtime watching on PACK files

The Phase 2.6 design doc planned to watch source root directories and invoke the cooker on change.
This was deferred — `crd-resources` would have needed to link `crd-cooker` or shell out to the cooker
CLI, neither of which fits the module isolation rule (ADR-0036). The shipped design polls cooked PACK
files for mtime changes instead. The cooker runs separately (either in CI, a dev-side watcher script,
or the future editor) and writes new PACK files; `poll_hot_reload` detects the mtime change on the
pack and reloads from the new data. The deviation is low-risk: hot-reload consumers always had to
run the cooker first anyway.

### `mount_manifest()` and `unmount()` wiring

After a successful `mount_manifest()`, a `PackWatch` is added pointing at the same pack file path,
with `initial_mtime` set from `Filesystem::stat()`. If `stat()` fails (in-memory virtual packs used
by tests), `last_processed_mtime` is left at 0 — `poll_hot_reload` will treat the path as unwatchable.

`unmount()` removes the corresponding `PackWatch` by scan + swap-remove, so unmounted packs are no
longer polled.

## Tests

`tests/resources/test_hot_reload.cpp` — 4 `TEST_CASE` entries, all using `reload_mount_now` to avoid
filesystem timing dependencies:

| Tag | What |
|---|---|
| `[resources][hot-reload][v1f]` | Payload swap + generation bump: write V1 blob, reload with V2 blob, verify new payload content and generation == 2 |
| `[resources][hot-reload][v1f]` | Failed reload preserves last-good: reload with null-returning loader, verify old payload still accessible |
| `[resources][hot-reload][v1f]` | Subscribe fires callback: reload triggers callback with correct id and new_generation |
| `[resources][hot-reload][v1f]` | Unsubscribe prevents callback: after `unsubscribe_reload`, reloading does not call the detached callback |

Local test types:
- `HRBlobResource` — plain `crd::u32 value` payload, trivially destructed
- `HRBlobLoader` — reads 4 bytes from first DATA chunk; returns null when blob is empty (failed path)
- `write_blob_pack()` — helper assembling a minimal 2-resource PACK (MFST + DATA chunks per resource)

## Smoke

`runtime/examples/smoke_resources_reload.cpp` — end-to-end test using real `poll_hot_reload(0)`:

1. Write a PACK to a temp file with V1 blob content.
2. Mount and load — verify V1 payload.
3. Sleep 1100 ms (ensures mtime is different from V1 write time on all filesystems).
4. Overwrite the same PACK file with V2 blob content.
5. Call `poll_hot_reload(0)` — debounce 0 so no wait.
6. Verify payload changed to V2 value, generation bumped to 2, callback fired with correct id.

Local `ReloadResult` struct (avoids raw `new`):
```cpp
struct ReloadResult { bool fired = false; crd::u32 gen = 0U; };
```

Output:
```
smoke_resources_reload: OK — payload swapped V1→V2, gen=2, callback fired
```

## Fixes and non-obvious implementation details

### C4834 on `[[nodiscard]] release()`

`RefCounted<T>::release()` is marked `[[nodiscard]]`. Three call sites in `do_reload_mount` discarded
the return value; MSVC `/WX` treats this as an error. Fix: `[[maybe_unused]] const crd::u32 r = task.block->release();` at all three sites.

### `CRD_LOG` format strings with `crd::containers::String`

`crd::containers::String` has no `std::formatter<>` specialization. Using it directly in a `CRD_LOG_WARN`
format string caused a MSVC `std::format` template error. Fix: create a local `StringView` at the top
of `do_reload_mount` and use it in all log calls:
```cpp
const crd::containers::StringView pv{watch.pack_path.data(), watch.pack_path.size()};
CRD_LOG_WARN(g_log_resources, "hot-reload: failed to re-read pack '{}'", pv);
```

### PCH corruption from parallel build

Adding `#include <chrono>` to `resource_manager.hpp` triggered a PCH rebuild. A parallel build kicked
off while the PCH was still being written, corrupting it. Fix: source `vcvars64.bat` before running
cmake to ensure the correct VS toolchain environment, then run a clean sequential build.

### Initial smoke used raw `new`

First draft of `smoke_resources_reload.cpp` allocated callback state with
`new std::pair<bool*, crd::u32*>`. This leaked memory and violated the RAII-only policy. Replaced
with a stack-allocated `ReloadResult` struct passed by pointer.

## Six-configuration results

| Config | Result |
|---|---|
| win-debug | 439/439 ✅ |
| win-relwithdebinfo | 439/439 ✅ |
| win-release | pre-existing delta unchanged ✅ |
| win-asan | pre-existing delta unchanged ✅ |
| win-clang-cl | 439/439 ✅ |
| win-tidy | 439/439 ✅ |

4 new hot-reload tests added (435 → 439).

## Decisions made

- **Atomic `payload` rather than reader locks.** `state` stays `Ready` during the swap; the acquire
  on `state` does not transitively protect the `payload` pointer. Making `payload` `std::atomic<void*>`
  with `exchange(acq_rel)` on write and `load(acquire)` on read is the minimal correct fix. No reader
  lock needed; swap is O(1) and wait-free from the reader's perspective.

- **Watch PACK mtime, not source roots.** Keeps `crd-resources` free of `crd-cooker` or `crd-shader`
  link dependencies (ADR-0036). The cooker is a separate process; polling the output pack is sufficient
  for dev hot-reload. The deviation is documented in the phase file.

- **Deferred free of old payloads.** One-frame grace period avoids use-after-free in code that holds
  raw payload pointers across a frame boundary. Draining happens at the top of each `poll_hot_reload`
  / `reload_mount_now`, so the window is bounded.

- **Callback list copied before firing.** Prevents iterator invalidation if a callback calls
  `subscribe_reload` or `unsubscribe_reload`. Cost is one small stack allocation per reloaded resource.

- **`reload_mount_now` for unit tests.** Tests using `poll_hot_reload` would need to write real files
  and sleep long enough for the OS to advance mtime — flaky and slow. `reload_mount_now` bypasses
  mtime entirely; tests inject new pack data directly. The smoke test exercises the real mtime path.
