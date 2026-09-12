#include <crd/core/assert.hpp>
#include <crd/scene/sparse_set_storage.hpp>

#include <cstring>
#include <new>

namespace crd::scene
{

// ---- Pool ----------------------------------------------------------------

SparseSetStorage::Pool::Pool(const ComponentInfo& i, crd::memory::IAllocator* a) noexcept
    : info(&i), alloc(a), sparse(a), entities(a)
{
}

SparseSetStorage::Pool::~Pool()
{
    // v1m4b2: release any live shared-pool entries first. These don't
    // own destructible state in their own dense slot; the pool itself
    // owns the bytes.
    if (shared_pool != nullptr)
    {
        for (crd::u32 d = 0; d < count; ++d)
        {
            const crd::u32 pool_idx = (d < shared_pool_idx.size())
                                          ? shared_pool_idx[d]
                                          : SharedComponentPool::kInvalidIdx;
            if (pool_idx != SharedComponentPool::kInvalidIdx)
            {
                shared_pool->release(pool_idx);
            }
        }
        shared_pool->~SharedComponentPool();
        alloc->deallocate(shared_pool);
        shared_pool = nullptr;
    }

    // Destruct any live OWNED elements (shared slots have no inline state).
    if (info != nullptr && info->destruct != nullptr && dense != nullptr)
    {
        for (crd::u32 i = 0; i < count; ++i)
        {
            const crd::u32 pool_idx = (i < shared_pool_idx.size())
                                          ? shared_pool_idx[i]
                                          : SharedComponentPool::kInvalidIdx;
            if (pool_idx == SharedComponentPool::kInvalidIdx)
            {
                info->destruct(slot_bytes(i));
            }
        }
    }
    if (dense != nullptr && alloc != nullptr)
    {
        alloc->deallocate(dense);
        dense = nullptr;
    }
    capacity = 0;
    count = 0;
}

void SparseSetStorage::Pool::grow_dense(crd::u32 min_capacity)
{
    if (capacity >= min_capacity)
    {
        return;
    }
    crd::u32 new_cap = (capacity == 0) ? 8U : capacity * 2U;
    if (new_cap < min_capacity)
    {
        new_cap = min_capacity;
    }

    const crd::usize byte_size = static_cast<crd::usize>(new_cap) * info->size;
    void* new_dense = alloc->allocate(byte_size, info->alignment);
    CRD_ASSERT(new_dense != nullptr);

    if (count > 0 && dense != nullptr)
    {
        // Move-construct existing live slots into the new buffer; destruct
        // sources in the old buffer. Trivially-movable types fall back to
        // memcpy + no-op destruct.
        if (info->move_construct != nullptr)
        {
            for (crd::u32 i = 0; i < count; ++i)
            {
                crd::u8* src = static_cast<crd::u8*>(dense) + static_cast<crd::usize>(i) * info->size;
                crd::u8* dst = static_cast<crd::u8*>(new_dense) + static_cast<crd::usize>(i) * info->size;
                info->move_construct(dst, src);
                if (info->destruct != nullptr)
                {
                    info->destruct(src);
                }
            }
        }
        else
        {
            std::memcpy(new_dense, dense, static_cast<crd::usize>(count) * info->size);
        }
    }

    if (dense != nullptr)
    {
        alloc->deallocate(dense);
    }
    dense = new_dense;
    capacity = new_cap;
}

void SparseSetStorage::Pool::set_sparse(crd::u32 entity_index, crd::u32 dense_index)
{
    if (entity_index >= sparse.size())
    {
        // Lazy-grow the sparse table. A scene that spawns to entity-index N
        // before any SparseSet insert pays the resize once; future inserts
        // at lower indices are O(1).
        sparse.resize(static_cast<crd::usize>(entity_index) + 1U, kInvalidDenseIndex);
    }
    sparse[entity_index] = dense_index;
}

crd::u32 SparseSetStorage::Pool::get_sparse(crd::u32 entity_index) const noexcept
{
    if (entity_index >= sparse.size())
    {
        return kInvalidDenseIndex;
    }
    return sparse[entity_index];
}

crd::u8* SparseSetStorage::Pool::slot_bytes(crd::u32 dense_index) noexcept
{
    return static_cast<crd::u8*>(dense) + static_cast<crd::usize>(dense_index) * info->size;
}

const crd::u8* SparseSetStorage::Pool::slot_bytes(crd::u32 dense_index) const noexcept
{
    return static_cast<const crd::u8*>(dense) + static_cast<crd::usize>(dense_index) * info->size;
}

// ---- SparseSetStorage ----------------------------------------------------

SparseSetStorage::SparseSetStorage(crd::memory::IAllocator* alloc, const ComponentRegistry& registry)
    : m_alloc(alloc), m_registry(&registry), m_pools(alloc), m_sink(NullStorageEventSink::instance())
{
    CRD_ASSERT(m_alloc != nullptr);
    // Reserve one pool slot per possible component id. Pool* defaults to
    // nullptr; pools are lazily created on first insert.
    m_pools.resize(kMaxComponents, nullptr);
}

SparseSetStorage::~SparseSetStorage()
{
    destroy_all_pools();
    // m_pools' member dtor walks pointers — but they've all been freed by
    // destroy_all_pools and the slots are nullptr now. Safe.
}

void SparseSetStorage::destroy_all_pools() noexcept
{
    for (Pool*& slot : m_pools)
    {
        if (slot != nullptr)
        {
            slot->~Pool();
            m_alloc->deallocate(slot);
            slot = nullptr;
        }
    }
}

void SparseSetStorage::set_event_sink(IStorageEventSink* sink) noexcept
{
    m_sink = (sink != nullptr) ? sink : NullStorageEventSink::instance();
}

SparseSetStorage::Pool* SparseSetStorage::find_pool(ComponentId c) noexcept
{
    if (c.is_null() || c.raw >= m_pools.size())
    {
        return nullptr;
    }
    return m_pools[c.raw];
}

const SparseSetStorage::Pool* SparseSetStorage::find_pool(ComponentId c) const noexcept
{
    if (c.is_null() || c.raw >= m_pools.size())
    {
        return nullptr;
    }
    return m_pools[c.raw];
}

SparseSetStorage::Pool& SparseSetStorage::ensure_pool(ComponentId c)
{
    CRD_ASSERT(!c.is_null() && c.raw < m_pools.size());
    if (m_pools[c.raw] != nullptr)
    {
        return *m_pools[c.raw];
    }

    const ComponentInfo* info = m_registry->info(c);
    CRD_ASSERT(info != nullptr);
    CRD_ASSERT(info->size > 0); // SparseSet pools cannot store zero-sized tags;
                                 // marker components flow through Archetype.
    CRD_ASSERT(info->storage_hint == StorageHint::SparseSet);

    void* mem = m_alloc->allocate(sizeof(Pool), alignof(Pool));
    CRD_ASSERT(mem != nullptr);
    Pool* pool = ::new (mem) Pool(*info, m_alloc);
    m_pools[c.raw] = pool;
    return *pool;
}

// ---- Mutation API --------------------------------------------------------

void SparseSetStorage::insert(EntityId e, ComponentId c, void* data)
{
    CRD_ASSERT(data != nullptr);
    Pool& pool = ensure_pool(c);

    const crd::u32 ei = e.index();
    const crd::u32 existing = pool.get_sparse(ei);

    if (existing != kInvalidDenseIndex)
    {
        // UPSERT in place: destruct old, move-construct new, fire on_update.
        // Per IStorageEventSink.hpp the (old_data, new_data) aliasing is
        // permitted — we pass the same slot pointer twice (the actual old
        // bytes are dead between destruct and move_construct).
        crd::u8* slot = pool.slot_bytes(existing);
        if (pool.info->destruct != nullptr)
        {
            pool.info->destruct(slot);
        }
        if (pool.info->move_construct != nullptr)
        {
            pool.info->move_construct(slot, data);
        }
        else
        {
            std::memcpy(slot, data, pool.info->size);
        }
        pool.version += 1;
        m_sink->on_update(e, c, slot, slot);
        return;
    }

    // Append path — grow dense if needed, write trailing slot.
    pool.grow_dense(pool.count + 1U);
    const crd::u32 new_dense = pool.count;
    crd::u8* slot = pool.slot_bytes(new_dense);

    if (pool.info->move_construct != nullptr)
    {
        pool.info->move_construct(slot, data);
    }
    else
    {
        std::memcpy(slot, data, pool.info->size);
    }
    pool.entities.push_back(e);
    pool.set_sparse(ei, new_dense);
    pool.count += 1;
    pool.version += 1;
    m_sink->on_insert(e, c, slot);
}

// v1m4b2 helper — read pool-idx from a parallel shared-pool-idx array.
// Returns kInvalidIdx if the dense slot is owned OR if the array hasn't
// been resized to cover the index yet (defaults to "owned").
namespace
{
inline crd::u32 pool_idx_at(const crd::containers::Array<crd::u32>& spi, crd::u32 dense_idx) noexcept
{
    if (dense_idx >= spi.size())
    {
        return SharedComponentPool::kInvalidIdx;
    }
    return spi[dense_idx];
}
} // namespace

void SparseSetStorage::insert_shared(EntityId e, ComponentId c, const void* data)
{
    CRD_ASSERT(data != nullptr);
    Pool& pool = ensure_pool(c);
    CRD_ASSERT(pool.info->inherit_policy == InheritPolicy::Inherit
               && "insert_shared called for non-Inherit component — programmer error");

    const crd::u32 ei       = e.index();
    const crd::u32 existing = pool.get_sparse(ei);
    CRD_ASSERT(existing == kInvalidDenseIndex
               && "insert_shared: entity already has this component (UPSERT path NYI for shared)");
    (void)existing; // Used only by CRD_ASSERT in debug; silence release-mode unused-var warning.

    // Lazy-create the shared pool on first shared insert.
    if (pool.shared_pool == nullptr)
    {
        void* mem = m_alloc->allocate(sizeof(SharedComponentPool), alignof(SharedComponentPool));
        CRD_ASSERT(mem != nullptr);
        pool.shared_pool = ::new (mem)
            SharedComponentPool(m_alloc, pool.info->size, pool.info->alignment);
    }

    // v1m4b3: content-hash dedup. Byte-identical inserts (cross-call too)
    // share ONE pool entry — refcount tracks the share count. FNV-1a 64.
    crd::u64 hash = 14695981039346656037ULL;
    const crd::u8* src_bytes = static_cast<const crd::u8*>(data);
    for (crd::usize i = 0; i < pool.info->size; ++i)
    {
        hash ^= src_bytes[i];
        hash *= 1099511628211ULL;
    }
    // 0 is a sentinel for "no hash" inside the pool (`acquire`-only path);
    // ensure a non-zero hash by setting bit 63 if all bytes happened to mix
    // to 0. Probability is negligible but the sentinel must be unambiguous.
    if (hash == 0U)
    {
        hash = 1ULL;
    }
    const crd::u32 pool_idx = pool.shared_pool->acquire_or_retain(data, hash);

    // Allocate the dense slot, but DON'T write to dense bytes — shared slots
    // read through the pool indirection. The dense buffer is wasted memory
    // for shared slots in v1m4b2; v1m4b3's dedup mitigates this in the
    // common case (many instances → one pool entry).
    pool.grow_dense(pool.count + 1U);
    const crd::u32 new_dense = pool.count;

    pool.entities.push_back(e);
    pool.set_sparse(ei, new_dense);
    if (pool.shared_pool_idx.size() <= new_dense)
    {
        pool.shared_pool_idx.resize(new_dense + 1U, SharedComponentPool::kInvalidIdx);
    }
    pool.shared_pool_idx[new_dense] = pool_idx;
    pool.count   += 1U;
    pool.version += 1;
    m_sink->on_insert(e, c, pool.shared_pool->entry_bytes(pool_idx));
}

void SparseSetStorage::remove(EntityId e, ComponentId c)
{
    Pool* pool = find_pool(c);
    if (pool == nullptr)
    {
        return; // no pool ever created for c — entity can't have it
    }
    const crd::u32 ei = e.index();
    const crd::u32 dense_idx = pool->get_sparse(ei);
    if (dense_idx == kInvalidDenseIndex)
    {
        return; // entity does not have this component — no-op (matches archetype semantics)
    }

    const crd::u32 our_pool_idx = pool_idx_at(pool->shared_pool_idx, dense_idx);
    const bool is_shared = (our_pool_idx != SharedComponentPool::kInvalidIdx);

    // Fire on_remove before any destruct so the sink may inspect the value.
    {
        const crd::u8* slot = is_shared
                                  ? pool->shared_pool->entry_bytes(our_pool_idx)
                                  : pool->slot_bytes(dense_idx);
        m_sink->on_remove(e, c, slot);
    }

    if (is_shared)
    {
        pool->shared_pool->release(our_pool_idx);
    }
    else if (pool->info->destruct != nullptr)
    {
        pool->info->destruct(pool->slot_bytes(dense_idx));
    }

    const crd::u32 last = pool->count - 1U;
    if (dense_idx != last)
    {
        const crd::u32 trailing_pool_idx = pool_idx_at(pool->shared_pool_idx, last);
        const bool trailing_is_shared = (trailing_pool_idx != SharedComponentPool::kInvalidIdx);

        // Move dense bytes only when the TRAILING slot is owned. Shared
        // trailing slots have no relevant dense bytes; only the pool_idx
        // moves.
        if (!trailing_is_shared)
        {
            crd::u8* slot     = pool->slot_bytes(dense_idx);
            crd::u8* trailing = pool->slot_bytes(last);
            if (pool->info->move_construct != nullptr)
            {
                pool->info->move_construct(slot, trailing);
                if (pool->info->destruct != nullptr)
                {
                    pool->info->destruct(trailing);
                }
            }
            else
            {
                std::memcpy(slot, trailing, pool->info->size);
            }
        }

        // Move the shared_pool_idx in lockstep with entities[].
        if (last < pool->shared_pool_idx.size())
        {
            if (dense_idx >= pool->shared_pool_idx.size())
            {
                pool->shared_pool_idx.resize(dense_idx + 1U, SharedComponentPool::kInvalidIdx);
            }
            pool->shared_pool_idx[dense_idx] = pool->shared_pool_idx[last];
        }

        const EntityId moved = pool->entities[last];
        pool->entities[dense_idx] = moved;
        pool->set_sparse(moved.index(), dense_idx);
    }

    // Reset the (now-unused) trailing slot's shared_pool_idx so it doesn't
    // leak past pool->count's range.
    if (last < pool->shared_pool_idx.size())
    {
        pool->shared_pool_idx[last] = SharedComponentPool::kInvalidIdx;
    }

    pool->entities.pop_back();
    pool->set_sparse(ei, kInvalidDenseIndex);
    pool->count -= 1U;
    pool->version += 1;
}

bool SparseSetStorage::has(EntityId e, ComponentId c) const
{
    const Pool* pool = find_pool(c);
    if (pool == nullptr)
    {
        return false;
    }
    return pool->get_sparse(e.index()) != kInvalidDenseIndex;
}

void* SparseSetStorage::get_mut(EntityId e, ComponentId c)
{
    Pool* pool = find_pool(c);
    if (pool == nullptr)
    {
        return nullptr;
    }
    const crd::u32 dense_idx = pool->get_sparse(e.index());
    if (dense_idx == kInvalidDenseIndex)
    {
        return nullptr;
    }

    // v1m4b2/v1m4b3: CoW break. If this slot is shared, copy pool bytes
    // into the dense slot, decrement the pool entry's refcount, and clear
    // the shared_pool_idx so subsequent reads/writes hit the inline path.
    // Idempotent — calling get_mut twice on a previously-shared slot is
    // safe; the second call sees an owned slot and goes straight through.
    if (dense_idx < pool->shared_pool_idx.size())
    {
        const crd::u32 pool_idx = pool->shared_pool_idx[dense_idx];
        if (pool_idx != SharedComponentPool::kInvalidIdx && pool->shared_pool != nullptr)
        {
            std::memcpy(pool->slot_bytes(dense_idx),
                        pool->shared_pool->entry_bytes(pool_idx),
                        pool->info->size);
            pool->shared_pool->release(pool_idx);
            pool->shared_pool_idx[dense_idx] = SharedComponentPool::kInvalidIdx;
        }
    }

    crd::u8* slot = pool->slot_bytes(dense_idx);
    pool->version += 1;             // declared write — pool-grain change detect
    m_sink->on_update(e, c, slot, slot);
    return slot;
}

const void* SparseSetStorage::get_const(EntityId e, ComponentId c) const
{
    const Pool* pool = find_pool(c);
    if (pool == nullptr)
    {
        return nullptr;
    }
    const crd::u32 dense_idx = pool->get_sparse(e.index());
    if (dense_idx == kInvalidDenseIndex)
    {
        return nullptr;
    }
    // v1m4b2: shared slots read through the SharedComponentPool indirection.
    if (dense_idx < pool->shared_pool_idx.size())
    {
        const crd::u32 pool_idx = pool->shared_pool_idx[dense_idx];
        if (pool_idx != SharedComponentPool::kInvalidIdx && pool->shared_pool != nullptr)
        {
            return pool->shared_pool->entry_bytes(pool_idx);
        }
    }
    return pool->slot_bytes(dense_idx);
}

void SparseSetStorage::for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data)
{
    if (fn == nullptr)
    {
        return;
    }

    // SparseSet pool's effective archetype mask = {c}. The iteration contract
    // (mirror of ArchetypeChunkStorage) yields a chunk iff `required ⊆ pool.mask`.
    // Equivalently:
    //   - empty required               → yield every non-empty pool
    //   - required has exactly one bit → yield the pool for that bit
    //   - required has > 1 bit         → yield nothing (a single SparseSet pool
    //                                      can't satisfy multi-bit AND; v1e's
    //                                      mixed visitor handles intersections)
    const crd::u32 popcount = required.popcount();
    if (popcount > 1U)
    {
        return;
    }

    auto yield_pool = [&](Pool& pool, ComponentId c)
    {
        if (pool.count == 0U)
        {
            return;
        }
        ChunkView view{};
        view.entities = pool.entities.data();
        view.entity_count = pool.count;
        view.present_mask = ComponentMask{};
        view.present_mask.set(c);
        // GEO-7: the SoA table — the pool's dense array IS the component array, EXCEPT when CoW sharing is live
        // (shared slots' bytes live in the shared pool, not `dense`) — those pools yield an empty table and
        // consumers fall back to per-entity access.
        if (pool.shared_pool == nullptr)
        {
            view.component_count       = 1;
            view.component_ids[0]      = c;
            view.component_arrays[0]   = pool.dense;
            view.component_versions[0] = pool.version;
        }
        fn(view, user_data);
    };

    if (popcount == 0U)
    {
        for (crd::u32 i = 0; i < m_pools.size(); ++i)
        {
            if (Pool* p = m_pools[i]; p != nullptr)
            {
                yield_pool(*p, ComponentId{static_cast<crd::u16>(i)});
            }
        }
        return;
    }

    // Exactly one bit set — find it and yield only that pool.
    for (crd::u32 word = 0; word < 4; ++word)
    {
        crd::u64 bits = required.bits[word];
        if (bits == 0)
        {
            continue;
        }
        // single bit → exactly one trailing zero count to extract
        crd::u32 bit = 0;
        while ((bits & 1U) == 0U)
        {
            bits >>= 1;
            ++bit;
        }
        const crd::u32 c_raw = (word * 64U) + bit;
        if (c_raw >= m_pools.size())
        {
            return;
        }
        if (Pool* p = m_pools[c_raw]; p != nullptr)
        {
            yield_pool(*p, ComponentId{static_cast<crd::u16>(c_raw)});
        }
        return;
    }
}

void SparseSetStorage::on_entity_destroyed(EntityId e)
{
    // Sweep every live pool. For pools where this entity has a slot, fire
    // on_remove and run the swap-with-last cleanup.
    //
    // NOTE: sink->on_entity_destroyed is fired by World, not here, so the sink
    // sees the singular event exactly once across both backends. This mirrors
    // the contract change applied to ArchetypeChunkStorage in v1d.
    const crd::u32 ei = e.index();
    for (crd::u32 i = 0; i < m_pools.size(); ++i)
    {
        Pool* p = m_pools[i];
        if (p == nullptr)
        {
            continue;
        }
        const crd::u32 dense_idx = p->get_sparse(ei);
        if (dense_idx == kInvalidDenseIndex)
        {
            continue;
        }

        const ComponentId c{static_cast<crd::u16>(i)};

        const crd::u32 our_pool_idx = pool_idx_at(p->shared_pool_idx, dense_idx);
        const bool is_shared = (our_pool_idx != SharedComponentPool::kInvalidIdx);

        // Fire on_remove before destruct.
        const crd::u8* report_bytes = is_shared
                                          ? p->shared_pool->entry_bytes(our_pool_idx)
                                          : p->slot_bytes(dense_idx);
        m_sink->on_remove(e, c, report_bytes);

        if (is_shared)
        {
            p->shared_pool->release(our_pool_idx);
        }
        else if (p->info->destruct != nullptr)
        {
            p->info->destruct(p->slot_bytes(dense_idx));
        }

        // Swap-with-last.
        const crd::u32 last = p->count - 1U;
        if (dense_idx != last)
        {
            const crd::u32 trailing_pool_idx = pool_idx_at(p->shared_pool_idx, last);
            const bool trailing_is_shared = (trailing_pool_idx != SharedComponentPool::kInvalidIdx);

            if (!trailing_is_shared)
            {
                crd::u8* slot     = p->slot_bytes(dense_idx);
                crd::u8* trailing = p->slot_bytes(last);
                if (p->info->move_construct != nullptr)
                {
                    p->info->move_construct(slot, trailing);
                    if (p->info->destruct != nullptr)
                    {
                        p->info->destruct(trailing);
                    }
                }
                else
                {
                    std::memcpy(slot, trailing, p->info->size);
                }
            }

            // Move shared_pool_idx in lockstep with entities[].
            if (last < p->shared_pool_idx.size())
            {
                if (dense_idx >= p->shared_pool_idx.size())
                {
                    p->shared_pool_idx.resize(dense_idx + 1U, SharedComponentPool::kInvalidIdx);
                }
                p->shared_pool_idx[dense_idx] = p->shared_pool_idx[last];
            }
            const EntityId moved = p->entities[last];
            p->entities[dense_idx] = moved;
            p->set_sparse(moved.index(), dense_idx);
        }

        // Clear the (now-unused) trailing slot's pool_idx.
        if (last < p->shared_pool_idx.size())
        {
            p->shared_pool_idx[last] = SharedComponentPool::kInvalidIdx;
        }

        p->entities.pop_back();
        p->set_sparse(ei, kInvalidDenseIndex);
        p->count -= 1U;
        p->version += 1;
    }
}

// ---- Diagnostics ---------------------------------------------------------

crd::u32 SparseSetStorage::entity_count(ComponentId c) const noexcept
{
    const Pool* p = find_pool(c);
    return (p != nullptr) ? p->count : 0U;
}

crd::u32 SparseSetStorage::pool_count() const noexcept
{
    crd::u32 n = 0;
    for (const Pool* p : m_pools)
    {
        if (p != nullptr)
        {
            ++n;
        }
    }
    return n;
}

crd::u64 SparseSetStorage::pool_version(ComponentId c) const noexcept
{
    const Pool* p = find_pool(c);
    return (p != nullptr) ? p->version : 0U;
}

crd::u32 SparseSetStorage::shared_pool_live_count(ComponentId c) const noexcept
{
    const Pool* p = find_pool(c);
    if (p == nullptr || p->shared_pool == nullptr)
    {
        return 0U;
    }
    return p->shared_pool->live_count();
}

} // namespace crd::scene
