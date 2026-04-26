#pragma once

#include <crd/containers/hash.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <functional> // std::equal_to
#include <new>        // placement new
#include <type_traits>
#include <utility>

namespace crd::containers
{
// -----------------------------------------------------------------------
// HashMap<K, V, Hash, KeyEqual>
//
// Open-addressing hash table with Robin Hood probing and *backshift
// deletion* (no tombstones). Power-of-two capacity for mask-based modulo.
//
// Why these choices?
//  - Open addressing: cache-friendly. Every probe stays in the same
//    contiguous array.
//  - Robin Hood: every slot tracks its probe distance from its ideal
//    home. On insert, if we encounter a "richer" slot (smaller distance),
//    we swap with it and continue placing the displaced entry. Result:
//    probe distances are bounded and uniformly distributed.
//  - Backshift on erase: instead of leaving a tombstone that pollutes
//    future lookups, we "shift back" the cluster — successive entries
//    whose probe distance > 0 get moved one slot toward their ideal.
//    Lookups stay fast indefinitely.
//
// Heterogeneous lookup: `find(...)`, `contains(...)`, `erase(...)` are
// template functions that accept any key type the hasher and equality
// predicate accept. Concretely, with `HashMap<String, V>` you can
// `find(StringView{...})` without allocating a temporary String —
// because `DefaultHash<String>` and `DefaultHash<StringView>` are
// pinned to produce the same u64 for identical bytes (v1b
// prerequisite).
//
// No exceptions. OOM in the heap allocator triggers `CRD_FATAL` via
// the underlying IAllocator. There is no `try_insert` for now — could
// be added if a sub-budget allocator workload demands it.
// -----------------------------------------------------------------------

namespace detail
{
// A slot is empty iff distance == kEmptyDistance.
// Distance encodes how far the live entry is from its ideal slot.
// Zero is a valid distance (entry is in its ideal slot).
inline constexpr u8 kEmptyDistance = 0xFFu;
inline constexpr u8 kMaxDistance = 0xFEu; // saturates before sentinel
} // namespace detail

template <typename K, typename V, typename Hash = DefaultHash<K>, typename KeyEqual = std::equal_to<>> class HashMap
{
public:
    using key_type = K;
    using mapped_type = V;
    using size_type = usize;

    // ---- Ctors ----------------------------------------------------

    explicit HashMap(memory::IAllocator* alloc = memory::default_allocator()) noexcept : m_alloc(alloc) {}

    HashMap(usize initial_capacity_hint, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        if (initial_capacity_hint > 0)
        {
            rehash(round_up_pow2(initial_capacity_hint));
        }
    }

    HashMap(const HashMap& other, memory::IAllocator* alloc = nullptr) : m_alloc(alloc ? alloc : other.m_alloc)
    {
        if (other.m_size == 0)
        {
            return;
        }
        rehash(other.m_capacity);
        for (usize i = 0; i < other.m_capacity; ++i)
        {
            if (other.m_dist[i] != detail::kEmptyDistance)
            {
                insert(other.m_keys[i], other.m_vals[i]);
            }
        }
    }

    HashMap(HashMap&& other) noexcept
        : m_alloc(other.m_alloc), m_keys(other.m_keys), m_vals(other.m_vals), m_dist(other.m_dist),
          m_capacity(other.m_capacity), m_mask(other.m_mask), m_size(other.m_size)
    {
        other.m_keys = nullptr;
        other.m_vals = nullptr;
        other.m_dist = nullptr;
        other.m_capacity = 0;
        other.m_mask = 0;
        other.m_size = 0;
    }

    ~HashMap()
    {
        destroy_all();
        free_buffers();
    }

    HashMap& operator=(const HashMap& other)
    {
        if (this == &other)
        {
            return *this;
        }
        clear();
        for (usize i = 0; i < other.m_capacity; ++i)
        {
            if (other.m_dist[i] != detail::kEmptyDistance)
            {
                insert(other.m_keys[i], other.m_vals[i]);
            }
        }
        return *this;
    }

    HashMap& operator=(HashMap&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        destroy_all();
        free_buffers();
        m_alloc = other.m_alloc;
        m_keys = other.m_keys;
        m_vals = other.m_vals;
        m_dist = other.m_dist;
        m_capacity = other.m_capacity;
        m_mask = other.m_mask;
        m_size = other.m_size;
        other.m_keys = nullptr;
        other.m_vals = nullptr;
        other.m_dist = nullptr;
        other.m_capacity = 0;
        other.m_mask = 0;
        other.m_size = 0;
        return *this;
    }

    // ---- Capacity --------------------------------------------------

    usize size() const noexcept { return m_size; }
    usize capacity() const noexcept { return m_capacity; }
    bool empty() const noexcept { return m_size == 0; }

    f32 load_factor() const noexcept
    {
        return m_capacity == 0 ? 0.0f : static_cast<f32>(m_size) / static_cast<f32>(m_capacity);
    }

    static constexpr f32 max_load_factor() noexcept { return kMaxLoadFactor; }

    void reserve(usize element_count)
    {
        const usize required_cap = required_capacity_for(element_count);
        if (required_cap > m_capacity)
        {
            rehash(required_cap);
        }
    }

    void clear() noexcept
    {
        destroy_all();
        // Stamp every dist byte as empty.
        if (m_dist)
        {
            std::memset(m_dist, detail::kEmptyDistance, m_capacity);
        }
        m_size = 0;
    }

    // ---- Modifiers -------------------------------------------------

    // insert: returns true if a NEW entry was inserted.
    // If the key already exists, the existing value is preserved
    // (use `operator[]` or erase+insert if you want overwrite).
    bool insert(const K& key, const V& val) { return emplace(key, val); }

    bool insert(const K& key, V&& val) { return emplace(key, std::move(val)); }

    template <typename... Args> bool emplace(const K& key, Args&&... args)
    {
        ensure_capacity_for_insert();
        return emplace_no_grow(key, std::forward<Args>(args)...);
    }

    // erase: returns true if an entry was removed.
    // Heterogeneous: any key type the hasher + equality accept.
    template <typename Q> bool erase(const Q& key)
    {
        if (m_size == 0)
        {
            return false;
        }
        usize idx;
        if (!find_index(key, idx))
        {
            return false;
        }
        // Destroy current slot.
        if constexpr (!std::is_trivially_destructible_v<K>)
        {
            m_keys[idx].~K();
        }
        if constexpr (!std::is_trivially_destructible_v<V>)
        {
            m_vals[idx].~V();
        }
        m_dist[idx] = detail::kEmptyDistance;
        --m_size;

        // Backshift: walk forward, moving any slot with distance > 0
        // one step backward (toward its ideal home).
        usize cur = idx;
        usize next = (cur + 1) & m_mask;
        while (m_dist[next] != detail::kEmptyDistance && m_dist[next] > 0)
        {
            ::new (static_cast<void*>(&m_keys[cur])) K(std::move(m_keys[next]));
            ::new (static_cast<void*>(&m_vals[cur])) V(std::move(m_vals[next]));
            m_dist[cur] = static_cast<u8>(m_dist[next] - 1);
            if constexpr (!std::is_trivially_destructible_v<K>)
            {
                m_keys[next].~K();
            }
            if constexpr (!std::is_trivially_destructible_v<V>)
            {
                m_vals[next].~V();
            }
            m_dist[next] = detail::kEmptyDistance;
            cur = next;
            next = (next + 1) & m_mask;
        }
        return true;
    }

    // ---- Lookup (heterogeneous) ------------------------------------

    template <typename Q> V* find(const Q& key) noexcept
    {
        usize idx;
        return find_index(key, idx) ? &m_vals[idx] : nullptr;
    }

    template <typename Q> const V* find(const Q& key) const noexcept
    {
        usize idx;
        return find_index(key, idx) ? &m_vals[idx] : nullptr;
    }

    template <typename Q> bool contains(const Q& key) const noexcept
    {
        usize idx;
        return find_index(key, idx);
    }

    // operator[]: returns a reference to the value for `key`. Inserts
    // a default-constructed V if the key is missing.
    V& operator[](const K& key)
    {
        ensure_capacity_for_insert();
        usize idx;
        if (find_index(key, idx))
        {
            return m_vals[idx];
        }
        (void)emplace_no_grow(key);
        // After insertion, find again to get the (possibly relocated)
        // slot due to Robin Hood swaps during placement.
        const bool ok = find_index(key, idx);
        CRD_ASSERT(ok);
        (void)ok;
        return m_vals[idx];
    }

    // ---- Iteration -------------------------------------------------
    //
    // Iterators skip empty slots automatically. Pair-like access via
    // ::key() and ::value() to avoid forcing a `std::pair` copy.

    class const_iterator
    {
    public:
        const_iterator(const HashMap* m, usize idx) noexcept : m_owner(m), m_idx(idx) { advance_to_live(); }

        const_iterator& operator++() noexcept
        {
            ++m_idx;
            advance_to_live();
            return *this;
        }

        const K& key() const noexcept { return m_owner->m_keys[m_idx]; }
        const V& value() const noexcept { return m_owner->m_vals[m_idx]; }

        bool operator==(const const_iterator& o) const noexcept { return m_owner == o.m_owner && m_idx == o.m_idx; }
        bool operator!=(const const_iterator& o) const noexcept { return !(*this == o); }

    private:
        void advance_to_live() noexcept
        {
            while (m_idx < m_owner->m_capacity && m_owner->m_dist[m_idx] == detail::kEmptyDistance)
            {
                ++m_idx;
            }
        }

        const HashMap* m_owner;
        usize m_idx;
    };

    const_iterator begin() const noexcept { return const_iterator{this, 0}; }
    const_iterator end() const noexcept { return const_iterator{this, m_capacity}; }

    // ---- Misc ------------------------------------------------------

    memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    static constexpr f32 kMaxLoadFactor = 0.875f;
    static constexpr usize kInitialCapacity = 8;

    // ---- Internal helpers -----------------------------------------

    static usize round_up_pow2(usize n) noexcept
    {
        if (n <= 1)
        {
            return 1;
        }
        usize p = 1;
        while (p < n)
        {
            p <<= 1;
        }
        return p;
    }

    static usize required_capacity_for(usize element_count) noexcept
    {
        // Capacity must satisfy: element_count <= capacity * kMaxLoadFactor
        // -> capacity >= ceil(element_count / kMaxLoadFactor)
        const usize needed = static_cast<usize>(static_cast<f32>(element_count) / kMaxLoadFactor) + 1;
        return round_up_pow2(needed < kInitialCapacity ? kInitialCapacity : needed);
    }

    usize ideal_slot_for_hash(u64 h) const noexcept
    {
        // Use the upper bits as the slot index. Helps when the hasher's
        // low bits are weak (splitmix64 mixes everything; this is
        // belt-and-suspenders).
        return static_cast<usize>(h) & m_mask;
    }

    template <typename Q> bool find_index(const Q& key, usize& out_idx) const noexcept
    {
        if (m_capacity == 0)
        {
            return false;
        }
        const u64 h = Hash{}(key);
        usize idx = ideal_slot_for_hash(h);
        u8 d = 0;
        while (true)
        {
            if (m_dist[idx] == detail::kEmptyDistance)
            {
                return false;
            }
            if (m_dist[idx] < d)
            {
                // The slot's occupant is "richer" than us — if our key
                // were here it would have evicted them by Robin Hood
                // rules. So our key isn't in the table.
                return false;
            }
            if (m_dist[idx] == d && KeyEqual{}(m_keys[idx], key))
            {
                out_idx = idx;
                return true;
            }
            idx = (idx + 1) & m_mask;
            if (d < detail::kMaxDistance)
            {
                ++d;
            }
        }
    }

    void ensure_capacity_for_insert()
    {
        if (m_capacity == 0)
        {
            rehash(kInitialCapacity);
            return;
        }
        // Grow when adding one more would exceed the load factor.
        if ((m_size + 1) > static_cast<usize>(static_cast<f32>(m_capacity) * kMaxLoadFactor))
        {
            rehash(m_capacity * 2);
        }
    }

    // emplace_no_grow: places a new entry assuming capacity is sufficient.
    // Uses Robin Hood: if probing finds a richer slot, swap and keep going
    // with the displaced entry. Returns true if a new entry was added,
    // false if the key already existed.
    template <typename... Args> bool emplace_no_grow(const K& key, Args&&... args)
    {
        const u64 h = Hash{}(key);
        usize idx = ideal_slot_for_hash(h);
        u8 d = 0;

        // Stage the new entry on the stack first so we can swap during probing.
        alignas(K) std::byte staged_key_storage[sizeof(K)];
        alignas(V) std::byte staged_val_storage[sizeof(V)];
        K* staged_key = ::new (static_cast<void*>(&staged_key_storage[0])) K(key);
        V* staged_val = ::new (static_cast<void*>(&staged_val_storage[0])) V(std::forward<Args>(args)...);

        bool inserted = true;

        while (true)
        {
            if (m_dist[idx] == detail::kEmptyDistance)
            {
                // Empty slot: place here.
                ::new (static_cast<void*>(&m_keys[idx])) K(std::move(*staged_key));
                ::new (static_cast<void*>(&m_vals[idx])) V(std::move(*staged_val));
                m_dist[idx] = d;
                if constexpr (!std::is_trivially_destructible_v<K>)
                {
                    staged_key->~K();
                }
                if constexpr (!std::is_trivially_destructible_v<V>)
                {
                    staged_val->~V();
                }
                ++m_size;
                return inserted;
            }
            if (m_dist[idx] == d && KeyEqual{}(m_keys[idx], *staged_key))
            {
                // Duplicate key. Discard our staged entry; keep existing value.
                if constexpr (!std::is_trivially_destructible_v<K>)
                {
                    staged_key->~K();
                }
                if constexpr (!std::is_trivially_destructible_v<V>)
                {
                    staged_val->~V();
                }
                return false;
            }
            if (m_dist[idx] < d)
            {
                // Robin Hood swap: take this slot, displace the occupant.
                using std::swap;
                K tmp_k = std::move(m_keys[idx]);
                V tmp_v = std::move(m_vals[idx]);
                if constexpr (!std::is_trivially_destructible_v<K>)
                {
                    m_keys[idx].~K();
                }
                if constexpr (!std::is_trivially_destructible_v<V>)
                {
                    m_vals[idx].~V();
                }
                ::new (static_cast<void*>(&m_keys[idx])) K(std::move(*staged_key));
                ::new (static_cast<void*>(&m_vals[idx])) V(std::move(*staged_val));
                const u8 displaced_d = m_dist[idx];
                m_dist[idx] = d;

                // Continue probing with the displaced entry.
                if constexpr (!std::is_trivially_destructible_v<K>)
                {
                    staged_key->~K();
                }
                if constexpr (!std::is_trivially_destructible_v<V>)
                {
                    staged_val->~V();
                }
                staged_key = ::new (static_cast<void*>(&staged_key_storage[0])) K(std::move(tmp_k));
                staged_val = ::new (static_cast<void*>(&staged_val_storage[0])) V(std::move(tmp_v));
                d = displaced_d;
            }
            idx = (idx + 1) & m_mask;
            if (d < detail::kMaxDistance)
            {
                ++d;
            }
        }
    }

    void rehash(usize new_capacity)
    {
        CRD_ASSERT(memory::is_pow2(new_capacity));
        CRD_ASSERT(new_capacity > 0);

        // Save old buffers
        K* old_keys = m_keys;
        V* old_vals = m_vals;
        u8* old_dist = m_dist;
        usize old_cap = m_capacity;

        // Allocate new buffers.
        m_keys = static_cast<K*>(m_alloc->allocate(sizeof(K) * new_capacity, alignof(K)));
        m_vals = static_cast<V*>(m_alloc->allocate(sizeof(V) * new_capacity, alignof(V)));
        m_dist = static_cast<u8*>(m_alloc->allocate(new_capacity, alignof(u8)));
        std::memset(m_dist, detail::kEmptyDistance, new_capacity);
        m_capacity = new_capacity;
        m_mask = new_capacity - 1;
        m_size = 0;

        // Reinsert old live entries.
        if (old_dist)
        {
            for (usize i = 0; i < old_cap; ++i)
            {
                if (old_dist[i] != detail::kEmptyDistance)
                {
                    emplace_no_grow(old_keys[i], std::move(old_vals[i]));
                    if constexpr (!std::is_trivially_destructible_v<K>)
                    {
                        old_keys[i].~K();
                    }
                    if constexpr (!std::is_trivially_destructible_v<V>)
                    {
                        old_vals[i].~V();
                    }
                }
            }
            m_alloc->deallocate(old_keys);
            m_alloc->deallocate(old_vals);
            m_alloc->deallocate(old_dist);
        }
    }

    void destroy_all() noexcept
    {
        if (!m_dist)
        {
            return;
        }
        if constexpr (!std::is_trivially_destructible_v<K> || !std::is_trivially_destructible_v<V>)
        {
            for (usize i = 0; i < m_capacity; ++i)
            {
                if (m_dist[i] != detail::kEmptyDistance)
                {
                    if constexpr (!std::is_trivially_destructible_v<K>)
                    {
                        m_keys[i].~K();
                    }
                    if constexpr (!std::is_trivially_destructible_v<V>)
                    {
                        m_vals[i].~V();
                    }
                }
            }
        }
    }

    void free_buffers() noexcept
    {
        if (m_keys)
        {
            m_alloc->deallocate(m_keys);
        }
        if (m_vals)
        {
            m_alloc->deallocate(m_vals);
        }
        if (m_dist)
        {
            m_alloc->deallocate(m_dist);
        }
        m_keys = nullptr;
        m_vals = nullptr;
        m_dist = nullptr;
        m_capacity = 0;
        m_mask = 0;
        m_size = 0;
    }

    memory::IAllocator* m_alloc = nullptr;
    K* m_keys = nullptr;
    V* m_vals = nullptr;
    u8* m_dist = nullptr; // kEmptyDistance = empty
    usize m_capacity = 0;
    usize m_mask = 0;
    usize m_size = 0;
};
} // namespace crd::containers
