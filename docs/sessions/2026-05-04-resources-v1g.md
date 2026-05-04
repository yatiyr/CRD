# 2026-05-04 — Phase 2.6 v1g: load_streamed + 2Q LRU eviction + memory budget + pinning

**Status:** SHIPPED — Phase 2.6 COMPLETE

## What shipped

`crd-resources` v1g closes the streaming story for Phase 2.6: a full memory-budget eviction system
(2Q, Johnson & Shasha 1994), pin/unpin pinning, and fiber-cooperative streamed loads.

---

## New public API

### Memory budget + eviction

```cpp
// Soft ceiling in bytes. Eviction runs after each load that exceeds the budget.
void set_memory_budget(crd::u64 bytes);

// Current bytes tracked against the budget. Thread-safe.
[[nodiscard]] crd::u64 current_memory_use() const noexcept;
```

Default budget is `~0ULL` (unlimited). `current_memory_use` tracks `blob_size` from the manifest
as a working-set proxy — avoids loader-private allocator intrusion.

### Pinning

```cpp
void pin  (ResourceId id);  // refcounted; pinned blocks are never evicted
void unpin(ResourceId id);  // must be paired 1:1 with each pin()
```

Safe to call before the first load. Pin state is recorded in `m_pin_counts` and wired into the
block during Phase 4 finalize in `load_sync_impl` / `run_load_job` / `run_stream_load_job`.

### Streamed load

```cpp
template <typename T>
[[nodiscard]] ResourceHandle<T> load_streamed(ResourceId id);
```

Submits a `StreamLoadJobFn` job. Inside the fiber: opens `crd::platform::AsyncFile`, calls
`read_async`, waits the counter, then dispatches the loader with `stream_file`, `stream_offset`,
and `stream_size` set in `LoadContext::bytes`. Same coalescing and re-issue semantics as
`load_async`.

---

## Implementation

### Files changed

| File | Change |
|---|---|
| `engine/resources/include/crd/resources/resource_control_block.hpp` | Added `EvictQueue` enum, `bool pinned`, `EvictQueue evict_queue`, `crd::u64 payload_size` |
| `engine/resources/include/crd/resources/loader.hpp` | `AsyncFile` forward-decl; added `stream_file`, `stream_offset`, `stream_size` to `LoadContext` |
| `engine/resources/include/crd/resources/resource_manager.hpp` | New public API + private 2Q state + `load_streamed_impl` / `run_stream_load_job` decls |
| `engine/resources/src/resource_manager.cpp` | 2Q helpers, `load_sync_impl` re-issue path, `load_streamed_impl`, `run_stream_load_job` |
| `tests/resources/test_eviction.cpp` | 5 new TEST_CASEs (new file) |
| `runtime/examples/smoke_resources_stream.cpp` | New smoke (new file) |
| `runtime/CMakeLists.txt` | Added `smoke_resources_stream` target |

### 2Q eviction policy

```
A1in  — FIFO probationary queue. New loads land here (back).
Am    — LRU main queue. Promoted from A1in on A1out ghost hit.
A1out — Ghost FIFO, bounded at kMaxA1out=256. Remembers recently evicted ids (no payload).

Insert:
  if id in A1out → remove from A1out; push_back to Am (ghost hit, promoted)
  else           → push_back to A1in (first encounter)

Touch (cache hit in Am):
  Linear scan for id; erase; push_back (LRU bump).

Evict:
  Prefer A1in front (probationary victims first).
  If nothing evictable in A1in, try Am front.
  Skip: pinned, use_count > 0, state != Ready.
  On evict: unload payload, m_memory_used -= payload_size, state = Unloaded.
  Push id to A1out back; if A1out.size() >= kMaxA1out, pop A1out front (FIFO).
```

Key correctness decision: A1out uses order-preserving `erase(i)` (not `swap_remove(0)`)
when trimming to capacity. `swap_remove` would corrupt FIFO order by moving the tail to the
front position.

### Re-issue path

An evicted block stays in `m_handles` with `state = Unloaded`, `payload = nullptr`.

In `load_sync_impl` Phase 1:
```cpp
if (s == LoadState::Unloaded) {
    re_issue = true;
    // fall through to load dispatch, reusing existing block
}
```

In Phase 4 finalize (and `run_load_job` / `run_stream_load_job`):
```cpp
if (re_issue) {
    block->generation.fetch_add(1U, memory_order_acq_rel);
    insert_into_2q(id, block);  // re-enters A1in (or Am if still in A1out ghost)
}
```

`load_async_impl` also supports re-issue: `AsyncLoadCtx` gains a `bool re_issue` field.
`run_load_job` reads it and bumps generation + re-inserts into 2Q in the same finalize path.

### StreamLoadJobFn

Fits within the 41-byte SBO: only stores an 8-byte `void*` pointer to heap-allocated
`StreamLoadCtx`. `StreamLoadCtx` holds: manager pointer, block pointer, loader pointer,
pack path string, blob_offset, blob_size, id, type_fourcc, re_issue flag.

```cpp
// Inside run_stream_load_job:
crd::platform::AsyncFile af = crd::platform::AsyncFile::open(ctx->pack_path.c_str());
crd::containers::Array<crd::u8> buf(...);
buf.resize(ctx->blob_size);
crd::jobs::Counter* c = af.read_async(ctx->blob_offset, as_span(buf));
crd::jobs::wait(c);
// Build LoadContext with stream_file = &af, stream_offset, stream_size.
void* payload = ctx->loader->load(lctx);
// Finalize same as run_load_job.
```

---

## Tests

**`tests/resources/test_eviction.cpp`** — 5 TEST_CASEs:

1. **Budget enforced**: 4 resources, budget = 2 × blob_sz. Load all 4 sequentially (drop each handle
   before the next). After: `current_memory_use() <= budget`.

2. **Pinned survives eviction pressure**: pin resource 0 before load. Budget = 2 blobs. Load all 4
   (dropping each). Re-load resource 0 — must be `Ready` with correct value.

3. **Re-issue increments generation**: load → drop → `set_memory_budget(0)` → reload.
   Check `generation() == 1`.

4. **2Q ghost hit promotes to Am**: A→B→C loads (A evicted to A1out). Reload A → A1out hit → Am.
   Load D → evicts C (A1in), not A (Am). Re-load A — still `Ready`, `generation == 1`.

5. **load_streamed delivers payload**: `load_streamed<EVBlobResource>(id)` → `wait_ready()` →
   `res->value == 0xDEADBEEF`. `generation() == 0`.

Note: test file does NOT call `crd::jobs::init()` — relies on `ResourcesJobsListener` in
`test_resource_manager.cpp` (same test binary).

---

## Smoke

**`runtime/examples/smoke_resources_stream.cpp`**:
- `crd::jobs::init()`
- Write PACK with value = `0xCAFEBABE`
- `rm.load_streamed<StreamBlobResource>(id)` → `wait_ready()` → verify value
- `crd::jobs::shutdown()`
- Exit 0

---

## Bugs fixed during implementation

| Bug | Fix |
|---|---|
| PCH race: adding `#include <crd/platform/async_file.hpp>` to resource_manager.cpp caused parallel ninja targets to race on PCH rebuild (C1083 "cannot open 'atomic'") | Clean build dir; reconfigure; build with `-j1` |
| Wrong arch (VsDevCmd.bat → x86): platform.hpp fired "#error: Unsupported architecture!" | Switch to `vcvars64.bat`; confirmed by `ml64.exe` in configure output |
| Unicode em dash `—` in test names corrupted by CTest filter | Replace with ASCII `-` |
| Double `crd::jobs::init()` in streaming test: `ResourcesJobsListener` already initialised jobs | Remove `jobs::init()` / `jobs::shutdown()` from `test_eviction.cpp` |
| A1out `swap_remove(0)` corrupts FIFO order when trimming ghost queue to capacity | Use order-preserving `erase(0)` |

---

## Six-configuration quality pass

| Config | Result |
|---|---|
| win-debug | 444/444 ✅ |
| win-relwithdebinfo | 444/444 ✅ |
| win-release | 441/441 ✅ |
| win-asan | 444/444 ✅ |
| win-clang-cl | 444/444 ✅ |
| win-tidy | 444/444 ✅ |

(win-release 3 fewer: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`)

---

## Proposed commit message

```
feat(resources): v1g - load_streamed, 2Q LRU eviction, memory budget, pinning

Implements Phase 2.6 v1g, closing the streaming story for crd-resources.

- set_memory_budget(bytes) / current_memory_use(): soft memory ceiling with
  2Q eviction (Johnson & Shasha 1994). A1in FIFO probationary, Am LRU main,
  A1out ghost FIFO (bounded at 256). Eviction order: A1in front first.

- pin(id) / unpin(id): ref-counted pinning. Pinned blocks survive all
  eviction pressure. Pin-before-load honoured via m_pin_counts check.

- load_streamed<T>(id): submits StreamLoadJobFn job; opens AsyncFile inside
  the fiber, calls read_async, waits counter, dispatches loader with
  stream_file/stream_offset/stream_size in LoadContext.

- Re-issue path: evicted blocks stay in m_handles as Unloaded. load_sync /
  load_async / load_streamed reuse the existing block and bump generation.

- 5 new unit tests (test_eviction.cpp), smoke_resources_stream.exe.

All 6 configurations green: 444/444 win-debug, 441/441 win-release.
Phase 2.6 COMPLETE.
```
