#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/shared_component_pool.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/storage_event_sink.hpp>

namespace crd::scene
{
// SparseSetStorage — the L2 escape-hatch backend per ADR-0050 §3.
//
// One pool per registered component whose storage hint is SparseSet. Each
// pool is the canonical sparse-set triple:
//
//   sparse[entity.index()]  -> dense_index    (sentinel = kInvalid)
//   dense (raw bytes)       -> T              (count * info.size, info.alignment)
//   entities[dense_index]   -> EntityId       (parallel array, back-resolution)
//
// Operations are O(1) amortised (Array growth amortised; raw dense buffer
// grows exponentially × 2). Removal is swap-with-last, so iteration order
// is "insertion order modulo prior swap-removes" — this is the documented
// SparseSet contract; tests pin it.
//
// Why both backends per ADR-0050:
//   - Archetype: cache-coherent multi-component iteration at million scale.
//   - SparseSet: O(1) churn, no archetype-explosion on toggle-heavy tags;
//     <5%-prevalence components don't pollute the archetype graph.
//
// Lifecycle event dispatch:
//   Every mutation calls into m_sink (default NullStorageEventSink). The same
//   sink that ArchetypeChunkStorage uses — World installs it on both
//   backends. The sink fires on_entity_destroyed ONCE per destroy (driven
//   from World, not backend), so dedup is the World's job per ADR-0053 §X.
//
// Layer-2 storage interface (ADR-0050 §1):
//   for_each_chunk yields ONE ChunkView per pool whose component is required.
//   v1d implements the single-bit and zero-bit `required` cases:
//     - empty required: yield every pool
//     - required = {c}: yield exactly the c-pool (if present and non-empty)
//     - required has multiple bits: yield nothing (deferred to v1e mixed
//       backend visitor — a single SparseSet pool can't satisfy multi-bit
//       intersection alone)
//
// Per-pool version counter:
//   Each pool carries one u64 version. Bumped on every insert / update /
//   remove. ADR-0053's ChangeDetectIndex (v1i) reads it to drive
//   .changed<T>() at pool granularity (the SparseSet analogue of the
//   per-chunk-per-component version counter that archetype storage carries).
//
// Memory:
//   Pools allocated via m_alloc with placement-new, indexed in m_pools by
//   ComponentId.raw. Pool count is bounded by kMaxComponents (256), pools
//   live the World's lifetime — direct allocation is the right shape (a
//   GrowablePool would be over-engineering for ≤256 long-lived structs).
//
// Allocator chain (closes 2026-05-07's audit promise end-to-end):
//   Every byte the storage holds — sparse table, dense buffer, entities
//   array, the Pool struct itself — flows through m_alloc. World on TLSF
//   means SparseSet on TLSF.
class SparseSetStorage : public IStorageBackend
{
public:
    SparseSetStorage(crd::memory::IAllocator* alloc, const ComponentRegistry& registry);
    ~SparseSetStorage() override;

    SparseSetStorage(const SparseSetStorage&) = delete;
    SparseSetStorage& operator=(const SparseSetStorage&) = delete;
    SparseSetStorage(SparseSetStorage&&) = delete; // referenced by World
    SparseSetStorage& operator=(SparseSetStorage&&) = delete;

    // ---- IStorageBackend -----------------------------------------------

    void insert(EntityId e, ComponentId c, void* data) override;
    void remove(EntityId e, ComponentId c) override;
    [[nodiscard]] bool has(EntityId e, ComponentId c) const override;
    [[nodiscard]] void* get_mut(EntityId e, ComponentId c) override;
    void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data) override;
    void on_entity_destroyed(EntityId e) override;

    // ---- v1m4b2 — Inherit / shared-pool entry point --------------------
    //
    // Insert `e` with a SHARED component for `c`. The bytes at `data`
    // are uploaded to the pool's SharedComponentPool exactly once;
    // subsequent calls with byte-identical `data` (identified by a content
    // hash; v1m4b3 adds the hash table) reuse the existing pool entry and
    // bump its refcount. The entity's dense slot stores the pool_idx and
    // is flagged as shared — read paths follow the pool indirection;
    // write paths (v1m4b3) copy-on-first-write to break the share.
    //
    // Currently uses per-call acquisition (no cross-call dedup yet —
    // v1m4b3 adds content-hash dedup so spawning N copies of the same
    // öbek shares one pool entry across all N).
    //
    // Caller MUST ensure `c`'s component is registered with InheritPolicy::Inherit.
    // The component's storage hint is forced to SparseSet at registration
    // time so the pool lives here. Calling `insert_shared` for a
    // non-Inherit component is a programming error (CRD_ASSERT in debug).
    void insert_shared(EntityId e, ComponentId c, const void* data);

    // ---- Read accessor (mirror of ArchetypeChunkStorage::get_const) ----

    [[nodiscard]] const void* get_const(EntityId e, ComponentId c) const;

    // ---- Diagnostics ---------------------------------------------------

    // Number of entities currently in pool c (0 if no pool exists for c).
    [[nodiscard]] crd::u32 entity_count(ComponentId c) const noexcept;

    // Number of distinct pools currently allocated.
    [[nodiscard]] crd::u32 pool_count() const noexcept;

    // Per-pool version counter — ChangeDetect (v1i) reads this. 0 if no pool.
    [[nodiscard]] crd::u64 pool_version(ComponentId c) const noexcept;

    // v1m4b3 diagnostics — number of LIVE entries in the shared-pool for `c`.
    // 0 if no pool, no shared_pool yet, or all entries released. Used by
    // tests to verify dedup + refcount eviction.
    [[nodiscard]] crd::u32 shared_pool_live_count(ComponentId c) const noexcept;

    // ---- Event sink wiring ---------------------------------------------

    void set_event_sink(IStorageEventSink* sink) noexcept;
    [[nodiscard]] IStorageEventSink* event_sink() noexcept { return m_sink; }

private:
    static constexpr crd::u32 kInvalidDenseIndex = 0xFFFFFFFFU;

    // One pool per SparseSet-hinted component. Owned by storage; destructor
    // releases sparse / entities / dense in turn through m_alloc.
    struct Pool
    {
        const ComponentInfo* info = nullptr;
        crd::memory::IAllocator* alloc = nullptr;

        crd::containers::Array<crd::u32> sparse;     // entity.index() -> dense_index | kInvalid
        crd::containers::Array<EntityId> entities;   // dense_index    -> EntityId
        void*       dense = nullptr;                 // count * info->size bytes, aligned to info->alignment
        crd::u32    capacity = 0;                    // slots
        crd::u32    count = 0;                       // live slots
        crd::u64    version = 0;                     // bumped on insert / update / remove

        // v1m4b2 — Inherit CoW backend.
        // `shared_pool` is lazy-allocated on first call to `insert_shared`.
        // `shared_pool_idx[d]` is `kInvalidPoolIdx` for owned slots (read
        // bytes from `dense`); a valid idx means SHARED — the bytes live
        // in `shared_pool` and the dense slot is unused.
        // Parallel to `entities` array; resized in lockstep with grow_dense.
        SharedComponentPool*             shared_pool = nullptr; // lazy
        crd::containers::Array<crd::u32> shared_pool_idx;       // dense_idx -> pool_idx | kInvalidPoolIdx

        Pool(const ComponentInfo& i, crd::memory::IAllocator* a) noexcept;
        ~Pool();

        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&) = delete;
        Pool& operator=(Pool&&) = delete;

        // Grow dense to hold at least `min_capacity` slots. Exponential ×2.
        void grow_dense(crd::u32 min_capacity);

        // Stamp `entity_index → dense_index` into sparse, lazy-resizing it.
        void set_sparse(crd::u32 entity_index, crd::u32 dense_index);
        [[nodiscard]] crd::u32 get_sparse(crd::u32 entity_index) const noexcept;

        // Address of the dense slot at `dense_index` (in bytes).
        [[nodiscard]] crd::u8* slot_bytes(crd::u32 dense_index) noexcept;
        [[nodiscard]] const crd::u8* slot_bytes(crd::u32 dense_index) const noexcept;
    };

    // Lazy pool acquire. Returns the existing pool if any; allocates a new
    // one otherwise (ComponentId.raw is the index into m_pools).
    Pool& ensure_pool(ComponentId c);
    [[nodiscard]] Pool* find_pool(ComponentId c) noexcept;
    [[nodiscard]] const Pool* find_pool(ComponentId c) const noexcept;

    // Tear down every live pool (called from destructor).
    void destroy_all_pools() noexcept;

    crd::memory::IAllocator*      m_alloc;
    const ComponentRegistry*      m_registry;
    crd::containers::Array<Pool*> m_pools; // sized to kMaxComponents, all nullptr until lazy
    IStorageEventSink*            m_sink;
};

} // namespace crd::scene
