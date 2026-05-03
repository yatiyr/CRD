# Session — 2026-05-04: Phase 2.6 v1d — AsyncFile + load_async<T> + wait_ready()

**Status:** SHIPPED  
**Tests after:** 429/429 win-debug, 429/429 win-relwithdebinfo, 429/429 win-asan, 429/429 win-clang-cl, 429/429 win-tidy, 426/426 win-release  
**Duration:** two sessions (context switch after implementation, docs written in second session)

---

## What shipped

### `crd::platform::AsyncFile` (`engine/platform/`)

Job-pool-based async file reader. Windows backend uses `ReadFile` inside a `crd-jobs` fiber-scheduled job. Linux can follow the same pattern via `pread`; IOCP/io_uring backends can slot in without changing the public API.

```cpp
// engine/platform/include/crd/platform/async_file.hpp
namespace crd::platform
{
class AsyncFile
{
public:
    [[nodiscard]] static AsyncFile open(crd::containers::StringView path);

    [[nodiscard]] bool is_open()  const noexcept;
    [[nodiscard]] u64  size()     const noexcept;

    // Submits a read job to the crd-jobs pool. Returns a Counter* that hits 0
    // when the read completes. Returns nullptr if offset+size exceeds file size.
    [[nodiscard]] crd::jobs::Counter*
    read_async(u64 offset, crd::containers::Span<u8> dst);
};
} // namespace crd::platform
```

`read_async` submits a `crd::jobs::run` job. The job opens a fresh OS handle (Windows `CreateFile`), seeks to `offset`, reads into `dst`, and closes the handle. The fiber that called `read_async` then calls `crd::jobs::wait(counter)` to suspend cooperatively until the read completes.

`crd-platform` gains a `PRIVATE` link-time dep on `crd-jobs`. This is acceptable: `crd-platform` was already tied to the job system via the fiber model for any cooperative I/O.

**Tests added** (`tests/platform/test_async_file.cpp`):
- Open nonexistent path → `!is_open()`, `size() == 0`.
- Open existing file → `is_open()`, `size() == known_byte_count`.
- `read_async` round-trip: write N bytes, read them back, verify byte-for-byte.
- Out-of-range read (requested `size > file size`) → returns `nullptr` counter.

Catch2 `EventListenerBase` pattern used for `jobs::init/shutdown` to avoid crashing during `catch_discover_tests` test-listing phase (static constructors fire during listing; `testRunStarting` does not).

### `ResourceManager::load_async<T>` + `load_async_impl`

Submits a heap-allocated `AsyncLoadCtx` to the job pool. Returns immediately with a handle whose `state()` is `Queued` (or `Ready`/`Failed` if already cached / in error).

**SBO constraint:** `crd::jobs::make_job<F>` requires `sizeof(F) ≤ 41`, `alignof(F) ≤ 8`, trivially copyable, trivially destructible. The job closure is:

```cpp
struct LoadJobFn {
    void* ctx_ptr;   // 8 bytes — points to heap-allocated AsyncLoadCtx
    void operator()() noexcept { ResourceManager::run_load_job(ctx_ptr); }
};
static_assert(sizeof(LoadJobFn) <= 41U);
```

`AsyncLoadCtx` carries everything `run_load_job` needs (manager pointer, block pointer, loader pointer, pack path, blob offset/size, resource id, type_fourcc).

**Manager in-flight ref:** `load_async_impl` calls `block->add_ref()` before submitting the job. `run_load_job` calls `block->release()` at the end so the manager's ref is accounted for correctly even if the caller drops their handle before the job finishes.

**Coalescing:** `load_async_impl` inserts the new block into `m_in_flight` under `m_mutex` before submitting the job. A second concurrent `load_async` (or `load_sync`) for the same id finds the block already there and returns a handle sharing the same block — no duplicate I/O.

### `ResourceHandleBase::wait_ready()`

Fiber-cooperative wait. Moved to `resource_handle.cpp` (non-inline) so it can include `jobs.hpp` and `loader.hpp` without pulling them into the public header.

```cpp
LoadState ResourceHandleBase::wait_ready()
{
    if (!m_block) { return LoadState::Unloaded; }

    LoadState s = m_block->state.load(std::memory_order_acquire);
    if (s != LoadState::Queued && s != LoadState::Loading)
    {
        // Terminal state. Still try to claim any counter left behind.
        void* raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
        if (raw != nullptr) { crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw)); }
        return m_block->state.load(std::memory_order_acquire);
    }

    // Try to claim the async counter for a fiber-cooperative wait.
    void* raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
    if (raw != nullptr)
    {
        crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw));
        return m_block->state.load(std::memory_order_acquire);
    }

    // Counter not yet stored (load_async_impl is between run() and store). Spin.
    while (s == LoadState::Queued || s == LoadState::Loading)
    {
        std::this_thread::yield();
        s = m_block->state.load(std::memory_order_acquire);
    }

    // Final claim attempt.
    raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
    if (raw != nullptr) { crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw)); }
    return m_block->state.load(std::memory_order_acquire);
}
```

`wait_ready()` is also safe on sync-loaded handles (no counter stored; the terminal-state fast path returns immediately without any `jobs::wait` call).

### `ResourceControlBlock::load_counter`

```cpp
std::atomic<void*> load_counter{nullptr};
```

Declared as `void*` (not `crd::jobs::Counter*`) so `resource_control_block.hpp` does not pull `jobs.hpp` into every translation unit that includes it. Cast to `Counter*` only in `resource_handle.cpp` and `resource_manager.cpp`.

### `resource_handle.cpp` (new source file)

Implements both `release_block()` (needs `loader.hpp` for `ILoader::unload`) and `wait_ready()` (needs `jobs.hpp`). Moving these out of the header was necessary because:

1. `release_block()` calls `m_block->loader->unload()`, requiring `ILoader` to be a complete type. `resource_control_block.hpp` only forward-declares `ILoader`.
2. `wait_ready()` calls `crd::jobs::wait()`, which must not appear in the public resource header (would couple `crd-resources` public API headers to `crd-jobs`).

### Mutex phasing in `load_sync_impl` and `load_async_impl`

The `m_mutex` is released **before** any I/O or `loader->load()` call. This is the non-recursive mutex pattern that enables:

1. Transitive dep resolution: a loader that calls `load_sync()` for a dep acquires `m_mutex` on the same thread without deadlock.
2. Long-running loads do not stall concurrent `load_async` submitters.

All reads/writes to `m_handles`, `m_in_flight`, `m_live`, `m_loaders`, and `m_mounts` happen inside the lock; the I/O and loader dispatch happen outside it.

### `run_load_job` visibility

`ResourceManager::run_load_job` was moved from `private` to `public` (with a "not part of user-facing API" comment). The job closure (`LoadJobFn`) lives in the anonymous namespace of `resource_manager.cpp`; anonymous-namespace code cannot access private members of a class in a different TU.

### `smoke_resources_async.exe`

End-to-end async smoke:

1. Assemble a PACK file (same two-pass approach as `smoke_resources.cpp`).
2. `jobs::init({.num_threads = 2})`.
3. Mount the pack, register a `BlobResourceLoader`.
4. `load_async<BlobResource>(id)` → `wait_ready()` → verify 5-byte payload.
5. Drop handle, `jobs::shutdown()`, exit 0.

Prints `smoke_resources_async: OK — BlobResource loaded async and verified`.

---

## Files changed

```
engine/platform/
  include/crd/platform/async_file.hpp      NEW — AsyncFile public API
  src/async_file.cpp                       NEW — ReadJob + IOCP-style job submit
  CMakeLists.txt                           MODIFIED — PRIVATE crd-jobs link

engine/resources/
  include/crd/resources/
    resource_control_block.hpp             MODIFIED — added load_counter (atomic<void*>)
    resource_handle.hpp                    MODIFIED — wait_ready() non-inline decl; release_block() non-inline decl
    resource_manager.hpp                   MODIFIED — load_async<T>, load_async_impl, in_flight_count, m_in_flight, m_mutex, run_load_job public
  src/
    resource_handle.cpp                    NEW — release_block() + wait_ready() implementations
    resource_manager.cpp                   MODIFIED — AsyncLoadCtx, LoadJobFn, load_async_impl, run_load_job, mutex phasing
  CMakeLists.txt                           MODIFIED — PRIVATE crd-jobs link

tests/platform/
  test_async_file.cpp                      NEW — 4 AsyncFile tests + EventListener
  CMakeLists.txt                           MODIFIED — crd-jobs link

tests/resources/
  test_resource_manager.cpp                MODIFIED — 5 v1d tests + ResourcesJobsListener
  CMakeLists.txt                           MODIFIED — crd-jobs link

runtime/
  examples/smoke_resources_async.cpp       NEW — async smoke
  CMakeLists.txt                           MODIFIED — smoke_resources_async target
```

---

## Key design decisions

### Counter stored as `void*` in ResourceControlBlock

`jobs.hpp` would transitively pull in the entire job system into any TU that includes `resource_control_block.hpp` (and therefore `resource_handle.hpp` and `resource_manager.hpp`). The void* field sidesteps that at the cost of a cast at two use sites. The alternative (forward-declaring `crd::jobs::Counter`) would require an `atomic<Counter*>` field, but forward declarations of types used in atomics are not guaranteed portable. `void*` atomic is universally supported.

### Catch2 EventListener for jobs init/shutdown

CMake's `catch_discover_tests` runs the test binary with `--list-tests` during configure time to enumerate test cases. Any `static` RAII object that calls `jobs::init()` fires during this listing phase and crashes (the job system calls `VirtualAlloc` for fiber stacks which may fail or hang in the CMake subprocess context). The `EventListenerBase` pattern fires `testRunStarting` only during actual test execution, not during listing.

### Counter leak fix — race between job completion and wait_ready()

After `load_async_impl` submits the job and stores the counter:

```cpp
crd::jobs::Counter* c = crd::jobs::run(make_job(fn));
block->load_counter.store(c, std::memory_order_release);
// job may have already completed here
LoadState s = block->state.load(std::memory_order_acquire);
if (s != LoadState::Queued && s != LoadState::Loading)
{
    void* raw_c = block->load_counter.exchange(nullptr, std::memory_order_acquire);
    if (raw_c != nullptr) { crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw_c)); }
}
```

If the job completes between `run()` and the `store`, `wait_ready()`'s terminal-state fast path fires — but without the fix it returned immediately without claiming the counter, permanently leaking it. The jobs subsystem's `CounterPool::shutdown()` assertion (`counters still acquired`) caught this during testing.

The fix: `wait_ready()`'s terminal-state fast path always attempts an exchange before returning:

```cpp
if (s != LoadState::Queued && s != LoadState::Loading)
{
    void* raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
    if (raw != nullptr) { crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw)); }
    return m_block->state.load(std::memory_order_acquire);
}
```

load_async_impl's own leak fix and wait_ready's terminal fix together cover all orderings.

### `permanent = true` before `state.store(Ready, release)`

Setting `permanent` before the release-store on `state` ensures any thread that observes `state == Ready` with an acquire load also observes `permanent == true`. `release_block()` checks `!m_block->permanent` after the refcount decrement; the ordering guarantees it sees the correct value.

---

## Issues encountered

1. **`release_block()` / `ILoader` incomplete type**: Moving `release_block()` to `.cpp` fixed this. The inline definition in `resource_handle.hpp` called `m_block->loader->unload()` but `ILoader` is only forward-declared in `resource_control_block.hpp`.

2. **`run_load_job` private-member access**: `LoadJobFn::operator()` is in the anonymous namespace and cannot access `ResourceManager::run_load_job` if declared `private`. Moving it to a `public` section (marked as internal-only) resolved the link error.

3. **ASan crash during catch_discover_tests**: Static RAII `jobs::init()` fires during CMake test-listing. Fixed with the Catch2 EventListener pattern. Applied to both `test_async_file.cpp` and `test_resource_manager.cpp`.

4. **`CounterPool::shutdown: counters still acquired` assertion**: The terminal-state fast path in `wait_ready()` was missing the counter-claim exchange. See "Counter leak fix" above.

---

## Test summary

| File | New tests | Tags |
|---|---|---|
| `tests/platform/test_async_file.cpp` | 4 | `[platform][async_file]` |
| `tests/resources/test_resource_manager.cpp` | 5 | `[resources]` |

v1d tests in `test_resource_manager.cpp`:
- `load_async`: round-trip async load + `wait_ready()` returns Ready.
- `load_async`: coalescing — two handles to same id share one block (`h1.block() == h2.block()`).
- `load_async`: unknown id returns Failed after `wait_ready()`.
- `load_async`: 4 concurrent loads for distinct ids all complete Ready.
- `wait_ready` on sync handle: returns immediately with Ready (no crash; no counter to claim).

---

## Proposed commit message

```
feat(resources): v1d — AsyncFile + load_async<T> + fiber-cooperative wait_ready()

Adds crd::platform::AsyncFile (job-pool-based async reads, IOCP-style Windows
backend). ResourceManager gains load_async<T> (SBO-job submission via AsyncLoadCtx /
LoadJobFn), m_in_flight coalescing HashMap, and a non-recursive m_mutex released
before all I/O. ResourceHandleBase::wait_ready() (fiber-cooperative via atomic
load_counter claim) moved to resource_handle.cpp. Counter leak fix covers the
job-completes-before-store race. Catch2 EventListener pattern used in platform and
resource tests to avoid static-init crash during catch_discover_tests listing.
smoke_resources_async validates the full async round-trip.

Nine new tests (4 platform, 5 resources). All 6 configs green: 429/429 win-debug.
```
