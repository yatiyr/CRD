# 2026-05-07 — Detour D-001-b: `GrowablePoolAllocator` + `ChunkAllocator` refactor (D-001 closed)

**Status at start:** D-001-a shipped (TLSF, production-grade, arbitrary alignment, `try_allocate`). v1c1 chunk allocator's `free` was still O(outstanding) — flagged in v1c1 session log as perf debt.

**Status at end:** Detour D-001 closed. `GrowablePoolAllocator` ships. `ChunkAllocator` refactored to wrap it. Chunk `allocate`/`free` are now both O(1) via intrusive free-list. v1c2 archetype storage tests pass unchanged. Phase 3.0 v1d resumes.

---

## Goal of this session

Two halves of one slice:

1. **`GrowablePoolAllocator`** — auto-growing pool of fixed-size aligned blocks. O(1) allocate (free-list pop) and O(1) deallocate (free-list push). When the free list is empty, allocate a new page from the parent containing `slots_per_page` contiguous slots and link them into the free list. The standard Bevy/Unity DOTS chunk-pool shape.

2. **`crd::scene::ChunkAllocator` refactor** — internal change only. Public API stays identical. Wrap `GrowablePoolAllocator(slot_size = 16 KB, slot_alignment = 64, slots_per_page = 64)`. v1c2 archetype storage tests (82 cases / 7012 assertions) must pass unchanged.

The v1c1 perf debt (`ChunkAllocator::free` walking `m_blocks` linearly to find the freed chunk's index) is closed by this — the pool's free-list push is O(1).

## What shipped

### New module files

```
engine/memory/include/crd/memory/allocators/growable_pool_allocator.hpp   ~95 LOC
engine/memory/src/allocators/growable_pool_allocator.cpp                  ~210 LOC
tests/memory/test_growable_pool_allocator.cpp                             ~280 LOC, 14 cases
```

### Modified

- `engine/scene/include/crd/scene/archetype_chunk.hpp` — `ChunkAllocator`'s private member changes from `Array<void*> m_blocks` + `IAllocator* m_alloc` to `GrowablePoolAllocator m_pool`. Public API unchanged. Move ctor/assignment defaulted (pool is movable).
- `engine/scene/src/archetype_chunk.cpp` — `ChunkAllocator::allocate` calls `m_pool.allocate(kChunkSize, kChunkAlignment)`. `free` calls `m_pool.deallocate(chunk.memory)`. `outstanding` returns `m_pool.slots_in_use()`. ~80 LOC removed (the `Array<void*>` book-keeping + linear-scan free path).

### Public API (final)

```cpp
namespace crd::memory {

class GrowablePoolAllocator : public IAllocator
{
public:
    GrowablePoolAllocator(usize slot_size, usize slot_alignment, usize slots_per_page,
                          IAllocator* parent = nullptr,
                          const char* name = "GrowablePool");

    void*  allocate(usize size, usize alignment) override;     // O(1); CRD_ASSERT size <= slot_size, alignment <= slot_alignment
    void   deallocate(void* p) noexcept override;              // O(1) free-list push
    bool   owns(const void* p) const noexcept override;        // O(pages) range check
    usize  allocation_size(const void* p) const noexcept override;  // returns slot_size when owned

    usize  slot_size() const noexcept;
    usize  slot_alignment() const noexcept;
    usize  slots_per_page() const noexcept;
    usize  page_count() const noexcept;
    usize  slots_in_use() const noexcept;
    usize  slots_free() const noexcept;
};

} // namespace crd::memory
```

## Design choices

### Single free-list across all pages

Every freed slot pushes onto a single `m_free_head`. Every allocation pops the head. No per-page bookkeeping for in-use counts. **Trade-off:** can't implement `shrink_to_fit` (release empty pages to parent) without per-page tracking. Decision: skip `shrink_to_fit`. Pages are kept for the lifetime of the allocator. The chunk-allocator workload doesn't benefit from shrinking — under steady-state churn, page utilisation oscillates. If a real consumer needs `shrink_to_fit`, adding per-page counters is a localised change.

### Pages stored in a manually-grown `void**` array (no `crd::containers::Array`)

`crd-memory` cannot depend on `crd-containers` — that would invert the dependency direction (`crd-containers` → `crd-memory`). I implement a doubling-capacity `void** m_pages` ourselves, growing through `m_parent->reallocate` (initial capacity 4, doubling on overflow). Matches the existing `PoolAllocator` style.

### Pages allocated at `slot_alignment` from the parent

`m_parent->allocate(m_page_bytes, m_slot_alignment)` returns a page-aligned block. Slots inside are placed at stride `align_up(slot_size, slot_alignment)` so every slot is naturally aligned. For the chunk use case (16 KB / 64-aligned), stride = 16 KB exactly, so 64 slots fit in 1 MB with no padding waste.

### Free-list pointers overlay the slot's first 8 bytes when free

When a slot is free, its first `sizeof(FreeNode*) = 8` bytes hold a `FreeNode*` to the next free slot. This requires `slot_size >= sizeof(FreeNode)` and `slot_alignment >= alignof(FreeNode)` — both pinned by `CRD_ASSERT` in the constructor. For chunk-style use (`slot_size = 16 KB`, `slot_alignment = 64`), trivially satisfied.

### First-allocation grows the first page (not the constructor)

Constructor does NOT pre-allocate any pages. The first call to `allocate()` triggers the first `grow()`. This means a default-constructed `GrowablePoolAllocator` consumes only its small bookkeeping (~64 bytes) — matching the lazy initialisation pattern used by other engine subsystems.

### Slots inserted into the free list back-to-front

`grow()` walks the new page's slots from highest-address to lowest, pushing each onto the free-list head. This means the next allocation gets the LOWEST-address slot in the new page. Slightly more cache-friendly on the first sweep through a fresh page (sequential allocations follow ascending memory).

### Move semantics

`GrowablePoolAllocator` is move-constructible / move-assignable. Source becomes empty (no pages, no in-use). Destination assumes ownership of pages array, free list, and counters. Move-assign frees existing pages first.

### `owns()` is O(pages), not O(slots)

`owns(p)` walks `m_pages_size` page pointers and checks if `p` is within `[page, page + m_page_bytes)`. Pages typically number in the single digits even for large workloads (1 MB pages × 64 chunks per page × N pages → N pages for N×64 chunks). Linear scan is fine.

## Refactor: `ChunkAllocator` → `GrowablePoolAllocator` wrapper

Before:
```cpp
class ChunkAllocator
{
    crd::memory::IAllocator*      m_alloc;
    crd::containers::Array<void*> m_blocks;  // O(N) free
};
```

After:
```cpp
class ChunkAllocator
{
    crd::memory::GrowablePoolAllocator m_pool;  // O(1) alloc + free
};
```

`allocate` / `free` / `outstanding` are one-liners. The 1 MB page size (= 64 × 16 KB chunks) was chosen empirically:
- Big enough to amortise page-allocation cost (a real archetype with thousands of entities → multiple chunks → still typically 1 page).
- Small enough to fit modern L2 caches (1 MB is the typical Intel Core L2 size on 12th-gen+).
- Powers-of-two slot count keep the math simple and the pool head-overhead-free.

## Tests added (`tests/memory/test_growable_pool_allocator.cpp`, 14 cases / 3174 assertions)

- Constructor records configuration; no pages until first `allocate`.
- First `allocate` creates one page; `slots_in_use` and `slots_free` correct.
- 8 contiguous allocations from a single page (no growth); pairwise-distinct, all aligned.
- Page growth: 10 allocations into 4-slot pages → 3 pages allocated; `slots_in_use == 10`, `slots_free == 2`.
- Free-list LIFO reuse: free middle two of four → next two allocations come from those slots, no growth.
- `deallocate(nullptr)` is no-op.
- `owns()` true for slots, false for nullptr / stack / external pointer.
- `owns()` correctly identifies slots across multiple pages (8 entities × 2 slots/page → ≥ 4 pages, all owned).
- `allocation_size` returns `slot_size` for owned, 0 for non-owned.
- Chunk-style alignment (16 KB / 64-aligned / 64 per page = 1 MB pages); 200 allocations exceed one page; all 64-aligned.
- ASan leak check: dtor with no manual deallocates → no leaks.
- Move ctor: source emptied, dst owns pages and pointers; verified by deallocating through dst.
- 2000-iteration random alloc/free stress (verified ASan-clean).
- `IAllocator` interface compatibility through a `IAllocator&` reference.

## Six-configuration sweep

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | ✅ | 598/598 | 17/17 |
| win-relwithdebinfo | ✅ | 598/598 | 17/17 |
| win-release        | ✅ | 595/595 | 17/17 |
| win-asan           | ✅ | 598/598 (DLL PATH fix) | 17/17 |
| win-clang-cl       | ✅ | 598/598 | 17/17 |
| win-tidy           | ✅ | — | — |

Test count delta: +14 (was 584/584 / 581/581 in win-debug / win-release pre-D-001-b).

`win-tidy` initially flagged one `bugprone-bitwise-pointer-cast` warning on the `memcpy` of the pages array (resizing path). The warning is a false positive — copying a `void*[]` is the intended operation. NOLINT-suppressed with a justification comment.

`clang-format -i` run on all five new + modified source files. Em-dash in one TEST_CASE name caused a CTest filter encoding break (same issue as v1c2's tests); replaced with ASCII hyphen.

## v1c1 perf debt closed

The v1c1 session log flagged `ChunkAllocator::free` as O(outstanding) — a linear scan of `m_blocks` to find the freed chunk's index, swap_remove, deallocate. Under steady-state archetype churn (entity moves between archetypes drop chunks at random positions), this could hot-spot at hundreds of archetypes × tens of chunks each.

After the GrowablePool refactor: `ChunkAllocator::free` is `m_pool.deallocate(p)`, which is one free-list push (O(1)). Page-allocation costs are amortised across 64 chunks. The debt is closed.

## D-001 detour closing summary

The detour pauses Phase 3.0 v1d to ship memory infrastructure end-to-end. Both halves landed same-day:

- **D-001-a** — `TlsfAllocator`. Canonical Conte/Masmano TLSF. After the alignment-bug fix, supports arbitrary power-of-two alignment up to 256 bytes (tested under ASan stress). `try_allocate` non-throwing path. 21 unit tests / 2208 assertions.
- **D-001-b** — `GrowablePoolAllocator` + `ChunkAllocator` refactor. O(1) chunk allocate/free. 14 unit tests / 3174 assertions. Closes v1c1 perf debt.

All exit criteria from `docs/detours/D-001-memory-infrastructure.md` met:
- Both allocators pass six-config DoD ✓
- v1c2 archetype-storage tests stay green ✓ (82/82 / 7012 assertions, unchanged)
- `MallocAllocator` remains the engine default ✓ (TLSF and GrowablePool are opt-in)
- Both session logs written ✓

The detour file is marked closed. `docs/debt.md` carries an entry summarising the close. `docs/detours/README.md` moves D-001 from active to closed.

## What's next — Phase 3.0 v1d

Main roadmap resumes. **`SparseSetStorage`** (~250 LOC + tests, ADR-0050) — the escape-hatch L2 backend for components flagged `StorageHint::SparseSet`:

- Per-component pool: `sparse[entity_index] -> dense_index`, `dense[dense_index] -> T`, `dense[dense_index] -> entity_id`.
- O(1) insert / remove (swap-with-last) / lookup / iteration over the dense array.
- Implements `IStorageBackend` — same interface as `ArchetypeChunkStorage`. The query layer (v1g) and index dispatcher (v1i) walk both backends uniformly.
- v1e ships the mixed-backend chunk visitor that completes the unified iteration story.

After v1d–v1e: relations (v1f), query DSL (v1g), schedule (v1h), index framework (v1i), Transform propagation (v1j), scene serialization (v1k–v1l), sandbox integration (v1m), reserved-slot freeze (v1n).

## Files touched

```
A  engine/memory/include/crd/memory/allocators/growable_pool_allocator.hpp
A  engine/memory/src/allocators/growable_pool_allocator.cpp
A  tests/memory/test_growable_pool_allocator.cpp
M  engine/scene/include/crd/scene/archetype_chunk.hpp     (ChunkAllocator wraps GrowablePool)
M  engine/scene/src/archetype_chunk.cpp                   (ChunkAllocator impl simplified)
M  CONTEXT.md                                              (D-001 closed; v1d active)
M  docs/debt.md                                            (D-001 close summary)
M  docs/detours/README.md                                  (D-001 → closed)
M  docs/detours/D-001-memory-infrastructure.md             (status: closed 2026-05-07)
A  docs/sessions/2026-05-07-detour-D-001b-growable-pool.md (this file)
```

## Proposed commit message

```
feat(memory): D-001-b — GrowablePoolAllocator + ChunkAllocator refactor (D-001 closed)

Land the GrowablePoolAllocator: auto-growing pages of fixed-size aligned
blocks, O(1) allocate/deallocate via intrusive free-list. Single-list-
across-all-pages design; pages kept allocated for the allocator's life
(no auto-shrink); slots laid out at slot_alignment stride within
slot_alignment-aligned pages.

Refactor crd::scene::ChunkAllocator to wrap GrowablePoolAllocator with
slot_size = 16 KB, slot_alignment = 64, slots_per_page = 64 (= 1 MB
pages). Public API unchanged; v1c2 archetype storage tests stay green.

Closes the v1c1 perf debt: ChunkAllocator::free is now O(1) via the
pool's free-list push, instead of O(outstanding) via the old
Array<void*> linear-scan.

D-001 detour closed. Both halves shipped same day. MallocAllocator
remains the engine default; TLSF and GrowablePool are opt-in.

14 unit tests / 3174 assertions added. Six-config green: win-debug
598/598, win-relwithdebinfo 598/598, win-release 595/595, win-asan
598/598, win-clang-cl 598/598, win-tidy clean. 17/17 headless smokes
per non-tidy config.
```
